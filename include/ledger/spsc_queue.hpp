#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ledger {

// Exactly one producer and one consumer. Construction/destruction require both
// threads to be stopped. Capacity is usable capacity (there is no spare slot).
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "SPSC capacity must be a nonzero power of two");
    static_assert(Capacity <= (std::uint64_t{1} << 63),
                  "SPSC capacity must fit unsigned counter distance");
    static_assert(std::is_trivially_copyable_v<T> && std::is_nothrow_copy_assignable_v<T>,
                  "SPSC items must support noexcept trivial copying");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "This SPSC implementation requires lock-free 64-bit atomics");

    // A conservative 64-byte cache-line assumption, stated in the documentation.
    struct alignas(64) Counter { std::atomic<std::uint64_t> value{0}; };
public:
    static constexpr std::size_t capacity = Capacity;
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer only. Failure leaves the queue and item unchanged.
    bool try_push(const T& item) noexcept {
        const auto head = head_.value.load(std::memory_order_relaxed);
        const auto tail = tail_.value.load(std::memory_order_acquire);
        if (head - tail == Capacity) return false;
        slots_[static_cast<std::size_t>(head & (Capacity - 1))] = item;
        head_.value.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. Failure leaves the destination unchanged.
    bool try_pop(T& item) noexcept {
        const auto tail = tail_.value.load(std::memory_order_relaxed);
        const auto head = head_.value.load(std::memory_order_acquire);
        if (head == tail) return false;
        item = slots_[static_cast<std::size_t>(tail & (Capacity - 1))];
        tail_.value.store(tail + 1, std::memory_order_release);
        return true;
    }

    // A concurrent observation, not a synchronization protocol for shutdown.
    bool empty() const noexcept {
        const auto tail = tail_.value.load(std::memory_order_acquire);
        return tail == head_.value.load(std::memory_order_acquire);
    }
private:
    Counter head_{};
    Counter tail_{};
    alignas(64) std::array<T, Capacity> slots_{};
};

} // namespace ledger
