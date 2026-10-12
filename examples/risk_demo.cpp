#include "ledger/risk.hpp"
#include <iostream>
#include <string_view>

namespace {
constexpr std::string_view reason_name(ledger::RiskReject reason) noexcept {
    using ledger::RiskReject;
    switch (reason) {
    case RiskReject::None: return "approved";
    case RiskReject::InvalidAction: return "invalid action";
    case RiskReject::InvalidSide: return "invalid side";
    case RiskReject::InvalidTimeInForce: return "invalid time in force";
    case RiskReject::InvalidId: return "invalid order ID";
    case RiskReject::InvalidQuantity: return "zero quantity";
    case RiskReject::OrderQuantityLimit: return "order quantity limit";
    case RiskReject::MissingMarketValuation: return "market valuation required";
    case RiskReject::InvalidPrice: return "invalid price";
    case RiskReject::PriceBand: return "price band";
    case RiskReject::OrderNotionalLimit: return "order notional limit";
    case RiskReject::InventoryOverflow: return "inventory arithmetic overflow";
    case RiskReject::InventoryLimit: return "inventory limit";
    }
    return "unknown decision";
}

bool show(std::string_view label, const ledger::RiskDecision& decision, ledger::RiskReject expected) {
    std::cout << label << ": " << reason_name(decision.reason);
    if (decision.approved()) {
        std::cout << ", projected inventory=" << decision.projected_inventory
                  << ", integer notional=" << decision.order_notional;
    }
    std::cout << '\n';
    return decision.reason == expected;
}
} // namespace

int main() {
    using namespace ledger;
    const RiskPolicy policy({500, 50'000, 1'000, 95, 105});
    // This is caller-known exposure, not state maintained by RiskPolicy.
    const RiskContext exposure{900, 0};
    Command command{1, 42, 100, 100, Side::Buy, Action::Limit, TimeInForce::GTC};
    bool correct = show("Limit at inventory boundary", policy.evaluate(command, exposure), RiskReject::None);
    command.quantity = 101;
    correct &= show("Limit beyond inventory boundary", policy.evaluate(command, exposure), RiskReject::InventoryLimit);

    command.quantity = 100;
    command.action = Action::Market;
    command.price = 0;
    correct &= show("Market without valuation", policy.evaluate(command, exposure), RiskReject::MissingMarketValuation);
    // The caller must ensure this bound is conservative for all possible fills.
    correct &= show("Market with valuation bound", policy.evaluate(command, {900, 105}), RiskReject::None);

    command.action = Action::Limit;
    command.price = 105;
    command.quantity = 500;
    correct &= show("Notional above budget", policy.evaluate(command), RiskReject::OrderNotionalLimit);

    command.action = Action::Cancel;
    command.price = -1;
    command.quantity = 0;
    correct &= show("Cancel during exposure breach", policy.evaluate(command, {2'000, 0}), RiskReject::None);
    std::cout << "Decisions are preflight checks; pending-order reservation belongs to the caller.\n";
    return correct ? 0 : 1;
}
