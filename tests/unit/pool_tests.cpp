#include "ledger/pool.hpp"

#include <iostream>
#include <stdexcept>

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

template <class Function> void run(const char* name, Function function) {
    function();
    std::cout << "PASS " << name << '\n';
}
}

int main() {
    try {
        run("empty_and_exhaustion", empty_and_exhaustion);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL pool suite: " << error.what() << '\n';
        return 1;
    }
}
