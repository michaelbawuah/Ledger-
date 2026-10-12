#include "ledger/study_workloads.hpp"

#include "ledger/reference_book.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <list>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace ledger::study {
namespace {

constexpr std::array<std::string_view, 6> names{
    "balanced", "cancel_heavy", "trade_storm", "replace_heavy",
    "sparse_fok", "rejection_stress"};

// Specified integer operations, rather than standard-library distributions,
// make the random sequence portable across compilers and standard libraries.
class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed) {}
    std::uint64_t draw(std::uint64_t bound) {
        if (bound == 0) throw std::logic_error("empty random range");
        const auto threshold = (std::uint64_t{0} - bound) % bound;
        for (;;) {
            const auto value = next();
            if (value >= threshold) return value % bound;
        }
    }
private:
    std::uint64_t next() noexcept {
        auto value = (state_ += UINT64_C(0x9e3779b97f4a7c15));
        value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
        return value ^ (value >> 31);
    }
    std::uint64_t state_;
};

Side opposite(Side side) noexcept { return side == Side::Buy ? Side::Sell : Side::Buy; }
std::size_t side_index(Side side) noexcept { return side == Side::Buy ? 0 : 1; }

class Generator {
public:
    Generator(std::string_view profile, std::size_t count, std::uint64_t seed,
              std::size_t depth, Price domain)
        : profile_(profile), count_(count), depth_(depth), random_(seed),
          config_{1, domain, static_cast<std::uint32_t>(std::max<std::size_t>(4 * depth, 1024))},
          oracle_(config_), storage_(required_event_capacity(config_.max_orders)), output_(storage_) {
        workload_.profile = profile;
        workload_.config = config_;
        workload_.preload.reserve(2 * depth_);
        workload_.commands.reserve(count_);
        live_.reserve(config_.max_orders);
        for (auto& ids : ids_) ids.reserve(config_.max_orders);
    }

    Workload generate() {
        for (std::size_t slot = 0; slot < depth_; ++slot) {
            append(limit(Side::Buy, slot_price(Side::Buy, slot)), true);
            append(limit(Side::Sell, slot_price(Side::Sell, slot)), true);
        }
        verify_state();
        for (std::size_t index = 0; index < count_; ++index) {
            Command command;
            bool expected_rejection = false;
            if (profile_ == "balanced") command = balanced();
            else if (profile_ == "cancel_heavy") command = cancel_heavy();
            else if (profile_ == "trade_storm") command = trade_storm();
            else if (profile_ == "replace_heavy") command = replace_heavy();
            else if (profile_ == "sparse_fok") command = sparse_fok(expected_rejection);
            else command = rejection_stress(expected_rejection);
            append(command, false, expected_rejection);
            if ((index + 1) % 4096 == 0) verify_state();
        }
        verify_state();
        return std::move(workload_);
    }

private:
    using Levels = std::map<Price, std::list<OrderId>>;
    struct Live {
        Side side;
        Price price;
        Quantity quantity;
        std::size_t index;
        std::list<OrderId>::iterator position;
    };
    struct Sweep { Quantity quantity; Price last_price; std::size_t fills; };

    Command fresh(Side side, Action action, Price price, Quantity quantity,
                  TimeInForce tif = TimeInForce::GTC) {
        return {++sequence_, sequence_, price, quantity, side, action, tif};
    }
    Side random_side() { return random_.draw(2) == 0 ? Side::Buy : Side::Sell; }
    Side deficient_side() const noexcept {
        return ids_[0].size() <= ids_[1].size() ? Side::Buy : Side::Sell;
    }
    std::size_t live_size() const noexcept { return live_.size(); }
    Price slot_price(Side side, std::size_t slot) const noexcept {
        const Price midpoint = config_.max_price / 2;
        if (depth_ == 1) return side == Side::Buy ? midpoint : midpoint + 1;
        const auto index = static_cast<Price>(slot);
        const auto divisor = static_cast<Price>(depth_ - 1);
        return side == Side::Buy ? midpoint - index * (midpoint - 1) / divisor
                                : midpoint + 1 + index * (config_.max_price - midpoint - 1) / divisor;
    }
    Price resting_price(Side side, bool empty_slot = false) {
        auto slot = static_cast<std::size_t>(random_.draw(depth_));
        if (empty_slot) {
            // Sparse FOK retains separated live levels during replenishment.
            // At most depth slots are occupied, so the fallback is bounded.
            for (std::size_t attempt = 0; attempt < 16; ++attempt) {
                if (!levels_[side_index(side)].contains(slot_price(side, slot)))
                    return slot_price(side, slot);
                slot = static_cast<std::size_t>(random_.draw(depth_));
            }
            for (slot = 0; slot < depth_; ++slot)
                if (!levels_[side_index(side)].contains(slot_price(side, slot)))
                    return slot_price(side, slot);
        }
        return slot_price(side, slot);
    }
    Command limit(Side side, Price price) {
        return fresh(side, Action::Limit, price, static_cast<Quantity>(1 + random_.draw(32)));
    }
    Command replenish(bool sparse = false) {
        const auto side = deficient_side();
        return limit(side, resting_price(side, sparse));
    }
    OrderId choose_live(Side side) {
        const auto& ids = ids_[side_index(side)];
        return ids[static_cast<std::size_t>(random_.draw(ids.size()))];
    }
    Side live_side() {
        const auto side = random_side();
        return ids_[side_index(side)].empty() ? opposite(side) : side;
    }
    Command cancel() {
        const auto side = live_side();
        auto command = fresh(side, Action::Cancel, 0, 0);
        command.id = choose_live(side);
        return command;
    }
    Command replace() {
        const auto side = live_side();
        auto command = limit(side, resting_price(side));
        command.action = Action::Replace;
        command.id = choose_live(side);
        return command;
    }
    Sweep sweep(Side taker, std::size_t desired_fills) const {
        const auto maker = opposite(taker);
        const auto& levels = levels_[side_index(maker)];
        std::uint64_t quantity = 0;
        Sweep result{};
        const auto visit = [&](Price price, const std::list<OrderId>& orders) {
            for (const auto id : orders) {
                quantity += live_.at(id).quantity;
                result.last_price = price;
                if (++result.fills == desired_fills) return true;
            }
            return false;
        };
        if (maker == Side::Sell) {
            for (const auto& [price, orders] : levels) if (visit(price, orders)) break;
        } else {
            for (auto level = levels.rbegin(); level != levels.rend(); ++level)
                if (visit(level->first, level->second)) break;
        }
        if (quantity == 0 || quantity > std::numeric_limits<Quantity>::max())
            throw std::logic_error("synthetic sweep quantity out of range");
        result.quantity = static_cast<Quantity>(quantity);
        return result;
    }
    Command trade(std::size_t max_fills, bool fok = false) {
        auto side = random_side();
        if (ids_[side_index(opposite(side))].empty()) side = opposite(side);
        const auto available = ids_[side_index(opposite(side))].size();
        const auto requested = static_cast<std::size_t>(1 + random_.draw(std::min(max_fills, available)));
        const auto selection = sweep(side, requested);
        if (fok || random_.draw(2) == 0)
            return fresh(side, Action::Limit, selection.last_price, selection.quantity,
                         fok ? TimeInForce::FOK : TimeInForce::IOC);
        return fresh(side, Action::Market, 0, selection.quantity);
    }
    Command failing_fok() {
        auto side = random_side();
        if (ids_[side_index(opposite(side))].empty()) side = opposite(side);
        const auto maker_count = ids_[side_index(opposite(side))].size();
        const auto liquidity = sweep(side, maker_count);
        if (liquidity.quantity == std::numeric_limits<Quantity>::max())
            throw std::logic_error("synthetic FOK rejection quantity exhausted");
        // The limit admits ALL existing opposite liquidity. Failure therefore
        // scans real, dispersed levels rather than rejecting on an empty book.
        return fresh(side, Action::Limit, side == Side::Buy ? config_.max_price : config_.min_price,
                     liquidity.quantity + 1, TimeInForce::FOK);
    }

    Command balanced() {
        if (ids_[0].size() < std::max<std::size_t>(1, depth_ / 2) ||
            ids_[1].size() < std::max<std::size_t>(1, depth_ / 2)) return replenish();
        const auto choice = random_.draw(100);
        if (live_size() >= 3 * depth_) return choice < 70 ? cancel() : trade(8);
        if (choice < 45) return replenish();
        if (choice < 70) return cancel();
        if (choice < 90) return trade(4);
        return replace();
    }
    Command cancel_heavy() {
        // A cancellation cannot create a replacement maker. After the 70%
        // selection phase consumes its initial supply, an explicit refill phase
        // is necessary; a sustained successful-cancel fraction cannot exceed 50%.
        if (refilling_ && live_size() >= 2 * depth_) refilling_ = false;
        if (live_size() <= depth_) refilling_ = true;
        if (refilling_) return replenish();
        return random_.draw(100) < 70 ? cancel() : replenish();
    }
    Command trade_storm() {
        if (ids_[0].size() < depth_ || ids_[1].size() < depth_) return replenish();
        return trade(64);
    }
    Command replace_heavy() {
        if (ids_[0].size() < std::max<std::size_t>(1, depth_ / 2) ||
            ids_[1].size() < std::max<std::size_t>(1, depth_ / 2)) return replenish();
        const auto choice = random_.draw(100);
        if (choice < 30) return replace();
        if (choice < 65 && live_size() < 3 * depth_) return replenish();
        if (choice < 90 || live_size() >= 3 * depth_) return cancel();
        return trade(2);
    }
    Command sparse_fok(bool& expected_rejection) {
        if (ids_[0].size() < depth_ || ids_[1].size() < depth_) return replenish(true);
        expected_rejection = random_.draw(2) != 0;
        return expected_rejection ? failing_fok() : trade(64, true);
    }
    Command rejection_stress(bool& expected_rejection) {
        if (random_.draw(100) >= 80) return replace();
        expected_rejection = true;
        const auto side = live_side();
        auto command = limit(side, resting_price(side));
        switch (random_.draw(8)) {
        case 0: command.action = Action::Cancel; command.price = 0; command.quantity = 0; break;
        case 1: command.id = choose_live(side); break;
        case 2: command.price = 0; break;
        case 3: command.quantity = 0; break;
        case 4: command.id = 0; break;
        case 5: command.action = Action::Replace; command.id = choose_live(side); command.side = opposite(side); break;
        case 6: command.tif = static_cast<TimeInForce>(3); break;
        default:
            // fresh() above consumed a sequence; preserve monotonic, contiguous
            // sequences while selecting this controlled shortage command.
            --sequence_; command = failing_fok(); break;
        }
        return command;
    }

    void insert(const Command& command, Quantity remaining) {
        auto& ids = ids_[side_index(command.side)];
        auto& queue = levels_[side_index(command.side)][command.price];
        queue.push_back(command.id);
        live_.emplace(command.id, Live{command.side, command.price, remaining, ids.size(), std::prev(queue.end())});
        ids.push_back(command.id);
        tracked_quantity_ += remaining;
    }
    void erase(OrderId id) {
        const auto found = live_.find(id);
        if (found == live_.end()) throw std::logic_error("untracked oracle removal");
        const auto old = found->second;
        auto& ids = ids_[side_index(old.side)];
        if (old.index + 1 != ids.size()) {
            ids[old.index] = ids.back();
            live_.at(ids.back()).index = old.index;
        }
        ids.pop_back();
        auto& levels = levels_[side_index(old.side)];
        const auto level = levels.find(old.price);
        level->second.erase(old.position);
        if (level->second.empty()) levels.erase(level);
        tracked_quantity_ -= old.quantity;
        live_.erase(found);
    }
    void append(const Command& command, bool preload, bool expected_rejection = false) {
        const auto result = oracle_.process(command, output_);
        if (result == ProcessResult::OutputFull) throw std::logic_error("study event storage insufficient");
        if ((result == ProcessResult::Rejected) != expected_rejection)
            throw std::logic_error("study command failed its intended outcome");
        if (result == ProcessResult::Applied) {
            std::uint64_t filled = 0;
            for (const auto& event : output_.events()) {
                if (event.type == EventType::Trade) {
                    filled += event.quantity;
                    auto found = live_.find(event.contra_id);
                    if (found == live_.end() || event.quantity > found->second.quantity)
                        throw std::logic_error("study maker tracking disagrees with oracle");
                    found->second.quantity -= event.quantity;
                    tracked_quantity_ -= event.quantity;
                    if (found->second.quantity == 0) erase(event.contra_id);
                } else if (event.type == EventType::Cancelled) erase(event.id);
            }
            if (command.action != Action::Cancel && command.action != Action::Market &&
                command.tif == TimeInForce::GTC && filled < command.quantity)
                insert(command, static_cast<Quantity>(command.quantity - filled));
        }
        if (live_size() != oracle_.size() || tracked_quantity_ != oracle_.resting_quantity() ||
            live_size() > config_.max_orders)
            throw std::logic_error("study live-order accounting violated");
        (preload ? workload_.preload : workload_.commands).push_back(command);
    }
    void verify_state() const {
        if (!oracle_.check_invariants()) throw std::logic_error("study reference invariants violated");
        const auto snapshot = oracle_.snapshot();
        if (snapshot.orders.size() != live_size()) throw std::logic_error("study snapshot size disagrees");
        std::vector<RestingOrder> tracked_orders;
        tracked_orders.reserve(live_size());
        const auto append_level = [&](Side side, Price price, const std::list<OrderId>& queue) {
            for (const auto id : queue) {
                const auto& order = live_.at(id);
                if (order.side != side || order.price != price || *order.position != id)
                    throw std::logic_error("study FIFO tracking content disagrees");
                tracked_orders.push_back({id, order.side, order.price, order.quantity});
            }
        };
        for (auto level = levels_[0].rbegin(); level != levels_[0].rend(); ++level)
            append_level(Side::Buy, level->first, level->second);
        for (const auto& [price, queue] : levels_[1]) append_level(Side::Sell, price, queue);
        if (tracked_orders != snapshot.orders) throw std::logic_error("study reference FIFO order disagrees");
        if (ids_[0].size() + ids_[1].size() != live_size())
            throw std::logic_error("study side vectors disagree with live index");
        for (std::size_t side = 0; side < ids_.size(); ++side) {
            for (std::size_t index = 0; index < ids_[side].size(); ++index) {
                const auto& order = live_.at(ids_[side][index]);
                if (order.index != index || side_index(order.side) != side)
                    throw std::logic_error("study live-ID lookup position disagrees");
            }
        }
    }

    std::string_view profile_;
    std::size_t count_, depth_;
    Random random_;
    Config config_;
    ReferenceBook oracle_;
    std::vector<Event> storage_;
    EventBuffer output_;
    Workload workload_;
    std::uint64_t sequence_{}, tracked_quantity_{};
    std::array<Levels, 2> levels_;
    std::array<std::vector<OrderId>, 2> ids_;
    std::unordered_map<OrderId, Live> live_;
    bool refilling_{};
};

} // namespace

std::vector<std::string> profiles() {
    return {names.begin(), names.end()};
}

Workload make_workload(std::string_view profile, std::size_t commands,
                       std::uint64_t seed, std::size_t depth, Price domain) {
    if (std::find(names.begin(), names.end(), profile) == names.end())
        throw std::invalid_argument("unsupported study profile");
    if (depth == 0 || depth > 16384 || domain < static_cast<Price>(2 * depth) ||
        domain > 1000000 || commands > 1000000)
        throw std::invalid_argument("study parameters exceed bounded depth/domain/count ranges");
    return Generator{profile, commands, seed, depth, domain}.generate();
}

} // namespace ledger::study
