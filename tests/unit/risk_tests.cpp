#include "ledger/risk.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void rejects_config(const ledger::RiskConfig& config) {
    try {
        (void)ledger::RiskPolicy(config);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("invalid risk configuration was accepted");
}

void configuration_validation() {
    const ledger::RiskConfig defaults{};
    require(ledger::RiskPolicy().config() == defaults, "default configuration changed");
    auto config = defaults;
    config.max_order_quantity = 0;
    rejects_config(config);
    config = defaults;
    config.max_order_notional = 0;
    rejects_config(config);
    config = defaults;
    config.max_abs_inventory = -1;
    rejects_config(config);
    config = defaults;
    config.min_price = 0;
    rejects_config(config);
    config = defaults;
    config.max_price = config.min_price - 1;
    rejects_config(config);
    config = defaults;
    config.max_abs_inventory = 0;
    require(ledger::RiskPolicy(config).config() == config, "zero inventory limit is valid");
}
} // namespace

int main() {
    try {
        configuration_validation();
        std::cout << "PASS risk policy startup configuration validation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
