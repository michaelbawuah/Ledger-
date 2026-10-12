#pragma once
#include "ledger/pool.hpp"
#include <stdexcept>
#include <vector>

namespace ledger::detail {
// A maximum load factor of one half leaves an empty probe terminator.
// Backward-shift deletion heals clusters instead of accumulating tombstones.
class IdIndex {
public:
    struct Entry { OrderId id{}; Slot slot{no_slot}; };
    explicit IdIndex(std::uint32_t max_orders) : entries_(table_capacity(max_orders)), mask_(entries_.size() - 1) {}

    Slot find(OrderId id) const noexcept {
        if (!id) return no_slot;
        std::size_t position = bucket(id);
        while (entries_[position].id) {
            if (entries_[position].id == id) return entries_[position].slot;
            position = (position + 1) & mask_;
        }
        return no_slot;
    }
    void insert(OrderId id, Slot slot) noexcept {
        std::size_t position = bucket(id);
        while (entries_[position].id) position = (position + 1) & mask_;
        entries_[position] = {id, slot};
        ++size_;
    }
    bool erase(OrderId id) noexcept {
        if (!id) return false;
        std::size_t hole = bucket(id);
        while (entries_[hole].id && entries_[hole].id != id) hole = (hole + 1) & mask_;
        if (!entries_[hole].id) return false;
        std::size_t cursor = (hole + 1) & mask_;
        while (entries_[cursor].id) {
            const std::size_t home = bucket(entries_[cursor].id);
            if (((hole - home) & mask_) < ((cursor - home) & mask_)) {
                entries_[hole] = entries_[cursor];
                hole = cursor;
            }
            cursor = (cursor + 1) & mask_;
        }
        entries_[hole] = Entry{};
        --size_;
        return true;
    }
    std::size_t size() const noexcept { return size_; }
    const std::vector<Entry>& entries() const noexcept { return entries_; }
    bool check_invariants() const noexcept {
        std::size_t occupied = 0;
        for (const auto& entry : entries_) {
            if (entry.id) {
                if (entry.slot == no_slot || find(entry.id) != entry.slot) return false;
                ++occupied;
            } else if (entry.slot != no_slot) return false;
        }
        return occupied == size_ && occupied <= entries_.size() / 2;
    }
private:
    static std::size_t table_capacity(std::uint32_t orders) {
        const std::uint64_t needed = static_cast<std::uint64_t>(orders) * 2;
        std::uint64_t capacity = 2;
        while (capacity < needed) capacity <<= 1;
        if (capacity > std::vector<Entry>{}.max_size())
            throw std::length_error("order ID index exceeds addressable storage");
        return static_cast<std::size_t>(capacity);
    }
    std::size_t bucket(OrderId id) const noexcept {
        // Finalizer mixing uses the complete 64-bit key, including its high bits.
        id ^= id >> 30;
        id *= UINT64_C(0xbf58476d1ce4e5b9);
        id ^= id >> 27;
        id *= UINT64_C(0x94d049bb133111eb);
        id ^= id >> 31;
        return static_cast<std::size_t>(id) & mask_;
    }
    std::vector<Entry> entries_;
    std::size_t mask_{};
    std::size_t size_{};
};
}
