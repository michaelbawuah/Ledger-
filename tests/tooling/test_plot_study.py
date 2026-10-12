"""Independent aggregation, schema, provenance and rendering checks for studies.

Fixtures are explicitly synthetic unit-test rows; their charts are temporary.
No fixture measurement is published as project evidence.
"""
import csv
from fractions import Fraction
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import warnings
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("plot_study", ROOT / "tools/plot_study.py")
plot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(plot)
DRAW_SPEC = importlib.util.spec_from_file_location("draw_architecture", ROOT / "tools/draw_architecture.py")
draw = importlib.util.module_from_spec(DRAW_SPEC)
DRAW_SPEC.loader.exec_module(draw)
HAVE_MATPLOTLIB = importlib.util.find_spec("matplotlib") is not None


class FrequencyTests(unittest.TestCase):
    def test_nearest_rank_uses_exact_counts_and_boundary(self):
        histogram = {10: 990, 20: 9, 100: 1}
        self.assertEqual(plot.nearest_rank(histogram, Fraction(99, 100)), 10)
        self.assertEqual(plot.nearest_rank(histogram, Fraction(999, 1000)), 20)
        self.assertEqual(plot.nearest_rank(histogram, 1), 100)

    def test_huge_counts_do_not_expand_samples_or_round_threshold(self):
        size = 10**70
        self.assertEqual(plot.nearest_rank({7: size-1, 100: 1}, .99), 7)
        # Exact threshold chooses 7: float(total * q) could lose this boundary.
        self.assertEqual(plot.nearest_rank({7: size//100*99, 100: size//100}, .99), 7)

    def test_invalid_frequency_or_fraction_fails(self):
        for histogram, q in (({}, .5), ({1: 0}, .5), ({-1: 2}, .5), ({1: True}, .5),
                             ({1.5: 2}, .5), ({1: 2}, 0), ({1: 2}, 1.1)):
            with self.subTest(histogram=histogram, q=q), self.assertRaises(plot.DataError):
                plot.nearest_rank(histogram, q)

    def test_list_filters_match_only_specified_values(self):
        self.assertTrue(plot.matches({"fills": 3, "engine": "fast-bitmap"}, {"fills": [3, 4]}))
        self.assertFalse(plot.matches({"fills": 2}, {"fills": [3, 4]}))


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def csv(self, name, columns, rows):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=columns)
            writer.writeheader()
            writer.writerows(rows)
        return path

    def bundle(self, name="balanced", depth=256, domain=20000, engines=("reference", "fast-bitmap")):
        directory = self.root / f"{name}-{depth}-{domain}"
        directory.mkdir()
        (directory / "summary.json").write_text('{"unit_test_fixture":true}', encoding="utf-8")
        runs, frequencies, diagnostics = [], [], []
        # Deliberately different three run medians exercise pooling vs run aggregation.
        for engine in engines:
            for run in (1, 2, 3):
                base = dict(profile=name, depth=depth, domain=domain, seed=1, engine=engine, run=run)
                latency = 10 * run
                runs.append(dict(base, commands=1000, total_ns=1000000*run,
                    commands_per_second=1000000/run, p50_ns=latency, p99_ns=latency*2,
                    p999_ns=latency*2, max_ns=latency*10, applied=999, rejected=1,
                    events=3000, trades=198, checksum=2**64-1, live_orders_final=2))
                for ns, count in ((latency,900),(latency*2,99),(latency*10,1)):
                    frequencies.append(dict(base,latency_ns=ns,count=count))
                diagnostics.extend([
                    dict(base,action="limit",result="applied",fills=0,levels_emptied=0,latency_ns=latency,count=900),
                    dict(base,action="market",result="applied",fills=2,levels_emptied=1,latency_ns=latency*2,count=99),
                    dict(base,action="limit",result="rejected",fills=0,levels_emptied=0,latency_ns=latency*10,count=1)])
        self.csv(f"{directory.name}/runs.csv",plot.RUN_COLUMNS,runs)
        self.csv(f"{directory.name}/histogram.csv",plot.HIST_COLUMNS,frequencies)
        self.csv(f"{directory.name}/diagnostics.csv",plot.DIAG_COLUMNS,diagnostics)
        meta=dict(profile=name,depth=depth,domain=domain,seed=1)
        self.csv(f"{directory.name}/timeline.csv",plot.TIMELINE_COLUMNS,[
            dict(meta,sequence=i,live_orders=2,resting_quantity=20,best_bid=99,best_ask=101,trade_count=i//10)
            for i in (0,500,1000)])
        self.csv(f"{directory.name}/depth.csv",plot.DEPTH_COLUMNS,[
            dict(meta,sequence=i,side=side,price=price,quantity=10,orders=1)
            for i in (0,500,1000) for side,price in (("buy",99),("sell",101))])
        return directory


class SchemaTests(Fixture):
    def test_uint64_checksum_preserved(self):
        directory = self.bundle()
        row = plot.read_csv(directory / "runs.csv",plot.RUN_COLUMNS)[0]
        self.assertEqual(row["checksum"],2**64-1)

    def test_missing_header_ragged_unknown_and_nan_rejected(self):
        samples = ("engine,run\nreference,1\n", "engine,run,latency_ns,count\nreference,1,10\n",
                   "engine,run,latency_ns,count\nsurprise,1,10,1\n", "engine,run,latency_ns,count\nreference,1,NaN,1\n",
                   "engine,run,latency_ns,count\nreference,1,10,0\n")
        for text in samples:
            path = self.root / "bad.csv"
            path.write_text(text,encoding="utf-8")
            with self.subTest(text=text), self.assertRaises(plot.DataError):
                plot.read_csv(path,plot.CORE_HIST_COLUMNS)

    def test_counter_and_quantile_tampering_fail_closed(self):
        directory = self.bundle()
        runs = plot.read_csv(directory / "runs.csv",plot.RUN_COLUMNS)
        hist = plot.read_csv(directory / "histogram.csv",plot.HIST_COLUMNS)
        diag = plot.read_csv(directory / "diagnostics.csv",plot.DIAG_COLUMNS)
        plot.validate_frequencies(runs,hist,diag)
        hist[0]["count"]+=1
        with self.assertRaisesRegex(plot.DataError,"sample count"):
            plot.validate_frequencies(runs,hist,diag)
        hist[0]["count"]-=1
        runs[0]["p99_ns"]+=1
        with self.assertRaisesRegex(plot.DataError,"p99_ns"):
            plot.validate_frequencies(runs,hist,diag)

    def test_duplicate_run_and_bin_rejected(self):
        directory = self.bundle()
        runs = plot.read_csv(directory / "runs.csv",plot.RUN_COLUMNS)
        hist = plot.read_csv(directory / "histogram.csv",plot.HIST_COLUMNS)
        with self.assertRaisesRegex(plot.DataError,"duplicate experiment"):
            plot.validate_frequencies(runs+[runs[0]],hist)
        with self.assertRaisesRegex(plot.DataError,"duplicate exact-frequency"):
            plot.validate_frequencies(runs,hist+[hist[0]])

    def test_snapshot_file_change_detected_before_delivery(self):
        directory = self.bundle()
        dataset = plot.Dataset(self.root)
        (directory / "summary.json").write_text("{}",encoding="utf-8")
        with self.assertRaisesRegex(plot.DataError,"source changed"):
            dataset.unchanged()

    def test_strategy_accounting_tamper_is_rejected(self):
        path = self.csv("trace.csv",plot.TRACE_COLUMNS,[dict(step=0,mid_ticks=100,inventory_units=2,
            cash_tick_units=-200,pnl_tick_units=0,maker_filled_units=2,maker_trade_events=1,max_abs_inventory_units=2),
            dict(step=1,mid_ticks=101,inventory_units=2,cash_tick_units=-200,pnl_tick_units=3,
                 maker_filled_units=2,maker_trade_events=1,max_abs_inventory_units=2)])
        with self.assertRaisesRegex(plot.DataError,"P&L identity"):
            plot.Dataset(strategy_trace=path)


@unittest.skipUnless(HAVE_MATPLOTLIB, "matplotlib is optional; install it for rendering QA")
class RenderingTests(Fixture):
    def test_manifest_reconstructs_pooled_and_run_quantiles_differently(self):
        self.bundle()
        dataset = plot.Dataset(self.root)
        renderer = plot.Renderer(dataset,self.root/"plots")
        pooled=renderer.quantile(dataset.histogram,{"engine":"fast-bitmap"},.99,"histogram")
        run_metric=renderer.metric(dataset.runs,"p99_ns",{"engine":"fast-bitmap"})
        self.assertEqual(pooled,60)
        self.assertEqual(run_metric,(40,20,60))
        self.assertEqual(renderer.series[0]["n"],3000)

    def test_thin_cells_omitted_instead_of_making_claim(self):
        self.bundle()
        dataset=plot.Dataset(self.root)
        renderer=plot.Renderer(dataset,self.root/"plots")
        self.assertIsNone(renderer.quantile(dataset.diagnostics,{"engine":"fast-bitmap","result":"rejected"},.99,minimum=30))
        self.assertEqual(renderer.series,[])

    def test_full_renderer_uses_only_sources_and_relative_manifest_paths(self):
        self.bundle()
        self.bundle("balanced",64,65536)
        self.bundle("balanced",256,65536)
        self.bundle("sparse_fok",64,1024,engines=("fast-bitmap","fast-fok-linear"))
        self.bundle("sparse_fok",64,4096,engines=("fast-bitmap","fast-fok-linear"))
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            manifest=plot.Renderer(plot.Dataset(self.root),self.root/"plots").render()
        names={Path(p["path"]).name for p in manifest["plots"]}
        self.assertIn("06-fill-level-heatmap.svg",names)
        self.assertIn("07-depth-scaling.svg",names)
        self.assertIn("09-fok-controlled-ablation.svg",names)
        self.assertIn("12-book-state-trajectory.svg",names)
        for source in manifest["sources"]:
            self.assertFalse(Path(source["path"]).is_absolute())
            self.assertEqual(source["sha256"],plot.sha256(self.root/"plots"/source["path"]))
        for chart in manifest["plots"]:
            path=self.root/"plots"/chart["path"]
            tree=ET.parse(path)
            self.assertEqual(tree.getroot().tag,"{http://www.w3.org/2000/svg}svg")
            self.assertTrue(chart["sources"])
        persisted=json.loads((self.root/"plots/plot-manifest.json").read_text())
        self.assertEqual(persisted,manifest)

    def test_vector_diagrams_are_accessible_and_semantically_explicit(self):
        for name,function in (("memory",draw.architecture),("fifo",draw.fifo)):
            path=self.root/f"{name}.svg"
            function(path)
            root=ET.parse(path).getroot()
            self.assertEqual(root.attrib["role"],"img")
            self.assertIsNotNone(root.find("{http://www.w3.org/2000/svg}title"))
            self.assertIsNotNone(root.find("{http://www.w3.org/2000/svg}desc"))
        self.assertIn("Two trades",(self.root/"fifo.svg").read_text())


if __name__ == "__main__":
    unittest.main()
