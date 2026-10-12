# Sparse fill-or-kill preflight

A fill-or-kill (FOK) order must either trade its entire quantity immediately or
leave the resting book unchanged. Before matching starts, `FastBook::can_fill`
sums executable quantity on the opposite side. A limit order stops at its limit;
a market order can consume every price level. Failed replacement preflight must
also preserve the original order and its FIFO position.

The original preflight visited every price tick from the best opposite quote,
including empty levels. Matching already used occupancy bitmaps to find the next
maker price, but that improvement did not apply to FOK validation. On a sparse
book, validation could therefore examine a wide flat price range before it
reached a small number of actual resting levels.

The new default preflight uses the existing per-side occupancy bitmap. It reads
the best level's aggregate quantity, then finds the next occupied level in price
priority with masked 64-bit words and `std::countr_zero`/`std::countl_zero`.
It stops once the requested quantity is available, the next occupied level is
outside the limit, or no more liquidity exists. Directional boundary checks
prevent moving below offset zero or above the last allocated level. Price
arithmetic uses offsets, so a valid narrow range ending at `INT64_MAX` remains
supported.

There are no new per-order counters, allocations, or matching-path diagnostics.
The pool, ID index, FIFO links, level quantities, matching reports, and order
semantics stay the same. The preflight only reads existing book state.

## Controlled ablation

The constructor keeps the two search decisions independent:

```cpp
FastBook(Config config = {}, bool bitmap_search = true, bool bitmap_fok = true);
```

| Constructor choices | Next maker price search | FOK preflight |
| --- | --- | --- |
| `FastBook(config, true, true)` | Occupancy bitmap | Occupancy bitmap |
| `FastBook(config, true, false)` | Occupancy bitmap | Original tick-by-tick scan |
| `FastBook(config, false, true)` | Tick-by-tick scan | Occupancy bitmap |
| `FastBook(config, false, false)` | Tick-by-tick scan | Original tick-by-tick scan |

Existing zero-, one-, and two-argument calls remain source-compatible. FOK
bitmap traversal is enabled by default, including in `FastBook(config, false)`.
Use the third argument explicitly when measuring the complete legacy linear
variant. The study names `fast-bitmap`, `fast-fok-linear`, and `fast-linear`
correspond to the first, second, and fourth rows, respectively.

Holding `bitmap_search=true` and changing only `bitmap_fok` isolates the FOK
preflight change. Both variants must consume identical commands and emit
identical execution reports; their final complete snapshots must also agree.
Throughput and latency comparisons belong to the captured study results.
This document does not claim a speedup from complexity alone.

## Cost and remaining limits

For the part of the book examined before a preflight decision, the original path
costs `O(price ticks)`. The bitmap path costs
`O(occupied levels + bitmap word gaps)`: each occupied level contributes an
aggregate-quantity read, and gaps require reads of empty 64-bit words. It is not
an `O(1)` liquidity query or a hierarchical bitmap. A next-level search may scan
empty words beyond a limit to discover that the next occupied level does not
cross. Very wide sparse domains still incur word-scanning and flat-array storage
costs. Dense books and short scans may benefit less or incur extra bitmap work;
the workload-specific measurements decide that tradeoff.

## Verification

`tests/unit/fok_tests.cpp` checks all four constructor combinations against the
independent STL `ReferenceBook`. Every command compares the process result,
every field of every report, the full FIFO/level/statistics snapshot, resting
quantity, best quotes, and both structural validators. The suite also probes
all C++ allocation forms around `FastBook::process` only.

The 96 targeted scenarios include both taker sides; successful and unsuccessful
limit and market FOK across wide empty gaps; limits that exclude deeper
liquidity; atomic rejected replacement; successful replacement; empty books;
full order pools; aggregate quantities wider than the command quantity type;
exact and partial final bitmap words; word-boundary transitions; and a price
interval ending at `INT64_MAX`. These scenarios contain 440 process calls,
all with zero observed C++ allocation requests on the fast path.

Local validation passed the targeted suite in optimized and AddressSanitizer /
UndefinedBehaviorSanitizer builds, all 74 existing named semantic tests, and
2,000,000 seeded differential commands with 4,184,863 compared reports and
490 complete structural checkpoints. Leak detection was disabled for the
sanitizer run because the sandbox's process tracing prevents LeakSanitizer from
enumerating threads. These are correctness checks, not latency measurements.
The complete acceptance replay and comparative performance study are recorded
separately in the repository's evidence files.
