# Initial recorded results

This packet preserves the original replay. The current core rebaseline and
51-configuration analysis are in [performance-study.md](performance-study.md).

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
| Reference | 8,792,170 | 100 | 471 | 651 | 4.426 ms |
| Fast cached-linear | 18,858,250 | 40 | 301 | 401 | 1.887 ms |
| Fast bitmap | 19,242,690 | 40 | 271 | 371 | 0.986 ms |

The fast bitmap implementation is 2.19x the reference batch throughput on this
stream. Compared with the same engine using cached-linear search, bitmap search
improves median throughput by 2.04%, p99 by 9.97% and p99.9 by 7.48%. The
throughput change is small, includes noisy repetitions, and is not a statistical
confidence claim. The latency tail improvement is workload-specific. All modes
were run in the recorded order reference, bitmap, linear; alternate ordering
and isolated hardware would strengthen a future performance study.

Large outliers despite core affinity show why percentiles do not imply bounded
worst-case latency. This is a virtual host; frequency, neighboring workloads,
cache state and scheduler effects are not fully controlled. References measured
on other machines and streams are not ranked against Ledger.

Raw machine/compiler/method data:
[summary.json](../bench/results/core/summary.json),
[runs.csv](../bench/results/core/runs.csv),
[latency_histogram.csv](../bench/results/core/latency_histogram.csv).
The latter losslessly compresses every measured nanosecond value into its
frequency per engine/run. Each run's counts sum to one million.

## Reproduce the core measurement

```sh
cmake --preset native && cmake --build --preset native --parallel
taskset -c 0 build/native/ledger_bench --commands 1000000 --seed 20261011 --runs 5 --warmup 50000 --outdir bench/results/local
python3 tools/plot_latency.py bench/results/local/latency_histogram.csv --out bench/results/local/latency.svg
```

`taskset` is a Linux affinity helper; select an allowed CPU on your machine.
On macOS/Windows omit it and record the resulting affinity limitations. Plotting
requires Python and matplotlib. Host-native executables must be rebuilt on a
different CPU.

## Profile and optimization evidence

Linux `perf` was not installed in this environment. A GNU gprof driver disables
sampling during generation and setup, then profiles 20M fast-engine commands.
Profiling instrumentation is excluded from headline measurements. Its
[flat/call profile](../bench/results/gprof-hot-path.txt) attributes sampled
self time to `process` (50.79%), `unlink` (23.81%), and `add_resting` (17.46%);
driver/report consumption accounts for the remainder. These identify command
dispatch/matching/report writes, FIFO/index removal, and FIFO/index insertion
as the three main sampled regions. Sampling is coarse (10 ms) and inlining
aggregates sub-operations, so the profile does not isolate allocator-free
hash probes from link maintenance.

The measured optimization is occupancy bitmap search versus cached-linear
search; both start at the emptied best level. The difference is reproduced
within the same binary and stream, rather than using an artificially slow
uncached baseline. No additional optimization speedups are claimed.

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
| Differential acceptance | 100 seeds x 1M; 100M commands; no mismatch | [JSON](../bench/results/differential-100m.json) |
| Exact report/trade equality | 209,631,029 reports; 34,195,803 trades | Same acceptance JSON |
| Full structure/FIFO snapshots | 24,500 checkpoints; per-command observable invariants | Same acceptance JSON |
| Named semantics | 74/74, both engines | [Sanitizer log](../bench/results/engine-semantics-asan.txt) |
| Fast C++ allocation requests | 1,000,140 calls; zero allocations | [Log](../bench/results/allocation-tests.txt) |
| GCC release CTest | 5/5 targets pass | [JUnit](../bench/results/ctest-gcc-release.xml) |
| Clang 18 ASan/UBSan CTest | 5/5 targets pass | [JUnit](../bench/results/ctest-clang-asan.xml) |
| Additional ASan/UBSan differential | 1M commands, four seeds, no mismatch | [JSON](../bench/results/differential-asan-1m.json) |
| Parser fuzz, unseeded | 1,916,251 executions, 31 s; no finding | [Log](../bench/results/fuzz.log) |
| Parser fuzz, seeded | 5,822,136 executions, 31 s; no finding | [Log](../bench/results/fuzz-seeded.log) |
| SPSC ThreadSanitizer | 1M exact transfers; no race finding | [Log](../bench/results/spsc-tsan.txt) |

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

The separate [SPSC run](../bench/results/spsc.json) transfers one million
commands in a repeating five-command small-book lifecycle. It records 7.41M
commands/s and 1.083 ms p99 enqueue-attempt-to-core latency, with 10,000 samples.
Threads are unpinned; producer backpressure, queue waiting and scheduler effects
are included. This is one demonstration run, not the five-run core measurement,
and is not an apples-to-apples throughput comparison.

The [market maker run](../bench/results/market-maker.json), 10,000 steps/seed 42,
reports inventory -20, cash 184,694 tick-units and final mark 10,104 ticks:
`184694 + (-20 * 10104) = -17386` tick-units total mark-to-market P&L.
Maximum absolute inventory is 20. Quote-unit fill rate is 40.02%; flow-order
maker-fill rate is 92.94%, with different denominators. It illustrates exact
execution accounting and a position cap, with no live-trading or profitability
claim. Full assumptions are in [strategy.md](strategy.md).

Remote GitHub CI, macOS/Windows execution and live data ingestion have not been
verified in this environment. Workflows are supplied for remote validation.
