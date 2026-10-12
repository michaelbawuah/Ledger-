#include "ledger/pool.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using ledger::detail::OrderPool;
using ledger::detail::Slot;
using ledger::detail::no_slot;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void empty_and_exhaustion() {
    OrderPool empty{0};
    require(empty.capacity() == 0 && empty.size() == 0 && empty.full(), "zero-capacity pool state");
    require(empty.acquire() == no_slot, "zero-capacity acquire must fail");
    require(empty.check_invariants(), "zero-capacity partition invariant");

    for (const auto capacity : {1U, 2U, 7U, 31U, 257U}) {
        OrderPool pool{capacity};
        require(pool.capacity() == capacity && !pool.full(), "constructed capacity");
        for (Slot expected = 0; expected < capacity; ++expected) {
            const auto slot = pool.acquire();
            require(slot == expected, "initial free list must cover each slot once");
            require(pool[slot].live && pool.size() == expected + 1, "acquire bookkeeping");
            require(pool.check_invariants(), "partition after acquire");
        }
        require(pool.full(), "pool must be full at capacity");
        for (unsigned attempt = 0; attempt < 8; ++attempt)
            require(pool.acquire() == no_slot && pool.size() == capacity, "failed acquire changed full pool");
        require(pool.check_invariants(), "partition after exhaustion");
    }
}

void lifo_recycling_and_payload_reset() {
    OrderPool pool{4};
    std::array<Slot, 4> slots{};
    for (auto& slot : slots) {
        slot = pool.acquire();
        auto& node = pool[slot];
        node.id = std::numeric_limits<ledger::OrderId>::max();
        node.price = std::numeric_limits<ledger::Price>::min();
        node.quantity = std::numeric_limits<ledger::Quantity>::max();
        node.side = ledger::Side::Sell;
        node.previous = 2;
        node.next = 3;
    }
    const std::array<Slot, 4> release_order{1, 3, 0, 2};
    for (const auto slot : release_order) {
        pool.release(slot);
        const auto& node = pool[slot];
        require(!node.live && node.id == 0 && node.price == 0 && node.quantity == 0,
                "released payload retained order data");
        require(node.previous == no_slot && node.side == ledger::Side::Buy, "released node metadata reset");
        require(pool.check_invariants(), "partition after out-of-order release");
    }
    require(pool.size() == 0 && !pool.full(), "fully recycled state");
    for (auto iterator = release_order.rbegin(); iterator != release_order.rend(); ++iterator) {
        const auto slot = pool.acquire();
        require(slot == *iterator, "free list must recycle last released first");
        const auto& node = pool[slot];
        require(node.live && node.id == 0 && node.price == 0 && node.quantity == 0,
                "reacquired payload was not cleared");
        require(node.previous == no_slot && node.next == no_slot && node.side == ledger::Side::Buy,
                "reacquired intrusive links were not detached");
        require(pool.check_invariants(), "partition after reacquire");
    }
}

void verify_partition(const OrderPool& pool, const std::vector<bool>& expected_live) {
    require(expected_live.size() == pool.capacity(), "oracle capacity mismatch");
    std::size_t count = 0;
    for (Slot slot = 0; slot < expected_live.size(); ++slot) {
        require(pool[slot].live == expected_live[slot], "pool ownership differs from slot oracle");
        count += expected_live[slot] ? 1 : 0;
    }
    require(pool.size() == count, "live count differs from oracle");
    require(pool.full() == (count == pool.capacity()), "full flag differs from oracle");
    require(pool.check_invariants(), "live/free partition diagnostic");
}

void exhaustive_release_permutations() {
    for (Slot capacity = 1; capacity <= 6; ++capacity) {
        std::vector<Slot> release_order(capacity);
        for (Slot slot = 0; slot < capacity; ++slot) release_order[slot] = slot;
        do {
            OrderPool pool{capacity};
            std::vector<bool> live(capacity, false);
            for (Slot slot = 0; slot < capacity; ++slot) {
                require(pool.acquire() == slot, "permutation initial slot ownership");
                live[slot] = true;
            }
            for (const auto slot : release_order) {
                pool.release(slot);
                live[slot] = false;
                verify_partition(pool, live);
            }
            for (auto iterator = release_order.rbegin(); iterator != release_order.rend(); ++iterator) {
                const auto slot = pool.acquire();
                require(slot == *iterator && !live[slot], "permutation recycle uniqueness");
                live[slot] = true;
                verify_partition(pool, live);
            }
        } while (std::next_permutation(release_order.begin(), release_order.end()));
    }
}

template <class Function> void run(const char* name, Function function) {
    function();
    std::cout << "PASS " << name << '\n';
}
}

int main() {
    try {
        run("empty_and_exhaustion", empty_and_exhaustion);
        run("lifo_recycling_and_payload_reset", lifo_recycling_and_payload_reset);
        run("exhaustive_release_permutations", exhaustive_release_permutations);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL pool suite: " << error.what() << '\n';
        return 1;
    }
}
