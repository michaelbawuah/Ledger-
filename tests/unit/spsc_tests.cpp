#include "ledger/spsc_queue.hpp"
#include "ledger/types.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {
bool boundary_tests() {
    ledger::SpscQueue<std::uint64_t, 4> queue;
    std::uint64_t value = 12345;
    if (!queue.empty() || queue.try_pop(value) || value != 12345) return false;
    for (std::uint64_t cycle = 0; cycle < 10000; ++cycle) {
        for (std::uint64_t j = 0; j < 4; ++j) {
            if (!queue.try_push(cycle * 4 + j)) return false;
        }
        if (queue.empty() || queue.try_push(999999)) return false;
        for (std::uint64_t j = 0; j < 4; ++j) {
            if (!queue.try_pop(value) || value != cycle * 4 + j) return false;
        }
        value = 12345;
        if (!queue.empty() || queue.try_pop(value) || value != 12345) return false;
    }
    ledger::SpscQueue<ledger::Command, 1> one;
    const ledger::Command command{7, 11, 9999, 3, ledger::Side::Sell,
                                  ledger::Action::Limit, ledger::TimeInForce::IOC};
    ledger::Command copy;
    if (!one.try_push(command) || one.try_push(command) || !one.try_pop(copy)
        || copy != command || !one.empty()) return false;
    return true;
}

bool stress_test() {
    ledger::SpscQueue<ledger::Command, 1024> queue;
    constexpr std::uint64_t count = 1000000;
    std::atomic<bool> start{false};
    bool exact = true; // Only consumer writes; parent reads after join.
    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t i = 1; i <= count; ++i) {
            const ledger::Command command{i, i * 3, static_cast<ledger::Price>(i + 1),
                static_cast<ledger::Quantity>((i % 127) + 1), ledger::Side::Sell,
                ledger::Action::Limit, ledger::TimeInForce::IOC};
            while (!queue.try_push(command)) std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t i = 1; i <= count; ++i) {
            ledger::Command command;
            while (!queue.try_pop(command)) std::this_thread::yield();
            const ledger::Command expected{i, i * 3, static_cast<ledger::Price>(i + 1),
                static_cast<ledger::Quantity>((i % 127) + 1), ledger::Side::Sell,
                ledger::Action::Limit, ledger::TimeInForce::IOC};
            if (command != expected) exact = false;
        }
    });
    start.store(true, std::memory_order_release);
    producer.join(); consumer.join();
    return exact && queue.empty();
}
}

int main() {
    if (!boundary_tests()) { std::cerr << "SPSC full/empty/wrap test failed\n"; return 1; }
    if (!stress_test()) { std::cerr << "SPSC million-item sequence test failed\n"; return 1; }
    std::cout << "SPSC: full/empty/wrap and 1,000,000 exact Command transfers passed\n";
}
