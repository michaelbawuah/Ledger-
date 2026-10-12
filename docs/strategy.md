# Synthetic market making and feed handoff

`ledger_market_maker` is an accounting and execution demonstration on a seeded synthetic
stream. It is **not live trading or a historical backtest**. No brokerage or
exchange connection is implemented. Prices and cash are integer tick units; a
tick's currency value is deliberately unspecified.

## Strategy mechanics

At each step the program cancels the previous remaining orders, posts external
liquidity at mid minus/plus two ticks, then places its own passive bid and ask at
mid minus/plus one tick. Each quote is at most `--quote-quantity` units. The bid
is capped by `inventory_limit - inventory`; the ask is capped by
`inventory_limit + inventory`. A quote on a side with zero room is skipped.
These caps keep signed inventory in the inclusive interval
`[-inventory_limit, inventory_limit]` after every individual fill.

One external immediate market order arrives per step. Its side and quantity are
generated with `std::mt19937_64`; quantity is 1 through twice the configured
quote quantity. The supplied seed reproduces the draws. After execution the
fair mark moves zero through three ticks in the aggressor's direction, bounded
to 1,000 through 19,000. This simple adverse-selection mechanism is disclosed
rather than tuned to produce a positive return.

The simulator updates inventory and cash only from actual `Trade` execution
reports whose maker ID is one of the two strategy quotes. A maker bid fill adds
inventory and deducts `execution_price * quantity` from cash; a maker ask fill
does the reverse. Every fill checks the inventory cap. Final mark-to-market P&L
is `cash + inventory * final_mid`, starting from zero cash and inventory. This
is an unrealized-and-realized total, not a realized-only P&L. Remaining quotes
are cancelled before reporting; final inventory is marked rather than
liquidated. The program checks signed buy-minus-sell inventory and total maker
fill volume against the execution accounting.

`quote_unit_fill_rate` is filled maker units divided by all maker quote units
submitted across refreshes. `flow_order_maker_fill_rate` is external market
orders producing at least one maker fill divided by external market orders.
They have different denominators and are not interchangeable. Maximum exposure
is maximum absolute inventory observed after each fill, in units.

```sh
./build/release/ledger_market_maker --steps 10000 --seed 42 --quote-quantity 5 --inventory-limit 20
```

The setup omits fees, rebates, execution latency, exchange queue competition,
hidden orders, market impact, a validated fill model, and historical calibration.
FIFO matching is real engine behavior, but the synthetic strategy deliberately
posts inside the external spread and has no competitors at its quote price.
Reported P&L therefore supplies an accounting example and makes no profitability
claim. This bounded single-instrument simulation does not establish production
trading readiness.

## Two-thread feed handoff

`SpscQueue<T, Capacity>` has a fixed power-of-two usable capacity. Exactly one
producer may call `try_push` and one consumer may call `try_pop`; additional
producers or consumers are unsupported. Items must be trivially copyable and
have noexcept copy assignment. Full and empty operations return `false` without
changing the item or queue. No entries are silently overwritten or dropped.
The caller chooses its backpressure policy; the benchmark spins and occasionally
yields while full or empty.

The producer publishes initialized slots with a release write to the head
counter, and the consumer observes them through an acquire load. The consumer
publishes its completed copy with a release write to the tail; the producer's
acquire load prevents reuse before that copy finishes. Only each owning thread
loads its own counter relaxed. Unsigned monotonically increasing counters and
power-of-two slot masking preserve indexing across counter wrap. Head, tail,
and storage start on separate 64-byte boundaries; machines with larger cache
lines can still share physical lines. A compile-time assertion rejects platforms
where 64-bit atomics are not always lock-free. Lock-free atomics do not imply
bounded scheduler latency. Construction and destruction require stopped threads;
`empty()` is an observation, not a concurrent shutdown protocol.

The SPSC unit program checks capacity one, repeated full/empty slot wrap, unchanged
failed-pop output, and an exact million-command sequence transferred between
threads. ThreadSanitizer must be run separately before reporting a clean
ThreadSanitizer result; normal stress success alone does not prove race freedom.

```sh
./build/release/ledger_spsc_tests
./build/release/ledger_spsc_bench --commands 1000000 --latency-samples 10000
./build/release/ledger_spsc_bench --commands 1000000 --latency-samples 0
```

The workload repeats five commands: rest a buy, partially fill it, cancel the
residual, rest a sell, and fully fill it. It is synthetic and keeps the book
small. The feed thread constructs each `Command`; a 4,096-slot SPSC passes it to
the engine thread, which processes it and drains the events. Engine construction,
result storage allocation, and thread creation precede the timed start.

`commands_per_second` measures total completed commands divided by elapsed
two-thread time, including event draining, handoff, backpressure, and joins.
Latency samples are taken at approximately even command intervals, using a
timestamp immediately before the first enqueue attempt and immediately after
engine processing returns. That interval includes producer backpressure, queue
wait, consumer scheduling, and engine processing, but excludes subsequent event
draining. It is **not engine-only latency** or exchange/network latency. Sampling
adds clock overhead; a zero-sample run measures throughput without those clock
reads. Threads are not pinned and operating-system scheduling can dominate tails.
