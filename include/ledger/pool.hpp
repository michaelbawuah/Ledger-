#pragma once
#include "ledger/types.hpp"
#include <limits>
#include <vector>

namespace ledger::detail {
using Slot = std::uint32_t;
inline constexpr Slot no_slot = std::numeric_limits<Slot>::max();

// Every slot is either in one intrusive price queue or the intrusive free list.
// Construction reserves all memory; acquire/release never touch the allocator.
class OrderPool {
public:
    struct Node {
        OrderId id{};
        Price price{};
        Quantity quantity{};
        Side side{Side::Buy};
        Slot previous{no_slot};
        Slot next{no_slot};
        bool live{};
    };

    explicit OrderPool(std::uint32_t capacity) : nodes_(capacity) {
        for (Slot i = 0; i < capacity; ++i)
            nodes_[i].next = i + 1 < capacity ? i + 1 : no_slot;
        free_head_ = capacity ? 0 : no_slot;
    }
    Slot acquire() noexcept {
        if (free_head_ == no_slot) return no_slot;
        const Slot slot = free_head_;
        free_head_ = nodes_[slot].next;
        nodes_[slot] = Node{};
        nodes_[slot].live = true;
        ++size_;
        return slot;
    }
    void release(Slot slot) noexcept {
        nodes_[slot] = Node{};
        nodes_[slot].next = free_head_;
        free_head_ = slot;
        --size_;
    }
    Node& operator[](Slot slot) noexcept { return nodes_[slot]; }
    const Node& operator[](Slot slot) const noexcept { return nodes_[slot]; }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return nodes_.size(); }
    bool full() const noexcept { return free_head_ == no_slot; }

    // Diagnostic only: verifies a partition of the slots and free-list acyclicity.
    bool check_invariants() const {
        std::vector<bool> free(nodes_.size(), false);
        std::size_t free_count = 0;
        for (Slot slot = free_head_; slot != no_slot; slot = nodes_[slot].next) {
            if (slot >= nodes_.size() || free[slot] || nodes_[slot].live) return false;
            free[slot] = true;
            ++free_count;
        }
        std::size_t live_count = 0;
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].live) {
                if (free[i]) return false;
                ++live_count;
            } else if (!free[i]) return false;
        }
        return live_count == size_ && live_count + free_count == nodes_.size();
    }
private:
    std::vector<Node> nodes_;
    Slot free_head_{no_slot};
    std::size_t size_{};
};
}
