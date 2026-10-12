"""Portable integration: real study output passes the independent evidence audit.

Set LEDGER_BINARY_DIR to the directory containing built executables. Pure offline
tooling discovery skips this class when the study binary has not been built.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BINARY_DIR = Path(os.environ.get("LEDGER_BINARY_DIR", ROOT / "build" / "release"))
BINARY = BINARY_DIR / ("ledger_study.exe" if os.name == "nt" else "ledger_study")
PROFILES = ("balanced", "cancel_heavy", "trade_storm", "replace_heavy", "sparse_fok", "rejection_stress")
ENGINES = ("reference", "fast-linear", "fast-bitmap", "fast-fok-linear")


class BuiltStudyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BINARY.is_file():
            raise unittest.SkipTest("build ledger_study and set LEDGER_BINARY_DIR to run study CLI tests")

    def invoke(self, *arguments):
        return subprocess.run([str(BINARY), *map(str, arguments)], capture_output=True,
                              text=True, timeout=60)

    def audit(self, directory):
        result = subprocess.run([sys.executable, str(ROOT / "tools" / "verify_study.py"),
                                 "--study-root", str(directory)], capture_output=True,
                                text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_all_profiles_produce_auditable_four_engine_evidence(self):
        with tempfile.TemporaryDirectory(prefix="ledger study ") as directory:
            root = Path(directory)
            for profile in PROFILES:
                output = root / profile
                with self.subTest(profile=profile):
                    result = self.invoke("--profile", profile, "--commands", 256,
                                         "--depth", 16, "--domain", 2048, "--seed", 7,
                                         "--runs", 1, "--outdir", output)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
                    self.assertEqual(summary["engines"], list(ENGINES))
                    self.assertEqual(summary["commands"], 256)
            audit = self.audit(root)
            self.assertEqual(audit["status"], "verified")
            self.assertEqual(audit["totals"]["study_directories"], 6)
            self.assertEqual(audit["totals"]["engine_runs"], 24)
            self.assertEqual(audit["totals"]["latency_observations"], 6 * 4 * 256)

    def test_minimum_size_and_uint64_seed_with_selected_engines(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.invoke("--commands", 1, "--depth", 1, "--domain", 2,
                                 "--seed", (1 << 64) - 1, "--runs", 1,
                                 "--engines", "fast-bitmap,reference", "--outdir", directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = self.audit(directory)
            self.assertEqual(report["totals"]["engine_runs"], 2)
            self.assertEqual(report["totals"]["latency_observations"], 2)

    def test_invalid_options_fail_before_writing_evidence(self):
        invalid = (("--commands", "0"), ("--commands", "1000001"),
                   ("--commands", "-1"), ("--commands", "1x"),
                   ("--depth", "0"), ("--depth", "16385"),
                   ("--domain", "1"), ("--domain", "1000001"),
                   ("--runs", "0"), ("--runs", "2"), ("--runs", "11"),
                   ("--seed", "18446744073709551616"),
                   ("--profile", "unknown"), ("--unknown", "1"),
                   ("--engines", ""), ("--engines", "reference,reference"),
                   ("--engines", "reference,"), ("--engines", "unknown"),
                   ("--commands", "1", "--commands", "2"), ("--commands",))
        with tempfile.TemporaryDirectory() as directory:
            for index, arguments in enumerate(invalid):
                output = Path(directory) / str(index)
                with self.subTest(arguments=arguments):
                    result = self.invoke("--outdir", output, *arguments)
                    self.assertEqual(result.returncode, 1, result.stdout)
                    self.assertIn("ledger_study:", result.stderr)
                    self.assertFalse(output.exists())

    def test_output_path_failure_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "existing-file"
            output.write_text("keep", encoding="utf-8")
            result = self.invoke("--commands", 1, "--depth", 1, "--domain", 2,
                                 "--runs", 1, "--outdir", output)
            self.assertEqual(result.returncode, 1)
            self.assertIn("ledger_study:", result.stderr)
            self.assertEqual(output.read_text(encoding="utf-8"), "keep")


if __name__ == "__main__":
    unittest.main()
