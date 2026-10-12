# Recorded results

This report uses the current [core-v2 packet](../bench/results/core-v2/summary.json).
Its bitmap engine reaches **18.74M commands/s**, **271 ns median run p99**, and
**2.27x reference throughput** on the same synthetic stream. The broader
[performance study](performance-study.md) covers 51 configurations, 612 engine
runs, 28.44 million diagnostic latency samples, and controlled FOK experiments.

These results were measured during this build on a shared Linux virtual host.
They describe a synthetic matching workload, not live trading performance.

## Core replay

- Host: AMD EPYC 9V74 80-Core Processor, x86_64, Linux 6.18.44; 9 logical CPUs
  visible, approximately 10 GB visible memory. CPU affinity: 0. The CPU frequency
  governor is not exposed by the host. Shared-host scheduling is uncontrolled.
- Compiler: GCC 13.3.0, C++23, `-O3 -DNDEBUG -march=native`.
- Seed: 20261011. Commands: 1,000,000. Five repetitions per engine, 50,000-command
  warmup on a separate book before each pass; measured passes begin with a fresh
  empty book. Construction/storage allocation is excluded.
- Domain: ticks 1..20,000, pool capacity 8,192. Peak live orders: 2,099;
  final live orders: 2,045. Mix: 599,490 limit, 299,747 cancel, 100,763 market.
  No replacements in the performance stream; correctness streams include them.
- Rejections: zero. Trades: 397,155. Events: 2,191,466. All variants/passes produce
  identical event/trade checksums and final observable state.
- Latency: `steady_clock` before/after `process()`, full per-command recording.
  Median empty timer pair is 20 ns; it is included rather than subtracted.
- Throughput: separate batch pass without per-command timestamps, including
  report draining/checksumming and dispatch. It is not inferred from a percentile.
- Quantiles are nearest rank in each run. Reported throughput/quantiles are
  medians across the five runs, not the best run or pooled quantiles.

| Engine | Median commands/s | p50 ns | p99 ns | p99.9 ns | Largest sample, all runs |
|---|---:|---:|---:|---:|---:|
| Reference | 8,250,569 | 100 | 481 | 681 | 1.627 ms |
| Fast cached-linear | 18,671,948 | 50 | 301 | 410 | 0.272 ms |
| Fast bitmap | 18,737,939 | 50 | 271 | 371 | 0.242 ms |

The fast bitmap implementation is 2.27x the reference batch throughput on this
stream. Compared with the same engine using cached-linear search, bitmap search
has a 0.35% higher median throughput, 9.97% lower p99 and 9.51% lower p99.9.
The observed throughput ranges overlap: bitmap 18.12–19.27M commands/s and
linear 18.52–18.85M commands/s. These five repetitions do not establish a
reliable throughput advantage between the fast modes. The tail differences
describe this workload, not a universal latency bound. The core replay uses
the fixed engine order reference, bitmap, linear; the broader study rotates
engine order and varies seeds, depth, domain and traffic profile.

![Current core latency distributions](figures/study/01-core-distribution.svg)

Large outliers despite core affinity show why percentiles do not imply bounded
worst-case latency. This is a virtual host; frequency, neighboring workloads,
cache state and scheduler effects are not fully controlled. References measured
on other machines and streams are not ranked against Ledger.

Raw machine/compiler/method data:
[summary.json](../bench/results/core-v2/summary.json),
[runs.csv](../bench/results/core-v2/runs.csv),
[latency_histogram.csv](../bench/results/core-v2/latency_histogram.csv).
The latter losslessly compresses every measured nanosecond value into its
frequency per engine/run. Each run's counts sum to one million.

The [original core packet](../bench/results/core/summary.json) remains an
unchanged historical capture. Its earlier headline measurements are not used
for the current report or resume wording.

## Reproduce the core measurement

```sh
cmake --preset native && cmake --build --preset native --parallel
taskset -c 0 build/native/ledger_bench --commands 1000000 --seed 20261011 --runs 5 --warmup 50000 --outdir bench/results/local
python3 tools/verify_results.py bench/results/local
python3 tools/plot_latency.py bench/results/local/latency_histogram.csv --out bench/results/local/latency.svg
```

`taskset` is a Linux affinity helper; select an allowed CPU on your machine.
On macOS/Windows omit it and record the resulting affinity limitations. Plotting
requires Python and matplotlib. Host-native executables must be rebuilt on a
different CPU.

## Profile and optimization evidence

The retained GNU gprof capture is historical profiling evidence from the
initial implementation. Linux `perf` was not installed in that environment.
The driver disables sampling during generation and setup, then profiles 20M
fast-engine commands. Profiling instrumentation is excluded from headline
measurements. Its
[flat/call profile](../bench/results/gprof-hot-path.txt) attributes sampled
self time to `process` (50.79%), `unlink` (23.81%), and `add_resting` (17.46%);
driver/report consumption accounts for the remainder. These identify command
dispatch/matching/report writes, FIFO/index removal, and FIFO/index insertion
as the three main sampled regions. Sampling is coarse (10 ms) and inlining
aggregates sub-operations, so the profile does not isolate allocator-free
hash probes from link maintenance.

The current core replay compares occupancy bitmap search with cached-linear
search; both start at the emptied best level. The newer
[FOK optimization](fok-optimization.md) skips empty price ranges during liquidity
preflight. A [controlled ablation](performance-study.md#controlled-fok-preflight-comparison)
compares bitmap FOK preflight with its retained legacy path in the same binary,
using identical streams and matching exact reports and final snapshots.
The core replay has no FOK traffic, so it does not measure that optimization.

```sh
g++ -std=c++23 -O3 -pg -g -Iinclude tools/profile_hot_path.cpp src/book.cpp src/reference_book.cpp -o build/profile-hot-path
build/profile-hot-path
gprof build/profile-hot-path gmon.out > build/profile.txt
```

This optional driver depends on GNU `moncontrol`; it is not built on portable
targets. A later `perf` run on dedicated hardware can refine these findings.

## Correctness evidence

| Check | Observed result | Evidence |
|---|---|---|
| Differential acceptance after FOK optimization | 100 seeds x 1M; 100M commands; four modes agree | [JSON](../bench/results/differential-v2.json) |
| Exact report/trade equality | 209,631,029 reports; 34,195,803 trades | Same acceptance JSON |
| Full structure/FIFO snapshots | 24,500 checkpoints; per-command observable invariants | Same acceptance JSON |
| Named semantics | 74/74, both engines | [Sanitizer log](../bench/results/engine-semantics-asan.txt) |
| Fast C++ allocation requests | 1,000,140 calls; zero allocations | [Log](../bench/results/allocation-tests.txt) |
| GCC native CTest | 15/15 targets pass | [Current verification](../bench/results/study-verification.json) |
| Clang 18 ASan/UBSan CTest | 15/15 targets pass | Same verification JSON |
| Additional ASan/UBSan differential | 1M commands, ten seeds; four modes agree | [JSON](../bench/results/differential-asan-v2.json) |
| Parser fuzz, unseeded | 1,916,251 executions, 31 s; no finding | [Log](../bench/results/fuzz.log) |
| Parser fuzz, seeded | 5,822,136 executions, 31 s; no finding | [Log](../bench/results/fuzz-seeded.log) |
| SPSC ThreadSanitizer | 1M exact transfers; no race finding | [Log](../bench/results/spsc-tsan.txt) |
| Study arithmetic and plot provenance | 51 studies, 612 runs, 16 measured figures and 1,280 numeric series verified | [Audit](../bench/results/study-audit.json) |
| Remote CI | All nine runnable jobs passed | [GitHub Actions run](https://github.com/michaelbawuah/Ledger/actions/runs/38194384600) |

The passing remote run covers GCC and Clang release/ASan configurations,
macOS, Windows, parser fuzzing, ThreadSanitizer, and reproduction/auditing of
the study figures. Its manual-only long differential job was skipped on the
push event; the full 100M-command local acceptance run is recorded above.

Address/undefined instrumentation was enabled for sanitizer and fuzz runs.
Local LeakSanitizer was disabled (`ASAN_OPTIONS=detect_leaks=0`, libFuzzer
`-detect_leaks=0`) because sandbox restrictions prevent its `/proc` task
inspection. Leak freedom is not claimed. CI leaves leak detection enabled on
normal GitHub runners. A passing fuzz campaign is bounded testing, not a proof
of parser correctness.

The full-acceptance differential uses an adversarial, bounded 128-order book
with dense/sparse phases, replacements, invalid fields, duplicate IDs, FOK/IOC
and integer extremes; 52.6% of those inputs are rejected intentionally.
Performance uses an independently generated deeper live-ID-aware stream with
zero rejects. These populations are not conflated.

## Queue and strategy

The current [SPSC study](performance-study.md#queue-capacity-changes-the-latency-boundary)
contains 48 captures: four queue capacities, two synthetic consumer-work
settings, three repetitions, and separate zero-sample throughput / timestamped
latency passes. Each transfers one million commands in a five-command small-book
lifecycle. Producer and consumer affinities are verified on CPUs 1 and 2.
The [raw captures](../bench/results/spsc-study/capture-metadata.json) record exact
transfer/event checksums, retry counts and timing boundaries. Handoff latency
includes producer retries, queue residence, scheduling and core processing;
it is distinct from matching-core latency.

The current [market maker run](../bench/results/strategy-study/summary.json), 10,000 steps/seed 42,
reports inventory -20, cash 184,694 tick-units and final mark 10,104 ticks:
`184694 + (-20 * 10104) = -17386` tick-units total mark-to-market P&L.
Maximum absolute inventory is 20. Quote-unit fill rate is 40.02%; flow-order
maker-fill rate is 92.94%, with different denominators. It illustrates exact
execution accounting and a position cap, with no live-trading or profitability
claim. Its [full trace](../bench/results/strategy-study/trace.csv) independently
checks every cash/inventory/P&L mark. Full assumptions are in
[strategy.md](strategy.md) and [telemetry.md](telemetry.md).
