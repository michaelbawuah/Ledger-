#include "ledger/book.hpp"
#include "ledger/reference_book.hpp"
#include "ledger/study_observer.hpp"
#include "ledger/study_workloads.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#ifdef __linux__
#include <sched.h>
#include <sys/utsname.h>
#endif

namespace {
using namespace ledger;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t initial_checksum = 14695981039346656037ULL;

struct Options {
    std::string profile{"balanced"}, outdir{"bench/results/study/balanced"};
    std::size_t commands{100000}, depth{256};
    std::uint64_t seed{20261011};
    Price domain{20000};
    unsigned runs{3};
    std::vector<std::string> engines{"reference", "fast-linear", "fast-bitmap", "fast-fok-linear"};
};
template<class T> T number(const std::string& value, const std::string& option) {
    T output{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), output);
    if (error != std::errc{} || end != value.data() + value.size())
        throw std::invalid_argument("invalid integer for " + option);
    return output;
}
Options parse(int argc, char** argv) {
    Options options;
    std::unordered_set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help") {
            if (argc != 2) throw std::invalid_argument("--help must be used alone");
            std::cout << "ledger_study --profile NAME --commands N --seed N --depth N --domain N --runs ODD --outdir DIR [--engines reference,fast-linear,fast-bitmap,fast-fok-linear]\n"
                      << "Profiles: balanced,cancel_heavy,trade_storm,replace_heavy,sparse_fok,rejection_stress\n"
                      << "Bounds: commands 1..1000000; depth 1..16384; domain 2*depth..1000000; runs odd 1..9.\n";
            std::exit(0);
        }
        if (!seen.insert(option).second) throw std::invalid_argument("duplicate option: " + option);
        if (++i == argc) throw std::invalid_argument("missing value for " + option);
        const std::string value = argv[i];
        if (option == "--profile") options.profile = value;
        else if (option == "--commands") options.commands = number<std::size_t>(value, option);
        else if (option == "--seed") options.seed = number<std::uint64_t>(value, option);
        else if (option == "--depth") options.depth = number<std::size_t>(value, option);
        else if (option == "--domain") options.domain = number<Price>(value, option);
        else if (option == "--runs") options.runs = number<unsigned>(value, option);
        else if (option == "--outdir") options.outdir = value;
        else if (option == "--engines") {
            options.engines.clear();
            for (std::size_t start = 0; start <= value.size();) {
                const auto end = value.find(',', start);
                options.engines.push_back(value.substr(start, end == std::string::npos ? end : end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
        } else throw std::invalid_argument("unknown option: " + option);
    }
    if (!options.commands || options.commands > 1000000) throw std::invalid_argument("commands must be in [1,1000000]");
    if (!options.depth || options.depth > 16384) throw std::invalid_argument("depth must be in [1,16384]");
    if (options.domain < static_cast<Price>(2 * options.depth) || options.domain > 1000000)
        throw std::invalid_argument("domain must be in [2*depth,1000000]");
    if (!options.runs || options.runs > 9 || !(options.runs % 2)) throw std::invalid_argument("runs must be odd and in [1,9]");
    if (options.outdir.empty()) throw std::invalid_argument("outdir must not be empty");
    const auto profiles = study::profiles();
    if (std::find(profiles.begin(), profiles.end(), options.profile) == profiles.end()) throw std::invalid_argument("unknown profile");
    const std::vector<std::string> valid{"reference", "fast-linear", "fast-bitmap", "fast-fok-linear"};
    std::unordered_set<std::string> selected;
    for (const auto& engine : options.engines)
        if (std::find(valid.begin(), valid.end(), engine) == valid.end() || !selected.insert(engine).second)
            throw std::invalid_argument("unknown or duplicate engine: " + engine);
    return options;
}
std::string quote(const std::string& value) {
    std::string result{"\""};
    constexpr char digits[] = "0123456789abcdef";
    for (const auto character : value) {
        const auto c = static_cast<unsigned char>(character);
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { result += "\\u00"; result += digits[c >> 4]; result += digits[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
std::string cpu_model() {
    std::ifstream input("/proc/cpuinfo");
    for (std::string line; std::getline(input, line);)
        if (line.starts_with("model name")) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) return line.substr(colon + 2);
        }
    return "unavailable";
}
std::string machine() {
#ifdef __linux__
    utsname name{};
    if (uname(&name) == 0) return std::string(name.sysname) + " " + name.release + " " + name.machine;
#endif
    return "unavailable";
}
std::string affinity() {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        std::string result;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &set)) {
            if (!result.empty()) result += ',';
            result += std::to_string(cpu);
        }
        return result;
    }
#endif
    return "unavailable";
}
std::string compiler() {
#ifdef __VERSION__
    return __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unavailable";
#endif
}
std::uint64_t elapsed(Clock::time_point before, Clock::time_point after) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
}
std::uint64_t timer_overhead() {
    std::vector<std::uint64_t> samples(20000);
    for (auto& sample : samples) { const auto before = Clock::now(); const auto after = Clock::now(); sample = elapsed(before, after); }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}
void mix(std::uint64_t& checksum, std::uint64_t value) noexcept { checksum ^= value; checksum *= 1099511628211ULL; }
void consume(std::uint64_t& checksum, std::span<const Event> events) noexcept {
    for (const auto& event : events) {
        mix(checksum, event.sequence); mix(checksum, static_cast<unsigned>(event.type));
        mix(checksum, static_cast<unsigned>(event.reason)); mix(checksum, event.id); mix(checksum, event.contra_id);
        mix(checksum, static_cast<unsigned>(event.side)); mix(checksum, std::bit_cast<std::uint64_t>(event.price));
        mix(checksum, event.quantity); mix(checksum, event.remaining);
    }
}
struct Pass {
    std::uint64_t checksum{initial_checksum}, applied{}, rejected{}, events{}, trades{}, total_ns{};
    Snapshot final;
};
bool same(const Pass& a, const Pass& b) {
    return a.checksum == b.checksum && a.applied == b.applied && a.rejected == b.rejected &&
           a.events == b.events && a.trades == b.trades && a.final == b.final;
}
struct Observation {
    std::uint64_t latency_ns{};
    Action action{};
    ProcessResult result{};
    std::uint32_t fills{}, levels_emptied{};
};
template<class Book> void preload(Book& book, const study::Workload& workload, EventBuffer& output, Pass& pass) {
    for (const auto& command : workload.preload) {
        if (book.process(command, output) != ProcessResult::Applied) throw std::runtime_error("preload command was not applied");
        consume(pass.checksum, output.events());
    }
}
void account(Pass& pass, ProcessResult result, std::span<const Event> events) {
    if (result == ProcessResult::OutputFull) throw std::runtime_error("event buffer unexpectedly full");
    pass.applied += result == ProcessResult::Applied;
    pass.rejected += result == ProcessResult::Rejected;
    pass.events += events.size();
    for (const auto& event : events) pass.trades += event.type == EventType::Trade;
    consume(pass.checksum, events);
}
template<class Factory> Pass pass(Factory factory, const study::Workload& workload, std::span<Observation> records = {}) {
    auto book = factory();
    std::vector<Event> storage(required_event_capacity(workload.config.max_orders));
    EventBuffer output(storage);
    Pass result;
    preload(book, workload, output, result);
    if (records.empty()) {
        const auto start = Clock::now();
        for (const auto& command : workload.commands) {
            const auto status = book.process(command, output);
            account(result, status, output.events());
        }
        result.total_ns = elapsed(start, Clock::now());
    } else {
        study::EventObserver observer(book.snapshot());
        for (std::size_t i = 0; i < workload.commands.size(); ++i) {
            const auto before = Clock::now();
            const auto status = book.process(workload.commands[i], output);
            const auto after = Clock::now();
            const auto mechanics = observer.apply(workload.commands[i], status, output.events());
            records[i] = {elapsed(before, after), workload.commands[i].action, status, mechanics.fills, mechanics.levels_emptied};
            account(result, status, output.events());
        }
        if (!observer.matches(book.snapshot())) throw std::runtime_error("observer differs from final complete resting snapshot");
    }
    if (!book.check_invariants()) throw std::runtime_error("study book invariant failed");
    result.final = book.snapshot();
    return result;
}
template<class Factory> void warmup(Factory factory, const study::Workload& workload) {
    auto book = factory();
    std::vector<Event> storage(required_event_capacity(workload.config.max_orders));
    EventBuffer output(storage);
    Pass ignored;
    preload(book, workload, output, ignored);
    const auto count = std::min<std::size_t>(10000, workload.commands.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto status = book.process(workload.commands[i], output);
        if (status == ProcessResult::OutputFull) throw std::runtime_error("warmup event buffer full");
        consume(ignored.checksum, output.events());
    }
}
void prefix(std::ostream& out, const Options& options) {
    out << options.profile << ',' << options.depth << ',' << options.domain << ',' << options.seed << ',';
}
void timeline(std::ostream& out, const Options& options, std::size_t sequence, const ReferenceBook& book) {
    prefix(out, options);
    out << sequence << ',' << book.size() << ',' << book.resting_quantity() << ','
        << book.best_bid() << ',' << book.best_ask() << ',' << book.stats().trade_count << '\n';
}
void depth(std::ostream& out, const Options& options, std::size_t sequence, const Snapshot& snapshot) {
    for (const auto& level : snapshot.levels) {
        prefix(out, options);
        out << sequence << ',' << (level.side == Side::Buy ? "buy" : "sell") << ','
            << level.price << ',' << level.quantity << ',' << level.orders << '\n';
    }
}
// Data visualization captures and correctness oracle use a fully separate,
// untimed reference replay. snapshot() never runs in headline measurement.
Pass capture(const Options& options, const study::Workload& workload, std::ostream& timelines, std::ostream& depths) {
    ReferenceBook book(workload.config);
    std::vector<Event> storage(required_event_capacity(workload.config.max_orders));
    EventBuffer output(storage);
    Pass result;
    preload(book, workload, output, result);
    timeline(timelines, options, 0, book);
    depth(depths, options, 0, book.snapshot());
    const auto interval = std::max<std::size_t>(1, workload.commands.size() / 100);
    for (std::size_t i = 0; i < workload.commands.size(); ++i) {
        const auto status = book.process(workload.commands[i], output);
        account(result, status, output.events());
        const auto step = i + 1;
        if (!(step % interval) || step == workload.commands.size()) timeline(timelines, options, step, book);
        if (step == workload.commands.size() / 2 || step == workload.commands.size()) depth(depths, options, step, book.snapshot());
    }
    if (!book.check_invariants()) throw std::runtime_error("reference capture invariant failed");
    result.final = book.snapshot();
    return result;
}
std::string action_name(Action action) {
    switch (action) {
        case Action::Limit: return "limit";
        case Action::Market: return "market";
        case Action::Cancel: return "cancel";
        case Action::Replace: return "replace";
    }
    throw std::runtime_error("unexpected action in study");
}
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, std::size_t numerator, std::size_t denominator) {
    return sorted[(sorted.size() * numerator + denominator - 1) / denominator - 1];
}
struct Trial {
    Pass throughput;
    double commands_per_second{};
    std::uint64_t p50{}, p99{}, p999{}, max{};
};
template<class Factory> Trial measure(const std::string& name, Factory factory, const Options& options,
        const study::Workload& workload, unsigned run, const Pass& expected, std::span<Observation> records,
        std::ostream& runs, std::ostream& histogram, std::ostream& diagnostics) {
    warmup(factory, workload);
    auto throughput = pass(factory, workload);
    warmup(factory, workload);
    const auto diagnostic = pass(factory, workload, records);
    if (!same(expected, throughput) || !same(expected, diagnostic))
        throw std::runtime_error(name + " differs from reference checksum, accounting or full snapshot");
    if (!throughput.total_ns) throw std::runtime_error("zero batch measurement duration");
    std::vector<std::uint64_t> latencies(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) latencies[i] = records[i].latency_ns;
    std::sort(latencies.begin(), latencies.end());
    Trial trial{std::move(throughput), 0, percentile(latencies, 50, 100), percentile(latencies, 99, 100),
                percentile(latencies, 999, 1000), latencies.back()};
    trial.commands_per_second = static_cast<double>(options.commands) * 1e9 / static_cast<double>(trial.throughput.total_ns);
    for (std::size_t i = 0; i < latencies.size();) {
        auto end = i + 1;
        while (end < latencies.size() && latencies[end] == latencies[i]) ++end;
        prefix(histogram, options);
        histogram << name << ',' << run << ',' << latencies[i] << ',' << end - i << '\n';
        i = end;
    }
    const auto key = [](const Observation& record) {
        return std::tuple{record.action, record.result, record.fills, record.levels_emptied, record.latency_ns};
    };
    std::sort(records.begin(), records.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
    for (std::size_t i = 0; i < records.size();) {
        auto end = i + 1;
        while (end < records.size() && key(records[end]) == key(records[i])) ++end;
        const auto& record = records[i];
        prefix(diagnostics, options);
        diagnostics << name << ',' << run << ',' << action_name(record.action) << ','
            << (record.result == ProcessResult::Applied ? "applied" : "rejected") << ','
            << record.fills << ',' << record.levels_emptied << ',' << record.latency_ns << ',' << end - i << '\n';
        i = end;
    }
    prefix(runs, options);
    runs << name << ',' << run << ',' << options.commands << ',' << trial.throughput.total_ns << ','
        << std::setprecision(17) << trial.commands_per_second << ',' << trial.p50 << ',' << trial.p99 << ',' << trial.p999 << ','
        << trial.max << ',' << trial.throughput.applied << ',' << trial.throughput.rejected << ',' << trial.throughput.events << ','
        << trial.throughput.trades << ',' << trial.throughput.checksum << ',' << trial.throughput.final.orders.size() << '\n';
    std::cerr << options.profile << ' ' << name << " run " << run << '/' << options.runs << ": "
              << std::fixed << std::setprecision(3) << trial.commands_per_second / 1e6 << " M commands/s, diagnostic p99 " << trial.p99 << " ns\n";
    return trial;
}
template<class T> T median(std::vector<T> values) { std::sort(values.begin(), values.end()); return values[values.size() / 2]; }
void report_engine(std::ostream& output, const std::string& engine, const std::vector<Trial>& trials) {
    std::vector<double> throughput;
    std::vector<std::uint64_t> p50, p99, p999, max;
    for (const auto& trial : trials) {
        throughput.push_back(trial.commands_per_second); p50.push_back(trial.p50); p99.push_back(trial.p99);
        p999.push_back(trial.p999); max.push_back(trial.max);
    }
    const auto& pass = trials.front().throughput;
    const auto& stats = pass.final.stats;
    output << quote(engine) << ":{\"median_commands_per_second\":" << std::setprecision(17) << median(throughput)
        << ",\"median_p50_ns\":" << median(p50) << ",\"median_p99_ns\":" << median(p99)
        << ",\"median_p999_ns\":" << median(p999) << ",\"max_ns\":" << *std::max_element(max.begin(), max.end())
        << ",\"applied\":" << pass.applied << ",\"rejected\":" << pass.rejected << ",\"events\":" << pass.events
        << ",\"trades\":" << pass.trades << ",\"checksum\":" << pass.checksum << ",\"live_orders_final\":" << pass.final.orders.size()
        << ",\"final_stats\":{\"submitted\":" << stats.submitted << ",\"executed\":" << stats.executed
        << ",\"cancelled\":" << stats.cancelled << ",\"expired\":" << stats.expired << ",\"rejected\":" << stats.rejected
        << ",\"trade_count\":" << stats.trade_count << "}}";
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        const auto workload = ledger::study::make_workload(options.profile, options.commands, options.seed, options.depth, options.domain);
        const auto overhead = timer_overhead();
        const std::filesystem::path outdir(options.outdir);
        std::filesystem::create_directories(outdir);
        std::ofstream runs(outdir / "runs.csv"), histogram(outdir / "histogram.csv"), diagnostics(outdir / "diagnostics.csv"),
            timelines(outdir / "timeline.csv"), depths(outdir / "depth.csv");
        if (!runs || !histogram || !diagnostics || !timelines || !depths) throw std::runtime_error("cannot create study CSV files");
        runs << "profile,depth,domain,seed,engine,run,commands,total_ns,commands_per_second,p50_ns,p99_ns,p999_ns,max_ns,applied,rejected,events,trades,checksum,live_orders_final\n";
        histogram << "profile,depth,domain,seed,engine,run,latency_ns,count\n";
        diagnostics << "profile,depth,domain,seed,engine,run,action,result,fills,levels_emptied,latency_ns,count\n";
        timelines << "profile,depth,domain,seed,sequence,live_orders,resting_quantity,best_bid,best_ask,trade_count\n";
        depths << "profile,depth,domain,seed,sequence,side,price,quantity,orders\n";
        const auto expected = capture(options, workload, timelines, depths);
        std::vector<Observation> records(workload.commands.size());
        std::map<std::string, std::vector<Trial>> trials;
        for (unsigned run = 1; run <= options.runs; ++run) {
            for (std::size_t offset = 0; offset < options.engines.size(); ++offset) {
                const auto& engine = options.engines[(offset + run - 1) % options.engines.size()];
                Trial trial;
                if (engine == "reference")
                    trial = measure(engine, [&] { return ReferenceBook(workload.config); }, options, workload, run, expected, records, runs, histogram, diagnostics);
                else {
                    const bool bitmap_search = engine != "fast-linear";
                    const bool bitmap_fok = engine == "fast-bitmap";
                    trial = measure(engine, [&] { return FastBook(workload.config, bitmap_search, bitmap_fok); }, options, workload, run, expected, records, runs, histogram, diagnostics);
                }
                trials[engine].push_back(std::move(trial));
            }
        }
        runs.flush(); histogram.flush(); diagnostics.flush(); timelines.flush(); depths.flush();
        if (!runs || !histogram || !diagnostics || !timelines || !depths) throw std::runtime_error("failed to finish study CSV output");
        std::ofstream summary(outdir / "summary.json");
        if (!summary) throw std::runtime_error("cannot create summary.json");
        summary << "{\n\"schema_version\":1,\"profile\":" << quote(options.profile) << ",\"commands\":" << options.commands
            << ",\"seed\":" << options.seed << ",\"depth\":" << options.depth << ",\"domain\":" << options.domain
            << ",\"runs\":" << options.runs << ",\"preload_commands\":" << workload.preload.size()
            << ",\"config\":{\"min_price\":" << workload.config.min_price << ",\"max_price\":" << workload.config.max_price
            << ",\"max_orders\":" << workload.config.max_orders << "},\n\"engines\":[";
        for (std::size_t i = 0; i < options.engines.size(); ++i) { if (i) summary << ','; summary << quote(options.engines[i]); }
        summary << "],\n\"machine\":{\"os\":" << quote(machine()) << ",\"cpu_model\":" << quote(cpu_model())
            << ",\"hardware_threads\":" << std::thread::hardware_concurrency() << ",\"affinity\":" << quote(affinity())
            << "},\n\"compiler\":{\"version\":" << quote(compiler()) << ",\"flags\":";
#ifdef LEDGER_COMPILER_FLAGS
        summary << quote(LEDGER_COMPILER_FLAGS);
#else
        summary << quote("unavailable");
#endif
        summary << "},\n\"clock\":{\"name\":\"std::chrono::steady_clock\",\"is_steady\":" << (Clock::is_steady ? "true" : "false")
            << ",\"period_num\":" << Clock::period::num << ",\"period_den\":" << Clock::period::den
            << ",\"timer_pair_median_ns\":" << overhead << "},\n\"method\":{"
            << "\"throughput\":\"Separate whole-batch process, status/event counting and all-event checksumming; no per-command clocks or observers. Preload excluded from time.\","
            << "\"latency\":\"Full process-only diagnostic pass, two clock reads per command; histogram and conditional frequencies are exact and derived from the same pass.\","
            << "\"observer\":\"Independent STL order/level maps consume reports outside each process clock interval. Their memory access can affect caches of subsequent commands; diagnostic latency is not headline core latency.\","
            << "\"levels_emptied\":\"Observed levels losing their final resting order through maker fills or cancellations; replace can remove and recreate a level. No inferred internal scan or probe counts.\","
            << "\"checksum_scope\":\"Every field of every preload and measured execution report; measured events/trades exclude preload, complete final snapshots and statistics include preload.\","
            << "\"warmup_commands\":" << std::min<std::size_t>(10000, workload.commands.size()) << ','
            << "\"timer_overhead_subtracted\":false,\"engine_order\":\"Rotate selected order by run index. Separate fresh book warmup before each pass.\","
            << "\"timeline_sequence\":\"Measured command index; zero is state after preload. Reference timeline/depth captures use a completely separate untimed pass.\"},\n\"results\":{";
        for (std::size_t i = 0; i < options.engines.size(); ++i) { if (i) summary << ','; report_engine(summary, options.engines[i], trials.at(options.engines[i])); }
        summary << "}\n}\n";
        summary.flush();
        if (!summary) throw std::runtime_error("failed to finish summary.json");
        std::cout << "Study verified: " << options.profile << ", " << options.commands << " measured commands, "
                  << options.engines.size() << " engines, " << options.runs << " runs; full snapshots and all-event checksums agree.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ledger_study: " << error.what() << '\n';
        return 1;
    }
}
