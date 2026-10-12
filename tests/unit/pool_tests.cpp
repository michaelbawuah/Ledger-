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

std::uint64_t random_step(std::uint64_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

void seeded_churn_with_independent_ownership() {
    for (const auto capacity : {1U, 3U, 64U, 257U}) {
        OrderPool pool{capacity};
        std::vector<bool> live(capacity, false);
        std::vector<ledger::OrderId> expected_id(capacity, 0);
        std::vector<Slot> free_stack;
        free_stack.reserve(capacity);
        for (Slot slot = capacity; slot > 0; --slot) free_stack.push_back(slot - 1);
        std::uint64_t random = UINT64_C(0x34823c9a2d571be1) ^ capacity;
        for (std::uint64_t operation = 1; operation <= 100000; ++operation) {
            const auto candidate = static_cast<Slot>(random_step(random) % capacity);
            if (live[candidate]) {
                pool.release(candidate);
                live[candidate] = false;
                expected_id[candidate] = 0;
                free_stack.push_back(candidate);
            } else {
                const auto slot = pool.acquire();
                require(!free_stack.empty() && slot == free_stack.back(), "churn free stack disagrees");
                free_stack.pop_back();
                require(!live[slot], "churn acquired a live slot");
                live[slot] = true;
                expected_id[slot] = operation;
                pool[slot].id = operation;
                pool[slot].price = static_cast<ledger::Price>(operation * 7);
                pool[slot].quantity = static_cast<ledger::Quantity>(operation % 251 + 1);
            }
            require(pool.size() + free_stack.size() == capacity, "churn cardinality partition");
            if (operation % 127 == 0) {
                verify_partition(pool, live);
                for (Slot slot = 0; slot < capacity; ++slot) {
                    require(pool[slot].id == expected_id[slot], "churn changed another slot's payload");
                    if (live[slot]) {
                        require(pool[slot].price == static_cast<ledger::Price>(expected_id[slot] * 7),
                                "live price corrupted during churn");
                        require(pool[slot].quantity == expected_id[slot] % 251 + 1,
                                "live quantity corrupted during churn");
                    }
                }
            }
        }
        verify_partition(pool, live);
        for (Slot slot = 0; slot < capacity; ++slot) if (live[slot]) pool.release(slot);
        require(pool.size() == 0 && pool.check_invariants(), "churn final drain");
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
        run("seeded_churn_with_independent_ownership", seeded_churn_with_independent_ownership);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL pool suite: " << error.what() << '\n';
        return 1;
    }
}
