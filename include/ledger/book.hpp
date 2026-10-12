#pragma once
#include "ledger/id_index.hpp"
#include <limits>
#include <vector>

namespace ledger {
class FastBook {
public:
    explicit FastBook(Config config = {}, bool bitmap_search = true);
    ProcessResult process(const Command& command, EventBuffer& output) noexcept;
    Snapshot snapshot() const;
    bool check_invariants() const;
    std::uint64_t resting_quantity() const noexcept { return resting_quantity_; }
    std::size_t size() const noexcept { return pool_.size(); }
    Price best_bid() const noexcept;
    Price best_ask() const noexcept;
    const Statistics& stats() const noexcept { return statistics_; }

private:
    using Slot = detail::Slot;
    struct Level {
        Slot head{detail::no_slot}, tail{detail::no_slot};
        std::uint64_t quantity{};
        std::uint32_t orders{};
    };
    inline static constexpr std::size_t no_level = std::numeric_limits<std::size_t>::max();
    static Config validate_config(Config config);
    static std::size_t level_count(Config config);
    std::size_t offset(Price price) const noexcept;
    Price price_at(std::size_t offset) const noexcept;
    std::vector<Level>& levels(Side side) noexcept;
    const std::vector<Level>& levels(Side side) const noexcept;
    std::vector<std::uint64_t>& bitmap(Side side) noexcept;
    const std::vector<std::uint64_t>& bitmap(Side side) const noexcept;
    std::size_t& best(Side side) noexcept;
    std::size_t best(Side side) const noexcept;
    std::size_t find_best(Side side) const noexcept;
    std::size_t find_next(Side side, std::size_t starting_level) const noexcept;
    bool crosses(const Command& command, Price maker_price) const noexcept;
    bool can_fill(const Command& command) const noexcept;
    void add_resting(const Command& command, Quantity remaining) noexcept;
    void unlink(Slot slot) noexcept;
    void cancel(Slot slot, std::uint64_t sequence, EventBuffer& output) noexcept;
    ProcessResult reject(const Command& command, RejectReason reason, EventBuffer& output) noexcept;

    Config config_;
    bool bitmap_search_;
    detail::OrderPool pool_;
    detail::IdIndex index_;
    std::vector<Level> bids_, asks_;
    std::vector<std::uint64_t> bid_bitmap_, ask_bitmap_;
    std::size_t best_bid_{no_level}, best_ask_{no_level};
    std::uint64_t resting_quantity_{};
    Statistics statistics_{};
};
}
