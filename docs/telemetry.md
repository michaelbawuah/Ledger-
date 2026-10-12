# Execution traces and SPSC backpressure experiments

These telemetry outputs supply measured inputs for strategy accounting and queue
charts. They describe synthetic, bounded workloads. Strategy P&L is an execution
accounting example; SPSC latency includes handoff and scheduling. Neither result
measures exchange/network latency or demonstrates live trading profitability.

## Strategy trace

```sh
./build/release/ledger_market_maker --steps 10000 --seed 42 \
  --quote-quantity 5 --inventory-limit 20 --trace strategy-trace.csv
```

`--trace FILE` writes a CSV header, step 0, then one row immediately after each
fair-mark update. The initial mark is 10,000 ticks and initial inventory and cash
are zero. There are exactly `steps + 1` data rows. Existing stdout JSON and the
seeded execution path remain unchanged when tracing is enabled. An invalid or
unwritable trace destination is an error, and output is checked before the JSON
summary is reported.

| CSV column | Meaning |
| --- | --- |
| `step` | Initial state 0; later rows 1 through the requested step count |
| `mid_ticks` | Fair mark after that step's synthetic adverse-selection update |
| `inventory_units` | Signed maker inventory reconstructed from actual `Trade` reports |
| `cash_tick_units` | Maker cash reconstructed from execution price times filled quantity |
| `pnl_tick_units` | `cash_tick_units + inventory_units * mid_ticks` |
| `maker_filled_units` | Cumulative quantity filled at the strategy's passive quotes |
| `maker_trade_events` | Cumulative actual maker `Trade` reports |
| `max_abs_inventory_units` | Maximum absolute inventory observed after any maker fill |

No fill probability or synthetic mark adjustment is substituted for execution
reports. A maker bid fill increases inventory and decreases cash; a maker ask
fill does the reverse. The final trace row agrees with all matching fields in
stdout JSON. Remaining quotes are cancelled before stdout is printed, which
changes neither maker cash nor inventory. Final P&L marks remaining inventory and
does not liquidate it.

The default 10,000-step seed-42 run finishes at **-17,386 tick units**, cash
184,694, inventory -20, and mark 10,104. Tick currency, fees, exchange competitors,
network delay, historical calibration, and brokerage connectivity are absent.
The negative result remains visible rather than selecting a profitable seed.
For the full quote and adverse-selection rules, see [strategy mechanics](strategy.md).

## Queue capacity and service-work controls

```sh
./build/release/ledger_spsc_bench --commands 1000000 --queue-capacity 8 \
  --consumer-work 64 --latency-samples 0
./build/release/ledger_spsc_bench --commands 1000000 --queue-capacity 8 \
  --consumer-work 64 --latency-samples 10000
```

`--queue-capacity` selects a compiled fixed-capacity runner: **8, 64, 512, or
4,096 usable slots**. Unsupported capacities are rejected. The default remains
4,096. All runners transfer the same deterministic five-command lifecycle:
rest a buy, partially fill it, cancel its residual, rest a sell, and fully fill
it. Commands are never dropped on a full queue. The producer retries until the
command is published; the consumer retries until it receives the next command.
After every 256 failed attempts the relevant thread yields.

`--consumer-work 0..4096` adds that many unsigned-64-bit XOR, multiplication, and
rotate rounds after each command's processing and event draining. Every round
depends on the preceding round and the command sequence, and the final state is
printed as `synthetic_work_checksum`. This observable dependency prevents the
compiler from discarding the arithmetic. It is bounded artificial service work,
not a modeled trading strategy, I/O stall, or calibrated cycle count. The default
is zero. The synthetic-work definition is recorded in `synthetic_work_method`.

| JSON field | Interpretation |
| --- | --- |
| `queue_capacity` | Selected usable queue capacity |
| `consumer_work` | Synthetic arithmetic rounds performed per command |
| `produced_commands`, `consumed_commands` | Actual successful transfers by each thread |
| `producer_full_retries` | Failed producer `try_push` attempts across the entire run |
| `consumer_empty_retries` | Failed consumer `try_pop` attempts across the entire run |
| `synthetic_work_checksum` | Final dependent arithmetic state, separate from event checksum |
| `producer_cpu`, `consumer_cpu` | Requested CPU index, or `null` when no pin was requested |
| `producer_pin_success`, `consumer_pin_success` | Whether affinity was successfully set and verified |
| `cpu_pinned` | True only if both thread affinity requests were verified |

Retry counts belong to separate threads and are read only after both threads
join. They are counts of failed attempts, not queue occupancy, nanoseconds
waiting, or numbers of commands delayed. Different retry policies and scheduler
behavior can change them. Counting retries adds an increment to the retry loop,
so comparisons should use this benchmark version consistently.

Completed transfer counts, sequence order, rejected results, queue emptiness,
trade count, event count, and an independently derived event checksum are checked
before output is emitted. A full five-command lifecycle produces two trades and
11 events; incomplete final lifecycles are included in exact expectations. The
existing `checksum` remains the sum of report ID plus quantity. It is a workload
integrity check rather than a collision-resistant digest; `synthetic_work_checksum`
is additional telemetry and never replaces it.

### CPU affinity

```sh
./build/release/ledger_spsc_bench --commands 1000000 --queue-capacity 64 \
  --consumer-work 0 --latency-samples 0 --producer-cpu 2 --consumer-cpu 3
```

On Linux, each thread calls `pthread_setaffinity_np` and verifies that its actual
allowed CPU set contains only the requested index. Affinity initialization and
thread creation finish before the start barrier and measured interval. A failed
request is reported as `false`, without claiming a pinned result or failing an
otherwise valid workload. Other platforms record the request and report
unsupported pinning as unsuccessful. Container CPU restrictions can reject a
CPU present on the host. Choose allowed CPUs explicitly, and use distinct CPUs
when testing two simultaneously running workers. Without a request, the success
fields and `cpu_pinned` are false.

### Measurement interpretation

`commands_per_second` covers command generation, queue handoff, engine processing,
event draining, retries, synthetic service work, scheduling, and final joins.
It excludes construction, storage allocation, and affinity setup. It is a complete
synthetic two-thread pipeline measurement, not core engine throughput.

Sample timing starts immediately before the first enqueue attempt and ends when
engine processing returns. It includes any producer full-queue waiting, queued
commands, consumer scheduling, and engine processing. It excludes the current
command's subsequent event draining and service work, while work on earlier
commands increases its queue delay. Each timestamp is published through the
same queue release/acquire ordering as its command.

Clock reads perturb measurements. Run `--latency-samples 0` separately for
throughput, and a nonzero sample run for latency. Requested samples are a maximum:
integer stride `ceil(commands / requested_samples)` produces
`ceil(commands / stride)` actual samples, recorded in `latency_samples`.
Percentiles use nearest rank on the measured samples. Retry counters are measured
in both modes; they should be compared within a consistent sampling mode.

A useful matrix runs all four capacities with work 0 and 64, three repetitions
per combination, separately for zero samples and 10,000 requested samples. This
is 48 captures at 1,000,000 commands each. Keep every JSON result, record the
benchmark revision and host/CPU metadata, and label files with run number, capacity,
work, and sampling mode. Min/max across three runs describes observed variation;
it is not a confidence interval. Run this matrix serially so competing benchmark
processes do not distort results.

## Reproducible integration checks

After building both CLI targets:

```sh
LEDGER_BINARY_DIR="$PWD/build/release" python3 -m unittest discover \
  -s tests/tooling -p test_telemetry.py -v
# Alternatively:
python3 tests/tooling/test_telemetry.py --binary-dir build/release -v
```

These tests check every row of the 10,000-step default strategy run, independent
integer P&L and cash reconstruction from passive execution prices, monotonic
cumulative fill/event counts, inventory caps, final JSON equality, the negative
seed-42 result, and two tight-cap scenarios plus a maximum-uint64 seed. They also
check every SPSC capacity in sampled and zero-sample modes, work 0 and 64,
hand-derived lifecycle event/checksum expectations, an independent arithmetic
checksum, partial lifecycle counts, Linux affinity success and failure, and strict
invalid/duplicate options.

Set `LEDGER_BINARY_DIR` for CI so these integration tests execute against the
built binaries. With no binaries present, this class is explicitly skipped during
pure offline tooling discovery. Normal integration success is separate from
ThreadSanitizer evidence. The capacity/work matrix should also run under a
ThreadSanitizer build when the environment supports it; sanitizer throughput and
latencies must not be used as performance results.
