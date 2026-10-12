#include "ledger/book.hpp"
#include "ledger/feed.hpp"
#include "ledger/reference_book.hpp"
#include "ledger/workload.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_set>
#ifdef __linux__
#include <sched.h>
#include <sys/utsname.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
constexpr std::uint64_t initial_checksum = 14695981039346656037ULL;
template<class T> T number(const std::string& value, const std::string& name) {
    T out{}; const auto [end, ec] = std::from_chars(value.data(), value.data()+value.size(), out);
    if (ec != std::errc{} || end != value.data()+value.size()) throw std::invalid_argument("invalid " + name);
    return out;
}
std::string quote(const std::string& value) {
    std::string out = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 32) out += static_cast<char>(c);
    }
    return out + '"';
}
std::string first_line(const std::string& path) { std::ifstream in(path); std::string line; std::getline(in, line); return line; }
std::string compiler_version() {
#if defined(__VERSION__)
    return __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown compiler";
#endif
}
std::string cpu_model() {
    std::ifstream in("/proc/cpuinfo"); std::string line;
    while (std::getline(in, line)) if (line.starts_with("model name")) {
        const auto pos = line.find(':'); if (pos != std::string::npos) return line.substr(pos + 2);
    }
    return "unavailable";
}
std::string machine() {
#ifdef __linux__
    utsname name{}; if (uname(&name) == 0) return std::string(name.sysname) + " " + name.release + " " + name.machine;
#endif
    return "unavailable";
}
std::string affinity() {
#ifdef __linux__
    cpu_set_t set; CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        std::string result;
        for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) {
            if (!result.empty()) result += ',';
            result += std::to_string(i);
        }
        return result;
    }
#endif
    return "unavailable";
}
void mix(std::uint64_t& sum, std::uint64_t value) noexcept { sum ^= value; sum *= 1099511628211ULL; }
void consume(std::uint64_t& checksum, std::uint64_t& trade_checksum, std::span<const ledger::Event> events) noexcept {
    for (const auto& e : events) {
        mix(checksum, e.sequence); mix(checksum, static_cast<unsigned>(e.type));
        mix(checksum, static_cast<unsigned>(e.reason)); mix(checksum, e.id); mix(checksum, e.contra_id);
        mix(checksum, static_cast<unsigned>(e.side)); mix(checksum, std::bit_cast<std::uint64_t>(e.price));
        mix(checksum, e.quantity); mix(checksum, e.remaining);
        if (e.type == ledger::EventType::Trade) {
            mix(trade_checksum, e.sequence); mix(trade_checksum, e.id); mix(trade_checksum, e.contra_id);
            mix(trade_checksum, std::bit_cast<std::uint64_t>(e.price)); mix(trade_checksum, e.quantity);
        }
    }
}
struct Pass {
    std::uint64_t checksum{initial_checksum}, trade_checksum{initial_checksum}, rejected{}, event_count{}, total_ns{};
    ledger::Statistics stats;
    std::size_t live_orders{};
    std::uint64_t resting_quantity{};
    ledger::Price best_bid{}, best_ask{};
};
bool same(const Pass& a, const Pass& b) {
    return a.checksum == b.checksum && a.trade_checksum == b.trade_checksum && a.rejected == b.rejected
        && a.event_count == b.event_count && a.stats == b.stats && a.live_orders == b.live_orders
        && a.resting_quantity == b.resting_quantity && a.best_bid == b.best_bid && a.best_ask == b.best_ask;
}
template<class Factory>
Pass pass(Factory factory, std::span<const ledger::Command> commands, ledger::Config config,
          std::span<std::uint64_t> latencies = {}) {
    auto book = factory();
    std::vector<ledger::Event> storage(ledger::required_event_capacity(config.max_orders));
    ledger::EventBuffer events(storage); Pass result;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        events.clear(); ledger::ProcessResult status;
        if (latencies.empty()) status = book.process(commands[i], events);
        else {
            const auto before = Clock::now();
            status = book.process(commands[i], events);
            const auto after = Clock::now();
            latencies[i] = static_cast<std::uint64_t>(std::chrono::duration_cast<Nanoseconds>(after-before).count());
        }
        if (status == ledger::ProcessResult::OutputFull) throw std::runtime_error("benchmark event buffer unexpectedly full");
        result.rejected += status == ledger::ProcessResult::Rejected;
        result.event_count += events.events().size();
        consume(result.checksum, result.trade_checksum, events.events());
    }
    result.total_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<Nanoseconds>(Clock::now()-start).count());
    result.stats = book.stats(); result.live_orders = book.size(); result.resting_quantity = book.resting_quantity();
    result.best_bid = book.best_bid(); result.best_ask = book.best_ask();
    if (!book.check_invariants()) throw std::runtime_error("benchmark invariant check failed");
    return result;
}
struct Trial {
    Pass throughput;
    std::uint64_t p50{}, p99{}, p999{}, maximum{};
    double ops_per_second{};
};
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, std::size_t numerator, std::size_t denominator) {
    return sorted[(sorted.size() * numerator + denominator - 1) / denominator - 1];
}
template<class T> T median(std::vector<T> values) {
    std::sort(values.begin(), values.end()); return values[values.size()/2];
}
std::uint64_t timer_overhead() {
    std::vector<std::uint64_t> samples(20000);
    for (auto& sample : samples) {
        const auto before = Clock::now(); const auto after = Clock::now();
        sample = static_cast<std::uint64_t>(std::chrono::duration_cast<Nanoseconds>(after-before).count());
    }
    return median(samples);
}
template<class Factory>
std::vector<Trial> benchmark(const std::string& name, Factory factory, std::span<const ledger::Command> commands,
                            ledger::Config config, unsigned runs, std::size_t warmup,
                            std::ofstream& raw, std::ofstream& run_csv, Pass& expected, bool& have_expected) {
    std::vector<Trial> trials; trials.reserve(runs);
    std::vector<std::uint64_t> latencies(commands.size()); // allocated before measured processing
    for (unsigned run = 1; run <= runs; ++run) {
        // A distinct book avoids replaying duplicate IDs into the measured initial state.
        (void)pass(factory, commands.first(std::min(warmup, commands.size())), config);
        auto throughput = pass(factory, commands, config); // aggregate timing; no per-command clock calls
        (void)pass(factory, commands.first(std::min(warmup, commands.size())), config);
        const auto measured = pass(factory, commands, config, latencies);
        if (!same(throughput, measured)) throw std::runtime_error(name + " changed behavior between timing passes");
        if (!have_expected) { expected = throughput; have_expected = true; }
        else if (!same(expected, throughput)) throw std::runtime_error(name + " differs from reference event/trade checksum or state");
        std::sort(latencies.begin(), latencies.end());
        // Lossless compression of all raw latencies: exact nanosecond value and frequency.
        // Counts sum to commands.size() for every engine/run; no sampling or histogram binning.
        for (std::size_t i = 0; i < latencies.size();) {
            auto end = i + 1; while (end < latencies.size() && latencies[end] == latencies[i]) ++end;
            raw << name << ',' << run << ',' << latencies[i] << ',' << end-i << '\n'; i = end;
        }
        Trial trial{throughput, percentile(latencies, 50, 100), percentile(latencies, 99, 100),
                    percentile(latencies, 999, 1000), latencies.back(),
                    static_cast<double>(commands.size()) * 1e9 / static_cast<double>(throughput.total_ns)};
        run_csv << name << ',' << run << ',' << commands.size() << ',' << throughput.total_ns << ','
                << std::setprecision(12) << trial.ops_per_second << ',' << trial.p50 << ',' << trial.p99 << ','
                << trial.p999 << ',' << trial.maximum << ',' << throughput.rejected << ','
                << throughput.event_count << ',' << throughput.stats.trade_count << ',' << throughput.checksum
                << ',' << throughput.trade_checksum << '\n';
        trials.push_back(trial);
        std::cerr << name << " run " << run << '/' << runs << ": " << std::fixed << std::setprecision(2)
                  << trial.ops_per_second / 1e6 << " M commands/s, p99 " << trial.p99 << " ns\n";
    }
    return trials;
}
void report_engine(std::ostream& out, const std::string& name, const std::vector<Trial>& trials, bool comma) {
    std::vector<double> throughput; std::vector<std::uint64_t> p50, p99, p999, maxima;
    for (const auto& t : trials) {
        throughput.push_back(t.ops_per_second); p50.push_back(t.p50); p99.push_back(t.p99);
        p999.push_back(t.p999); maxima.push_back(t.maximum);
    }
    const auto& first = trials.front().throughput;
    if (comma) out << ",\n";
    out << "    " << quote(name) << ": {\"median_commands_per_second\":" << median(throughput)
        << ",\"median_p50_ns\":" << median(p50) << ",\"median_p99_ns\":" << median(p99)
        << ",\"median_p999_ns\":" << median(p999) << ",\"median_run_max_ns\":" << median(maxima)
        << ",\"max_ns\":" << *std::max_element(maxima.begin(), maxima.end())
        << ",\"rejected_commands\":" << first.rejected << ",\"live_orders_final\":" << first.live_orders
        << ",\"events\":" << first.event_count << ",\"trades\":" << first.stats.trade_count
        << ",\"event_checksum\":" << first.checksum << ",\"trade_checksum\":" << first.trade_checksum << '}';
}
}
int main(int argc, char** argv) {
    try {
        std::size_t count = 1'000'000, warmup = 50000; std::uint64_t seed = 20261011;
        unsigned runs = 5; ledger::Config config; std::string input, outdir = "bench/results";
        std::unordered_set<std::string> seen;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "bench_replay [--input FILE | --commands N] [--seed N] [--runs ODD_N] [--warmup N] [--outdir DIR] [--min-price N] [--max-price N] [--capacity N]\n";
                return 0;
            }
            if (!seen.insert(option).second) throw std::invalid_argument("duplicate option: " + option);
            if (i + 1 == argc) throw std::invalid_argument("missing value for " + option);
            const std::string value = argv[++i];
            if (option == "--input") input = value;
            else if (option == "--outdir") outdir = value;
            else if (option == "--commands") count = number<std::size_t>(value, option);
            else if (option == "--seed") seed = number<std::uint64_t>(value, option);
            else if (option == "--runs") runs = number<unsigned>(value, option);
            else if (option == "--warmup") warmup = number<std::size_t>(value, option);
            else if (option == "--min-price") config.min_price = number<ledger::Price>(value, option);
            else if (option == "--max-price") config.max_price = number<ledger::Price>(value, option);
            else if (option == "--capacity") config.max_orders = number<std::uint32_t>(value, option);
            else throw std::invalid_argument("unknown option: " + option);
        }
        if (!input.empty() && (seen.contains("--commands") || seen.contains("--seed")))
            throw std::invalid_argument("--input cannot be combined with --commands or --seed");
        if (!count || count > 5'000'000) throw std::invalid_argument("commands must be in [1, 5000000]");
        if (!runs || runs > 31 || runs % 2 == 0) throw std::invalid_argument("runs must be odd and in [1, 31]");
        if (warmup > 5'000'000 || outdir.empty()) throw std::invalid_argument("invalid warmup count or output directory");
        if (config.min_price <= 0 || config.max_price < config.min_price
            || config.max_price - config.min_price > 999999 || !config.max_orders || config.max_orders > 1'000'000)
            throw std::invalid_argument("price domain must contain 1..1000000 positive ticks; capacity must be 1..1000000");
        ledger::WorkloadStatistics generated{};
        std::vector<ledger::Command> commands;
        if (input.empty()) {
            ledger::WorkloadGenerator<ledger::ReferenceBook> generator(seed, config);
            commands.reserve(count);
            for (std::size_t i = 0; i < count; ++i) commands.push_back(generator.next());
            generated = generator.stats();
        } else commands = ledger::read_commands(input, 5'000'000);
        if (commands.empty()) throw std::invalid_argument("cannot benchmark an empty feed");
        std::uint64_t limits{}, cancels{}, markets{}, replaces{};
        for (const auto& command : commands) {
            limits += command.action == ledger::Action::Limit; cancels += command.action == ledger::Action::Cancel;
            markets += command.action == ledger::Action::Market; replaces += command.action == ledger::Action::Replace;
        }
        std::filesystem::create_directories(outdir);
        const auto path = std::filesystem::path(outdir);
        std::ofstream raw(path / "latency_histogram.csv"), run_csv(path / "runs.csv");
        if (!raw || !run_csv) throw std::runtime_error("cannot create benchmark CSV files");
        raw << "engine,run,latency_ns,count\n";
        run_csv << "engine,run,commands,throughput_total_ns,commands_per_second,p50_ns,p99_ns,p999_ns,max_ns,rejected_commands,events,trades,event_checksum,trade_checksum\n";
        const auto overhead = timer_overhead(); Pass expected; bool have_expected = false;
        const auto reference = benchmark("reference", [&] { return ledger::ReferenceBook(config); }, commands,
                                         config, runs, warmup, raw, run_csv, expected, have_expected);
        const auto bitmap = benchmark("fast-bitmap", [&] { return ledger::FastBook(config, true); }, commands,
                                      config, runs, warmup, raw, run_csv, expected, have_expected);
        const auto linear = benchmark("fast-linear", [&] { return ledger::FastBook(config, false, false); }, commands,
                                      config, runs, warmup, raw, run_csv, expected, have_expected);
        raw.close(); run_csv.close();
        if (!raw || !run_csv) throw std::runtime_error("failed to write benchmark CSV files");
        std::ofstream report(path / "summary.json"); if (!report) throw std::runtime_error("cannot create benchmark report");
        report << std::setprecision(12) << "{\n  \"schema_version\":1,\n  \"commands\":" << commands.size()
               << ",\n  \"seed\":" << (input.empty() ? std::to_string(seed) : "null")
               << ",\n  \"input\":" << (input.empty() ? "null" : quote(input)) << ",\n  \"runs\":" << runs
               << ",\n  \"warmup_commands\":" << std::min(warmup, commands.size())
               << ",\n  \"config\":{\"min_price\":" << config.min_price << ",\"max_price\":" << config.max_price
               << ",\"max_orders\":" << config.max_orders << "},\n  \"mix\":{\"limit\":" << limits
               << ",\"cancel\":" << cancels << ",\"market\":" << markets << ",\"replace\":" << replaces << "},\n"
               << "  \"generated_peak_live_orders\":" << (input.empty() ? std::to_string(generated.peak_live_orders) : "null")
               << ",\n  \"generated_empty_book_cancels\":" << (input.empty() ? std::to_string(generated.empty_book_cancels) : "null")
               << ",\n  \"generated_capacity_ioc_limits\":" << (input.empty() ? std::to_string(generated.capacity_ioc_limits) : "null")
               << ",\n  \"machine\":{\"system\":" << quote(machine()) << ",\"cpu_model\":" << quote(cpu_model())
               << ",\"logical_cpus\":" << std::thread::hardware_concurrency() << ",\"affinity_cpus\":" << quote(affinity())
               << ",\"memory\":" << quote(first_line("/proc/meminfo")) << ",\"governor_cpu0\":"
               << quote(first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")) << "},\n"
               << "  \"compiler\":{\"version\":" << quote(compiler_version())
#ifdef LEDGER_COMPILER_FLAGS
               << ",\"flags\":" << quote(LEDGER_COMPILER_FLAGS)
#else
               << ",\"flags\":\"not injected; inspect CMakeCache/build invocation\""
#endif
#ifdef __OPTIMIZE__
               << ",\"optimization_enabled\":true"
#else
               << ",\"optimization_enabled\":false"
#endif
#ifdef NDEBUG
               << ",\"ndebug\":true"
#else
               << ",\"ndebug\":false"
#endif
               << "},\n  \"clock\":{\"steady\":" << (Clock::is_steady ? "true" : "false")
               << ",\"period_numerator\":" << Clock::period::num << ",\"period_denominator\":" << Clock::period::den
               << ",\"median_empty_pair_ns\":" << overhead << "},\n"
               << "  \"method\":\"Fresh empty book for each pass; warmed caches; aggregate throughput pass has no per-command clock calls and includes event checksums; latency measures process() only with caller buffer clear and event consumption outside the sample; clocks are not subtracted; exact lossless latency frequencies; medians across runs\",\n"
               << "  \"engines\":{\n";
        report_engine(report, "reference", reference, false);
        report_engine(report, "fast-bitmap", bitmap, true);
        report_engine(report, "fast-linear", linear, true);
        report << "\n  }\n}\n"; report.close(); if (!report) throw std::runtime_error("failed to write benchmark JSON");
        std::cout << (path / "summary.json").string() << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "bench_replay: " << e.what() << '\n'; return 1; }
}
