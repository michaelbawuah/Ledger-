#include "ledger/id_index.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
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

using Oracle = std::unordered_map<OrderId, Slot>;

void verify_index(const IdIndex& index, const Oracle& oracle) {
    require(index.size() == oracle.size(), "index size differs from map oracle");
    std::unordered_set<OrderId> observed;
    for (const auto& entry : index.entries()) {
        if (!entry.id) {
            require(entry.slot == no_slot, "empty entry retained a slot/tombstone");
            continue;
        }
        const auto expected = oracle.find(entry.id);
        require(expected != oracle.end() && expected->second == entry.slot, "unexpected physical entry");
        require(observed.insert(entry.id).second, "duplicate physical entry");
    }
    require(observed.size() == oracle.size(), "physical entries missing oracle IDs");
    for (const auto& [id, slot] : oracle)
        require(index.find(id) == slot, "lookup differs from map oracle");
    require(index.check_invariants(), "index internal invariants");
}

// Discover collisions through a one-entry table. The fixture does not copy the
// hash finalizer, so it continues to exercise actual collisions if hashing changes.
std::vector<OrderId> colliding_keys(std::uint32_t max_orders, std::size_t bucket, std::size_t count) {
    IdIndex probe{max_orders};
    std::vector<OrderId> keys;
    for (OrderId candidate = 1; keys.size() < count; ++candidate) {
        probe.insert(candidate, 0);
        if (probe.entries()[bucket].id == candidate) keys.push_back(candidate);
        require(probe.erase(candidate), "collision fixture probe erase");
    }
    return keys;
}

void collision_cluster_deletion() {
    constexpr std::uint32_t max_orders = 8;
    constexpr std::size_t bucket = 4;
    const auto keys = colliding_keys(max_orders, bucket, max_orders + 1);
    for (std::size_t victim = 0; victim < max_orders; ++victim) {
        IdIndex index{max_orders};
        Oracle oracle;
        for (Slot slot = 0; slot < max_orders; ++slot) {
            index.insert(keys[slot], slot);
            oracle.emplace(keys[slot], slot);
            require(index.entries()[bucket + slot].id == keys[slot], "fixture did not create cluster");
        }
        verify_index(index, oracle);
        require(index.erase(keys[victim]), "cluster victim erase");
        oracle.erase(keys[victim]);
        require(index.find(keys[victim]) == no_slot, "erased clustered key remains visible");
        verify_index(index, oracle);
        for (std::size_t position = victim; position + 1 < max_orders; ++position)
            require(index.entries()[bucket + position].id == keys[position + 1], "cluster gap was not healed");
        require(index.entries()[bucket + max_orders - 1].id == 0, "cluster retained terminal tombstone");
        index.insert(keys.back(), 99);
        oracle.emplace(keys.back(), 99);
        verify_index(index, oracle);
    }
}

template <class Function> void run(const char* name, Function function) {
    function();
    std::cout << "PASS " << name << '\n';
}
}

int main() {
    try {
        run("capacity_rounding_and_full_width_keys", capacity_rounding_and_full_width_keys);
        run("collision_cluster_deletion", collision_cluster_deletion);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL index suite: " << error.what() << '\n';
        return 1;
    }
}
