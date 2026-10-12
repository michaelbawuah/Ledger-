# Workload studies: measurement and independent audit

The study suite measures how Ledger behaves under different command mixes,
initial depths, and price domains. Every figure is generated from captured CSV
frequencies or telemetry. A diagram explains a mechanism; a measured plot shows
an observed result. Neither a visual design nor a favorable synthetic workload
establishes superiority to another project's engine on different hardware.

## Controlled comparisons

All selected engines replay exactly the same deterministic preload and command
stream. The seed changes the generated workload. `depth` means **initial orders
per side**, so preload contains `2 * depth` resting orders and no trades. The
inclusive price range is `1..domain`; capacity is `max(4 * depth, 1024)` orders.
The CLI bounds measured commands at 1,000,000, depth at 16,384 per side, and
price domain at 1,000,000 ticks. Domain must be at least twice initial depth.
Runs default to three and must be odd, between one and nine.

| Engine | Next-price lookup | FOK liquidity preflight | Purpose |
| --- | --- | --- | --- |
| `reference` | Ordered STL levels | Ordered STL levels | Independently implemented semantics |
| `fast-linear` | Linear tick search | Linear tick search | Full linear-search baseline |
| `fast-bitmap` | Hierarchical bitmap | Hierarchical bitmap | Optimized implementation |
| `fast-fok-linear` | Hierarchical bitmap | Linear tick search | Isolate the FOK preflight change |

The FOK comparison uses `fast-bitmap` and `fast-fok-linear` with matching profile,
seed, depth, domain, command count, and run number. Their common price lookup and
storage design hold those mechanisms constant. A `fast-linear` comparison also
changes normal matching search and cannot isolate the FOK mechanism by itself.
Profiles cover balanced traffic, cancellation, trade sweeps, replacement,
sparse FOK liquidity, and rejection. Their names describe generation rules;
read the resulting action/outcome frequencies to see the actual mix.

## Timing boundaries

Throughput uses a separate whole-batch pass without individual command clocks
or diagnostic observers. Its elapsed time includes processing, result/event
counting, and all-event checksumming. Preload is untimed. This is engine-batch
throughput, not network-to-exchange latency.

Latency uses two steady-clock reads around each `process()` call in a separate
pass. The observer updates independent order and price-level maps after that
interval. It reconstructs fill counts and levels losing their last resting
order from execution reports. Its accesses can affect caches before the next
command, so study latency is explicitly **diagnostic latency**. The exact
unconditional histogram and conditional frequencies come from this same pass;
this makes their marginal counts directly reconcilable. Original core
measurements remain a separate evidence packet with their own timing method.

Warmup uses a separate fresh book before each measured pass. Engine order
rotates between repetitions. Every pass starts from the same preload. Timer
pair overhead is recorded and never subtracted from measured samples. Machine,
compiler, clock, warmup, observer, and checksum scope are stored in the summary.
These measures reduce obvious ordering effects; they cannot eliminate host
scheduling, shared-machine interference, clock granularity, or all cache effects.

The C++ harness compares full final snapshots and statistics, and verifies every
reported event field through deterministic checksumming, against the reference
engine. Measured event/trade totals exclude preload; checksum scope and final
statistics include it. Snapshot capture for timeline and depth runs in another
untimed reference pass.

## Raw evidence

Keep the entire directory together when sharing or replotting a study.

| File | Contents | Audit relationship |
| --- | --- | --- |
| `summary.json` | Parameters, engine selection, method, host metadata, aggregate results | Parameters match every CSV; aggregate results match runs |
| `runs.csv` | Per-engine/run elapsed time, throughput, quantiles, outcomes, events, trades, checksum, final live orders | Exact complete selected engine/run set |
| `histogram.csv` | Exact latency value/count bins | Counts sum to measured commands; reconstruct quantiles and maximum |
| `diagnostics.csv` | Exact action/result/fills/emptied-level/latency count bins | Marginal equals histogram; weighted fills equal trade reports |
| `timeline.csv` | Reference snapshots by measured sequence | Ordered cadence; bounded quantity/live state; final totals agree |
| `depth.csv` | Buy/sell levels at initial, midpoint, final state | Per-level bounds, canonical ordering, snapshot totals match timeline where sequences coincide |

Timeline cadence is `max(1, commands / 100)` using integer division, including
sequence zero after preload and the final sequence. Depth uses sequence zero,
`floor(commands / 2)`, and the final sequence, with duplicate stages captured
once. An empty depth stage contributes no level rows. The audit checks the final
quantity identity:

`submitted = 2 * executed + cancelled + expired + final resting quantity`.

`executed` counts one quantity per matched trade; two submitted sides are removed
from resting/submitted accounting. Trade count is separate from traded quantity.
No exchange timestamps or per-command submission quantities appear in this
packet, so the audit does not claim to prove intermediate quantity conservation
or wall-clock sequencing.

## Audit and reproduce

The audit requires Python 3.9 or newer and the standard library only. It reads
local files without network access. It recursively discovers study directories:

```sh
python3 tools/verify_study.py --study-root bench/results/study
python3 tools/verify_study.py --study-root bench/results/study --plot-manifest docs/charts/manifest.json --out bench/results/study-audit.json
python3 -m unittest discover -s tests/tooling -p 'test_verify_study.py'
```

Use the actual chart manifest path produced by the plotting command when it
differs from the example. A new measurement can use an isolated output directory:

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
build/release/ledger_study --profile sparse_fok --commands 100000 --seed 20261011 --depth 256 --domain 20000 --runs 3 --outdir bench/results/local-study/sparse-fok
python3 tools/verify_study.py --study-root bench/results/local-study
```

The independent Python audit does not import the plotting script or its
percentile implementation. It selects nearest-rank p50, p99, and p99.9 directly
from cumulative integer frequency counts. For fraction `a/b`, rank is
`ceil(N * a / b)`; no samples are expanded into a large array. Throughput is
reconstructed using Decimal arithmetic as `commands * 1e9 / total_ns`. A relative
tolerance of `1e-10`, with an absolute floor of `1e-6`, accommodates numeric text
serialization only. It is not performance uncertainty.

The audit rejects duplicate/missing runs, CSV header drift, zero/negative
frequency counts, duplicate bins, nonfinite numeric values, boolean integer
fields, parameter mismatches, contradictory outcomes, and impossible diagnostic
accounting. A rejected command has no fills or emptied levels. A cancellation
has no fills and empties at most one level. Other emptied-level counts cannot
exceed fills plus one possible cancelled level for replacement. Every trade
produces three reports, while applied replacement adds a cancellation report;
optional expiry reports are bounded by applied non-cancel commands.

The audit JSON contains schema version, status, per-study observed totals,
SHA256 source hashes, and explicit limits of what it establishes. Unit-test
fixtures are deliberately synthetic and live only in test code or temporary
directories; they are not benchmark measurements.

## Figure provenance and interpretation

The chart manifest lists the raw source files and SHA256 hashes behind each
figure. Quantitative series record their filters, values, observation count,
and source files. The audit independently checks reconstructed histogram
quantiles/distributions, run medians and extrema, paired run ratios, frequency
fractions, exact CSV trace rows, and JSON telemetry values/aggregates. Core
histogram figures additionally require the complete existing core evidence
packet to pass its independent audit, even when only the frequency CSV is
plotted.

Error bars show observed minimum and maximum runs. They are **not confidence
intervals**. Each pooled series preserves explicit profile, engine, depth,
domain, seed/run filters. Conditional cells with fewer than 30 observations
must be omitted or marked sparse instead of being presented as stable tails.
Empty or omitted cells are not zero-latency measurements.

SHA256 identifies the source bytes; it does not prove how measurements were
collected. Aggregate snapshots cannot independently recover FIFO order IDs or
reexecute the original stream, and checksums can collide. Source-consistency
checks complement the C++ reference comparison, sanitizers, stress tests, and
fuzzing rather than replacing them. Queue plots show a synthetic producer/consumer
pipeline, and strategy traces show a simulation. Neither establishes live
trading performance, profitable execution, or an exchange-connected service.
