#include "ledger/book.hpp"
#include "ledger/spsc_queue.hpp"
#include "ledger/types.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t nanoseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count());
}
std::uint64_t number(std::string_view input) {
    std::uint64_t n{};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), n);
    if (input.empty() || error != std::errc{} || end != input.data() + input.size())
        throw std::invalid_argument("expected an unsigned decimal integer");
    return n;
}
ledger::Command lifecycle_command(std::uint64_t index) {
    using namespace ledger;
    const auto base = (index / 5) * 4;
    switch (index % 5) {
    case 0: return {index + 1, base + 1, 9999, 2, Side::Buy, Action::Limit, TimeInForce::GTC};
    case 1: return {index + 1, base + 2, 9999, 1, Side::Sell, Action::Limit, TimeInForce::IOC};
    case 2: return {index + 1, base + 1, 0, 0, Side::Buy, Action::Cancel, TimeInForce::GTC};
    case 3: return {index + 1, base + 3, 10001, 2, Side::Sell, Action::Limit, TimeInForce::GTC};
    default:return {index + 1, base + 4, 10001, 2, Side::Buy, Action::Limit, TimeInForce::IOC};
    }
}
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double p) {
    const auto index = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size()))) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

struct Options {
    std::uint64_t commands{1000000}, requested_samples{10000}, queue_capacity{4096}, consumer_work{};
    int producer_cpu{-1}, consumer_cpu{-1};
};

// A failed affinity request does not silently claim pinned measurements. On
// unsupported platforms a requested CPU is recorded but success stays false.
bool pin_current_thread(int cpu) {
#if defined(__linux__)
    if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
    cpu_set_t requested;
    CPU_ZERO(&requested);
    CPU_SET(cpu, &requested);
    if (pthread_setaffinity_np(pthread_self(), sizeof(requested), &requested) != 0) return false;
    cpu_set_t actual;
    CPU_ZERO(&actual);
    return pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual) == 0
        && CPU_COUNT(&actual) == 1 && CPU_ISSET(cpu, &actual);
#else
    (void)cpu;
    return false;
#endif
}

// Each round depends on the prior state and command sequence; the final state
// is printed, so this bounded synthetic arithmetic cannot be optimized away.
// It models consumer service work, not I/O, a market strategy, or CPU cycles.
std::uint64_t synthetic_work(std::uint64_t state, std::uint64_t sequence, std::uint64_t rounds) {
    for (std::uint64_t round = 0; round < rounds; ++round) {
        state ^= sequence + round + UINT64_C(0x9e3779b97f4a7c15);
        state = std::rotl(state * UINT64_C(0xbf58476d1ce4e5b9), 17);
    }
    return state;
}

void print_cpu(int cpu) {
    if (cpu < 0) std::cout << "null";
    else std::cout << cpu;
}

template <std::size_t Capacity>
void run_benchmark(const Options& options) {
    const auto commands = options.commands;
    const auto requested_samples = options.requested_samples;
    const std::uint64_t stride = requested_samples == 0 ? commands + 1
        : (commands + requested_samples - 1) / requested_samples;
    const auto sample_count = requested_samples == 0 ? 0 : (commands + stride - 1) / stride;
    std::vector<std::uint64_t> enqueue_time(static_cast<std::size_t>(sample_count));
    std::vector<std::uint64_t> latency(static_cast<std::size_t>(sample_count));
    ledger::SpscQueue<ledger::Command, Capacity> queue;
    ledger::FastBook book(ledger::Config{9900, 10100, 64});
    std::array<ledger::Event, ledger::required_event_capacity(64)> event_storage{};
    ledger::EventBuffer output(event_storage);
    std::atomic<bool> start{false};
    std::atomic<unsigned> ready{0};
    // Each counter has exactly one writer. Main observes them only after join.
    std::uint64_t trades = 0, events = 0, rejected = 0, checksum = 0;
    std::uint64_t producer_full_retries = 0, consumer_empty_retries = 0;
    std::uint64_t produced = 0, consumed = 0, work_checksum = 0;
    bool producer_pinned = false, consumer_pinned = false;
    std::thread producer([&] {
        producer_pinned = pin_current_thread(options.producer_cpu);
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t i = 0; i < commands; ++i) {
            const auto command = lifecycle_command(i);
            // Time precedes the first enqueue attempt: full-queue waiting is
            // intentionally included, and queue publication orders this write.
            if (requested_samples != 0 && i % stride == 0) enqueue_time[i / stride] = nanoseconds();
            std::uint32_t spins = 0;
            while (!queue.try_push(command)) {
                ++producer_full_retries;
                if (++spins % 256 == 0) std::this_thread::yield();
            }
            ++produced;
        }
    });
    std::thread consumer([&] {
        consumer_pinned = pin_current_thread(options.consumer_cpu);
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t i = 0; i < commands; ++i) {
            ledger::Command command;
            std::uint32_t spins = 0;
            while (!queue.try_pop(command)) {
                ++consumer_empty_retries;
                if (++spins % 256 == 0) std::this_thread::yield();
            }
            ++consumed;
            const auto result = book.process(command, output);
            if (requested_samples != 0 && i % stride == 0) latency[i / stride] = nanoseconds() - enqueue_time[i / stride];
            if (result != ledger::ProcessResult::Applied || command.sequence != i + 1) ++rejected;
            for (const auto& event : output.events()) {
                ++events; checksum += event.id + event.quantity;
                if (event.type == ledger::EventType::Trade) ++trades;
            }
            // Measurement ends at process, before this command's added work.
            // Work still contributes to subsequent queue waiting and throughput.
            work_checksum = synthetic_work(work_checksum, command.sequence, options.consumer_work);
        }
    });
    // Affinity and thread initialization finish outside the measured interval.
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    const auto began = Clock::now();
    start.store(true, std::memory_order_release);
    producer.join(); consumer.join();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - began).count();
    constexpr std::array<std::uint64_t, 5> partial_events{0, 1, 5, 6, 7};
    const auto expected_trades = (commands / 5) * 2 + (commands % 5 >= 2 ? 1 : 0);
    const auto expected_events = (commands / 5) * 11 + partial_events[commands % 5];
    // One complete lifecycle contributes 11 report IDs plus quantities; cycle
    // j (zero-based, IDs starting at 4j+1) sums to 44(j+1).
    const auto cycles = commands / 5;
    std::uint64_t expected_checksum = 22 * cycles * (cycles + 1);
    constexpr std::array<std::uint64_t, 5> phase_id_counts{1, 4, 1, 1, 4};
    constexpr std::array<std::uint64_t, 5> phase_offsets{3, 11, 2, 5, 23};
    for (std::uint64_t phase = 0; phase < commands % 5; ++phase)
        expected_checksum += phase_id_counts[phase] * (4 * cycles) + phase_offsets[phase];
    if (rejected != 0 || !queue.empty() || produced != commands || consumed != commands
        || trades != expected_trades || events != expected_events || checksum != expected_checksum)
        throw std::runtime_error("benchmark workload failed validation");
    std::sort(latency.begin(), latency.end());
    std::cout << std::setprecision(10)
        << "{\n  \"workload\": \"synthetic_five_command_lifecycle_small_book\",\n"
        << "  \"commands\": " << commands << ",\n  \"events\": " << events
        << ",\n  \"trades\": " << trades << ",\n  \"checksum\": " << checksum
        << ",\n  \"queue_capacity\": " << Capacity
        << ",\n  \"cpu_pinned\": " << (producer_pinned && consumer_pinned ? "true" : "false")
        << ",\n  \"producer_cpu\": ";
    print_cpu(options.producer_cpu);
    std::cout << ",\n  \"consumer_cpu\": ";
    print_cpu(options.consumer_cpu);
    std::cout << ",\n  \"producer_pin_success\": " << (producer_pinned ? "true" : "false")
        << ",\n  \"consumer_pin_success\": " << (consumer_pinned ? "true" : "false")
        << ",\n  \"producer_full_retries\": " << producer_full_retries
        << ",\n  \"consumer_empty_retries\": " << consumer_empty_retries
        << ",\n  \"produced_commands\": " << produced
        << ",\n  \"consumed_commands\": " << consumed
        << ",\n  \"consumer_work\": " << options.consumer_work
        << ",\n  \"synthetic_work_checksum\": " << work_checksum
        << ",\n  \"synthetic_work_method\": \"dependent_u64_xor_multiply_rotate_rounds_after_process\""
        << ",\n  \"elapsed_seconds\": " << elapsed << ",\n  \"commands_per_second\": " << static_cast<double>(commands) / elapsed
        << ",\n  \"latency_samples\": " << latency.size();
    if (!latency.empty()) {
        long double total = 0; for (auto value : latency) total += value;
        std::cout << ",\n  \"enqueue_attempt_to_core_process_ns\": {\"min\": " << latency.front()
            << ", \"mean\": " << static_cast<double>(total / latency.size())
            << ", \"p50\": " << percentile(latency, 0.5)
            << ", \"p90\": " << percentile(latency, 0.9)
            << ", \"p99\": " << percentile(latency, 0.99)
            << ", \"p999\": " << percentile(latency, 0.999)
            << ", \"max\": " << latency.back() << "}";
    }
    std::cout << "\n}\n";
}
}

int main(int argc, char** argv) {
    try {
        Options options;
        bool seen_commands = false, seen_samples = false, seen_capacity = false;
        bool seen_work = false, seen_producer_cpu = false, seen_consumer_cpu = false;
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: bench_spsc [--commands 1..20000000] [--latency-samples 0..commands] "
                "[--queue-capacity 8|64|512|4096] [--consumer-work 0..4096] "
                "[--producer-cpu N] [--consumer-cpu N]\n";
            return 0;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            if (arg == "--commands" && !seen_commands) {
                options.commands = number(argv[++i]); seen_commands = true;
            } else if (arg == "--latency-samples" && !seen_samples) {
                options.requested_samples = number(argv[++i]); seen_samples = true;
            } else if (arg == "--queue-capacity" && !seen_capacity) {
                options.queue_capacity = number(argv[++i]); seen_capacity = true;
            } else if (arg == "--consumer-work" && !seen_work) {
                options.consumer_work = number(argv[++i]); seen_work = true;
            } else if ((arg == "--producer-cpu" && !seen_producer_cpu)
                       || (arg == "--consumer-cpu" && !seen_consumer_cpu)) {
                const auto cpu = number(argv[++i]);
                if (cpu > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                    throw std::invalid_argument("CPU index out of range");
                if (arg == "--producer-cpu") { options.producer_cpu = static_cast<int>(cpu); seen_producer_cpu = true; }
                else { options.consumer_cpu = static_cast<int>(cpu); seen_consumer_cpu = true; }
            } else throw std::invalid_argument("unknown or duplicate option");
        }
        if (options.commands == 0 || options.commands > 20000000) throw std::invalid_argument("commands out of range");
        if (!seen_samples) options.requested_samples = std::min(options.requested_samples, options.commands);
        if (options.requested_samples > options.commands) throw std::invalid_argument("samples exceed commands");
        if (options.consumer_work > 4096) throw std::invalid_argument("consumer work out of range");
        switch (options.queue_capacity) {
        case 8: run_benchmark<8>(options); break;
        case 64: run_benchmark<64>(options); break;
        case 512: run_benchmark<512>(options); break;
        case 4096: run_benchmark<4096>(options); break;
        default: throw std::invalid_argument("queue capacity must be 8, 64, 512, or 4096");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bench_spsc: " << error.what() << '\n'; return 2;
    }
}
