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

private:
    const RiskConfig config_;
};

} // namespace ledger
