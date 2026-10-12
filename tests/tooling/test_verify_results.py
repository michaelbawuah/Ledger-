"""Tamper and reconstruction tests for the offline benchmark verifier."""
import copy
import csv
import importlib.util
import json
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


class SchemaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def write_summary(self, value):
        path = self.root / "summary.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

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


if __name__ == "__main__":
    unittest.main()
