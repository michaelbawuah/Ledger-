#include "ledger/book.hpp"
#include "ledger/spsc_queue.hpp"
#include "ledger/types.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

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
}

int main(int argc, char** argv) {
    try {
        std::uint64_t commands = 1000000, requested_samples = 10000;
        bool seen_commands = false, seen_samples = false;
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: bench_spsc [--commands 1..20000000] [--latency-samples 0..commands]\n";
            return 0;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            if (arg == "--commands" && !seen_commands) {
                commands = number(argv[++i]); seen_commands = true;
            } else if (arg == "--latency-samples" && !seen_samples) {
                requested_samples = number(argv[++i]); seen_samples = true;
            } else throw std::invalid_argument("unknown or duplicate option");
        }
        if (commands == 0 || commands > 20000000) throw std::invalid_argument("commands out of range");
        if (!seen_samples) requested_samples = std::min(requested_samples, commands);
        if (requested_samples > commands) throw std::invalid_argument("samples exceed commands");
        const std::uint64_t stride = requested_samples == 0 ? commands + 1
            : (commands + requested_samples - 1) / requested_samples;
        const auto sample_count = requested_samples == 0 ? 0 : (commands + stride - 1) / stride;
        std::vector<std::uint64_t> enqueue_time(static_cast<std::size_t>(sample_count));
        std::vector<std::uint64_t> latency(static_cast<std::size_t>(sample_count));
        ledger::SpscQueue<ledger::Command, 4096> queue;
        ledger::FastBook book(ledger::Config{9900, 10100, 64});
        std::array<ledger::Event, ledger::required_event_capacity(64)> event_storage{};
        ledger::EventBuffer output(event_storage);
        std::atomic<bool> start{false};
        std::uint64_t trades = 0, events = 0, rejected = 0, checksum = 0;
        std::thread producer([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::uint64_t i = 0; i < commands; ++i) {
                const auto command = lifecycle_command(i);
                if (requested_samples != 0 && i % stride == 0) enqueue_time[i / stride] = nanoseconds();
                std::uint32_t spins = 0;
                while (!queue.try_push(command)) if (++spins % 256 == 0) std::this_thread::yield();
            }
        });
        std::thread consumer([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::uint64_t i = 0; i < commands; ++i) {
                ledger::Command command;
                std::uint32_t spins = 0;
                while (!queue.try_pop(command)) if (++spins % 256 == 0) std::this_thread::yield();
                const auto result = book.process(command, output);
                if (requested_samples != 0 && i % stride == 0) latency[i / stride] = nanoseconds() - enqueue_time[i / stride];
                if (result != ledger::ProcessResult::Applied || command.sequence != i + 1) ++rejected;
                for (const auto& event : output.events()) {
                    ++events; checksum += event.id + event.quantity;
                    if (event.type == ledger::EventType::Trade) ++trades;
                }
            }
        });
        const auto began = Clock::now();
        start.store(true, std::memory_order_release);
        producer.join(); consumer.join();
        const auto elapsed = std::chrono::duration<double>(Clock::now() - began).count();
        constexpr std::array<std::uint64_t, 5> partial_events{0, 1, 5, 6, 7};
        const auto expected_trades = (commands / 5) * 2 + (commands % 5 >= 2 ? 1 : 0);
        const auto expected_events = (commands / 5) * 11 + partial_events[commands % 5];
        if (rejected != 0 || !queue.empty() || trades != expected_trades || events != expected_events)
            throw std::runtime_error("benchmark workload failed validation");
        std::sort(latency.begin(), latency.end());
        std::cout << std::setprecision(10)
            << "{\n  \"workload\": \"synthetic_five_command_lifecycle_small_book\",\n"
            << "  \"commands\": " << commands << ",\n  \"events\": " << events
            << ",\n  \"trades\": " << trades << ",\n  \"checksum\": " << checksum
            << ",\n  \"queue_capacity\": 4096,\n  \"cpu_pinned\": false,\n"
            << "  \"elapsed_seconds\": " << elapsed << ",\n  \"commands_per_second\": " << static_cast<double>(commands) / elapsed
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bench_spsc: " << error.what() << '\n'; return 2;
    }
}
