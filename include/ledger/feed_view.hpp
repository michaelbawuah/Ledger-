#pragma once

#include "ledger/feed.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace ledger {

// Borrows an immutable complete feed. The owner must keep its byte storage alive
// and unchanged for the lifetime of the view and all iterators obtained from it.
class FeedView {
public:
    FeedView() noexcept = default;

    // Validate the complete feed before publishing it. Failure leaves out intact.
    // No command vector is materialized and no memory is allocated.
    static bool parse(std::span<const std::byte> bytes, FeedView& out,
                      std::uint64_t max_records = 20'000'000) noexcept;

    bool valid() const noexcept { return valid_; }
    std::size_t size() const noexcept { return records_.size() / feed_record_size; }
    bool empty() const noexcept { return records_.empty(); }
    std::span<const std::byte> encoded_records() const noexcept { return records_; }

    // Safe indexed decode; an invalid view or index leaves out unchanged.
    bool read(std::size_t index, Command& out) const noexcept;

private:
    explicit FeedView(std::span<const std::byte> records) noexcept
        : records_(records), valid_(true) {}

    std::span<const std::byte> records_{};
    bool valid_{};
};

}
