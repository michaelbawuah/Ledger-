#include "ledger/book.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace {
thread_local bool tracking = false;
thread_local std::size_t allocations = 0;
void count_allocation() noexcept { if (tracking) ++allocations; }
void* allocate(std::size_t bytes) {
    count_allocation();
    if (auto* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc{};
}
void* allocate_aligned(std::size_t bytes, std::size_t alignment) {
    count_allocation(); void* value{};
    if (posix_memalign(&value, alignment, bytes ? bytes : alignment) == 0) return value;
    throw std::bad_alloc{};
}
}

// Count C++ heap requests, including arrays, aligned storage and nothrow forms.
// Construction, snapshots, diagnostics and printing run outside the measured scope.
void* operator new(std::size_t bytes) { return allocate(bytes); }
void* operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept { try { return allocate(bytes); } catch (...) { return nullptr; } }
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept { try { return allocate(bytes); } catch (...) { return nullptr; } }
void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { std::free(value); }
void* operator new(std::size_t bytes, std::align_val_t alignment) { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t bytes, std::align_val_t alignment) { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); }
void operator delete(void* value, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void* value, std::align_val_t) noexcept { std::free(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); } catch (...) { return nullptr; }
}
void operator delete(void* value, std::align_val_t, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete[](void* value, std::align_val_t, const std::nothrow_t&) noexcept { std::free(value); }

namespace {
using namespace ledger;
static_assert(noexcept(std::declval<FastBook&>().process(std::declval<const Command&>(), std::declval<EventBuffer&>())));
static_assert(noexcept(std::declval<const FastBook&>().stats()));
static_assert(noexcept(std::declval<const FastBook&>().size()));
static_assert(noexcept(std::declval<const FastBook&>().resting_quantity()));
static_assert(noexcept(std::declval<const FastBook&>().best_bid()));
static_assert(noexcept(std::declval<const FastBook&>().best_ask()));

void require(bool valid, const char* description) { if (!valid) throw std::runtime_error(description); }
std::uint64_t random_step(std::uint64_t& state) noexcept {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state;
}
std::size_t test_variant(bool bitmap) {
    constexpr Config config{1, 257, 64}; FastBook book{config, bitmap};
    std::array<Event, required_event_capacity(64)> events{}; EventBuffer output{events};
    std::uint64_t sequence = 0, rng = 0x1b35d4974ea9bd81ULL;
    std::size_t calls = 0, applied = 0, rejected = 0;
    auto run = [&](Command command, EventBuffer& buffer) {
        command.sequence = ++sequence; const auto before = allocations; tracking = true;
        const auto result = book.process(command, buffer);
        (void)book.size(); (void)book.resting_quantity(); (void)book.stats(); (void)book.best_bid(); (void)book.best_ask();
        tracking = false; ++calls;
        require(allocations == before, "hot path requested C++ heap storage");
        if (result == ProcessResult::Applied) ++applied;
        if (result == ProcessResult::Rejected) ++rejected;
        return result;
    };
    // Explicitly exercise maximum-size reports, FOK validation, replacement,
    // cancellation, full capacity, malformed input and OutputFull before churn.
    for (OrderId id = 1; id <= 64; ++id)
        require(run({0, id, 129, 2, Side::Sell, Action::Limit, TimeInForce::GTC}, output) == ProcessResult::Applied, "initial population failed");
    require(run({0, 65, 129, 128, Side::Buy, Action::Limit, TimeInForce::GTC}, output) == ProcessResult::Rejected, "full book GTC accepted");
    require(run({0, 1, 130, 1, Side::Sell, Action::Replace, TimeInForce::GTC}, output) == ProcessResult::Applied, "full book replacement failed");
    require(run({0, 65, 129, 128, Side::Buy, Action::Limit, TimeInForce::FOK}, output) == ProcessResult::Rejected, "insufficient FOK applied");
    require(run({0, 65, 129, 126, Side::Buy, Action::Limit, TimeInForce::FOK}, output) == ProcessResult::Applied, "multi-maker FOK failed");
    require(run({0, 1, 0, 0, Side::Buy, Action::Cancel, TimeInForce::GTC}, output) == ProcessResult::Applied, "cancel failed");
    std::array<Event, 1> undersized{}; EventBuffer short_output{undersized};
    require(run({0, 3, 100, 3, Side::Buy, Action::Limit, TimeInForce::GTC}, short_output) == ProcessResult::OutputFull, "short output changed state");
    for (std::size_t i = 0; i < 500000; ++i) {
        const auto random = random_step(rng); Command command{};
        command.id = (random >> 3) % 96 + 1;
        command.price = static_cast<Price>((random >> 15) % 257 + 1);
        command.quantity = static_cast<Quantity>((random >> 31) % 32 + 1);
        command.side = static_cast<Side>((random >> 10) & 1);
        command.action = static_cast<Action>(random % 4);
        command.tif = static_cast<TimeInForce>((random >> 12) % 3);
        if (i % 97 == 0) command.side = static_cast<Side>(255);
        if (i % 103 == 0) command.quantity = std::numeric_limits<Quantity>::max();
        run(command, output);
    }
    require(book.check_invariants(), "final book invariants failed");
    const auto& statistics = book.stats();
    require(statistics.submitted == 2 * statistics.executed + statistics.cancelled + statistics.expired + book.resting_quantity(), "quantity conservation failed");
    require(applied > 1000 && rejected > 1000, "allocation workload did not exercise both outcomes");
    std::cout << (bitmap ? "bitmap" : "linear") << ": " << calls << " hot-path calls, 0 C++ allocations; " << applied << " applied, " << rejected << " rejected\n";
    return calls;
}
}
int main() {
    try { const auto total = test_variant(true) + test_variant(false); std::cout << "PASS " << total << " total allocation checks\n"; return 0; }
    catch (const std::exception& error) { tracking = false; std::cerr << "FAIL allocation check: " << error.what() << '\n'; return 1; }
}
