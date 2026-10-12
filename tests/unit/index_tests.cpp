#include "ledger/id_index.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using ledger::OrderId;
using ledger::detail::IdIndex;
using ledger::detail::Slot;
using ledger::detail::no_slot;

static_assert(noexcept(std::declval<const IdIndex&>().find(OrderId{})));
static_assert(noexcept(std::declval<IdIndex&>().insert(OrderId{}, Slot{})));
static_assert(noexcept(std::declval<IdIndex&>().erase(OrderId{})));
static_assert(noexcept(std::declval<const IdIndex&>().check_invariants()));

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void capacity_rounding_and_full_width_keys() {
    for (const auto max_orders : {0U, 1U, 2U, 3U, 7U, 8U, 9U, 127U, 128U, 129U}) {
        IdIndex index{max_orders};
        std::size_t expected_capacity = 2;
        while (expected_capacity < 2 * max_orders) expected_capacity *= 2;
        require(index.entries().size() == expected_capacity, "index table capacity rounding");
        require(index.size() == 0 && index.check_invariants(), "initial index invariants");
        require(index.find(0) == no_slot && !index.erase(0), "zero sentinel lookup/erase");
        require(index.find(std::numeric_limits<OrderId>::max()) == no_slot, "absent maximum ID");
    }

    const std::vector<OrderId> keys{
        1, 2, 255, 256, 65535, 65536, UINT64_C(0xffffffff), UINT64_C(0x100000000),
        UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000001),
        UINT64_C(0x00000000deadbeef), UINT64_C(0xabcdef01deadbeef),
        std::numeric_limits<OrderId>::max() - 1, std::numeric_limits<OrderId>::max()};
    IdIndex index{static_cast<std::uint32_t>(keys.size())};
    for (Slot slot = 0; slot < keys.size(); ++slot) {
        index.insert(keys[slot], slot);
        require(index.size() == slot + 1 && index.check_invariants(), "boundary key insert");
    }
    for (Slot slot = 0; slot < keys.size(); ++slot)
        require(index.find(keys[slot]) == slot, "full-width key was truncated or aliased");
    require(index.find(0) == no_slot && !index.erase(0), "zero sentinel after population");
    for (const auto key : keys) {
        require(index.erase(key), "boundary key erase");
        require(index.find(key) == no_slot && !index.erase(key), "repeated erase must be harmless");
        require(index.check_invariants(), "boundary key erase invariant");
    }
    require(index.size() == 0, "boundary keys did not fully drain");
}

template <class Function> void run(const char* name, Function function) {
    function();
    std::cout << "PASS " << name << '\n';
}
}

int main() {
    try {
        run("capacity_rounding_and_full_width_keys", capacity_rounding_and_full_width_keys);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL index suite: " << error.what() << '\n';
        return 1;
    }
}
