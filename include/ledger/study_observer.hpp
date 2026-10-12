#pragma once

#include "ledger/types.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace ledger::study {

struct Mechanics {
    std::uint32_t fills{};
    std::uint32_t levels_emptied{};
    std::uint64_t filled_quantity{};
};

// Reconstructs observable book changes from execution reports. This deliberately
// uses independent STL storage and is never called inside process() timing.
// levels_emptied means a level lost its final resting order to a maker execution
// or a cancellation; it is not a count of engine scans or bitmap operations.
class EventObserver {
public:
    explicit EventObserver(const Snapshot& initial) {
        orders_.reserve(initial.orders.size() * 2 + 1);
        for (const auto& order : initial.orders) add(order);
        if (!matches(initial)) throw std::runtime_error("observer initial snapshot mismatch");
    }

    Mechanics apply(const Command& command, ProcessResult result,
                    std::span<const Event> reports) {
        if (result == ProcessResult::OutputFull)
            throw std::runtime_error("observer cannot interpret OutputFull");
        if (result == ProcessResult::Rejected) {
            if (reports.size() != 1 || reports.front().type != EventType::Rejected)
                throw std::runtime_error("invalid rejection reports");
            return {};
        }
        Mechanics mechanics;
        unsigned accepted = 0, cancelled = 0, expired = 0;
        std::uint64_t expired_quantity = 0;
        for (const auto& event : reports) {
            if (event.sequence != command.sequence)
                throw std::runtime_error("execution report sequence mismatch");
            if (event.type == EventType::Cancelled) {
                ++cancelled;
                const auto found = orders_.find(event.id);
                if (event.id != command.id || found == orders_.end() ||
                    found->second.order.side != event.side ||
                    found->second.order.price != event.price ||
                    found->second.order.quantity != event.quantity)
                    throw std::runtime_error("cancellation report does not match observed order");
                remove(found, event.quantity, mechanics);
            } else if (event.type == EventType::Accepted) {
                ++accepted;
                if (event.id != command.id || event.quantity != command.quantity)
                    throw std::runtime_error("acceptance report does not match command");
            } else if (event.type == EventType::Trade) {
                ++mechanics.fills;
                mechanics.filled_quantity += event.quantity;
                const auto found = orders_.find(event.contra_id);
                if (event.id != command.id || found == orders_.end() ||
                    found->second.order.side == command.side ||
                    found->second.order.price != event.price || !event.quantity ||
                    event.quantity > found->second.order.quantity)
                    throw std::runtime_error("trade report does not match observed maker");
                remove(found, event.quantity, mechanics);
            } else if (event.type == EventType::Expired) {
                ++expired;
                expired_quantity += event.quantity;
                if (event.id != command.id)
                    throw std::runtime_error("expiration report id mismatch");
            } else if (event.type == EventType::Rejected) {
                throw std::runtime_error("rejected report in applied command");
            }
        }
        const bool submission = command.action != Action::Cancel;
        const bool cancellation = command.action == Action::Cancel || command.action == Action::Replace;
        if (accepted != static_cast<unsigned>(submission) ||
            cancelled != static_cast<unsigned>(cancellation) ||
            mechanics.filled_quantity > command.quantity || expired > 1)
            throw std::runtime_error("execution report command accounting mismatch");
        if (!submission) {
            if (mechanics.fills || expired) throw std::runtime_error("cancel produced fills or expiration");
            return mechanics;
        }
        const auto residual = static_cast<Quantity>(command.quantity - mechanics.filled_quantity);
        const bool rests = command.action != Action::Market && command.tif == TimeInForce::GTC;
        if (rests) {
            if (expired) throw std::runtime_error("GTC residual unexpectedly expired");
            if (residual) add({command.id, command.side, command.price, residual});
        } else if (expired_quantity != residual || expired != static_cast<unsigned>(residual != 0)) {
            throw std::runtime_error("execution report residual mismatch");
        }
        return mechanics;
    }

    bool matches(const Snapshot& snapshot) const {
        if (snapshot.orders.size() != orders_.size() || snapshot.levels.size() != levels_.size()) return false;
        std::vector<ObservedOrder> sorted;
        sorted.reserve(orders_.size());
        for (const auto& [id, order] : orders_) { (void)id; sorted.push_back(order); }
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            if (a.order.side != b.order.side) return a.order.side == Side::Buy;
            if (a.order.price != b.order.price)
                return a.order.side == Side::Buy ? a.order.price > b.order.price : a.order.price < b.order.price;
            return a.arrival < b.arrival;
        });
        for (std::size_t i = 0; i < sorted.size(); ++i)
            if (sorted[i].order != snapshot.orders[i]) return false;
        for (const auto& level : snapshot.levels) {
            const auto found = levels_.find({level.side, level.price});
            if (found == levels_.end() || found->second.quantity != level.quantity ||
                found->second.orders != level.orders) return false;
        }
        return true;
    }

private:
    struct ObservedOrder { RestingOrder order; std::uint64_t arrival{}; };
    struct Level { std::uint64_t quantity{}; std::uint32_t orders{}; };
    using Orders = std::unordered_map<OrderId, ObservedOrder>;
    Orders orders_;
    std::map<std::pair<Side, Price>, Level> levels_;
    std::uint64_t arrival_{};

    void add(const RestingOrder& order) {
        if (!order.quantity || !orders_.emplace(order.id, ObservedOrder{order, ++arrival_}).second)
            throw std::runtime_error("observer duplicate or empty resting order");
        auto& level = levels_[{order.side, order.price}];
        level.quantity += order.quantity;
        ++level.orders;
    }
    void remove(Orders::iterator found, Quantity quantity, Mechanics& mechanics) {
        const auto key = std::pair{found->second.order.side, found->second.order.price};
        const auto level = levels_.find(key);
        if (quantity > found->second.order.quantity || level == levels_.end() || quantity > level->second.quantity)
            throw std::runtime_error("observer quantity underflow");
        found->second.order.quantity -= quantity;
        level->second.quantity -= quantity;
        if (!found->second.order.quantity) {
            orders_.erase(found);
            if (!--level->second.orders) {
                if (level->second.quantity) throw std::runtime_error("observer nonempty removed level");
                levels_.erase(level);
                ++mechanics.levels_emptied;
            }
        }
    }
};

} // namespace ledger::study
