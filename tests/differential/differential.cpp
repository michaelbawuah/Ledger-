#include "ledger/book.hpp"
#include "ledger/reference_book.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace ledger;
constexpr Config config{1, 1024, 128};
struct Options {
    std::uint64_t seeds{4}, orders{100000}, seed{0x739af142de56b807ULL}, first_seed{}, checkpoint{4096};
    bool linear{true}; std::string output;
};
std::uint64_t number(const std::string& value) {
    if (value.empty() || value[0] == '-') throw std::invalid_argument("expected unsigned integer");
    std::size_t end{}; const auto result = std::stoull(value, &end, 0);
    if (end != value.size()) throw std::invalid_argument("invalid integer: " + value);
    return result;
}
Options arguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help") {
            std::cout << "Usage: ledger_differential [--seeds N] [--orders N] [--seed N] [--first-seed N]\n"
                      << "       [--checkpoint N] [--no-linear] [--output report.json]\n"
                      << "Acceptance: --seeds 100 --orders 1000000\n"
                      << "Partition: run --first-seed 0 --seeds 25, then starts 25/50/75 with the same --seed.\n"
                      << "Every command compares full reports/results/stats/quantities/quotes and conservation;\n"
                      << "full structural validators and FIFO snapshots run at each checkpoint and seed end.\n";
            std::exit(0);
        }
        if (argument == "--no-linear") { options.linear = false; continue; }
        if (++i == argc) throw std::invalid_argument("missing value for " + std::string(argument));
        if (argument == "--seeds") options.seeds = number(argv[i]);
        else if (argument == "--orders") options.orders = number(argv[i]);
        else if (argument == "--seed") options.seed = number(argv[i]);
        else if (argument == "--first-seed") options.first_seed = number(argv[i]);
        else if (argument == "--checkpoint") options.checkpoint = number(argv[i]);
        else if (argument == "--output") options.output = argv[i];
        else throw std::invalid_argument("unknown argument: " + std::string(argument));
    }
    if (!options.seeds || !options.orders || !options.checkpoint) throw std::invalid_argument("seeds/orders/checkpoint must be positive");
    if (options.seeds > std::numeric_limits<std::uint64_t>::max() - options.first_seed || options.orders > std::numeric_limits<std::uint64_t>::max() / options.seeds)
        throw std::invalid_argument("requested count overflow");
    return options;
}
std::uint64_t next_random(std::uint64_t& state) noexcept {
    auto value = (state += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}
Command generate(std::uint64_t& state, std::uint64_t index) noexcept {
    const auto random = next_random(state); const auto second = next_random(state);
    Command command{}; command.sequence = index + 1;
    command.id = (random >> 7) % 256 + 1;
    command.quantity = static_cast<Quantity>((random >> 27) % 64 + 1);
    command.side = static_cast<Side>((random >> 18) & 1);
    const auto action = random % 100;
    command.action = action < 45 ? Action::Limit : action < 70 ? Action::Cancel : action < 90 ? Action::Replace : Action::Market;
    const auto tif = (random >> 20) % 100;
    command.tif = tif < 72 ? TimeInForce::GTC : tif < 88 ? TimeInForce::IOC : TimeInForce::FOK;
    const auto phase = (index / 16384) % 4;
    if (phase == 0) command.price = 480 + static_cast<Price>(second % 64);
    else if (phase == 1) {
        constexpr std::array<Price, 12> sparse{1, 64, 65, 128, 129, 256, 257, 512, 513, 768, 1023, 1024};
        command.price = sparse[second % sparse.size()];
    } else if (phase == 2) command.price = 512;
    else command.price = static_cast<Price>(second % 1024 + 1);
    // The initial bounded population guarantees pool saturation and quote boundaries.
    if (index < config.max_orders) {
        command.id = index + 1; command.side = Side::Sell; command.action = Action::Limit;
        command.tif = TimeInForce::GTC; command.price = 512; command.quantity = 4;
    } else if (index % 127 == 0) command.quantity = std::numeric_limits<Quantity>::max();
    else if (index % 131 == 0) command.id = std::numeric_limits<OrderId>::max();
    else if (index % 137 == 0) command.id = 0;
    else if (index % 139 == 0) command.quantity = 0;
    else if (index % 149 == 0) command.side = static_cast<Side>(255);
    else if (index % 151 == 0) command.action = static_cast<Action>(255);
    else if (index % 157 == 0) command.tif = static_cast<TimeInForce>(255);
    else if (index % 163 == 0) command.price = index & 1 ? 0 : std::numeric_limits<Price>::max();
    else if (index % 167 == 0 && command.action == Action::Market) command.price = std::numeric_limits<Price>::min();
    return command;
}
std::ostream& print_command(std::ostream& out, const Command& c) {
    return out << "seq=" << c.sequence << " id=" << c.id << " price=" << c.price << " qty=" << c.quantity
               << " side=" << unsigned(c.side) << " action=" << unsigned(c.action) << " tif=" << unsigned(c.tif);
}
void print_event(std::ostream& out, const Event& e) {
    out << "seq=" << e.sequence << " type=" << unsigned(e.type) << " reason=" << unsigned(e.reason)
        << " id=" << e.id << " contra=" << e.contra_id << " side=" << unsigned(e.side)
        << " price=" << e.price << " qty=" << e.quantity << " remaining=" << e.remaining << '\n';
}
[[noreturn]] void failure(const char* description, std::uint64_t seed, const Command& command,
                         const std::array<Command, 16>& history, std::span<const Event> actual, std::span<const Event> expected) {
    std::cerr << "FAIL " << description << " seed=" << seed << ' '; print_command(std::cerr, command) << '\n';
    std::cerr << "Recent deterministic trace:\n";
    const auto start = command.sequence > history.size() ? command.sequence - history.size() : 0;
    for (auto i = start; i < command.sequence; ++i) print_command(std::cerr, history[i % history.size()]) << '\n';
    std::cerr << "Actual reports (" << actual.size() << "):\n";
    for (auto event : actual.first(std::min<std::size_t>(actual.size(), 24))) print_event(std::cerr, event);
    std::cerr << "Reference reports (" << expected.size() << "):\n";
    for (auto event : expected.first(std::min<std::size_t>(expected.size(), 24))) print_event(std::cerr, event);
    throw std::runtime_error(description);
}
bool equal_reports(std::span<const Event> left, std::span<const Event> right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}
template<class Book> bool observable_invariants(const Book& book) noexcept {
    const auto& stats = book.stats();
    const auto bid = book.best_bid(), ask = book.best_ask();
    return book.size() <= config.max_orders &&
           stats.submitted == 2 * stats.executed + stats.cancelled + stats.expired + book.resting_quantity() &&
           ((!bid || !ask) || bid < ask) &&
           (bid == 0 || (bid >= config.min_price && bid <= config.max_price)) &&
           (ask == 0 || (ask >= config.min_price && ask <= config.max_price));
}
struct Counts {
    std::uint64_t commands{}, reports{}, trades{}, applied{}, rejected{}, checkpoints{};
};
void run_seed(std::uint64_t seed, const Options& options, Counts& counts) {
    FastBook fast{config, true}, linear{config, false}; ReferenceBook reference{config};
    std::array<Event, required_event_capacity(config.max_orders)> fast_storage{}, reference_storage{}, linear_storage{};
    EventBuffer fast_output{fast_storage}, reference_output{reference_storage}, linear_output{linear_storage};
    std::array<Command, 16> history{}; auto state = seed;
    for (std::uint64_t index = 0; index < options.orders; ++index) {
        const auto command = generate(state, index); history[index % history.size()] = command;
        const auto fast_result = fast.process(command, fast_output), reference_result = reference.process(command, reference_output);
        auto fail = [&](const char* message) { failure(message, seed, command, history, fast_output.events(), reference_output.events()); };
        if (fast_result != reference_result) fail("process result mismatch");
        if (!equal_reports(fast_output.events(), reference_output.events())) fail("full execution report mismatch");
        if (fast.stats() != reference.stats() || fast.size() != reference.size() || fast.resting_quantity() != reference.resting_quantity() ||
            fast.best_bid() != reference.best_bid() || fast.best_ask() != reference.best_ask()) fail("observable state mismatch");
        if (!observable_invariants(fast) || !observable_invariants(reference)) fail("per-command observable invariant violation");
        if (options.linear) {
            const auto result = linear.process(command, linear_output);
            if (result != reference_result || !equal_reports(linear_output.events(), reference_output.events()) || linear.stats() != reference.stats() ||
                linear.size() != reference.size() || linear.resting_quantity() != reference.resting_quantity() || linear.best_bid() != reference.best_bid() ||
                linear.best_ask() != reference.best_ask() || !observable_invariants(linear))
                failure("linear search mismatch", seed, command, history, linear_output.events(), reference_output.events());
        }
        for (const auto& event : fast_output.events()) {
            if (event.sequence != command.sequence) fail("event sequence mismatch");
            if (event.type == EventType::Trade) {
                if (!event.quantity || !event.contra_id || event.id == event.contra_id || event.price < config.min_price || event.price > config.max_price)
                    fail("invalid execution report");
                ++counts.trades;
            }
            if (event.type == EventType::Filled && event.remaining != 0) fail("filled event has residual");
        }
        ++counts.commands; counts.reports += fast_output.events().size();
        if (fast_result == ProcessResult::Applied) ++counts.applied;
        else if (fast_result == ProcessResult::Rejected) ++counts.rejected;
        else fail("unexpected OutputFull with sufficient storage");
        if ((index + 1) % options.checkpoint == 0 || index + 1 == options.orders) {
            if (!fast.check_invariants() || !reference.check_invariants() || (options.linear && !linear.check_invariants())) fail("full structural invariant violation");
            const auto expected = reference.snapshot();
            if (fast.snapshot() != expected || (options.linear && linear.snapshot() != expected)) fail("full FIFO/level/statistics snapshot mismatch");
            ++counts.checkpoints;
        }
    }
}
void report(std::ostream& out, const Options& options, const Counts& counts, double seconds) {
    out << "{\n  \"status\": \"pass\",\n  \"generator\": \"splitmix64-bounded-adversarial-v1\",\n"
        << "  \"base_seed\": " << options.seed << ",\n  \"first_seed\": " << options.first_seed << ",\n"
        << "  \"seeds\": " << options.seeds << ",\n  \"commands_per_seed\": " << options.orders << ",\n"
        << "  \"commands_compared\": " << counts.commands << ",\n  \"reports_compared\": " << counts.reports << ",\n"
        << "  \"trades_compared\": " << counts.trades << ",\n  \"applied_commands\": " << counts.applied << ",\n"
        << "  \"rejected_commands\": " << counts.rejected << ",\n  \"structural_checkpoints\": " << counts.checkpoints << ",\n"
        << "  \"checkpoint_interval\": " << options.checkpoint << ",\n  \"linear_variant_compared\": " << (options.linear ? "true" : "false") << ",\n"
        << "  \"per_command_checks\": [\"process_result\", \"all_event_fields\", \"statistics\", \"size\", \"resting_quantity\", \"best_quotes\", \"quantity_conservation\", \"uncrossed_quotes\"],\n"
        << "  \"checkpoint_checks\": [\"full_structural_invariants\", \"full_FIFO_orders\", \"all_levels\", \"all_statistics\"],\n"
        << "  \"elapsed_seconds\": " << std::fixed << std::setprecision(6) << seconds << "\n}\n";
}
}
int main(int argc, char** argv) {
    try {
        const auto options = arguments(argc, argv); Counts counts; const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < options.seeds; ++index) {
            auto state = options.seed + options.first_seed + index; const auto seed = next_random(state);
            run_seed(seed, options, counts);
            std::cout << "PASS seed_index=" << options.first_seed + index << " seed=" << seed << " commands=" << options.orders << " cumulative=" << counts.commands << '\n' << std::flush;
        }
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        report(std::cout, options, counts, seconds);
        if (!options.output.empty()) {
            std::ofstream file(options.output); if (!file) throw std::runtime_error("cannot open output report");
            report(file, options, counts, seconds); if (!file) throw std::runtime_error("cannot save output report");
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "Differential verification failed: " << error.what() << '\n'; return 1; }
}
