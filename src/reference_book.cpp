#include "ledger/reference_book.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

namespace ledger {
namespace {

bool valid_side(Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

bool valid_action(Action action) noexcept {
    return action == Action::Limit || action == Action::Market ||
           action == Action::Cancel || action == Action::Replace;
}

bool valid_tif(TimeInForce tif) noexcept {
    return tif == TimeInForce::GTC || tif == TimeInForce::IOC ||
           tif == TimeInForce::FOK;
}

Price submission_price(const Command& command) noexcept {
    return command.action == Action::Market ? 0 : command.price;
}

bool eligible(const Command& command, Price maker_price) noexcept {
    if (command.action == Action::Market) return true;
    return command.side == Side::Buy ? maker_price <= command.price
                                    : maker_price >= command.price;
}

} // namespace

ReferenceBook::ReferenceBook(Config config) : config_(config) {
    if (config.min_price <= 0 || config.max_price < config.min_price ||
        config.max_orders == 0) {
        throw std::invalid_argument("invalid order-book configuration");
    }
}

Price ReferenceBook::best_bid() const noexcept {
    return bids_.empty() ? 0 : bids_.rbegin()->first;
}

Price ReferenceBook::best_ask() const noexcept {
    return asks_.empty() ? 0 : asks_.begin()->first;
}

RejectReason ReferenceBook::validate(const Command& command) const {
    if (!valid_action(command.action) || !valid_side(command.side) ||
        !valid_tif(command.tif)) return RejectReason::InvalidCommand;
    if (command.id == 0) return RejectReason::InvalidId;
    if (command.action != Action::Cancel && command.quantity == 0)
        return RejectReason::InvalidQuantity;
    if ((command.action == Action::Limit || command.action == Action::Replace) &&
        (command.price < config_.min_price || command.price > config_.max_price))
        return RejectReason::InvalidPrice;

    const auto found = index_.find(command.id);
    if (command.action == Action::Cancel || command.action == Action::Replace) {
        if (found == index_.end()) return RejectReason::UnknownId;
        if (command.action == Action::Replace && found->second.side != command.side)
            return RejectReason::WrongSide;
    } else if (found != index_.end()) {
        return RejectReason::DuplicateId;
    }

    // Reserve the possibility of a GTC residual before any executions.
    if (command.action == Action::Limit && command.tif == TimeInForce::GTC &&
        index_.size() >= config_.max_orders) return RejectReason::Capacity;
    if (command.action != Action::Cancel && command.tif == TimeInForce::FOK &&
        !can_fill(command)) return RejectReason::WouldNotFill;
    return RejectReason::None;
}

bool ReferenceBook::can_fill(const Command& command) const noexcept {
    std::uint64_t available = 0;
    if (command.side == Side::Buy) {
        for (const auto& [price, level] : asks_) {
            if (!eligible(command, price)) break;
            available += level.quantity;
            if (available >= command.quantity) return true;
        }
    } else {
        for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
            if (!eligible(command, it->first)) break;
            available += it->second.quantity;
            if (available >= command.quantity) return true;
        }
    }
    return false;
}

ProcessResult ReferenceBook::process(const Command& command, EventBuffer& output) {
    output.clear();
    if (output.capacity() < required_event_capacity(config_.max_orders))
        return ProcessResult::OutputFull;

    const RejectReason reason = validate(command);
    if (reason != RejectReason::None) {
        stats_.rejected += command.quantity;
        output.push({command.sequence, EventType::Rejected, reason, command.id,
                     0, command.side, command.price, command.quantity, 0});
        return ProcessResult::Rejected;
    }

    if (command.action == Action::Cancel) {
        cancel_live(command, output);
    } else {
        if (command.action == Action::Replace) cancel_live(command, output);
        submit(command, output);
    }
    return ProcessResult::Applied;
}

void ReferenceBook::cancel_live(const Command& command, EventBuffer& output) {
    const auto entry = index_.find(command.id);
    const Location location = entry->second;
    const RestingOrder original = *location.order;
    Levels& levels = location.side == Side::Buy ? bids_ : asks_;
    const auto level = levels.find(location.price);
    level->second.quantity -= original.quantity;
    level->second.orders.erase(location.order);
    if (level->second.orders.empty()) levels.erase(level);
    index_.erase(entry);
    resting_quantity_ -= original.quantity;
    stats_.cancelled += original.quantity;
    output.push({command.sequence, EventType::Cancelled, RejectReason::None,
                 original.id, 0, original.side, original.price,
                 original.quantity, 0});
}

void ReferenceBook::submit(const Command& command, EventBuffer& output) {
    stats_.submitted += command.quantity;
    output.push({command.sequence, EventType::Accepted, RejectReason::None,
                 command.id, 0, command.side, submission_price(command),
                 command.quantity, command.quantity});

    Quantity remaining = command.quantity;
    Levels& opposite = command.side == Side::Buy ? asks_ : bids_;
    while (remaining > 0 && !opposite.empty()) {
        auto level = command.side == Side::Buy ? opposite.begin()
                                               : std::prev(opposite.end());
        const Price price = level->first;
        if (!eligible(command, price)) break;

        RestingOrder& maker = level->second.orders.front();
        const OrderId maker_id = maker.id;
        const Side maker_side = maker.side;
        const Quantity fill = std::min(remaining, maker.quantity);
        remaining -= fill;
        maker.quantity -= fill;
        const Quantity maker_remaining = maker.quantity;
        level->second.quantity -= fill;
        resting_quantity_ -= fill;
        stats_.executed += fill;
        ++stats_.trade_count;

        output.push({command.sequence, EventType::Trade, RejectReason::None,
                     command.id, maker_id, command.side, price, fill, remaining});
        output.push({command.sequence,
                     maker_remaining == 0 ? EventType::Filled : EventType::PartiallyFilled,
                     RejectReason::None, maker_id, command.id, maker_side, price,
                     fill, maker_remaining});
        output.push({command.sequence,
                     remaining == 0 ? EventType::Filled : EventType::PartiallyFilled,
                     RejectReason::None, command.id, maker_id, command.side, price,
                     fill, remaining});

        if (maker_remaining == 0) {
            index_.erase(maker_id);
            level->second.orders.pop_front();
            if (level->second.orders.empty()) opposite.erase(level);
        }
    }

    if (remaining == 0) return;
    if (command.action != Action::Market && command.tif == TimeInForce::GTC) {
        rest(command, remaining);
    } else {
        stats_.expired += remaining;
        output.push({command.sequence, EventType::Expired, RejectReason::None,
                     command.id, 0, command.side, submission_price(command),
                     remaining, 0});
    }
}

void ReferenceBook::rest(const Command& command, Quantity remaining) {
    Levels& levels = command.side == Side::Buy ? bids_ : asks_;
    auto [level, inserted] = levels.try_emplace(command.price);
    (void)inserted;
    level->second.orders.push_back({command.id, command.side, command.price, remaining});
    const auto order = std::prev(level->second.orders.end());
    index_.emplace(command.id, Location{command.side, command.price, order});
    level->second.quantity += remaining;
    resting_quantity_ += remaining;
}

Snapshot ReferenceBook::snapshot() const {
    Snapshot result;
    result.stats = stats_;
    result.orders.reserve(index_.size());
    result.levels.reserve(bids_.size() + asks_.size());
    const auto append = [&result](Side side, Price price, const Level& level) {
        result.levels.push_back({side, price, level.quantity,
                                static_cast<std::uint32_t>(level.orders.size())});
        for (const RestingOrder& order : level.orders) result.orders.push_back(order);
    };
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it)
        append(Side::Buy, it->first, it->second);
    for (const auto& [price, level] : asks_) append(Side::Sell, price, level);
    return result;
}

bool ReferenceBook::check_invariants() const {
    if (index_.size() > config_.max_orders) return false;
    if (!bids_.empty() && !asks_.empty() && best_bid() >= best_ask()) return false;

    std::unordered_set<OrderId> seen;
    std::uint64_t total = 0;
    const auto check_levels = [&](Side side, const Levels& levels) {
        for (const auto& [price, level] : levels) {
            if (price < config_.min_price || price > config_.max_price ||
                level.orders.empty()) return false;
            std::uint64_t level_quantity = 0;
            for (const RestingOrder& order : level.orders) {
                if (order.id == 0 || order.side != side || order.price != price ||
                    order.quantity == 0 || !seen.insert(order.id).second) return false;
                const auto entry = index_.find(order.id);
                if (entry == index_.end() || entry->second.side != side ||
                    entry->second.price != price || &*entry->second.order != &order)
                    return false;
                level_quantity += order.quantity;
            }
            if (level_quantity != level.quantity) return false;
            total += level_quantity;
        }
        return true;
    };
    if (!check_levels(Side::Buy, bids_) || !check_levels(Side::Sell, asks_)) return false;
    if (seen.size() != index_.size() || total != resting_quantity_) return false;
    return stats_.submitted == 2 * stats_.executed + stats_.cancelled +
                               stats_.expired + resting_quantity_;
}

} // namespace ledger
