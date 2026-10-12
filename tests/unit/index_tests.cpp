#include "ledger/id_index.hpp"

#include <algorithm>
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

void wrapped_clusters_and_mixed_home_buckets() {
    constexpr std::uint32_t max_orders = 6;
    IdIndex shape{max_orders};
    const auto capacity = shape.entries().size();
    const auto home = capacity - 2;
    const auto keys = colliding_keys(max_orders, home, max_orders);
    std::vector<std::size_t> erase_order{0, 1, 2, 3, 4, 5};
    do {
        IdIndex index{max_orders};
        Oracle oracle;
        for (Slot slot = 0; slot < max_orders; ++slot) {
            index.insert(keys[slot], slot);
            oracle.emplace(keys[slot], slot);
            require(index.entries()[(home + slot) % capacity].id == keys[slot], "fixture did not wrap");
        }
        for (const auto victim : erase_order) {
            require(index.erase(keys[victim]), "wrapped cluster erase");
            oracle.erase(keys[victim]);
            require(index.find(keys[victim]) == no_slot, "wrapped erased ID survived");
            verify_index(index, oracle);
        }
        for (Slot slot = max_orders; slot > 0; --slot) {
            index.insert(keys[slot - 1], slot - 1);
            oracle.emplace(keys[slot - 1], slot - 1);
        }
        verify_index(index, oracle);
    } while (std::next_permutation(erase_order.begin(), erase_order.end()));

    // A backward shift must skip entries already at home, while moving a later
    // displaced key across the skipped entries and the physical table boundary.
    const auto home_keys = colliding_keys(max_orders, home, 2);
    const auto next_key = colliding_keys(max_orders, home + 1, 1).front();
    const auto zero_key = colliding_keys(max_orders, 0, 1).front();
    IdIndex mixed{max_orders};
    Oracle oracle{{home_keys[0], 10}, {next_key, 11}, {zero_key, 12}, {home_keys[1], 13}};
    mixed.insert(home_keys[0], 10);
    mixed.insert(next_key, 11);
    mixed.insert(zero_key, 12);
    mixed.insert(home_keys[1], 13);
    verify_index(mixed, oracle);
    require(mixed.erase(home_keys[0]), "mixed-home head erase");
    oracle.erase(home_keys[0]);
    verify_index(mixed, oracle);
    require(mixed.entries()[home].id == home_keys[1], "displaced mixed-home key was not shifted");
    require(mixed.entries()[home + 1].id == next_key && mixed.entries()[0].id == zero_key,
            "backshift moved keys ahead of their home buckets");
    require(mixed.entries()[1].id == 0, "mixed-home terminal hole was not cleared");
}

std::uint64_t random_step(std::uint64_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

void long_running_half_full_churn() {
    for (const auto max_orders : {1U, 3U, 31U, 128U}) {
        IdIndex index{max_orders};
        Oracle oracle;
        std::vector<OrderId> active(max_orders);
        std::uint64_t random = UINT64_C(0xf823974acd137ec5) ^ max_orders;
        auto fresh_id = [&] {
            OrderId id{};
            do { id = random_step(random); } while (id == 0 || oracle.contains(id));
            return id;
        };
        for (Slot slot = 0; slot < max_orders; ++slot) {
            active[slot] = fresh_id();
            index.insert(active[slot], slot);
            oracle.emplace(active[slot], slot);
        }
        verify_index(index, oracle);
        for (std::uint64_t iteration = 0; iteration < 200000; ++iteration) {
            const auto slot = static_cast<Slot>(random_step(random) % max_orders);
            const auto removed = active[slot];
            require(index.erase(removed), "sustained churn erase failed");
            oracle.erase(removed);
            require(index.find(removed) == no_slot && !index.erase(removed), "churn removed key persisted");
            const auto replacement = fresh_id();
            active[slot] = replacement;
            index.insert(replacement, slot);
            oracle.emplace(replacement, slot);
            require(index.find(replacement) == slot && index.size() == max_orders, "churn replacement lookup/count");
            const auto sample_slot = static_cast<Slot>(random_step(random) % max_orders);
            require(index.find(active[sample_slot]) == sample_slot, "churn damaged a retained entry");
            if (iteration % 251 == 0) verify_index(index, oracle);
        }
        verify_index(index, oracle);
        for (const auto id : active) require(index.erase(id), "churn final drain failed");
        oracle.clear();
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
        run("wrapped_clusters_and_mixed_home_buckets", wrapped_clusters_and_mixed_home_buckets);
        run("long_running_half_full_churn", long_running_half_full_churn);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL index suite: " << error.what() << '\n';
        return 1;
    }
}
