#include "ledger/risk.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>

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

ledger::Command limit_order() {
    return {1, 42, 100, 10, ledger::Side::Buy, ledger::Action::Limit, ledger::TimeInForce::GTC};
}

void expect(const ledger::RiskPolicy& policy, const ledger::Command& command,
            ledger::RiskReject reason, const ledger::RiskContext& context = {}) {
    const auto decision = policy.evaluate(command, context);
    require(decision.reason == reason, "unexpected risk decision");
    require(decision.approved() == (reason == ledger::RiskReject::None), "decision approval differs");
    require(static_cast<bool>(decision) == decision.approved(), "decision bool differs");
}

void command_envelope() {
    using namespace ledger;
    static_assert(noexcept(std::declval<const RiskPolicy&>().evaluate(
        std::declval<const Command&>(), std::declval<const RiskContext&>())));
    const RiskPolicy policy;
    auto command = limit_order();
    expect(policy, command, RiskReject::None);
    command.action = static_cast<Action>(255);
    expect(policy, command, RiskReject::InvalidAction);
    command = limit_order();
    command.side = static_cast<Side>(255);
    expect(policy, command, RiskReject::InvalidSide);
    command = limit_order();
    command.tif = static_cast<TimeInForce>(255);
    expect(policy, command, RiskReject::InvalidTimeInForce);
    command = limit_order();
    command.id = 0;
    expect(policy, command, RiskReject::InvalidId);
    command = limit_order();
    const auto original = command;
    const RiskContext context{-7, 101};
    const auto original_context = context;
    (void)policy.evaluate(command, context);
    require(command == original && context == original_context, "evaluation mutated input");
    command.action = Action::Cancel;
    expect(policy, command, RiskReject::None);
}
} // namespace

int main() {
    try {
        configuration_validation();
        command_envelope();
        std::cout << "PASS risk configuration and noexcept command envelope\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
