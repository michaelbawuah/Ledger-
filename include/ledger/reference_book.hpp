#pragma once

#include "ledger/types.hpp"

#include <list>
#include <map>
#include <unordered_map>

namespace ledger {

// A deliberately conventional matching oracle. Its STL storage and matching
// implementation are independent of the bounded fast book.
class ReferenceBook {
public:
    explicit ReferenceBook(Config config = {});

    ReferenceBook(const ReferenceBook&) = delete;
    ReferenceBook& operator=(const ReferenceBook&) = delete;
    ReferenceBook(ReferenceBook&&) noexcept = default;
    ReferenceBook& operator=(ReferenceBook&&) noexcept = default;

    ProcessResult process(const Command& command, EventBuffer& output);
    Snapshot snapshot() const;
    bool check_invariants() const;
    std::uint64_t resting_quantity() const noexcept { return resting_quantity_; }
    std::size_t size() const noexcept { return index_.size(); }
    Price best_bid() const noexcept;
    Price best_ask() const noexcept;
    const Statistics& stats() const noexcept { return stats_; }

private:
    struct Level {
        std::list<RestingOrder> orders;
        std::uint64_t quantity{};
    };
    using Levels = std::map<Price, Level>;
    struct Location {
        Side side;
        Price price;
        std::list<RestingOrder>::iterator order;
    };

    Config config_;
    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderId, Location> index_;
    Statistics stats_;
    std::uint64_t resting_quantity_{};

    RejectReason validate(const Command& command) const;
    bool can_fill(const Command& command) const noexcept;
    void cancel_live(const Command& command, EventBuffer& output);
    void submit(const Command& command, EventBuffer& output);
    void rest(const Command& command, Quantity remaining);
};

} // namespace ledger
