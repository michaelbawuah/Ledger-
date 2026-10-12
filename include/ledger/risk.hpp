#pragma once

#include "ledger/types.hpp"
#include <limits>
#include <stdexcept>

namespace ledger {

// Prices are positive integer ticks; notional is quantity times ticks.
struct RiskConfig {
    Quantity max_order_quantity{1'000'000};
    std::uint64_t max_order_notional{std::numeric_limits<std::uint64_t>::max()};
    std::int64_t max_abs_inventory{1'000'000};
    Price min_price{1};
    Price max_price{std::numeric_limits<Price>::max()};
    bool operator==(const RiskConfig&) const = default;
};

struct RiskContext {
    std::int64_t position{};
    Price market_valuation_price{};
    bool operator==(const RiskContext&) const = default;
};

enum class RiskReject : std::uint8_t {
    None, InvalidAction, InvalidSide, InvalidTimeInForce, InvalidId,
    InvalidQuantity, OrderQuantityLimit,
    MissingMarketValuation, InvalidPrice, PriceBand,
    OrderNotionalLimit,
};

struct RiskDecision {
    RiskReject reason{RiskReject::None};
    std::int64_t projected_inventory{};
    Price valuation_price{};
    std::uint64_t order_notional{};
    bool approved() const noexcept { return reason == RiskReject::None; }
    explicit operator bool() const noexcept { return approved(); }
    bool operator==(const RiskDecision&) const = default;
};

// Construction is the startup validation boundary. Decisions will not throw.
class RiskPolicy {
public:
    explicit RiskPolicy(RiskConfig config = {}) : config_(config) {
        if (config_.max_order_quantity == 0 || config_.max_order_notional == 0 ||
            config_.max_abs_inventory < 0 || config_.min_price <= 0 ||
            config_.max_price < config_.min_price) {
            throw std::invalid_argument("invalid risk policy configuration");
        }
    }

    const RiskConfig& config() const noexcept { return config_; }

    RiskDecision evaluate(const Command& command, const RiskContext& context = {}) const noexcept {
        if (command.action > Action::Replace) return {RiskReject::InvalidAction};
        if (command.side > Side::Sell) return {RiskReject::InvalidSide};
        if (command.tif > TimeInForce::FOK) return {RiskReject::InvalidTimeInForce};
        if (command.id == 0) return {RiskReject::InvalidId};
        // A valid cancel must remain possible even when exposure exceeds limits.
        if (command.action == Action::Cancel) return {RiskReject::None, context.position};
        if (command.quantity == 0) return {RiskReject::InvalidQuantity};
        if (command.quantity > config_.max_order_quantity) return {RiskReject::OrderQuantityLimit};
        const bool market = command.action == Action::Market;
        const Price valuation = market ? context.market_valuation_price : command.price;
        if (market && valuation <= 0) return {RiskReject::MissingMarketValuation};
        if (valuation <= 0) return {RiskReject::InvalidPrice};
        if (valuation < config_.min_price || valuation > config_.max_price) return {RiskReject::PriceBand};
        const auto unsigned_price = static_cast<std::uint64_t>(valuation);
        // Divide before multiplying: price * quantity may exceed uint64_t.
        if (command.quantity > config_.max_order_notional / unsigned_price) {
            return {RiskReject::OrderNotionalLimit};
        }
        const auto notional = unsigned_price * static_cast<std::uint64_t>(command.quantity);
        return {RiskReject::None, context.position, valuation, notional};
    }

private:
    const RiskConfig config_;
};

} // namespace ledger
