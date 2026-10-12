#include "ledger/book.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace ledger {
Config FastBook::validate_config(Config config) {
    if (config.min_price <= 0 || config.max_price < config.min_price || config.max_orders == 0)
        throw std::invalid_argument("prices must define a positive inclusive range and max_orders must be positive");
    return config;
}
std::size_t FastBook::level_count(Config config) {
    // Both bounds are positive: subtraction cannot overflow signed Price.
    const auto count = static_cast<std::uint64_t>(config.max_price - config.min_price) + 1;
    if (count > std::vector<Level>{}.max_size())
        throw std::length_error("price range exceeds addressable flat-level storage");
    return static_cast<std::size_t>(count);
}
FastBook::FastBook(Config config, bool bitmap_search, bool bitmap_fok)
    : config_(validate_config(config)), bitmap_search_(bitmap_search), bitmap_fok_(bitmap_fok), pool_(config_.max_orders),
      index_(config_.max_orders), bids_(level_count(config_)), asks_(bids_.size()),
      bid_bitmap_(bids_.size() / 64 + (bids_.size() % 64 != 0)), ask_bitmap_(bid_bitmap_.size()) {}

std::size_t FastBook::offset(Price price) const noexcept { return static_cast<std::size_t>(price - config_.min_price); }
Price FastBook::price_at(std::size_t value) const noexcept { return config_.min_price + static_cast<Price>(value); }
std::vector<FastBook::Level>& FastBook::levels(Side side) noexcept { return side == Side::Buy ? bids_ : asks_; }
const std::vector<FastBook::Level>& FastBook::levels(Side side) const noexcept { return side == Side::Buy ? bids_ : asks_; }
std::vector<std::uint64_t>& FastBook::bitmap(Side side) noexcept { return side == Side::Buy ? bid_bitmap_ : ask_bitmap_; }
const std::vector<std::uint64_t>& FastBook::bitmap(Side side) const noexcept { return side == Side::Buy ? bid_bitmap_ : ask_bitmap_; }
std::size_t& FastBook::best(Side side) noexcept { return side == Side::Buy ? best_bid_ : best_ask_; }
std::size_t FastBook::best(Side side) const noexcept { return side == Side::Buy ? best_bid_ : best_ask_; }
Price FastBook::best_bid() const noexcept { return best_bid_ == no_level ? 0 : price_at(best_bid_); }
Price FastBook::best_ask() const noexcept { return best_ask_ == no_level ? 0 : price_at(best_ask_); }

std::size_t FastBook::find_best(Side side) const noexcept {
    return find_next(side, side == Side::Buy ? bids_.size() - 1 : 0);
}
std::size_t FastBook::find_next(Side side, std::size_t starting_level) const noexcept {
    const auto& side_levels = levels(side);
    if (!bitmap_search_) {
        if (side == Side::Buy) {
            for (std::size_t i = starting_level + 1; i != 0; --i)
                if (side_levels[i - 1].orders) return i - 1;
        } else {
            for (std::size_t i = starting_level; i < side_levels.size(); ++i)
                if (side_levels[i].orders) return i;
        }
        return no_level;
    }
    return find_next_bitmap(side, starting_level);
}
std::size_t FastBook::find_next_bitmap(Side side, std::size_t starting_level) const noexcept {
    const auto& bits = bitmap(side);
    const std::size_t first_word = starting_level / 64;
    const unsigned first_bit = static_cast<unsigned>(starting_level % 64);
    if (side == Side::Buy) {
        const std::uint64_t first_bits = bits[first_word] & (UINT64_MAX >> (63 - first_bit));
        if (first_bits) return first_word * 64 + 63 - std::countl_zero(first_bits);
        for (std::size_t word = first_word; word != 0; --word)
            if (bits[word - 1]) return (word - 1) * 64 + 63 - std::countl_zero(bits[word - 1]);
    } else {
        const std::uint64_t first_bits = bits[first_word] & (UINT64_MAX << first_bit);
        if (first_bits) return first_word * 64 + std::countr_zero(first_bits);
        for (std::size_t word = first_word + 1; word < bits.size(); ++word)
            if (bits[word]) return word * 64 + std::countr_zero(bits[word]);
    }
    return no_level;
}
bool FastBook::crosses(const Command& command, Price maker_price) const noexcept {
    return command.action == Action::Market || (command.side == Side::Buy ? command.price >= maker_price : command.price <= maker_price);
}
bool FastBook::can_fill(const Command& command) const noexcept {
    const Side opposite = command.side == Side::Buy ? Side::Sell : Side::Buy;
    const auto& side_levels = levels(opposite);
    const std::size_t first = best(opposite);
    if (first == no_level) return false;
    std::uint64_t available = 0;
    if (bitmap_fok_) {
        std::size_t level = first;
        while (level != no_level && crosses(command, price_at(level))) {
            available += side_levels[level].quantity;
            if (available >= command.quantity) return true;
            if (opposite == Side::Sell) {
                if (level == side_levels.size() - 1) break;
                level = find_next_bitmap(opposite, level + 1);
            } else {
                if (level == 0) break;
                level = find_next_bitmap(opposite, level - 1);
            }
        }
        return false;
    }
    if (opposite == Side::Sell) {
        for (std::size_t i = first; i < side_levels.size() && crosses(command, price_at(i)); ++i) {
            available += side_levels[i].quantity;
            if (available >= command.quantity) return true;
        }
    } else {
        for (std::size_t i = first + 1; i != 0; --i) {
            const std::size_t level = i - 1;
            if (!crosses(command, price_at(level))) break;
            available += side_levels[level].quantity;
            if (available >= command.quantity) return true;
        }
    }
    return false;
}
void FastBook::add_resting(const Command& command, Quantity remaining) noexcept {
    const Slot slot = pool_.acquire();
    auto& order = pool_[slot];
    order.id = command.id;
    order.price = command.price;
    order.quantity = remaining;
    order.side = command.side;
    auto& level = levels(command.side)[offset(command.price)];
    order.previous = level.tail;
    if (level.tail != detail::no_slot) pool_[level.tail].next = slot;
    else level.head = slot;
    level.tail = slot;
    level.quantity += remaining;
    ++level.orders;
    resting_quantity_ += remaining;
    const std::size_t price_index = offset(command.price);
    bitmap(command.side)[price_index / 64] |= UINT64_C(1) << (price_index % 64);
    auto& current_best = best(command.side);
    if (current_best == no_level || (command.side == Side::Buy ? price_index > current_best : price_index < current_best))
        current_best = price_index;
    index_.insert(command.id, slot);
}
void FastBook::unlink(Slot slot) noexcept {
    const auto order = pool_[slot];
    const std::size_t price_index = offset(order.price);
    auto& level = levels(order.side)[price_index];
    if (order.previous != detail::no_slot) pool_[order.previous].next = order.next;
    else level.head = order.next;
    if (order.next != detail::no_slot) pool_[order.next].previous = order.previous;
    else level.tail = order.previous;
    level.quantity -= order.quantity;
    resting_quantity_ -= order.quantity;
    --level.orders;
    if (!level.orders) {
        bitmap(order.side)[price_index / 64] &= ~(UINT64_C(1) << (price_index % 64));
        if (best(order.side) == price_index) best(order.side) = find_next(order.side, price_index);
    }
    index_.erase(order.id);
    pool_.release(slot);
}
void FastBook::cancel(Slot slot, std::uint64_t sequence, EventBuffer& output) noexcept {
    const auto order = pool_[slot];
    output.push({sequence, EventType::Cancelled, RejectReason::None, order.id, 0, order.side, order.price, order.quantity, 0});
    statistics_.cancelled += order.quantity;
    unlink(slot);
}
ProcessResult FastBook::reject(const Command& command, RejectReason reason, EventBuffer& output) noexcept {
    output.push({command.sequence, EventType::Rejected, reason, command.id, 0, command.side, command.price, command.quantity, 0});
    statistics_.rejected += command.quantity;
    return ProcessResult::Rejected;
}
ProcessResult FastBook::process(const Command& command, EventBuffer& output) noexcept {
    output.clear();
    if (output.capacity() < required_event_capacity(config_.max_orders)) return ProcessResult::OutputFull;
    if (static_cast<unsigned>(command.action) > static_cast<unsigned>(Action::Replace) ||
        static_cast<unsigned>(command.side) > static_cast<unsigned>(Side::Sell) ||
        static_cast<unsigned>(command.tif) > static_cast<unsigned>(TimeInForce::FOK))
        return reject(command, RejectReason::InvalidCommand, output);
    if (!command.id) return reject(command, RejectReason::InvalidId, output);
    if (command.action != Action::Cancel && !command.quantity) return reject(command, RejectReason::InvalidQuantity, output);
    if ((command.action == Action::Limit || command.action == Action::Replace) &&
        (command.price < config_.min_price || command.price > config_.max_price))
        return reject(command, RejectReason::InvalidPrice, output);
    const Slot existing = index_.find(command.id);
    if (command.action == Action::Cancel || command.action == Action::Replace) {
        if (existing == detail::no_slot) return reject(command, RejectReason::UnknownId, output);
    } else if (existing != detail::no_slot) return reject(command, RejectReason::DuplicateId, output);
    if (command.action == Action::Cancel) {
        cancel(existing, command.sequence, output);
        return ProcessResult::Applied;
    }
    if (command.action == Action::Replace && pool_[existing].side != command.side)
        return reject(command, RejectReason::WrongSide, output);
    if (command.action == Action::Limit && command.tif == TimeInForce::GTC && pool_.full())
        return reject(command, RejectReason::Capacity, output);
    if (command.tif == TimeInForce::FOK && !can_fill(command))
        return reject(command, RejectReason::WouldNotFill, output);

    if (command.action == Action::Replace) cancel(existing, command.sequence, output);
    const Price report_price = command.action == Action::Market ? 0 : command.price;
    output.push({command.sequence, EventType::Accepted, RejectReason::None, command.id, 0, command.side, report_price, command.quantity, command.quantity});
    statistics_.submitted += command.quantity;
    Quantity remaining = command.quantity;
    const Side opposite = command.side == Side::Buy ? Side::Sell : Side::Buy;
    while (remaining && best(opposite) != no_level && crosses(command, price_at(best(opposite)))) {
        const Slot maker_slot = levels(opposite)[best(opposite)].head;
        auto& maker = pool_[maker_slot];
        const Quantity traded = std::min(remaining, maker.quantity);
        remaining -= traded;
        maker.quantity -= traded;
        levels(opposite)[best(opposite)].quantity -= traded;
        resting_quantity_ -= traded;
        statistics_.executed += traded;
        ++statistics_.trade_count;
        output.push({command.sequence, EventType::Trade, RejectReason::None, command.id, maker.id, command.side, maker.price, traded, remaining});
        output.push({command.sequence, maker.quantity ? EventType::PartiallyFilled : EventType::Filled, RejectReason::None, maker.id, command.id, maker.side, maker.price, traded, maker.quantity});
        output.push({command.sequence, remaining ? EventType::PartiallyFilled : EventType::Filled, RejectReason::None, command.id, maker.id, command.side, maker.price, traded, remaining});
        if (!maker.quantity) unlink(maker_slot);
    }
    if (remaining) {
        if (command.action != Action::Market && command.tif == TimeInForce::GTC) add_resting(command, remaining);
        else {
            statistics_.expired += remaining;
            output.push({command.sequence, EventType::Expired, RejectReason::None, command.id, 0, command.side, report_price, remaining, 0});
        }
    }
    return ProcessResult::Applied;
}
Snapshot FastBook::snapshot() const {
    Snapshot result;
    result.orders.reserve(pool_.size());
    result.stats = statistics_;
    const auto append = [&](Side side, std::size_t price_index) {
        const auto& level = levels(side)[price_index];
        if (!level.orders) return;
        result.levels.push_back({side, price_at(price_index), level.quantity, level.orders});
        for (Slot slot = level.head; slot != detail::no_slot; slot = pool_[slot].next) {
            const auto& order = pool_[slot];
            result.orders.push_back({order.id, order.side, order.price, order.quantity});
        }
    };
    for (std::size_t i = bids_.size(); i != 0; --i) append(Side::Buy, i - 1);
    for (std::size_t i = 0; i < asks_.size(); ++i) append(Side::Sell, i);
    return result;
}
bool FastBook::check_invariants() const {
    if (!pool_.check_invariants() || !index_.check_invariants() || pool_.size() != index_.size()) return false;
    std::vector<bool> seen(pool_.capacity(), false);
    std::size_t order_count = 0;
    std::uint64_t total_quantity = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
        const auto& side_levels = levels(side);
        const auto& bits = bitmap(side);
        for (std::size_t i = 0; i < side_levels.size(); ++i) {
            const auto& level = side_levels[i];
            if ((level.orders != 0) != ((bits[i / 64] & (UINT64_C(1) << (i % 64))) != 0)) return false;
            std::uint64_t quantity = 0;
            std::uint32_t count = 0;
            Slot previous = detail::no_slot;
            for (Slot slot = level.head; slot != detail::no_slot; slot = pool_[slot].next) {
                if (slot >= pool_.capacity() || seen[slot]) return false;
                const auto& order = pool_[slot];
                if (!order.live || !order.id || !order.quantity || order.side != side ||
                    order.price != price_at(i) || order.previous != previous || index_.find(order.id) != slot) return false;
                seen[slot] = true;
                previous = slot;
                quantity += order.quantity;
                ++count;
            }
            if (count != level.orders || quantity != level.quantity || previous != level.tail) return false;
            if (!count && (level.head != detail::no_slot || level.tail != detail::no_slot)) return false;
            total_quantity += quantity;
            order_count += count;
        }
        if (side_levels.size() % 64 && (bits.back() >> (side_levels.size() % 64)) != 0) return false;
        if (best(side) != find_best(side)) return false;
    }
    for (const auto& entry : index_.entries()) {
        if (entry.id && (entry.slot >= pool_.capacity() || !seen[entry.slot] || pool_[entry.slot].id != entry.id)) return false;
    }
    for (std::size_t i = 0; i < pool_.capacity(); ++i)
        if (pool_[static_cast<Slot>(i)].live != seen[i]) return false;
    if (order_count != pool_.size() || total_quantity != resting_quantity_) return false;
    if (best_bid_ != no_level && best_ask_ != no_level && best_bid_ >= best_ask_) return false;
    return statistics_.submitted == 2 * statistics_.executed + statistics_.cancelled + statistics_.expired + resting_quantity_;
}
}
