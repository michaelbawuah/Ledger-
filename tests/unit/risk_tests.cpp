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

void quantity_limits() {
    using namespace ledger;
    RiskConfig config{};
    config.max_order_quantity = 12;
    const RiskPolicy policy(config);
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const auto action : {Action::Limit, Action::Market, Action::Replace}) {
            auto command = limit_order();
            command.side = side;
            command.action = action;
            command.quantity = 0;
            expect(policy, command, RiskReject::InvalidQuantity);
            command.quantity = 1;
            expect(policy, command, RiskReject::None, {0, 100});
            command.quantity = 12;
            expect(policy, command, RiskReject::None, {0, 100});
            command.quantity = 13;
            expect(policy, command, RiskReject::OrderQuantityLimit);
            command.quantity = std::numeric_limits<Quantity>::max();
            expect(policy, command, RiskReject::OrderQuantityLimit);
        }
    }
}

void integer_price_band() {
    using namespace ledger;
    RiskConfig config{};
    config.min_price = 95;
    config.max_price = 105;
    const RiskPolicy policy(config);
    auto command = limit_order();
    for (const auto price : {Price{0}, Price{-1}, std::numeric_limits<Price>::min()}) {
        command.price = price;
        expect(policy, command, RiskReject::InvalidPrice);
    }
    for (const auto price : {Price{94}, Price{106}, std::numeric_limits<Price>::max()}) {
        command.price = price;
        expect(policy, command, RiskReject::PriceBand);
    }
    for (const auto price : {Price{95}, Price{105}}) {
        command.price = price;
        expect(policy, command, RiskReject::None);
        require(policy.evaluate(command).valuation_price == price, "limit valuation differs");
    }
    command.action = Action::Market;
    command.price = std::numeric_limits<Price>::min(); // Market price comes only from context.
    expect(policy, command, RiskReject::MissingMarketValuation);
    expect(policy, command, RiskReject::MissingMarketValuation, {0, -1});
    expect(policy, command, RiskReject::PriceBand, {0, 94});
    expect(policy, command, RiskReject::None, {0, 95});
    expect(policy, command, RiskReject::None, {0, 105});
    expect(policy, command, RiskReject::PriceBand, {0, 106});
    require(policy.evaluate(command, {0, 105}).valuation_price == 105, "market valuation differs");
}

void notional_without_overflow() {
    using namespace ledger;
    RiskConfig config{};
    config.max_order_notional = 1'001;
    const RiskPolicy policy(config);
    auto command = limit_order();
    for (const auto side : {Side::Buy, Side::Sell}) {
        command.side = side;
        expect(policy, command, RiskReject::None);
        require(policy.evaluate(command).order_notional == 1'000, "accepted notional differs");
        command.quantity = 11;
        expect(policy, command, RiskReject::OrderNotionalLimit);
        command.quantity = 10;
    }
    command.action = Action::Market;
    expect(policy, command, RiskReject::None, {0, 100});
    expect(policy, command, RiskReject::OrderNotionalLimit, {0, 101});

    config.max_order_quantity = std::numeric_limits<Quantity>::max();
    config.max_order_notional = std::numeric_limits<std::uint64_t>::max();
    config.max_abs_inventory = std::numeric_limits<std::int64_t>::max();
    const RiskPolicy extreme(config);
    command = limit_order();
    command.price = std::numeric_limits<Price>::max();
    command.quantity = 2;
    expect(extreme, command, RiskReject::None);
    require(extreme.evaluate(command).order_notional ==
            std::numeric_limits<std::uint64_t>::max() - 1, "wide accepted notional differs");
    command.quantity = 3;
    expect(extreme, command, RiskReject::OrderNotionalLimit);
    command.quantity = std::numeric_limits<Quantity>::max();
    expect(extreme, command, RiskReject::OrderNotionalLimit);
    command.price = 1;
    expect(extreme, command, RiskReject::None);
}

void projected_inventory_limits() {
    using namespace ledger;
    RiskConfig config{};
    config.max_abs_inventory = 100;
    const RiskPolicy policy(config);
    auto command = limit_order();
    expect(policy, command, RiskReject::None, {90, 0});
    require(policy.evaluate(command, {90, 0}).projected_inventory == 100, "buy projection differs");
    expect(policy, command, RiskReject::InventoryLimit, {91, 0});
    expect(policy, command, RiskReject::None, {-110, 0}); // Recovery to the allowed interval.
    command.side = Side::Sell;
    expect(policy, command, RiskReject::None, {-90, 0});
    require(policy.evaluate(command, {-90, 0}).projected_inventory == -100, "sell projection differs");
    expect(policy, command, RiskReject::InventoryLimit, {-91, 0});
    expect(policy, command, RiskReject::None, {110, 0});

    config.max_abs_inventory = 0;
    const RiskPolicy flat_only(config);
    expect(flat_only, command, RiskReject::None, {10, 0});
    expect(flat_only, command, RiskReject::InventoryLimit, {9, 0});

    config.max_abs_inventory = std::numeric_limits<std::int64_t>::max();
    const RiskPolicy extreme(config);
    command.quantity = 1;
    const auto low = std::numeric_limits<std::int64_t>::min();
    const auto high = std::numeric_limits<std::int64_t>::max();
    expect(extreme, command, RiskReject::InventoryOverflow, {low, 0});
    expect(extreme, command, RiskReject::InventoryLimit, {low + 1, 0});
    expect(extreme, command, RiskReject::None, {high, 0});
    command.side = Side::Buy;
    expect(extreme, command, RiskReject::InventoryOverflow, {high, 0});
    expect(extreme, command, RiskReject::None, {high - 1, 0});
    expect(extreme, command, RiskReject::None, {low, 0});
    require(extreme.evaluate(command, {low, 0}).projected_inventory == low + 1,
            "INT64_MIN buy recovery differs");
}

void cancel_and_conservative_replace() {
    using namespace ledger;
    RiskConfig config{};
    config.max_order_quantity = 10;
    config.max_order_notional = 1'000;
    config.max_abs_inventory = 100;
    config.min_price = 95;
    config.max_price = 105;
    const RiskPolicy policy(config);
    auto command = limit_order();
    command.action = Action::Cancel;
    command.price = std::numeric_limits<Price>::min();
    command.quantity = std::numeric_limits<Quantity>::max();
    const RiskContext breached{std::numeric_limits<std::int64_t>::min(), -1};
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const auto tif : {TimeInForce::GTC, TimeInForce::IOC, TimeInForce::FOK}) {
            command.side = side;
            command.tif = tif;
            const auto decision = policy.evaluate(command, breached);
            require(decision.approved() && decision.projected_inventory == breached.position,
                    "valid cancel was blocked by exposure");
            require(decision.valuation_price == 0 && decision.order_notional == 0,
                    "cancel diagnostics imply an exposure change");
        }
    }
    command.id = 0;
    expect(policy, command, RiskReject::InvalidId, breached);
    command.id = 42;
    command.side = static_cast<Side>(2);
    expect(policy, command, RiskReject::InvalidSide, breached);
    command.side = Side::Buy;
    command.tif = static_cast<TimeInForce>(3);
    expect(policy, command, RiskReject::InvalidTimeInForce, breached);

    command = limit_order();
    command.action = Action::Replace;
    expect(policy, command, RiskReject::InventoryLimit, {95, 0});
    command.quantity = 5;
    const auto approved = policy.evaluate(command, {95, 0});
    require(approved.approved() && approved.projected_inventory == 100,
            "replace quantity was not assessed in full");
    require(policy.evaluate(command, {95, 0}) == approved, "evaluation retained hidden exposure state");
    require(policy.config() == config, "evaluation mutated the policy");
    command.quantity = 11;
    const auto rejected = policy.evaluate(command, {95, 0});
    require(!rejected.approved() && rejected.projected_inventory == 0 &&
            rejected.valuation_price == 0 && rejected.order_notional == 0,
            "rejected decision leaked a partially assessed exposure");
}
} // namespace

int main() {
    try {
        configuration_validation();
        command_envelope();
        quantity_limits();
        integer_price_band();
        notional_without_overflow();
        projected_inventory_limits();
        cancel_and_conservative_replace();
        std::cout << "PASS risk boundaries, cancellation, and conservative replacement\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
