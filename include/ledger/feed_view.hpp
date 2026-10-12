#pragma once

#include "ledger/feed.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>

namespace ledger {

// Borrows an immutable complete feed. The owner must keep its byte storage alive
// and unchanged for the lifetime of the view and all iterators obtained from it.
class FeedView {
public:
    class Iterator {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = Command;
        using difference_type = std::ptrdiff_t;
        using reference = Command;

        Iterator() noexcept = default;
        // Commands are values, never references to a native object wire layout.
        Command operator*() const noexcept;
        Iterator& operator++() noexcept { ++index_; return *this; }
        Iterator operator++(int) noexcept { auto previous = *this; ++*this; return previous; }
        bool operator==(const Iterator& other) const noexcept {
            return records_.data() == other.records_.data()
                && records_.size() == other.records_.size() && index_ == other.index_;
        }

    private:
        friend class FeedView;
        Iterator(std::span<const std::byte> records, std::size_t index) noexcept
            : records_(records), index_(index) {}
        std::span<const std::byte> records_{};
        std::size_t index_{};
    };

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
    Iterator begin() const noexcept { return Iterator(records_, 0); }
    Iterator end() const noexcept { return Iterator(records_, size()); }

private:
    explicit FeedView(std::span<const std::byte> records) noexcept
        : records_(records), valid_(true) {}

    std::span<const std::byte> records_{};
    bool valid_{};
};

}
