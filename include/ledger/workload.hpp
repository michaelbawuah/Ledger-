#pragma once
#include "ledger/types.hpp"
#include <algorithm>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace ledger {
struct WorkloadStatistics {
    std::uint64_t commands{}, limits{}, cancels{}, markets{}, rejected_commands{},
                  empty_book_cancels{}, capacity_ioc_limits{};
    std::size_t peak_live_orders{};
};
// Stateful streaming generator. Book is an independent oracle (normally ReferenceBook).
// Generation, oracle execution, and live-ID tracking are outside benchmark timings.
// The same seed/configuration produces the same stream on all standard libraries:
// bounded random values use a specified rejection sampler over mt19937_64 output.
template<class Book>
class WorkloadGenerator {
public:
    explicit WorkloadGenerator(std::uint64_t seed, Config config = {})
        : config_(config), random_(seed), book_(config),
          storage_(required_event_capacity(config.max_orders)), output_(storage_) {
        if (config.min_price <= 0 || config.max_price < config.min_price || !config.max_orders)
            throw std::invalid_argument("invalid workload configuration");
        live_.reserve(config.max_orders);
        indices_.reserve(static_cast<std::size_t>(config.max_orders) * 2);
    }
    WorkloadGenerator(const WorkloadGenerator&) = delete;
    WorkloadGenerator& operator=(const WorkloadGenerator&) = delete;
    WorkloadGenerator(WorkloadGenerator&&) = delete;
    WorkloadGenerator& operator=(WorkloadGenerator&&) = delete;
    Command next() {
        if (stats_.commands == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("workload sequence exhausted");
        Command c; c.sequence = stats_.commands + 1; c.id = c.sequence;
        c.side = draw(2) ? Side::Sell : Side::Buy;
        const auto choice = draw(100);
        if (choice < 60) {
            c.action = Action::Limit; ++stats_.limits;
            c.quantity = static_cast<Quantity>(50 + draw(101));
            const auto midpoint = config_.min_price + (config_.max_price - config_.min_price) / 2;
            const auto delta = static_cast<Price>(1 + draw(64));
            // Two percent of limits cross the central spread; most provide liquidity.
            const bool cross = draw(100) < 2;
            const bool above = (c.side == Side::Sell) != cross;
            c.price = above ? midpoint + std::min(delta, config_.max_price - midpoint)
                            : midpoint - std::min(delta, midpoint - config_.min_price);
            // Guard the configured pool without inventing extra cancel commands.
            if (live_.size() + 1 >= config_.max_orders) {
                c.tif = TimeInForce::IOC; ++stats_.capacity_ioc_limits;
            }
        } else if (choice < 90) {
            c.action = Action::Cancel; ++stats_.cancels;
            if (!live_.empty()) {
                const auto index = static_cast<std::size_t>(draw(live_.size()));
                c.id = live_[index].id; c.side = live_[index].side;
            } else {
                // An explicitly counted unknown-ID cancel preserves the requested mix.
                c.id = c.sequence; ++stats_.empty_book_cancels;
            }
        } else {
            c.action = Action::Market; ++stats_.markets;
            const auto target = std::max<std::size_t>(1, std::min<std::size_t>(2048, config_.max_orders / 2));
            // Larger sweeps above the target keep million-command feeds below pool capacity.
            c.quantity = live_.size() > target ? static_cast<Quantity>(350 + draw(451))
                                               : static_cast<Quantity>(48 + draw(177));
        }
        output_.clear();
        const auto result = book_.process(c, output_);
        if (result == ProcessResult::OutputFull) throw std::logic_error("generator event capacity violated");
        if (result == ProcessResult::Rejected) ++stats_.rejected_commands;
        else {
            Quantity filled{};
            for (const auto& event : output_.events()) {
                if (event.type == EventType::Trade) filled += event.quantity;
                if ((event.type == EventType::Filled && event.id != c.id)
                    || event.type == EventType::Cancelled) remove(event.id);
            }
            if (c.action == Action::Limit && c.tif == TimeInForce::GTC && filled < c.quantity) {
                indices_.emplace(c.id, live_.size()); live_.push_back({c.id, c.side});
            }
        }
        ++stats_.commands;
        stats_.peak_live_orders = std::max(stats_.peak_live_orders, live_.size());
        return c;
    }
    const WorkloadStatistics& stats() const noexcept { return stats_; }
    std::size_t live_orders() const noexcept { return live_.size(); }
private:
    struct Live { OrderId id; Side side; };
    std::uint64_t draw(std::uint64_t bound) {
        const auto threshold = (std::uint64_t{0} - bound) % bound;
        for (;;) { const auto value = random_(); if (value >= threshold) return value % bound; }
    }
    void remove(OrderId id) {
        const auto it = indices_.find(id);
        if (it == indices_.end()) return;
        const auto index = it->second;
        if (index + 1 != live_.size()) {
            live_[index] = live_.back(); indices_[live_[index].id] = index;
        }
        live_.pop_back(); indices_.erase(it);
    }
    Config config_; std::mt19937_64 random_; Book book_;
    std::vector<Event> storage_; EventBuffer output_;
    std::vector<Live> live_; std::unordered_map<OrderId, std::size_t> indices_;
    WorkloadStatistics stats_;
};
template<class Book>
std::vector<Command> make_workload(std::size_t count, std::uint64_t seed, Config config = {}) {
    WorkloadGenerator<Book> generator(seed, config);
    std::vector<Command> commands; commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) commands.push_back(generator.next());
    return commands;
}
}
