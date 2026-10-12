"""Independent reconstruction and mutation tests; these are synthetic fixtures."""
import copy
import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "verify_study.py"
SPEC = importlib.util.spec_from_file_location("verify_study", SCRIPT)
v = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(v)


def summary(seed=1):
    results = {}
    for engine in ("reference", "fast-linear", "fast-bitmap", "fast-fok-linear"):
        results[engine] = dict(median_commands_per_second=1000000,
                              median_p50_ns=10, median_p99_ns=30, median_p999_ns=30,
                              max_ns=30, applied=8, rejected=2, events=18, trades=2,
                              checksum=2**64 - 1, live_orders_final=4,
                              final_stats=dict(submitted=10, executed=2, cancelled=1,
                                               expired=1, rejected=0, trade_count=2))
    return dict(schema_version=1, profile="balanced", commands=10, seed=seed,
                depth=2, domain=16, runs=3, preload_commands=4,
                config=dict(min_price=1, max_price=16, max_orders=1024),
                engines=list(results), results=results,
                machine=dict(os="fixture", cpu_model="fixture", hardware_threads=1,
                             affinity="", extra_optional_field="allowed"),
                compiler=dict(version="fixture", flags=""),
                clock=dict(name="steady", is_steady=True, period_num=1,
                           period_den=1000000000, timer_pair_median_ns=1),
                method=dict(throughput="Fixture elapsed pass", latency="Fixture diagnostic pass",
                            observer="Fixture observer outside timed process",
                            levels_emptied="Removed maker/cancel levels",
                            checksum_scope="All event fields", warmup_commands=0,
                            timer_overhead_subtracted=False,
                            engine_order="rotate selected order by run index",
                            timeline_sequence="measured command index; zero after preload"))


def rows(s):
    prefix = {k: s[k] for k in v.PREFIX}
    tables = {name: [] for name in ("runs.csv", "histogram.csv", "diagnostics.csv", "timeline.csv", "depth.csv")}
    cells = [("limit", "applied", 0, 0, 10, 4), ("cancel", "applied", 0, 0, 10, 2),
             ("limit", "applied", 1, 1, 20, 1), ("replace", "applied", 1, 1, 20, 1),
             ("market", "rejected", 0, 0, 30, 1), ("limit", "rejected", 0, 0, 30, 1)]
    for engine in s["engines"]:
        for run in range(1, s["runs"] + 1):
            shared = dict(prefix, engine=engine, run=run)
            tables["runs.csv"].append(dict(shared, commands=10, total_ns=10000,
                                            commands_per_second=1000000, p50_ns=10,
                                            p99_ns=30, p999_ns=30, max_ns=30,
                                            applied=8, rejected=2, events=18, trades=2,
                                            checksum=2**64 - 1, live_orders_final=4))
            for latency, count in ((10, 6), (20, 2), (30, 2)):
                tables["histogram.csv"].append(dict(shared, latency_ns=latency, count=count))
            for action, result, fills, levels, latency, count in cells:
                tables["diagnostics.csv"].append(dict(shared, action=action, result=result,
                                                    fills=fills, levels_emptied=levels,
                                                    latency_ns=latency, count=count))
    for sequence in range(11):
        tables["timeline.csv"].append(dict(prefix, sequence=sequence, live_orders=4,
                                          resting_quantity=4, best_bid=2, best_ask=15,
                                          trade_count=sequence // 5))
    for sequence in (0, 5, 10):
        for side, price in (("buy", 2), ("sell", 15)):
            tables["depth.csv"].append(dict(prefix, sequence=sequence, side=side,
                                           price=price, quantity=2, orders=2))
    return tables


COLUMNS = {"runs.csv": v.RUN_COLUMNS, "histogram.csv": v.HISTOGRAM_COLUMNS,
           "diagnostics.csv": v.DIAGNOSTICS_COLUMNS, "timeline.csv": v.TIMELINE_COLUMNS,
           "depth.csv": v.DEPTH_COLUMNS}


class FixtureMixin:
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.study = self.root / "balanced" / "depth2-domain16-seed1"
        self.s = summary()
        self.tables = rows(self.s)
        self.write()

    def write(self, directory=None):
        directory = self.study if directory is None else directory
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "summary.json").write_text(json.dumps(self.s), encoding="utf-8")
        for name, columns in COLUMNS.items():
            with (directory / name).open("w", encoding="utf-8", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=columns)
                writer.writeheader()
                writer.writerows(self.tables[name])

    def audit(self):
        return v.verify_directory(self.study)

    def reject(self):
        self.write()
        with self.assertRaises(v.VerificationError):
            self.audit()



class StudyFixture(FixtureMixin, unittest.TestCase):
    def test_valid_complete_fixture_and_extra_machine_field(self):
        report, tables = self.audit()
        self.assertEqual(report["engine_runs"], 12)
        self.assertEqual(report["latency_observations"], 120)
        self.assertEqual(report["diagnostic_observations"], 120)
        self.assertEqual(len(report["source_sha256"]), 6)
        self.assertEqual(len(tables["runs.csv"]), 12)

    def test_cumulative_quantile_does_not_expand_frequency_counts(self):
        self.assertEqual(v.percentile({2: 10**50, 3: 1}, 999, 1000), 2)
        self.assertEqual(v.percentile({10: 990, 20: 9, 30: 1}, 999, 1000), 20)
        self.assertEqual(v.median([1, 4, 3, 2]), 2.5)

    def test_recursively_verifies_multiple_seeds(self):
        self.s = summary(seed=2)
        self.tables = rows(self.s)
        self.write(self.root / "nested" / "seed2")
        reports, tables = v.verify_study_root(self.root)
        self.assertEqual({r["seed"] for r in reports}, {1, 2})
        self.assertEqual(len(tables), 10)

    def test_rejects_duplicate_study_identity(self):
        self.write(self.root / "duplicate")
        with self.assertRaises(v.VerificationError):
            v.verify_study_root(self.root)

    def test_rejects_empty_study_root(self):
        with tempfile.TemporaryDirectory() as empty:
            with self.assertRaises(v.VerificationError):
                v.verify_study_root(empty)

    def test_rejects_csv_header_drift(self):
        p = self.study / "histogram.csv"
        p.write_text(p.read_text().replace("latency_ns", "latency_us", 1))
        with self.assertRaises(v.VerificationError):
            self.audit()

    def test_rejects_csv_ragged_rows(self):
        for data in ("profile,depth,domain,seed,engine,run,latency_ns,count\nbalanced,2,16,1,reference,1,10\n",
                     "profile,depth,domain,seed,engine,run,latency_ns,count\nbalanced,2,16,1,reference,1,10,6,extra\n"):
            (self.study / "histogram.csv").write_text(data)
            with self.assertRaises(v.VerificationError):
                self.audit()

    def test_rejects_duplicate_json_keys_and_nonfinite_values(self):
        p = self.study / "summary.json"
        for data in ('{"commands":1,"commands":2}', '{"commands":NaN}'):
            p.write_text(data)
            with self.assertRaises(v.VerificationError):
                self.audit()

    def test_rejects_bounds_boolean_and_invalid_numeric_core(self):
        original = copy.deepcopy(self.s)
        for name, value in (("commands", 1000001), ("commands", True), ("runs", 2),
                            ("runs", 11), ("depth", 16385), ("domain", 3),
                            ("domain", 1000001), ("seed", -1), ("seed", 2**64)):
            self.s = copy.deepcopy(original)
            self.s[name] = value
            self.reject()

    def test_rejects_metadata_and_method_numeric_drift(self):
        original = copy.deepcopy(self.s)
        for section, field, value in (("clock", "is_steady", False), ("clock", "period_num", 0),
                                      ("machine", "hardware_threads", True),
                                      ("method", "warmup_commands", -1),
                                      ("method", "timer_overhead_subtracted", True)):
            self.s = copy.deepcopy(original)
            self.s[section][field] = value
            self.reject()

    def test_rejects_summary_configuration_mismatch(self):
        self.s["config"]["max_orders"] = 1025
        self.reject()

    def test_rejects_preload_identity_mismatch(self):
        self.s["preload_commands"] = 3
        self.reject()

    def test_rejects_duplicate_missing_or_unknown_engine_runs(self):
        original = copy.deepcopy(self.tables)
        for operation in ("duplicate", "missing", "unknown"):
            self.tables = copy.deepcopy(original)
            if operation == "duplicate":
                self.tables["runs.csv"].append(self.tables["runs.csv"][0])
            elif operation == "missing":
                self.tables["runs.csv"].pop()
            else:
                self.tables["runs.csv"][0]["engine"] = "fictional"
            self.reject()

    def test_rejects_run_configuration_or_seed_mismatch(self):
        for field, value in (("depth", 3), ("domain", 17), ("seed", 2), ("profile", "cancel_heavy")):
            original = self.tables["runs.csv"][0][field]
            self.tables["runs.csv"][0][field] = value
            self.reject()
            self.tables["runs.csv"][0][field] = original

    def test_rejects_throughput_and_allows_only_serialization_rounding(self):
        self.tables["runs.csv"][0]["commands_per_second"] = "999999.9999999"
        self.write()
        self.audit()
        for value in ("1000010", "0", "NaN", "Infinity", "1e99999999"):
            self.tables["runs.csv"][0]["commands_per_second"] = value
            self.reject()

    def test_rejects_elapsed_time_and_count_drift(self):
        for field, value in (("total_ns", 0), ("commands", 11), ("applied", 7), ("trades", 1000)):
            original = self.tables["runs.csv"][0][field]
            self.tables["runs.csv"][0][field] = value
            self.reject()
            self.tables["runs.csv"][0][field] = original

    def test_rejects_checksum_and_final_state_drift(self):
        for field in ("checksum", "live_orders_final", "events", "trades"):
            original = self.tables["runs.csv"][0][field]
            self.tables["runs.csv"][0][field] -= 1
            self.reject()
            self.tables["runs.csv"][0][field] = original

    def test_rejects_missing_and_duplicate_histogram_bins(self):
        original = copy.deepcopy(self.tables)
        self.tables["histogram.csv"].append(self.tables["histogram.csv"][0])
        self.reject()
        self.tables = copy.deepcopy(original)
        self.tables["histogram.csv"].pop()
        self.reject()

    def test_rejects_frequency_count_quantile_and_max_drift(self):
        original = copy.deepcopy(self.tables)
        for table, field, value in (("histogram.csv", "count", 7), ("histogram.csv", "count", 0),
                                    ("histogram.csv", "latency_ns", 11),
                                    ("runs.csv", "p99_ns", 29), ("runs.csv", "max_ns", 31)):
            self.tables = copy.deepcopy(original)
            self.tables[table][0][field] = value
            self.reject()

    def test_rejects_summary_result_medians_and_schema_drift(self):
        original = copy.deepcopy(self.s)
        for field, value in (("median_p50_ns", 11), ("median_p99_ns", True),
                            ("median_commands_per_second", 1000100), ("max_ns", 31)):
            self.s = copy.deepcopy(original)
            self.s["results"]["reference"][field] = value
            self.reject()

    def test_rejects_diagnostic_marginal_and_weighted_fill_drift(self):
        for field, value in (("latency_ns", 11), ("count", 5), ("fills", 1)):
            original = self.tables["diagnostics.csv"][0][field]
            self.tables["diagnostics.csv"][0][field] = value
            self.reject()
            self.tables["diagnostics.csv"][0][field] = original

    def test_rejects_diagnostic_duplicates_and_invalid_labels(self):
        original = copy.deepcopy(self.tables)
        self.tables["diagnostics.csv"].append(self.tables["diagnostics.csv"][0])
        self.reject()
        for field, value in (("action", "stop"), ("result", "skipped"), ("count", 0)):
            self.tables = copy.deepcopy(original)
            self.tables["diagnostics.csv"][0][field] = value
            self.reject()

    def test_rejects_impossible_emptied_levels_rejected_effects_and_cancel_fills(self):
        original = copy.deepcopy(self.tables)
        for index, field, value in ((0, "levels_emptied", 1), (0, "levels_emptied", -1),
                                    (1, "fills", 1), (4, "levels_emptied", 1),
                                    (4, "fills", 1), (2, "fills", 1025)):
            self.tables = copy.deepcopy(original)
            self.tables["diagnostics.csv"][index][field] = value
            self.reject()

    def test_rejects_impossible_event_accounting_even_with_cross_engine_agreement(self):
        for row in self.tables["runs.csv"]:
            row["events"] = 100
        for result in self.s["results"].values():
            result["events"] = 100
        self.reject()

    def test_rejects_timeline_order_cadence_trade_count_and_capacity_drift(self):
        original = copy.deepcopy(self.tables)
        for index, field, value in ((1, "sequence", 0), (2, "trade_count", 5),
                                    (1, "live_orders", 1025), (1, "resting_quantity", 3),
                                    (1, "best_bid", 15), (1, "best_ask", 17)):
            self.tables = copy.deepcopy(original)
            self.tables["timeline.csv"][index][field] = value
            self.reject()
        self.tables = copy.deepcopy(original)
        self.tables["timeline.csv"].pop(1)
        self.reject()

    def test_rejects_depth_duplicate_missing_levels_and_quantity_drift(self):
        original = copy.deepcopy(self.tables)
        self.tables["depth.csv"].append(self.tables["depth.csv"][0])
        self.reject()
        for field, value in (("quantity", 3), ("orders", 0), ("side", "bid"),
                             ("price", 0), ("price", 15), ("sequence", 4)):
            self.tables = copy.deepcopy(original)
            self.tables["depth.csv"][0][field] = value
            self.reject()
        self.tables = copy.deepcopy(original)
        self.tables["depth.csv"].pop(0)
        self.reject()

    def test_rejects_final_quantity_conservation_and_stats_drift(self):
        self.s["results"]["reference"]["final_stats"]["submitted"] = 11
        self.reject()

    def test_cli_exports_audit_json_and_returns_nonzero_for_mutation(self):
        out = self.root / "audit.json"
        result = subprocess.run([sys.executable, str(SCRIPT), "--study-root", str(self.root), "--out", str(out)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(out.read_text())["status"], "verified")
        self.tables["diagnostics.csv"][0]["levels_emptied"] = 4
        self.write()
        result = subprocess.run([sys.executable, str(SCRIPT), "--study-root", str(self.root)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("study verification failed", result.stderr)


class PlotManifestTests(FixtureMixin, unittest.TestCase):
    def manifest(self):
        self.figure = self.root / "figure.svg"
        self.figure.write_text("<svg/>")
        self.path = self.root / "chart-manifest.json"
        files = {name: str((self.study / name).relative_to(self.root)) for name in COLUMNS}
        telemetry = self.root / "queue.json"
        telemetry.write_text('{"measurement":{"latency_ns":15},"commands":10,"producer_full_retries":2}')
        sources = [dict(path=p, sha256=v.sha256(self.root / p)) for p in files.values()]
        sources.append(dict(path="queue.json", sha256=v.sha256(telemetry)))
        filters = dict(profile="balanced", depth=2, domain=16, seed=1)
        series = [
            dict(kind="run_metric", filters=dict(filters, engine="reference"), metric="p99_ns", aggregation="median", value=30, min=30, max=30, n=3, source_files=[files["runs.csv"]]),
            dict(kind="histogram_quantile", table="diagnostics", filters=dict(filters, engine="reference", action="limit", fills=[0, 1]), q=0.99, value=30, n=18, source_files=[files["diagnostics.csv"]]),
            dict(kind="paired_run_ratio", filters=filters, metric="commands_per_second", numerator_engine="fast-bitmap", denominator_engine="reference", value=1, min=1, max=1, n=3, source_files=[files["runs.csv"]]),
            dict(kind="csv_rows", source=files["timeline.csv"], x="sequence", y=["live_orders"], filters=dict(filters, sequence=[0, 10]), values=[[0, 4], [10, 4]]),
            dict(kind="json_metric", source="queue.json", metric="measurement.latency_ns", value=15),
            dict(kind="histogram_distribution", table="histogram", filters=dict(filters, engine="reference"), n=30, values=[[10, 18], [20, 6], [30, 6]], source_files=[files["histogram.csv"]]),
            dict(kind="frequency_fraction", table="diagnostics", filters=dict(filters, engine="reference"), field="action", category="limit", value=0.6, numerator=18, denominator=30, source_files=[files["diagnostics.csv"]]),
            dict(kind="json_aggregate", source_files=["queue.json"], metric="measurement.latency_ns", filters={}, aggregation="median", value=15, min=15, max=15, n=1),
            dict(kind="json_ratio_aggregate", source_files=["queue.json"], numerator_metric="producer_full_retries", denominator_metric="commands", filters={}, aggregation="median", value=0.2, min=0.2, max=0.2, n=1),
        ]
        self.m = dict(schema_version=1, sources=sources, plots=[dict(path="figure.svg", sources=[s["path"] for s in sources], series=series)])
        self.path.write_text(json.dumps(self.m))
        reports, self.loaded = v.verify_study_root(self.root)
        return v.verify_plot_manifest(self.path, self.loaded)

    def rewrite_manifest(self):
        self.path.write_text(json.dumps(self.m))
        with self.assertRaises(v.VerificationError):
            v.verify_plot_manifest(self.path, self.loaded)

    def test_checks_all_plot_series_kinds(self):
        report = self.manifest()
        self.assertEqual(report["checked_series"], 9)

    def test_rejects_source_hash_and_missing_figure(self):
        self.manifest()
        self.m["sources"][0]["sha256"] = "0" * 64
        self.rewrite_manifest()
        self.manifest()
        self.figure.unlink()
        with self.assertRaises(v.VerificationError):
            v.verify_plot_manifest(self.path, self.loaded)

    def test_rejects_plotted_quantile_median_min_max_and_sample_counts(self):
        for index, field, value in ((0, "value", 31), (0, "min", 29), (0, "max", 31),
                                    (0, "n", 4), (1, "value", 20), (1, "n", 17),
                                    (2, "value", 2), (2, "min", 0.9)):
            self.manifest()
            self.m["plots"][0]["series"][index][field] = value
            self.rewrite_manifest()

    def test_rejects_csv_trace_and_json_value_mutation(self):
        self.manifest()
        self.m["plots"][0]["series"][3]["values"][0][1] = 5
        self.rewrite_manifest()
        self.manifest()
        self.m["plots"][0]["series"][4]["value"] = 16
        self.rewrite_manifest()

    def test_rejects_duplicate_series_sources_unknown_filter_and_incomplete_pairs(self):
        for change in ("duplicate", "unknown", "incomplete"):
            self.manifest()
            series = self.m["plots"][0]["series"][0]
            if change == "duplicate":
                series["source_files"].append(series["source_files"][0])
            elif change == "unknown":
                series["filters"]["fictional"] = 1
            else:
                self.m["plots"][0]["series"][2]["filters"]["engine"] = "reference"
            self.rewrite_manifest()


    def test_rejects_histogram_distribution_fraction_and_json_aggregate_mutations(self):
        for index, field, value in ((5, "values", [[10, 19], [20, 6], [30, 5]]),
                                    (6, "numerator", 19), (6, "denominator", 31),
                                    (6, "value", 0.5), (7, "max", 16),
                                    (7, "n", 2), (8, "value", 0.3)):
            self.manifest()
            self.m["plots"][0]["series"][index][field] = value
            self.rewrite_manifest()

    def core_fixture(self):
        core_script = SCRIPT.parents[1] / "tests" / "tooling" / "test_verify_results.py"
        spec = importlib.util.spec_from_file_location("core_test_fixtures", core_script)
        helpers = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helpers)
        core = self.root / "core"
        core.mkdir(exist_ok=True)
        (core / "summary.json").write_text(json.dumps(helpers.fixture_summary()))
        runs, histogram = helpers.fixture_rows()
        for name, columns, source_rows in (("runs.csv", helpers.verifier.RUN_COLUMNS, runs),
                                           ("latency_histogram.csv", helpers.verifier.HISTOGRAM_COLUMNS, histogram)):
            with (core / name).open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=columns)
                writer.writeheader()
                writer.writerows(source_rows)
        return core

    def add_core_plot(self):
        self.manifest()
        core = self.core_fixture()
        source = "core/latency_histogram.csv"
        self.m["sources"].append(dict(path=source, sha256=v.sha256(core / "latency_histogram.csv")))
        self.m["plots"][0]["sources"].append(source)
        self.m["plots"][0]["series"].append(dict(kind="histogram_quantile", table="histogram", filters={"engine": "reference"}, source_files=[source], q=0.999, n=3000, value=20))
        self.path.write_text(json.dumps(self.m))
        return core

    def test_core_frequency_figures_require_complete_independent_core_audit(self):
        self.add_core_plot()
        report = v.verify_plot_manifest(self.path, self.loaded)
        self.assertEqual(report["checked_series"], 10)
        self.assertEqual(next(iter(report["core_evidence_audits"].values()))["verified_latency_samples"], 9000)

    def test_rejects_core_summary_mutation_even_if_plotted_histogram_unchanged(self):
        core = self.add_core_plot()
        summary_path = core / "summary.json"
        value = json.loads(summary_path.read_text())
        value["engines"]["reference"]["median_p99_ns"] = 11
        summary_path.write_text(json.dumps(value))
        with self.assertRaises(v.VerificationError):
            v.verify_plot_manifest(self.path, self.loaded)


    def trace_fixture(self):
        directory = self.root / "strategy"
        directory.mkdir(exist_ok=True)
        self.trace_path = directory / "trace.csv"
        self.trace_rows = [dict(zip(v.TRACE_COLUMNS, row)) for row in (
            (0, 100, 0, 0, 0, 0, 0, 0),
            (1, 100, 1, -101, -1, 1, 1, 1),
            (2, 99, 0, -3, -3, 2, 2, 1))]
        self.trace_summary = dict(steps=2, seed=42, inventory_limit_units=2,
            max_abs_inventory_units=1, inventory_units=0, cash_tick_units=-3,
            final_mid_ticks=99, mark_to_market_pnl_tick_units=-3,
            maker_buy_units=1, maker_sell_units=1, maker_trade_events=2,
            maker_filled_units=2, historical_backtest=False, live_trading=False,
            fees_modeled=False, queue_position_model=False)
        self.write_trace()
        return directory

    def write_trace(self):
        self.trace_path.with_name("summary.json").write_text(json.dumps(self.trace_summary))
        with self.trace_path.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=v.TRACE_COLUMNS)
            writer.writeheader()
            writer.writerows(self.trace_rows)

    def test_strategy_trace_checks_negative_pnl_and_final_summary(self):
        self.trace_fixture()
        rows, report = v.verify_strategy_trace(self.trace_path)
        self.assertEqual(report["final_pnl_tick_units"], -3)
        self.assertEqual(len(rows), 3)

    def test_rejects_strategy_accounting_step_quantity_and_limit_mutations(self):
        for index, field, value in ((1, "pnl_tick_units", 1), (1, "cash_tick_units", -100),
                                    (1, "step", 2), (1, "inventory_units", 3),
                                    (1, "maker_trade_events", 2), (1, "maker_filled_units", 0),
                                    (2, "max_abs_inventory_units", 2)):
            self.trace_fixture()
            self.trace_rows[index][field] = value
            self.write_trace()
            with self.assertRaises(v.VerificationError):
                v.verify_strategy_trace(self.trace_path)
        self.trace_fixture()
        self.trace_summary["maker_sell_units"] = 2
        self.write_trace()
        with self.assertRaises(v.VerificationError):
            v.verify_strategy_trace(self.trace_path)

    def test_strategy_plot_source_gets_independent_trace_audit(self):
        self.manifest()
        self.trace_fixture()
        source = "strategy/trace.csv"
        self.m["sources"].append(dict(path=source, sha256=v.sha256(self.trace_path)))
        self.m["plots"][0]["sources"].append(source)
        self.m["plots"][0]["series"].append(dict(kind="csv_rows", source=source,
            x="step", y=["pnl_tick_units"], filters={}, values=[[0, 0], [1, -1], [2, -3]]))
        self.path.write_text(json.dumps(self.m))
        report = v.verify_plot_manifest(self.path, self.loaded)
        self.assertEqual(next(iter(report["trace_evidence_audits"].values()))["trace_rows"], 3)

    def test_csv_rows_are_independently_sorted_by_declared_x_axis(self):
        self.tables["depth.csv"][0]["orders"] = 1
        self.tables["depth.csv"][0]["quantity"] = 1
        another = dict(self.tables["depth.csv"][0], price=1)
        self.tables["depth.csv"].insert(1, another)
        self.write()
        self.manifest()
        source = str((self.study / "depth.csv").relative_to(self.root))
        self.m["plots"][0]["series"].append(dict(kind="csv_rows", source=source,
            x="price", y=["quantity"], filters={"sequence": 0, "side": "buy"},
            values=[[1, 1], [2, 1]]))
        self.path.write_text(json.dumps(self.m))
        report = v.verify_plot_manifest(self.path, self.loaded)
        self.assertEqual(report["checked_series"], 10)


if __name__ == "__main__":
    unittest.main()
