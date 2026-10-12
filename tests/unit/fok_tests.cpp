#include "ledger/book.hpp"
#include "ledger/reference_book.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
thread_local bool tracking{};
thread_local std::size_t allocations{};
void* allocate(std::size_t bytes) {
    if (tracking) ++allocations;
    if (auto* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc{};
}
void* allocate_aligned(std::size_t bytes, std::size_t alignment) {
    if (tracking) ++allocations;
    void* p{};
#ifdef _WIN32
    p = _aligned_malloc(bytes ? bytes : alignment, alignment);
    if (p) return p;
#else
    if (posix_memalign(&p, alignment, bytes ? bytes : alignment) == 0) return p;
#endif
    throw std::bad_alloc{};
}
void free_aligned(void* p) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
}
// The allocation probe surrounds only FastBook::process; constructing books,
// the independent STL oracle, and diagnostic snapshots run outside its scope.
void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return allocate(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return allocate(n); } catch (...) { return nullptr; } }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate_aligned(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate_aligned(n, static_cast<std::size_t>(a)); }
void operator delete(void* p, std::align_val_t) noexcept { free_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { free_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { free_aligned(p); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return allocate_aligned(n, static_cast<std::size_t>(a)); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return allocate_aligned(n, static_cast<std::size_t>(a)); } catch (...) { return nullptr; } }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { free_aligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { free_aligned(p); }

namespace {
using namespace ledger;
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
struct Fixture {
    FastBook fast;
    ReferenceBook reference;
    std::array<Event, required_event_capacity(16)> actual{}, expected{};
    EventBuffer actual_output{actual}, expected_output{expected};
    std::uint64_t sequence{};
    std::size_t calls{};
    Fixture(Config config, bool search, bool fok) : fast{config, search, fok}, reference{config} {}
    void run(OrderId id, Side side, Price price, Quantity quantity,
             Action action = Action::Limit, TimeInForce tif = TimeInForce::GTC,
             ProcessResult expected_result = ProcessResult::Applied) {
        const Command command{++sequence, id, price, quantity, side, action, tif};
        const auto before = allocations;
        tracking = true;
        const auto result = fast.process(command, actual_output);
        tracking = false;
        ++calls;
        require(allocations == before, "FOK scenario allocated on the fast hot path");
        require(result == expected_result, "unexpected process result");
        require(result == reference.process(command, expected_output), "oracle result mismatch");
        require(actual_output.events().size() == expected_output.events().size(), "oracle event count mismatch");
        require(std::equal(actual_output.events().begin(), actual_output.events().end(), expected_output.events().begin()), "oracle event fields mismatch");
        require(fast.snapshot() == reference.snapshot(), "oracle full FIFO/level/statistics state mismatch");
        require(fast.check_invariants() && reference.check_invariants(), "structural invariant failed");
        require(fast.resting_quantity() == reference.resting_quantity(), "resting quantity mismatch");
        require(fast.best_bid() == reference.best_bid() && fast.best_ask() == reference.best_ask(), "best quote mismatch");
    }
    void reject(OrderId id, Side side, Price price, Quantity quantity, Action action = Action::Limit) {
        const auto before = fast.snapshot();
        run(id, side, price, quantity, action, TimeInForce::FOK, ProcessResult::Rejected);
        const auto after = fast.snapshot();
        require(after.orders == before.orders && after.levels == before.levels, "rejected FOK changed orders or priority");
        require(after.stats.cancelled == before.stats.cancelled && after.stats.executed == before.stats.executed &&
                after.stats.trade_count == before.stats.trade_count, "rejected FOK changed trading/cancellation state");
        require(actual_output.events().size() == 1 && actual_output.events()[0].reason == RejectReason::WouldNotFill,
                "FOK rejection should emit only WouldNotFill");
    }
};
Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

std::size_t sparse_limit(bool search, bool fok, Side taker) {
    Fixture f{{1, 20001, 16}, search, fok};
    const auto maker = opposite(taker);
    const bool buy = taker == Side::Buy;
    const std::array<Price, 3> prices = buy ? std::array<Price, 3>{65, 4097, 20001} : std::array<Price, 3>{19937, 15905, 1};
    f.run(1, maker, prices[0], 3);
    f.run(2, maker, prices[1], 4);
    f.run(3, maker, prices[2], 9);
    const Price limit = buy ? 12000 : 8000;
    f.reject(4, taker, limit, 8);
    // These bounds prevent even the best quote from crossing.
    f.reject(4, taker, buy ? 64 : 19938, 1);
    f.run(4, taker, limit, 7, Action::Limit, TimeInForce::FOK);
    require(f.fast.size() == 1 && f.fast.resting_quantity() == 9, "limit FOK crossed beyond its limit");
    f.run(5, taker, std::numeric_limits<Price>::min(), 9, Action::Market, TimeInForce::FOK);
    require(f.fast.size() == 0, "market FOK failed to exhaust residual liquidity");
    return f.calls;
}
std::size_t market_fok(bool search, bool fok, Side taker) {
    Fixture f{{1, 20001, 16}, search, fok};
    f.reject(9, taker, 0, 1, Action::Market);
    f.run(1, opposite(taker), 1, 3);
    f.run(2, opposite(taker), 20001, 4);
    f.reject(9, taker, 0, 8, Action::Market);
    f.run(9, taker, std::numeric_limits<Price>::max(), 7, Action::Market, TimeInForce::FOK);
    require(f.fast.size() == 0, "market FOK did not consume all levels");
    return f.calls;
}
std::size_t replace_fok(bool search, bool fok, Side taker) {
    Fixture f{{1, 20001, 16}, search, fok};
    const bool buy = taker == Side::Buy;
    f.run(9, taker, buy ? 1 : 20001, 5);
    f.run(1, opposite(taker), buy ? 65 : 19937, 3);
    f.run(2, opposite(taker), buy ? 4097 : 15905, 4);
    f.run(3, opposite(taker), buy ? 20001 : 1, 9);
    f.reject(9, taker, buy ? 12000 : 8000, 8, Action::Replace);
    require(f.fast.size() == 4, "rejected replacement removed original");
    f.run(9, taker, buy ? 12000 : 8000, 7, Action::Replace, TimeInForce::FOK);
    require(f.actual_output.events()[0].type == EventType::Cancelled && f.actual_output.events()[0].id == 9,
            "accepted replacement must cancel the original first");
    require(f.fast.size() == 1, "successful replacement left its original resting");
    return f.calls;
}
std::size_t boundaries(bool search, bool fok, Side taker, Config config) {
    Fixture f{config, search, fok};
    f.run(1, opposite(taker), config.min_price, 1);
    f.run(2, opposite(taker), config.max_price, 1);
    f.reject(3, taker, 0, 3, Action::Market);
    f.run(3, taker, taker == Side::Buy ? config.max_price : config.min_price, 2, Action::Limit, TimeInForce::FOK);
    require(f.fast.size() == 0, "boundary FOK did not consume end bits");
    return f.calls;
}
std::size_t full_pool(bool search, bool fok, Side taker) {
    Fixture f{{1, 20001, 4}, search, fok};
    const auto maker = opposite(taker);
    for (OrderId id = 1; id <= 4; ++id) f.run(id, maker, id <= 2 ? 1 : 20001, 1);
    f.reject(5, taker, 0, 5, Action::Market);
    f.run(5, taker, taker == Side::Buy ? 20001 : 1, 4, Action::Limit, TimeInForce::FOK);
    require(f.fast.size() == 0, "full pool rejected liquidity-consuming FOK");
    return f.calls;
}
std::size_t wide_quantity(bool search, bool fok, Side taker) {
    Fixture f{{1, 20001, 16}, search, fok};
    constexpr auto maximum = std::numeric_limits<Quantity>::max();
    f.run(1, opposite(taker), 1, maximum);
    f.run(2, opposite(taker), 20001, maximum);
    f.run(3, taker, 0, maximum, Action::Market, TimeInForce::FOK);
    require(f.fast.resting_quantity() == maximum, "wide aggregate quantity truncated");
    return f.calls;
}
}

int main() {
    try {
        std::size_t cases{}, calls{};
        for (bool search : {false, true}) for (bool fok : {false, true}) {
            std::size_t variant_calls{};
            for (Side taker : {Side::Buy, Side::Sell}) {
                variant_calls += sparse_limit(search, fok, taker); ++cases;
                variant_calls += market_fok(search, fok, taker); ++cases;
                variant_calls += replace_fok(search, fok, taker); ++cases;
                variant_calls += full_pool(search, fok, taker); ++cases;
                variant_calls += wide_quantity(search, fok, taker); ++cases;
                // Exact final bitmap words, partial final words, and transitions
                // across 63/64/127/128 are all compared with the independent oracle.
                for (Price domain : {64, 65, 127, 128, 129, 257}) {
                    variant_calls += boundaries(search, fok, taker, {1, domain, 16}); ++cases;
                }
                constexpr auto top = std::numeric_limits<Price>::max();
                variant_calls += boundaries(search, fok, taker, {top - 256, top, 16}); ++cases;
            }
            calls += variant_calls;
            std::cout << "PASS bitmap_search=" << search << " bitmap_fok=" << fok
                      << " " << variant_calls << " commands, exact oracle reports/state, 0 allocations\n";
        }
        std::cout << "PASS " << cases << " FOK scenarios, " << calls << " commands\n";
        return 0;
    } catch (const std::exception& error) {
        tracking = false;
        std::cerr << "FAIL FOK: " << error.what() << '\n';
        return 1;
    }
}
