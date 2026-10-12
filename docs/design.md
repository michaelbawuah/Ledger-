# Designing Ledger

Ledger models the execution core of a single instrument. The production-shaped
parts are deterministic sequencing, explicit rejects, maker-price trades,
price-time priority, and a caller-drained execution-report stream. It has no
exchange or brokerage connection. The complete behavior is specified in
[semantics.md](semantics.md); data-structure inspiration is credited in
[references.md](references.md).

## Two implementations, one contract

The reference uses ordered `std::map` levels, `std::list` FIFO queues and an
`std::unordered_map` ID index holding stable iterators. It favors readable logic.
It independently validates, matches, cancels, and constructs execution reports;
it does not call any fast-engine matching helpers. Shared structures describe
commands/events only. This reduces the risk that one bug is duplicated by a
shared implementation.

The fast engine uses flat arrays indexed by `price - min_price`, one for each
side. Each level stores aggregate quantity and head/tail slot indices. All orders
live in a fixed arena constructed at startup; doubly linked FIFO chains refer
to slot indices, and a free list recycles completed/cancelled slots. An ID table
with a maximum load factor of one half maps arbitrary nonzero 64-bit IDs to
slots. Backward-shift deletion heals probe chains rather than retaining
tombstones that would accumulate over a long replay.

Cached best bid/ask offsets are updated on insertion. When the best level
empties, search starts at that level and moves outward. The baseline examines
individual levels; the optimized version skips empty 64-price words using an
occupancy bitmap and C++ bit-counting operations. Both modes use the same
matching implementation and are tested against the reference. The benchmark
therefore isolates the search representation on an identical input stream.

| Operation | Fast engine cost | Reference cost |
|---|---|---|
| Reach a known price | O(1) | O(log L) |
| Find ID | Expected O(1), bounded worst-case O(N) | Expected O(1) |
| Append/unlink FIFO node | O(1) | O(1) |
| Empty-best price search | O(P) linear; O(ceil(P/64)) bitmap worst case | Tree erase/find O(log L) |
| Match | O(T + price search + ID maintenance) | O(T + level/tree maintenance) |
| FOK preflight | O(P) worst case | O(L) worst case |
| Startup storage | O(P + N), including report storage | Grows with live orders/levels |

P is the configured price range, N live-order capacity, L occupied levels and T
the number of makers executed. Cancelling an order unlinks its FIFO node in
constant time; the complete cancellation may also search for a new best price.
No claim of unconditional worst-case O(1) cancellation or matching is made.

## Exact arithmetic and bounded resources

Prices are positive signed 64-bit ticks and order quantities are unsigned 32-bit
integers. A level sum and conservation counters use 64-bit integers. Positivity
reserves zero as the empty best-quote sentinel and makes range subtraction safe.
Currency tick size, fees, fractional quantities and multiple instruments belong
in a venue-specific wrapper. Lifetime counters can wrap after sufficiently
large cumulative flow; this simulator does not persist counters indefinitely.

`process()` is noexcept in the fast book and uses no heap allocator, lock, I/O,
callback, or resizing. Construction and diagnostic snapshots may allocate and
throw. The allocation test replaces C++ `new` overloads and scopes counting to
processing and quote getters; source review additionally checks that the hot
path calls no direct malloc API. This demonstrates allocation-free processing
for the tested paths rather than promising a real-time scheduler guarantee.

Output capacity is checked before validation or mutation. Up to N makers can be
filled in one command, each producing a Trade and two status reports. Acceptance,
expiry and replacement cancellation fit inside `3*N+4` caller-owned slots.
An undersized output buffer returns OutputFull without changing the book or
statistics. The caller must drain reports before the next command. This batch
buffer avoids dropping reports or blocking the matcher; network publication is
outside the core.

FOK checks eligible opposite liquidity before any state changes. Replacement
first validates fields and FOK feasibility, then cancels and submits the same
ID, losing priority even on a size reduction. New GTC orders at full pool
capacity are conservatively rejected before matching. This sacrifices a
potential crossing fill to guarantee resource availability for any residual.

## Verification and measurement boundaries

Each differential command compares its result, every event field, statistics,
live count, resting quantity, best prices and observable conservation/uncrossed
quote invariants. Full arena/index/level/FIFO invariants and snapshots are
checked periodically and at every seed end; semantic unit tests run full checks
after each command. The adversarial generator intentionally includes rejects,
ID reuse, capacity pressure, dense/sparse prices, IOC/FOK and integer boundaries.
A separate benchmark generator tracks real live IDs through a reference book
and targets a 60/30/10 limit/cancel/market mix. The workloads serve different
purposes and their counts are reported separately.

The binary protocol uses explicit little-endian encoding, fixed sizes, a
versioned header, validated reserved bytes and enum bounds. Framing validation
is separated from semantic validation: a wire-valid zero quantity is decoded,
then rejected by the engine. The header and record decoders are noexcept and
do not mutate their outputs on failure. The file reader checks its record cap,
integer size bounds and exact byte length before allocating storage.

Throughput measures a complete batch separately from individually timed
latency. Event checksums prevent unused work from disappearing, and matching
checksums/state must agree across engines. Warmup, run repetitions, compiler,
CPU, affinity, workload counts and raw results are recorded. `steady_clock`
measurement overhead remains in latency samples and is reported separately.
Shared virtual-host tail latency is sensitive to scheduling; it does not
predict exchange or network latency. See [results.md](results.md).

The SPSC adapter has exactly one producer and one consumer, release/acquire
publication, fixed storage and explicit full/empty returns. It is verified
separately under ThreadSanitizer. Queue-to-core latency includes scheduling and
backpressure and is distinct from single-thread core latency. See
[strategy.md](strategy.md) for the accounting-only market maker and its limits.

## What remains outside v1

Stop/stop-limit orders, multi-instrument routing, Nasdaq ITCH, UDP market-data
publication, Python bindings, exchange sessions and durable recovery are not
implemented. A real venue adapter would need instrument tick tables, feed
sequence recovery, risk checks, self-trade rules, durable sequencing and a
bounded publication/backpressure policy. The bounded price range should be
selected from actual instruments or replaced with a sparse hierarchical index.
