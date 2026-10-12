"""Tamper and reconstruction tests for the offline benchmark verifier."""
import copy
import csv
import importlib.util
import json
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "verify_results.py"
SPEC = importlib.util.spec_from_file_location("verify_results", SCRIPT)
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)


def fixture_summary():
    metrics = dict(median_commands_per_second=1000000, median_p50_ns=10,
                   median_p99_ns=10, median_p999_ns=20, median_run_max_ns=30,
                   max_ns=30, rejected_commands=0, live_orders_final=2,
                   events=2000, trades=10, event_checksum=2**64 - 1, trade_checksum=42)
    return dict(schema_version=1, commands=1000, seed=1, input=None, runs=3,
                warmup_commands=10, config=dict(min_price=1, max_price=20000, max_orders=8192),
                mix=dict(limit=600, cancel=300, market=100, replace=0),
                generated_peak_live_orders=5, generated_empty_book_cancels=0,
                generated_capacity_ioc_limits=0,
                machine=dict(system="test", cpu_model="test", logical_cpus=1,
                             affinity_cpus="0", memory="test", governor_cpu0=""),
                compiler=dict(version="test", flags="test", optimization_enabled=True, ndebug=True),
                clock=dict(steady=True, period_numerator=1, period_denominator=1000000000,
                           median_empty_pair_ns=1), method="Synthetic test fixture",
                engines={engine: copy.deepcopy(metrics) for engine in verifier.ENGINES})


def fixture_rows():
    runs, histogram = [], []
    for engine in sorted(verifier.ENGINES):
        for run in range(1, 4):
            runs.append(dict(engine=engine, run=run, commands=1000, throughput_total_ns=1000000,
                             commands_per_second=1000000, p50_ns=10, p99_ns=10, p999_ns=20,
                             max_ns=30, rejected_commands=0, events=2000, trades=10,
                             event_checksum=2**64 - 1, trade_checksum=42))
            for latency, count in ((10, 990), (20, 9), (30, 1)):
                histogram.append(dict(engine=engine, run=run, latency_ns=latency, count=count))
    return runs, histogram


class FileFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def write_summary(self, value):
        path = self.root / "summary.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def write_csv(self, name, columns, rows):
        path = self.root / name
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=columns)
            writer.writeheader()
            writer.writerows(rows)
        return path


class SchemaTests(FileFixture):
    def test_accepts_schema_and_preserves_uint64_checksum(self):
        result = verifier.read_summary(self.write_summary(fixture_summary()))
        self.assertEqual(result["engines"]["reference"]["event_checksum"], 2**64 - 1)

    def test_rejects_missing_and_extra_summary_fields(self):
        for extra in (False, True):
            value = fixture_summary()
            if extra:
                value["surprise"] = 1
            else:
                del value["commands"]
            with self.assertRaises(verifier.VerificationError):
                verifier.read_summary(self.write_summary(value))

    def test_rejects_boolean_count_and_unknown_engine(self):
        for key, replacement in (("commands", True), ("engines", {"unknown": {}})):
            value = fixture_summary()
            value[key] = replacement
            with self.assertRaises(verifier.VerificationError):
                verifier.read_summary(self.write_summary(value))

    def test_rejects_duplicate_json_fields_and_nonfinite_constants(self):
        for text in ('{"commands":1,"commands":2}', '{"commands":NaN}'):
            path = self.root / "summary.json"
            path.write_text(text, encoding="utf-8")
            with self.assertRaises(verifier.VerificationError):
                verifier.read_summary(path)

    def test_rejects_csv_header_and_ragged_rows(self):
        for text in ("engine,run\nreference,1\n", "engine,run,latency_ns,count\nreference,1,10\n",
                     "engine,run,latency_ns,count\nreference,1,10,1,extra\n"):
            path = self.root / "hist.csv"
            path.write_text(text, encoding="utf-8")
            with self.assertRaises(verifier.VerificationError):
                verifier.read_csv(path, verifier.HISTOGRAM_COLUMNS)


class FrequencyTests(FileFixture):
    def evidence(self, run_rows=None, hist_rows=None):
        summary = fixture_summary()
        original_runs, original_hist = fixture_rows()
        runs = verifier.read_runs(self.write_csv("runs.csv", verifier.RUN_COLUMNS,
                                                original_runs if run_rows is None else run_rows), summary)
        return verifier.read_histograms(self.write_csv("hist.csv", verifier.HISTOGRAM_COLUMNS,
                                                       original_hist if hist_rows is None else hist_rows),
                                        summary, runs)

    def test_reconstructs_nearest_rank_without_expanding_samples(self):
        result = self.evidence()
        self.assertEqual(verifier.nearest_rank(result[("reference", 1)], 999, 1000), 20)
        self.assertEqual(verifier.nearest_rank({7: 10**80, 9: 1}, 99, 100), 7)

    def test_rejects_duplicate_and_missing_runs(self):
        runs, _ = fixture_rows()
        for tampered in (runs[:-1], runs + [runs[0]]):
            with self.assertRaises(verifier.VerificationError):
                self.evidence(run_rows=tampered)

    def test_rejects_duplicate_missing_and_zero_count_bins(self):
        _, histogram = fixture_rows()
        zero = copy.deepcopy(histogram)
        zero[0]["count"] = 0
        for tampered in (histogram + [histogram[0]], histogram[:-3], zero):
            with self.assertRaises(verifier.VerificationError):
                self.evidence(hist_rows=tampered)

    def test_rejects_count_percentile_and_maximum_tampering(self):
        for field, replacement in (("count", 991), ("latency_ns", 11)):
            _, histogram = fixture_rows()
            histogram[0][field] = replacement
            with self.assertRaises(verifier.VerificationError):
                self.evidence(hist_rows=histogram)
        runs, _ = fixture_rows()
        runs[0]["max_ns"] = 31
        with self.assertRaises(verifier.VerificationError):
            self.evidence(run_rows=runs)


class MetricTests(FileFixture):
    def metrics(self, summary=None, rows=None):
        summary = fixture_summary() if summary is None else summary
        original, _ = fixture_rows()
        runs = verifier.read_runs(self.write_csv("runs.csv", verifier.RUN_COLUMNS,
                                                original if rows is None else rows), summary)
        return verifier.verify_metrics(summary, runs)

    def test_recomputes_speedup_ratios(self):
        report = self.metrics()
        self.assertEqual(report["bitmap_reference_throughput_ratio"], 1)
        self.assertEqual(report["bitmap_linear_p99_reduction_percent"], 0)

    def test_upper_median_matches_cpp_for_even_run_count(self):
        self.assertEqual(verifier.upper_median([1, 4, 3, 2]), 3)

    def test_rejects_throughput_and_summary_median_tampering(self):
        rows, _ = fixture_rows()
        rows[0]["commands_per_second"] = 1000100
        with self.assertRaises(verifier.VerificationError):
            self.metrics(rows=rows)
        value = fixture_summary()
        value["engines"]["reference"]["median_p99_ns"] = 11
        with self.assertRaises(verifier.VerificationError):
            self.metrics(summary=value)

    def test_rejects_zero_nonfinite_and_exponent_overflow_metrics(self):
        for replacement in ("0", "NaN", "Infinity", "1e999999999"):
            rows, _ = fixture_rows()
            rows[0]["commands_per_second"] = replacement
            with self.assertRaises(verifier.VerificationError):
                self.metrics(rows=rows)

    def test_allows_only_decimal_serialization_rounding(self):
        rows, _ = fixture_rows()
        rows[0]["throughput_total_ns"] = 1000003
        rows[0]["commands_per_second"] = "999997.000009"
        self.metrics(rows=rows)


class AccountingTests(FileFixture):
    def accounting(self, summary=None, rows=None):
        summary = fixture_summary() if summary is None else summary
        verifier.read_summary(self.write_summary(summary))
        original, _ = fixture_rows()
        runs = verifier.read_runs(self.write_csv("runs.csv", verifier.RUN_COLUMNS,
                                                original if rows is None else rows), summary)
        verifier.verify_accounting(summary, runs)

    def test_accepts_matching_counts_and_uint64_checksums(self):
        self.accounting()

    def test_rejects_checksum_and_event_count_tampering(self):
        for field in ("event_checksum", "trade_checksum", "events", "trades", "rejected_commands"):
            rows, _ = fixture_rows()
            rows[0][field] -= 1 if rows[0][field] else -1
            with self.assertRaises(verifier.VerificationError):
                self.accounting(rows=rows)

    def test_rejects_cross_engine_final_state_and_capacity_drift(self):
        for tamper in ("state", "capacity"):
            summary = fixture_summary()
            if tamper == "state":
                summary["engines"]["fast-bitmap"]["live_orders_final"] = 3
            else:
                summary["generated_peak_live_orders"] = 8193
            with self.assertRaises(verifier.VerificationError):
                self.accounting(summary=summary)


    def test_rejects_metadata_types_and_integer_overflow(self):
        mutations = (("machine", "logical_cpus", True), ("clock", "steady", False),
                     ("compiler", "ndebug", 1), ("config", "max_orders", 2**64),
                     ("clock", "period_denominator", 0))
        for section, field, replacement in mutations:
            summary = fixture_summary()
            summary[section][field] = replacement
            with self.assertRaises(verifier.VerificationError):
                self.accounting(summary=summary)


class IntegrationTests(FileFixture):
    def write_evidence(self):
        self.write_summary(fixture_summary())
        runs, histogram = fixture_rows()
        self.write_csv("runs.csv", verifier.RUN_COLUMNS, runs)
        self.write_csv("latency_histogram.csv", verifier.HISTOGRAM_COLUMNS, histogram)

    def test_complete_fixture_and_deterministic_cli_json(self):
        self.write_evidence()
        report = verifier.verify_directory(self.root)
        self.assertEqual(report["verified_latency_samples"], 9000)
        command = [sys.executable, str(SCRIPT), str(self.root)]
        first = subprocess.run(command, text=True, capture_output=True, check=True)
        second = subprocess.run(command, text=True, capture_output=True, check=True)
        self.assertEqual(first.stdout, second.stdout)
        self.assertEqual(json.loads(first.stdout), report)

    def test_cli_reports_missing_evidence_without_traceback(self):
        process = subprocess.run([sys.executable, str(SCRIPT), str(self.root)],
                                 text=True, capture_output=True)
        self.assertEqual(process.returncode, 1)
        self.assertIn("verification failed", process.stderr)
        self.assertNotIn("Traceback", process.stderr)

    def test_committed_core_evidence_is_consistent(self):
        for packet in ("core-v2", "core"):
            with self.subTest(packet=packet):
                report = verifier.verify_directory(SCRIPT.parents[1] / "bench/results" / packet)
                self.assertEqual(report["verified_engine_runs"], 15)
                self.assertEqual(report["verified_latency_samples"], 15000000)


class ReplayMetadataTests(FileFixture):
    def test_input_replay_accepts_null_generator_metadata(self):
        summary = fixture_summary()
        summary["input"] = "recorded feed.lob"
        for key in ("seed", "generated_peak_live_orders", "generated_empty_book_cancels",
                    "generated_capacity_ioc_limits"):
            summary[key] = None
        loaded = verifier.read_summary(self.write_summary(summary))
        rows, _ = fixture_rows()
        runs = verifier.read_runs(self.write_csv("runs.csv", verifier.RUN_COLUMNS, rows), loaded)
        verifier.verify_accounting(loaded, runs)

    def test_rejects_mixed_replay_and_generator_metadata(self):
        for input_path in (None, "recorded.lob"):
            summary = fixture_summary()
            summary["input"] = input_path
            if input_path is None:
                summary["seed"] = None
            with self.assertRaises(verifier.VerificationError):
                verifier.read_summary(self.write_summary(summary))

    def test_bounds_run_sets_and_configuration_before_allocating_them(self):
        for key, replacement, message in (("runs", 2**64 - 1, "odd count"),
                                           ("runs", 2, "odd count"),
                                           ("commands", 5_000_001, "command limit")):
            summary = fixture_summary()
            summary[key] = replacement
            with self.assertRaisesRegex(verifier.VerificationError, message):
                verifier.read_summary(self.write_summary(summary))
        for field, replacement in (("max_orders", 1_000_001), ("max_price", 1_000_001)):
            summary = fixture_summary()
            summary["config"][field] = replacement
            with self.assertRaises(verifier.VerificationError):
                verifier.read_summary(self.write_summary(summary))

if __name__ == "__main__":
    unittest.main()
