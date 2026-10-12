# Optional integer pre-trade risk helper

`ledger/risk.hpp` provides a header-only `RiskPolicy`. It evaluates one command
against immutable limits and caller-supplied single-instrument exposure. It is
independent of `FastBook`: the matching engine does not invoke it automatically.

`evaluate()` is `const noexcept`, performs no allocation, and changes neither the
command, context, nor policy. Startup construction may throw `std::invalid_argument`
for a bad configuration. Rejections return a reason instead of throwing.

## Configuration and units

| Field | Meaning | Valid range |
| --- | --- | --- |
| `max_order_quantity` | Maximum full quantity of one order | Positive `uint32_t` |
| `max_order_notional` | Maximum `valuation_price * quantity` | Positive `uint64_t` |
| `max_abs_inventory` | Allowed projected interval `[-limit, +limit]` | Nonnegative `int64_t` |
| `min_price`, `max_price` | Inclusive integer valuation band | `0 < min_price <= max_price` |

Prices are integer ticks. Notional is tick-units times quantity, not dollars.
The helper has no tick-size conversion, contract multiplier, fees, currency
conversion, margin calculation, portfolio aggregation, or broker connection.
A zero inventory limit is valid: only an order whose full fill returns exposure
to zero can pass the inventory check.

## Decision boundary

Checks run in this order, making the first rejection deterministic:

1. Validate action, side, time-in-force enum values, and nonzero order ID.
2. Allow a valid cancel. Its price, quantity, valuation, and exposure are ignored.
3. Require nonzero quantity no greater than `max_order_quantity`.
4. For market orders, require a positive `RiskContext::market_valuation_price`.
   For limit and replace orders, require a positive command price.
5. Check the valuation against the inclusive configured price band.
6. Check notional by comparing quantity with `max_order_notional / price`.
7. Reject signed overflow before adding buy quantity or subtracting sell quantity.
8. Require the full-fill projected inventory to lie in the configured interval.

Notional multiplication occurs only after the division check proves it fits in
`uint64_t`. Inventory is compared with an interval instead of using `abs`, so
`INT64_MIN` cannot overflow. The code uses standard fixed-width integer types;
it does not require `__int128`.

An approved exposure-increasing command reports its full-fill
`projected_inventory`, integer `valuation_price`, and `order_notional`. A cancel
reports the supplied position with zero valuation and zero notional. Every
rejected decision zeroes these diagnostic fields; inspect its `reason` first.

## Caller responsibilities

`RiskContext::position` must reflect the exposure the caller intends to assess.
The helper does not update positions after fills, reserve resting orders, release
reservations after cancels, or synchronize concurrent submissions. Two commands
checked against the same stale position can both pass and jointly exceed a limit.
An integrating gateway must own reservation accounting and admission sequencing.
For opposing pending orders, use appropriate worst-case directional exposure;
netting all pending buys and sells can hide risk.

Replace commands are checked conservatively as a complete new quantity. The
helper does not subtract an old order's remaining quantity or assume its exposure
has been released. The matching engine remains responsible for ID existence,
duplicate IDs, side consistency, capacity, and order-processing semantics.

For a market order, the supplied valuation must be a conservative bound on possible
fill prices. The helper checks that estimate; it does not constrain the engine's
execution prices or guarantee the estimate remains valid after market movement.
If such a bound is unavailable, do not interpret an approval as a guaranteed
notional cap. A priced IOC order is an alternative when an execution-price cap is
required by the caller's application.

A supplied position outside the allowed interval may be accepted when the full
new quantity returns it to the interval. This permits recovery orders. Valid
cancels always remain available during an exposure breach.

## Example

```cpp
#include "ledger/risk.hpp"

const ledger::RiskPolicy risk({500, 50'000, 1'000, 95, 105});
const ledger::Command buy{1, 42, 100, 100, ledger::Side::Buy,
                          ledger::Action::Limit, ledger::TimeInForce::GTC};
const auto decision = risk.evaluate(buy, {900, 0});
// decision.approved() == true; projected_inventory == 1000.
// Reserve exposure in caller-owned accounting before admitting another order.
// Then submit to FastBook and reconcile reservations with its result/events.
```

The standalone demonstration exercises limit approval, an inventory rejection,
missing and supplied market valuation, notional rejection, and cancel bypass:

```sh
c++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -Iinclude \
    examples/risk_demo.cpp -o /tmp/ledger-risk-demo
/tmp/ledger-risk-demo
```

## Verification

`tests/unit/risk_tests.cpp` checks startup validation, decision input immutability,
quantity and price edges, signed and unsigned integer extremes, both sides,
conservative replacements, valid cancels during breached exposure, every byte
encoding of the three command enums, deterministic rejection precedence,
19,280 bounded inventory combinations, and 10,000 seeded notional cases.

```sh
c++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -Iinclude \
    tests/unit/risk_tests.cpp -o /tmp/ledger-risk-tests
/tmp/ledger-risk-tests

c++ -std=c++23 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iinclude tests/unit/risk_tests.cpp -o /tmp/ledger-risk-tests-sanitized
ASAN_OPTIONS=detect_leaks=0 /tmp/ledger-risk-tests-sanitized
```

Both commands passed locally with 67,524 assertions. This is verification of the
helper's documented preflight arithmetic, not certification of a trading gateway,
broker, real-money deployment, or full portfolio risk system.
