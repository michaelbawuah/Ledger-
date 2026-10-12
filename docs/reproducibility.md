# Reproducing and auditing Ledger results

Ledger separates engine correctness, benchmark arithmetic, and measured host
performance. Re-running one of these checks does not establish the others.
The recorded [measurement report](results.md) describes the original machine,
compiler, workload, warmup and timing boundaries.

## Audit the committed evidence

The two audit scripts require Python 3.9 or newer and only its standard library.
They read local files and make no network requests. From the repository root:

```sh
python3 tools/verify_results.py
python3 tools/check_docs.py
python3 -m unittest discover -s tests/tooling -p 'test_*.py'
```

On Windows, use `python` instead of `python3` if that is your interpreter name.
The scripts also work from another working directory when invoked by their
absolute paths; their default repository paths are relative to the scripts.

[verify_results.py](../tools/verify_results.py) reads the core
[summary](../bench/results/core/summary.json),
[individual runs](../bench/results/core/runs.csv), and
[exact latency frequencies](../bench/results/core/latency_histogram.csv).
It verifies all three engines and every numbered run, not a selected best run.
The committed evidence contains 15 engine/run combinations and 15,000,000
latency samples. Each individual run contains 1,000,000 samples.

The checks include:

- Exact JSON and CSV schemas; supported engine names; no duplicate JSON fields,
  runs or histogram bins; no missing or extra run combinations.
- Integer command/count/checksum fields without conversion through floating
  point; positive frequency counts; bounded benchmark inputs; finite positive
  reported throughput.
- Histogram count totals, nearest-rank p50/p99/p99.9 and sample maxima for each
  run, plus medians and maxima in the summary.
- Throughput calculated independently as `commands * 1000000000 / elapsed_ns`.
  Decimal arithmetic uses 80 significant digits. A relative tolerance of
  `1e-10`, with an absolute floor of `0.000001` commands/s, accommodates text
  serialization rounding rather than host-performance variation.
- Reported event/trade/rejection counts, event/trade checksums and final live
  counts agree across engines and repetitions. Final counts fit book capacity;
  generated workloads also check their recorded peak live count.

Nearest-rank percentiles use rank `ceil(sample_count * fraction)` with integer
arithmetic and select that sample from sorted frequency bins. The implementation
never expands the histogram into millions of Python objects. Medians mirror the
C++ reporter's sorted middle value. The benchmark CLI requires an odd run count
from 1 through 31.

The JSON printed by the audit recalculates bitmap/reference throughput ratio and
bitmap/linear throughput and tail-latency changes. Decimal values are serialized
as strings so their displayed precision is deterministic. With the committed
evidence, these reproduce approximately 2.1886x throughput, 2.0386% throughput
improvement, 9.9668% p99 reduction and 7.4813% p99.9 reduction.

This arithmetic audit establishes consistency of the recorded artifacts. It
does not reconstruct executions from the original commands or prove that a
checksum cannot collide. Independent engine comparison and structural tests
provide separate [correctness evidence](results.md#correctness-evidence).

## Run a new measurement

Build a portable release first and check engine tests:

```sh
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
build/release/ledger_bench --commands 1000000 --seed 20261011 --runs 5 --warmup 50000 --outdir bench/results/local
python3 tools/verify_results.py bench/results/local
```

For host-native measurements and Linux CPU affinity, follow the exact commands
in [results.md](results.md#reproduce-the-core-measurement). Select an allowed CPU
on the current host. A portable release and a native release have different
compiler flags, so keep their evidence in separate directories. Preserve all
three audit inputs together; editing a summary alone invalidates the packet.
Do not overwrite the committed core packet with a new host's measurements.

An imported binary feed is also supported:

```sh
build/release/ledger_generate --out orders.lob --commands 1000000 --seed 20261011
build/release/ledger_bench --input orders.lob --runs 5 --warmup 50000 --outdir bench/results/imported
python3 tools/verify_results.py bench/results/imported
```

For `--input`, the summary records the feed path and null seed/generator counters.
The audit requires these null values rather than inventing generation metadata.
The feed's commands still determine mix counts, execution checksums and measured
latency frequencies. Store the input feed if exact replay is required.

New host results can legitimately differ in speed, tails or maximum samples.
Record CPU, affinity, compiler flags and background-load limitations alongside
them. The audit compares a run packet with itself, not with the original
headline speed. It leaves timing samples and measurements unchanged.

## Keep documentation navigable

[check_docs.py](../tools/check_docs.py) validates local inline links, images,
reference links, encoded spaces and parent-relative paths. Resolved targets must
exist inside the repository; symlinks cannot escape that boundary. HTTP,
mail and other URL schemes are skipped without fetching their destinations.
Fragments identify sections for readers but are not checked against generated
heading anchors.

Default discovery skips build directories, CMake build directories, caches,
the Git metadata directory and dependency output. Fenced/indented code, inline
code and HTML comments are examples rather than active links. This is a focused
repository-link checker, not a complete Markdown renderer or HTML validator.

To inspect selected files with an explicit repository boundary:

```sh
python3 tools/check_docs.py --root . README.md docs/results.md docs/reproducibility.md
```

A missing file, root escape or unresolved explicit reference returns a nonzero
exit status with the source document and line number. The tooling test suite
uses small temporary fixtures to verify both accepted cases and intentional
tampering; it also audits the committed core packet and current documentation.
