"""Execution-accounting and queue telemetry integration tests on built CLIs.

LEDGER_BINARY_DIR selects the build directory. Direct execution also accepts
--binary-dir DIR; otherwise the existing build/release binaries are used. Missing
binaries skip this integration class during pure offline tooling discovery.
"""
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BINARY_DIR = Path(os.environ.get("LEDGER_BINARY_DIR", ROOT / "build" / "release"))
TRACE_COLUMNS = ["step", "mid_ticks", "inventory_units", "cash_tick_units", "pnl_tick_units",
                 "maker_filled_units", "maker_trade_events", "max_abs_inventory_units"]
MASK = (1 << 64) - 1


def executable(name):
    return BINARY_DIR / (name + (".exe" if os.name == "nt" else ""))


def expected_lifecycle(commands):
    """Five hand-derived reports groups, summed without reading engine output."""
    cycles, residual = divmod(commands, 5)
    trades = 2 * cycles + (residual >= 2)
    events = 11 * cycles + [0, 1, 5, 6, 7][residual]
    base = 4 * cycles
    phase_checksums = [base + 3, 4 * base + 11, base + 2, base + 5, 4 * base + 23]
    checksum = 22 * cycles * (cycles + 1) + sum(phase_checksums[:residual])
    return trades, events, checksum & MASK


def expected_work(commands, rounds):
    state = 0
    for sequence in range(1, commands + 1):
        for round_index in range(rounds):
            state ^= (sequence + round_index + 0x9e3779b97f4a7c15) & MASK
            state = (state * 0xbf58476d1ce4e5b9) & MASK
            state = ((state << 17) | (state >> (64 - 17))) & MASK
    return state


class BuiltTelemetryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for name in ("ledger_market_maker", "ledger_spsc_bench"):
            if not executable(name).is_file():
                raise unittest.SkipTest(f"build {name} and set LEDGER_BINARY_DIR to run telemetry tests")

    def invoke(self, name, *arguments, success=True):
        result = subprocess.run([str(executable(name)), *map(str, arguments)],
                                capture_output=True, text=True, timeout=40)
        self.assertEqual(result.returncode, 0 if success else 2, result.stderr)
        return json.loads(result.stdout) if success else result

    def check_trace(self, trace, stats):
        with trace.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            self.assertEqual(reader.fieldnames, TRACE_COLUMNS)
            rows = [{key: int(value) for key, value in row.items()} for row in reader]
        self.assertEqual(len(rows), stats["steps"] + 1)
        self.assertEqual(rows[0], dict(zip(TRACE_COLUMNS, (0, 10000, 0, 0, 0, 0, 0, 0))))
        observed_max = 0
        buy_units = sell_units = 0
        for step, row in enumerate(rows):
            with self.subTest(step=step):
                self.assertEqual(row["step"], step)
                # This independent integer identity must hold at every mark.
                self.assertEqual(row["pnl_tick_units"], row["cash_tick_units"]
                                 + row["inventory_units"] * row["mid_ticks"])
                self.assertLessEqual(abs(row["inventory_units"]), stats["inventory_limit_units"])
                observed_max = max(observed_max, abs(row["inventory_units"]))
                self.assertEqual(row["max_abs_inventory_units"], observed_max)
                if step:
                    previous = rows[step - 1]
                    delta = row["inventory_units"] - previous["inventory_units"]
                    fill_delta = row["maker_filled_units"] - previous["maker_filled_units"]
                    event_delta = row["maker_trade_events"] - previous["maker_trade_events"]
                    # One aggressor side per step, so the maker fill has one
                    # sign. Execution uses the old mark's passive bid or ask.
                    self.assertEqual(fill_delta, abs(delta))
                    self.assertEqual(event_delta, int(delta != 0))
                    self.assertLessEqual(fill_delta, stats["quote_quantity"])
                    execution_price = previous["mid_ticks"] - (1 if delta > 0 else -1)
                    self.assertEqual(row["cash_tick_units"] - previous["cash_tick_units"],
                                     -delta * execution_price)
                    if delta > 0:
                        buy_units += delta
                    else:
                        sell_units -= delta
                    self.assertLessEqual(abs(row["mid_ticks"] - previous["mid_ticks"]), 3)
        self.assertEqual(buy_units, stats["maker_buy_units"])
        self.assertEqual(sell_units, stats["maker_sell_units"])
        final = rows[-1]
        for column, stat in (("mid_ticks", "final_mid_ticks"), ("inventory_units", "inventory_units"),
                             ("cash_tick_units", "cash_tick_units"),
                             ("pnl_tick_units", "mark_to_market_pnl_tick_units"),
                             ("maker_filled_units", "maker_filled_units"),
                             ("maker_trade_events", "maker_trade_events"),
                             ("max_abs_inventory_units", "max_abs_inventory_units")):
            self.assertEqual(final[column], stats[stat])

    def test_default_trace_all_rows_and_golden_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "trace.csv"
            stats = self.invoke("ledger_market_maker", "--trace", trace)
            untraced = self.invoke("ledger_market_maker")
            self.assertEqual(stats, untraced)
            self.check_trace(trace, stats)
            self.assertEqual(stats["mark_to_market_pnl_tick_units"], -17386)
            golden = dict(inventory_units=-20, cash_tick_units=184694, final_mid_ticks=10104,
                          maker_buy_units=18025, maker_sell_units=18045, maker_trade_events=9294,
                          maker_filled_units=36070, maker_quote_units_submitted=90122,
                          flow_orders_with_maker_fill=9294, flow_orders=10000, flow_units=54770)
            for key, value in golden.items():
                self.assertEqual(stats[key], value)

    def test_trace_tight_inventory_limit_and_varied_seed(self):
        with tempfile.TemporaryDirectory() as directory:
            for seed, quantity, cap in ((0, 1, 1), (999, 10, 7), ((1 << 64) - 1, 5, 20)):
                with self.subTest(seed=seed, quantity=quantity, cap=cap):
                    trace = Path(directory) / f"{seed}.csv"
                    stats = self.invoke("ledger_market_maker", "--steps", 250, "--seed", seed,
                                        "--quote-quantity", quantity, "--inventory-limit", cap,
                                        "--trace", trace)
                    self.check_trace(trace, stats)

    def test_strategy_trace_invalid_paths_and_options(self):
        with tempfile.TemporaryDirectory() as directory:
            for args in (("--trace", ""), ("--trace",), ("--trace", directory),
                         ("--trace", "x", "--trace", "y")):
                with self.subTest(args=args):
                    self.invoke("ledger_market_maker", *args, success=False)

    def check_queue(self, stats, commands, capacity, work, samples):
        trades, events, checksum = expected_lifecycle(commands)
        self.assertEqual(stats["commands"], commands)
        self.assertEqual(stats["produced_commands"], commands)
        self.assertEqual(stats["consumed_commands"], commands)
        self.assertEqual(stats["queue_capacity"], capacity)
        self.assertEqual(stats["consumer_work"], work)
        self.assertEqual(stats["trades"], trades)
        self.assertEqual(stats["events"], events)
        self.assertEqual(stats["checksum"], checksum)
        self.assertEqual(stats["synthetic_work_checksum"], expected_work(commands, work))
        self.assertGreaterEqual(stats["producer_full_retries"], 0)
        self.assertGreaterEqual(stats["consumer_empty_retries"], 0)
        self.assertGreater(stats["elapsed_seconds"], 0)
        self.assertAlmostEqual(stats["commands_per_second"] * stats["elapsed_seconds"],
                               commands, delta=commands * 1e-8)
        if samples:
            stride = (commands + samples - 1) // samples
            self.assertEqual(stats["latency_samples"], (commands + stride - 1) // stride)
            values = stats["enqueue_attempt_to_core_process_ns"]
            self.assertLessEqual(values["min"], values["p50"])
            for first, second in (("p50", "p90"), ("p90", "p99"), ("p99", "p999"), ("p999", "max")):
                self.assertLessEqual(values[first], values[second])
        else:
            self.assertEqual(stats["latency_samples"], 0)
            self.assertNotIn("enqueue_attempt_to_core_process_ns", stats)

    def test_queue_capacities_and_service_work_preserve_exact_transfers(self):
        for capacity in (8, 64, 512, 4096):
            for work, samples in ((0, 0), (0, 31), (64, 31)):
                with self.subTest(capacity=capacity, work=work, samples=samples):
                    stats = self.invoke("ledger_spsc_bench", "--commands", 1001,
                                        "--queue-capacity", capacity, "--consumer-work", work,
                                        "--latency-samples", samples)
                    self.check_queue(stats, 1001, capacity, work, samples)
                    self.assertIsNone(stats["producer_cpu"])
                    self.assertIsNone(stats["consumer_cpu"])
                    self.assertFalse(stats["cpu_pinned"])
                    self.assertFalse(stats["producer_pin_success"])
                    self.assertFalse(stats["consumer_pin_success"])

    def test_queue_partial_lifecycles_and_uint64_work_checksum(self):
        for commands in range(1, 11):
            with self.subTest(commands=commands):
                stats = self.invoke("ledger_spsc_bench", "--commands", commands,
                                    "--queue-capacity", 8, "--consumer-work", 3,
                                    "--latency-samples", commands)
                self.check_queue(stats, commands, 8, 3, commands)

    def test_queue_reports_affinity_success_and_failure(self):
        if not hasattr(os, "sched_getaffinity"):
            self.skipTest("Linux CPU affinity verification")
        available = sorted(os.sched_getaffinity(0))
        stats = self.invoke("ledger_spsc_bench", "--commands", 501, "--latency-samples", 0,
                            "--producer-cpu", available[0], "--consumer-cpu", available[-1])
        self.assertEqual(stats["producer_cpu"], available[0])
        self.assertEqual(stats["consumer_cpu"], available[-1])
        self.assertTrue(stats["producer_pin_success"])
        self.assertTrue(stats["consumer_pin_success"])
        self.assertTrue(stats["cpu_pinned"])
        unavailable = self.invoke("ledger_spsc_bench", "--commands", 7, "--latency-samples", 0,
                                  "--producer-cpu", (1 << 31) - 1)
        self.assertFalse(unavailable["producer_pin_success"])
        self.assertFalse(unavailable["cpu_pinned"])
        self.assertEqual(unavailable["producer_cpu"], (1 << 31) - 1)

    def test_queue_rejects_invalid_or_duplicate_cli_options(self):
        for args in (("--queue-capacity", 0), ("--queue-capacity", 16), ("--queue-capacity", -8),
                     ("--queue-capacity",), ("--queue-capacity", 8, "--queue-capacity", 8),
                     ("--consumer-work", 4097), ("--consumer-work", -1),
                     ("--consumer-work", 0, "--consumer-work", 0), ("--producer-cpu", -1),
                     ("--producer-cpu", 1 << 31), ("--producer-cpu", 0, "--producer-cpu", 1),
                     ("--consumer-cpu", 0, "--consumer-cpu", 1),
                     ("--commands", 3, "--latency-samples", 4), ("--commands", 0),
                     ("--commands", 20000001), ("--unknown", 3)):
            with self.subTest(args=args):
                self.invoke("ledger_spsc_bench", *args, success=False)


if __name__ == "__main__":
    if "--binary-dir" in sys.argv:
        index = sys.argv.index("--binary-dir")
        BINARY_DIR = Path(sys.argv[index + 1]).resolve()
        del sys.argv[index:index + 2]
    unittest.main()
