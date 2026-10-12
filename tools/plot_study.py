#!/usr/bin/env python3
"""Render Ledger's captured experiments; exact frequency quantiles, no fake data.

Examples::
  python3 tools/plot_study.py --study-root bench/results/study \
    --core-root bench/results/core --strategy-trace bench/results/strategy.csv \
    --spsc-root bench/results/spsc-study --outdir docs/figures

Matplotlib is the only nonstandard dependency. SVG is the published format.
Each figure includes its cohort and units; plot-manifest.json records hashes,
input paths, filters, plotted numbers, and aggregation rules for offline audit.
Missing optional datasets skip their figures. Malformed data fails closed.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import sys
import textwrap
import warnings

SCHEMA_VERSION = 1
MIN_CELL = 30
RUN_COLUMNS = "profile depth domain seed engine run commands total_ns commands_per_second p50_ns p99_ns p999_ns max_ns applied rejected events trades checksum live_orders_final".split()
HIST_COLUMNS = "profile depth domain seed engine run latency_ns count".split()
DIAG_COLUMNS = "profile depth domain seed engine run action result fills levels_emptied latency_ns count".split()
TIMELINE_COLUMNS = "profile depth domain seed sequence live_orders resting_quantity best_bid best_ask trade_count".split()
DEPTH_COLUMNS = "profile depth domain seed sequence side price quantity orders".split()
TRACE_COLUMNS = "step mid_ticks inventory_units cash_tick_units pnl_tick_units maker_filled_units maker_trade_events max_abs_inventory_units".split()
CORE_RUN_COLUMNS = "engine run commands throughput_total_ns commands_per_second p50_ns p99_ns p999_ns max_ns rejected_commands events trades event_checksum trade_checksum".split()
CORE_HIST_COLUMNS = "engine run latency_ns count".split()
COLORS = {"reference": "#64748b", "fast-bitmap": "#087f8c", "fast-linear": "#e07a5f", "fast-fok-linear": "#b594c2"}
LABELS = {"reference": "STL reference", "fast-bitmap": "Bitmap + FOK bitmap", "fast-linear": "Linear price + FOK scan", "fast-fok-linear": "Bitmap price + legacy FOK"}
PROFILES = ["balanced", "cancel_heavy", "trade_storm", "replace_heavy", "sparse_fok", "rejection_stress"]
ENGINES = ["reference", "fast-linear", "fast-fok-linear", "fast-bitmap"]
TEAL, SLATE, CORAL = COLORS["fast-bitmap"], COLORS["reference"], COLORS["fast-linear"]


class DataError(ValueError):
    """A source cannot support an auditable figure."""


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def nearest_rank(histogram, fraction):
    """Exact weighted nearest-rank quantile with O(number of distinct times).

    Fraction(str(q)) avoids floating-point threshold drift and supports counts
    far larger than RAM. No expanded samples or latency bins are created.
    """
    q = fraction if isinstance(fraction, Fraction) else Fraction(str(fraction))
    if not 0 < q <= 1 or not histogram:
        raise DataError("quantile requires a nonempty histogram and 0 < q <= 1")
    if any(isinstance(k, bool) or not isinstance(k, int) or k < 0 or
           isinstance(v, bool) or not isinstance(v, int) or v <= 0
           for k, v in histogram.items()):
        raise DataError("histogram latencies must be nonnegative integers; counts positive integers")
    total = sum(histogram.values())
    threshold = (total * q.numerator + q.denominator - 1) // q.denominator
    seen = 0
    for latency, count in sorted(histogram.items()):
        seen += count
        if seen >= threshold:
            return latency
    raise DataError("invalid histogram")


def read_csv(path, columns):
    path = Path(path)
    result = []
    with path.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != columns:
            raise DataError(f"{path}: expected CSV header {','.join(columns)}")
        for number, raw in enumerate(reader, 2):
            if None in raw or any(value is None or value == "" for value in raw.values()):
                raise DataError(f"{path}:{number}: ragged or empty row")
            row = {"_source": str(path.resolve())}
            for key, value in raw.items():
                if key in ("profile", "engine", "action", "result", "side"):
                    row[key] = value
                elif key == "commands_per_second":
                    try:
                        row[key] = float(value)
                    except ValueError as exc:
                        raise DataError(f"{path}:{number}: bad float {key}") from exc
                    if not math.isfinite(row[key]) or row[key] <= 0:
                        raise DataError(f"{path}:{number}: {key} must be positive and finite")
                else:
                    try:
                        # int(), unlike float(), preserves uint64 checksums exactly.
                        row[key] = int(value)
                    except ValueError as exc:
                        raise DataError(f"{path}:{number}: bad integer {key}") from exc
            if row.get("engine", "reference") not in ENGINES:
                raise DataError(f"{path}:{number}: unknown engine")
            if row.get("action", "limit") not in ("limit", "market", "cancel", "replace"):
                raise DataError(f"{path}:{number}: unknown action")
            if row.get("result", "applied") not in ("applied", "rejected"):
                raise DataError(f"{path}:{number}: unknown result")
            if "count" in row and row["count"] <= 0:
                raise DataError(f"{path}:{number}: count must be positive")
            if "latency_ns" in row and row["latency_ns"] < 0:
                raise DataError(f"{path}:{number}: negative latency")
            if any(row.get(key, 0) < 0 for key in ("fills", "levels_emptied", "commands", "events", "trades", "orders", "quantity")):
                raise DataError(f"{path}:{number}: negative count")
            result.append(row)
    if not result:
        raise DataError(f"{path}: empty CSV")
    return result


def matches(row, filters):
    for key, value in filters.items():
        if isinstance(value, list):
            if row.get(key) not in value:
                return False
        elif row.get(key) != value:
            return False
    return True


def selected(rows, **filters):
    return [row for row in rows if matches(row, filters)]


def run_key(row):
    return tuple(row.get(key) for key in ("profile", "depth", "domain", "seed", "engine", "run"))


def validate_frequencies(runs, histogram, diagnostics=None):
    """Catch source tampering before it becomes a visual claim."""
    run_map = {}
    for row in runs:
        key = run_key(row)
        if key in run_map:
            raise DataError("duplicate experiment/run key; input directories overlap")
        run_map[key] = row
        elapsed = row.get("total_ns", row.get("throughput_total_ns", 0))
        if elapsed <= 0 or row["commands"] <= 0:
            raise DataError("commands and elapsed time must be positive")
        expected = row["commands"] * 1e9 / elapsed
        if not math.isclose(row["commands_per_second"], expected, rel_tol=2e-9, abs_tol=.001):
            raise DataError("throughput does not reconstruct from commands / elapsed time")
    grouped = defaultdict(Counter)
    seen = set()
    for row in histogram:
        key = run_key(row)
        full_key = key + (row["latency_ns"],)
        if full_key in seen:
            raise DataError("duplicate exact-frequency bin")
        seen.add(full_key)
        if key not in run_map:
            raise DataError("histogram references an absent run")
        grouped[key][row["latency_ns"]] += row["count"]
    if set(grouped) != set(run_map):
        raise DataError("histogram run set does not match runs.csv")
    for key, hist in grouped.items():
        run = run_map[key]
        if sum(hist.values()) != run["commands"]:
            raise DataError("histogram sample count does not match commands")
        for field, q in (("p50_ns", .5), ("p99_ns", .99), ("p999_ns", .999), ("max_ns", 1)):
            if nearest_rank(hist, q) != run[field]:
                raise DataError(f"{field} does not reconstruct from exact frequencies")
    if diagnostics is not None:
        counts, seen = Counter(), set()
        for row in diagnostics:
            key = run_key(row)
            full_key = key + tuple(row[field] for field in ("action", "result", "fills", "levels_emptied", "latency_ns"))
            if full_key in seen:
                raise DataError("duplicate diagnostic bin")
            seen.add(full_key)
            if key not in run_map:
                raise DataError("diagnostics reference an absent run")
            counts[key] += row["count"]
        if set(counts) != set(run_map) or any(counts[key] != run_map[key]["commands"] for key in run_map):
            raise DataError("diagnostic sample counts do not match commands")


class Dataset:
    def __init__(self, study_root=None, core_root=None, strategy_trace=None, spsc_root=None):
        self.runs, self.histogram, self.diagnostics, self.timeline, self.depth = [], [], [], [], []
        self.core_runs, self.core_histogram, self.trace, self.queues = [], [], [], []
        self.sources = {}
        if study_root:
            root = Path(study_root)
            dirs = sorted(path.parent for path in root.rglob("summary.json")) if root.exists() else []
            for directory in dirs:
                # summary discovery only selects actual study bundles, never core/queue files.
                if not (directory / "diagnostics.csv").exists():
                    continue
                self.register(directory / "summary.json")
                for name, columns, target in (("runs.csv", RUN_COLUMNS, self.runs),
                        ("histogram.csv", HIST_COLUMNS, self.histogram),
                        ("diagnostics.csv", DIAG_COLUMNS, self.diagnostics),
                        ("timeline.csv", TIMELINE_COLUMNS, self.timeline),
                        ("depth.csv", DEPTH_COLUMNS, self.depth)):
                    path = directory / name
                    target.extend(read_csv(path, columns))
                    self.register(path)
            if not self.runs:
                warnings.warn("no captured study bundles; study figures omitted")
            else:
                validate_frequencies(self.runs, self.histogram, self.diagnostics)
        if core_root:
            directory = Path(core_root)
            if (directory / "runs.csv").exists():
                self.core_runs = read_csv(directory / "runs.csv", CORE_RUN_COLUMNS)
                self.core_histogram = read_csv(directory / "latency_histogram.csv", CORE_HIST_COLUMNS)
                validate_frequencies(self.core_runs, self.core_histogram)
                for name in ("runs.csv", "latency_histogram.csv", "summary.json"):
                    self.register(directory / name)
            else:
                warnings.warn("core benchmark unavailable; core distribution omitted")
        if strategy_trace:
            path = Path(strategy_trace)
            if path.exists():
                self.trace = read_csv(path, TRACE_COLUMNS)
                self.register(path)
                if self.trace[0]["step"] != 0 or any(b["step"] <= a["step"] for a, b in zip(self.trace, self.trace[1:])):
                    raise DataError("strategy steps must start at zero and strictly increase")
                for row in self.trace:
                    if row["pnl_tick_units"] != row["cash_tick_units"] + row["inventory_units"] * row["mid_ticks"]:
                        raise DataError("strategy P&L identity does not reconstruct")
            else:
                warnings.warn("strategy trace unavailable; strategy figure omitted")
        if spsc_root:
            root = Path(spsc_root)
            paths = [root] if root.is_file() else sorted(root.rglob("*.json")) if root.exists() else []
            for path in paths:
                with path.open(encoding="utf-8") as stream:
                    row = json.load(stream)
                if "queue_capacity" not in row or "commands_per_second" not in row:
                    continue
                row["_source"] = str(path.resolve())
                self.queues.append(row)
                self.register(path)
            if not self.queues:
                warnings.warn("SPSC captures unavailable; queue figures omitted")

    def register(self, path):
        path = Path(path).resolve()
        if not path.exists():
            raise DataError(f"missing source: {path}")
        self.sources[str(path)] = sha256(path)

    def unchanged(self):
        for path, original in self.sources.items():
            if sha256(path) != original:
                raise DataError(f"source changed while plotting: {path}")


class Renderer:
    def __init__(self, dataset, outdir, png=False):
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        self.plt = plt
        plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10,
            "axes.titlesize": 11, "axes.labelsize": 10, "axes.edgecolor": "#d5dee5",
            "axes.labelcolor": "#334155", "text.color": "#1e293b", "xtick.color": "#475569",
            "ytick.color": "#475569", "grid.color": "#e2e8f0", "grid.alpha": .8,
            "figure.facecolor": "white", "axes.facecolor": "white", "svg.fonttype": "none",
            "svg.hashsalt": "ledger-study-v1", "savefig.bbox": "tight"})
        self.data, self.outdir, self.png = dataset, Path(outdir), png
        self.outdir.mkdir(parents=True, exist_ok=True)
        self.plots, self.series = [], []
        cohorts = Counter((r["depth"], r["domain"]) for r in dataset.runs)
        self.depth, self.domain = (256, 20000) if (256, 20000) in cohorts else (cohorts.most_common(1)[0][0] if cohorts else (256, 20000))
        self.cohort = {"depth": self.depth, "domain": self.domain}
        self.profiles = [p for p in PROFILES if selected(dataset.runs, profile=p, **self.cohort)]
        self.engine_order = [e for e in ENGINES if any(r["engine"] == e for r in dataset.runs)]

    def figure(self, title, panels=1, width=11.8, height=5.5, nrows=1):
        fig, axes = self.plt.subplots(nrows, panels, figsize=(width, height), squeeze=False)
        fig.suptitle(title, x=.065, y=.99, ha="left", fontsize=18, fontweight="bold")
        return fig, axes.ravel().tolist()

    def finish(self, fig, name, caption):
        for ax in fig.axes:
            if ax.get_label() == "<colorbar>":
                continue
            ax.spines[["top", "right"]].set_visible(False)
            ax.grid(axis="y", linewidth=.6)
            ax.set_axisbelow(True)
        fig.text(.065, .012, textwrap.fill(caption, width=150), fontsize=9, color="#64748b", va="bottom")
        fig.tight_layout(rect=(.035, .10, .985, .91))
        path = self.outdir / f"{name}.svg"
        fig.savefig(path, metadata={"Date": None, "Description": caption})
        if self.png:
            fig.savefig(path.with_suffix(".png"), dpi=150)
        sources = sorted({source for series in self.series for source in
                          series.get("source_files", [series["source"]] if "source" in series else [])})
        self.plots.append({"path": str(path.resolve()), "sources": sources, "caption": caption,
                           "sample_threshold": MIN_CELL, "series": self.series})
        self.series = []
        self.plt.close(fig)

    def metric(self, rows, metric, filters):
        chosen = [r for r in rows if matches(r, filters)]
        if not chosen:
            return None
        values = [r[metric] for r in chosen]
        stats = (statistics.median(values), min(values), max(values))
        self.series.append({"kind": "run_metric", "source_files": sorted({r["_source"] for r in chosen}),
            "filters": filters, "metric": metric, "aggregation": "median", "value": stats[0],
            "min": stats[1], "max": stats[2], "n": len(values)})
        return stats

    def quantile(self, rows, filters, q, table="diagnostics", minimum=1):
        chosen = [r for r in rows if matches(r, filters)]
        histogram = Counter()
        for row in chosen:
            histogram[row["latency_ns"]] += row["count"]
        count = sum(histogram.values())
        if count < minimum:
            return None
        value = nearest_rank(histogram, q)
        self.series.append({"kind": "histogram_quantile", "source_files": sorted({r["_source"] for r in chosen}),
            "table": table, "filters": filters, "q": q, "value": value, "n": count})
        return value

    def csv_series(self, rows, x, ys, filters):
        chosen = sorted([r for r in rows if matches(r, filters)], key=lambda r: r[x])
        if not chosen:
            return []
        self.series.append({"kind": "csv_rows", "source": chosen[0]["_source"], "x": x,
                           "y": list(ys), "filters": filters,
                           "values": [[r[x]] + [r[y] for y in ys] for r in chosen]})
        return chosen

    def cohort_caption(self):
        return f"Initial depth {self.depth}; price domain {self.domain:,} ticks. All captured seeds at this cohort."

    def distribution(self):
        if not self.data.core_histogram:
            return
        fig, axes = self.figure("Where the core spends its time", panels=2, height=5.5)
        for engine in ENGINES:
            rows = selected(self.data.core_histogram, engine=engine)
            if not rows:
                continue
            histogram = Counter()
            for row in rows:
                histogram[row["latency_ns"]] += row["count"]
            total, cumulative = sum(histogram.values()), 0
            xs, cdf, survival = [], [], []
            for latency, count in sorted(histogram.items()):
                cumulative += count
                xs.append(max(1, latency))
                cdf.append(cumulative / total)
                survival.append(max(1 / total, (total - cumulative) / total))
            for ax, ys in zip(axes, (cdf, survival)):
                ax.step(xs, ys, where="post", color=COLORS[engine], label=LABELS[engine], linewidth=1.8)
                ax.set_xscale("log")
                ax.set_xlabel("process() latency (ns)")
            self.series.append({"kind": "histogram_distribution", "source_files": sorted({r["_source"] for r in rows}),
                "table": "histogram", "filters": {"engine": engine}, "n": total,
                "values": [[x, n] for x, n in sorted(histogram.items())]})
            self.quantile(self.data.core_histogram, {"engine": engine}, .99, "histogram")
        axes[0].set(ylabel="Fraction of samples ≤ latency", ylim=(0, 1.01), title="Pooled exact-frequency CDF")
        axes[1].set(ylabel="Fraction of samples > latency", title="Survival tail · log scales")
        axes[1].set_yscale("log")
        axes[0].legend(frameon=False, fontsize=9)
        self.finish(fig, "01-core-distribution", "Synthetic core replay; pooled samples across runs. Clock overhead included. Tail floor = one sample; CPU latency only.")

    def workload_overview(self):
        if not self.profiles:
            return
        fig, axes = self.figure("Workload changes the performance envelope", panels=2, height=6.3)
        for i, engine in enumerate(self.engine_order):
            for ax, metric, scale in zip(axes, ("commands_per_second", "p99_ns"), (1e6, 1)):
                xs, values, low, high = [], [], [], []
                for j, profile in enumerate(self.profiles):
                    stats = self.metric(self.data.runs, metric, dict(self.cohort, profile=profile, engine=engine))
                    if stats:
                        mid, minimum, maximum = stats
                        xs.append(j + (i - (len(self.engine_order)-1)/2)*.16)
                        values.append(mid / scale); low.append((mid-minimum)/scale); high.append((maximum-mid)/scale)
                ax.errorbar(xs, values, yerr=[low, high], fmt="o", capsize=3, ms=6,
                    color=COLORS[engine], label=LABELS[engine], linewidth=1.5)
        for ax in axes:
            ax.set_xticks(range(len(self.profiles)), [p.replace("_", "\n") for p in self.profiles])
        axes[0].set(ylabel="Throughput (million commands/s)", title="Separate unclocked throughput pass")
        axes[1].set(ylabel="Median of run p99 (ns)", title="Diagnostic process() latency")
        axes[1].set_yscale("log")
        axes[0].legend(frameon=False, fontsize=8)
        self.finish(fig, "02-workload-envelope", self.cohort_caption()+" Dots = median runs; bars = observed min–max (not confidence intervals).")

    def action_tails(self):
        if not self.profiles:
            return
        import numpy as np
        from matplotlib.colors import LogNorm
        actions = ["limit", "market", "cancel", "replace"]
        for name, result, title in (("03-action-latency", "applied", "Action costs depend on the surrounding workload"),
                                    ("04-rejection-latency", "rejected", "Rejected commands still have measurable cost")):
            supported=Counter()
            for row in selected(self.data.diagnostics,engine="fast-bitmap",result=result,**self.cohort):
                supported[(row['profile'],row['action'])]+=row['count']
            if not supported or max(supported.values())<MIN_CELL:
                warnings.warn(f"no {result} action groups with ≥ {MIN_CELL} samples; conditional figure omitted")
                continue
            fig, axes = self.figure(title, panels=2, height=6.1)
            for ax, q in zip(axes, (.5, .99)):
                values = np.full((len(self.profiles), len(actions)), np.nan)
                for i, profile in enumerate(self.profiles):
                    for j, action in enumerate(actions):
                        value = self.quantile(self.data.diagnostics,
                            dict(self.cohort, profile=profile, engine="fast-bitmap", action=action, result=result), q, minimum=MIN_CELL)
                        if value is not None:
                            values[i,j] = max(value, 1)
                valid = values[np.isfinite(values)]
                if not len(valid):
                    ax.text(.5,.5,"No groups with ≥ 30 samples", transform=ax.transAxes, ha="center")
                    continue
                image = ax.imshow(values, cmap="YlGnBu", norm=LogNorm(vmin=max(1,valid.min()), vmax=max(valid.max(),valid.min()*1.01)), aspect="auto")
                for i in range(len(self.profiles)):
                    for j in range(len(actions)):
                        if math.isfinite(values[i,j]):
                            ax.text(j,i,f"{values[i,j]:,.0f}",ha="center",va="center",fontsize=9,
                                    color="white" if values[i,j] > math.sqrt(valid.min()*valid.max()) else "#16324f")
                        else:
                            ax.text(j,i,"—",ha="center",va="center",color="#94a3b8")
                ax.set_xticks(range(len(actions)), actions)
                ax.set_yticks(range(len(self.profiles)), [p.replace("_"," ") for p in self.profiles])
                ax.set_title(f"Pooled p{q*100:g} (ns) · {result}")
                ax.grid(False)
                fig.colorbar(image, ax=ax, fraction=.042, pad=.04, label="Latency (ns; log color)")
            self.finish(fig, name, self.cohort_caption()+" Bitmap engine; pooled exact diagnostic frequencies. Masked cells: N < 30. Observer runs outside timing.")

    def fill_latency(self):
        profile = "trade_storm" if "trade_storm" in self.profiles else "balanced"
        rows = selected(self.data.diagnostics, profile=profile, **self.cohort, result="applied")
        if not rows:
            return
        fig, axes = self.figure("One command can produce many maker fills", panels=2, height=5.9)
        counts = sorted({r["fills"] for r in rows})
        for engine in self.engine_order:
            for ax, q in zip(axes, (.5,.99)):
                xs, ys = [], []
                for fills in counts:
                    value = self.quantile(rows, dict(self.cohort, profile=profile, result="applied", engine=engine, fills=fills), q, minimum=MIN_CELL)
                    if value is not None:
                        xs.append(fills); ys.append(value)
                if xs:
                    ax.plot(xs,ys,"o-",color=COLORS[engine],label=LABELS[engine],markersize=4,linewidth=1.4)
                ax.set(xlabel="Maker trade reports per command", ylabel=f"Pooled p{q*100:g} latency (ns)")
                ax.set_yscale("log")
                ax.set_xscale("symlog",linthresh=1)
        axes[0].legend(frameon=False,fontsize=8)
        self.finish(fig,"05-fill-conditioned-latency",self.cohort_caption()+f" {profile.replace('_',' ')}; applied commands, all actions. Exact fill counts; groups with N < 30 omitted.")

    def fill_level_heatmap(self):
        import numpy as np
        from matplotlib.colors import LogNorm
        profile = "trade_storm" if "trade_storm" in self.profiles else "balanced"
        rows = selected(self.data.diagnostics, profile=profile, engine="fast-bitmap", result="applied", **self.cohort)
        if not rows:
            return
        groups = [(0,0),(1,1),(2,2),(3,4),(5,8),(9,16),(17,32),(33,10**12)]
        labels = ["0","1","2","3–4","5–8","9–16","17–32","33+"]
        fill_values, level_values = sorted({r["fills"] for r in rows}), sorted({r["levels_emptied"] for r in rows})
        values, counts = np.full((8,8),np.nan), np.zeros((8,8),dtype=int)
        for i,(level_lo,level_hi) in enumerate(groups):
            for j,(fill_lo,fill_hi) in enumerate(groups):
                filters = dict(self.cohort, profile=profile, engine="fast-bitmap", result="applied",
                           fills=[v for v in fill_values if fill_lo<=v<=fill_hi],
                           levels_emptied=[v for v in level_values if level_lo<=v<=level_hi])
                cell = [r for r in rows if matches(r,filters)]
                counts[i,j] = sum(r["count"] for r in cell)
                value = self.quantile(rows,filters,.99,minimum=MIN_CELL)
                if value is not None:
                    values[i,j] = max(1,value)
        valid = values[np.isfinite(values)]
        if not len(valid):
            return
        fig, (ax,) = self.figure("Fills and emptied levels explain different work",height=7.3)
        image = ax.imshow(values,origin="lower",aspect="auto",cmap="YlGnBu",
            norm=LogNorm(vmin=max(1,valid.min()),vmax=max(valid.max(),valid.min()*1.01)))
        for i in range(8):
            for j in range(8):
                if math.isfinite(values[i,j]):
                    ax.text(j,i,f"{values[i,j]:,.0f} ns\nN={counts[i,j]:,}",ha="center",va="center",fontsize=8,
                        color="white" if values[i,j]>math.sqrt(valid.min()*valid.max()) else "#16324f")
                elif counts[i,j]:
                    ax.text(j,i,f"N={counts[i,j]}\nomitted",ha="center",va="center",fontsize=7,color="#64748b")
        ax.set(xticks=range(8),xticklabels=labels,yticks=range(8),yticklabels=labels,
            xlabel="Maker fill reports per command (count groups)",ylabel="Price levels emptied by the command (count groups)")
        fig.colorbar(image,ax=ax,pad=.03,fraction=.03,label="Pooled p99 process() latency (ns; log color)")
        self.finish(fig,"06-fill-level-heatmap",self.cohort_caption()+f" {profile}; bitmap applied commands. Exact latency frequencies within count groups; N < 30 masked. Levels reconstructed by observer.")

    def scaling(self):
        for parameter,fixed,profile,name,title in (
            ("depth",{"domain":65536},"balanced","07-depth-scaling","How initial resting depth changes the workload"),
            ("domain",{"depth":64},"sparse_fok","08-domain-scaling","Sparse prices expose the cost of a wider domain")):
            rows = selected(self.data.runs,profile=profile,**fixed)
            xs = sorted({r[parameter] for r in rows})
            if len(xs)<2:
                warnings.warn(f"{parameter} sweep unavailable; scaling figure omitted")
                continue
            fig,axes=self.figure(title,panels=2,height=5.9)
            for engine in self.engine_order:
                for ax,metric,scale in zip(axes,("commands_per_second","p99_ns"),(1e6,1)):
                    present,mids,lows,highs=[],[],[],[]
                    for x in xs:
                        stats=self.metric(rows,metric,{"engine":engine,parameter:x})
                        if stats:
                            med,lo,hi=stats;present.append(x);mids.append(med/scale);lows.append((med-lo)/scale);highs.append((hi-med)/scale)
                    if present:
                        ax.errorbar(present,mids,yerr=[lows,highs],fmt="o-",capsize=3,color=COLORS[engine],label=LABELS[engine],markersize=4)
                    ax.set_xscale("log",base=2)
                    ax.set_xlabel("Initial resting orders (requested depth)" if parameter=="depth" else "Configured price domain (ticks)")
            axes[0].set(ylabel="Throughput (million commands/s)",title="Median separate throughput runs")
            axes[1].set(ylabel="Median run p99 latency (ns)",title="Diagnostic process() latency")
            axes[1].set_yscale("log")
            axes[0].legend(frameon=False,fontsize=8)
            fixed_text=", ".join(f"{k}={v:,}" for k,v in fixed.items())
            sizes=sorted({r['commands'] for r in rows})
            size_text=', '.join(f'{n:,}' for n in sizes)
            self.finish(fig,name,f"{profile.replace('_',' ')}; {fixed_text}; commands/run: {size_text}. Dots = medians; bars = min–max. Generator changes accompany depth/domain.")

    def fok_ablation(self):
        ablation_depth=64
        rows=selected(self.data.runs,profile="sparse_fok",depth=ablation_depth)
        domains=sorted({r["domain"] for r in rows})
        engine_a,engine_b="fast-bitmap","fast-fok-linear"
        if not rows or not all(any(r["engine"]==e for r in rows) for e in (engine_a,engine_b)):
            warnings.warn("paired FOK control unavailable; ablation figure omitted")
            return
        fig,axes=self.figure("Isolate FOK preflight: bitmap versus legacy linear scan",panels=2,height=5.9)
        for ax,metric in zip(axes,("commands_per_second","p99_ns")):
            xs,mids,lows,highs=[],[],[],[]
            for domain in domains:
                filters={"profile":"sparse_fok","depth":ablation_depth,"domain":domain}
                chosen=[r for r in rows if matches(r,filters) and r["engine"] in (engine_a,engine_b)]
                pairs=defaultdict(dict)
                for r in chosen:
                    pairs[(r["seed"],r["run"])][r["engine"]]=r[metric]
                ratios=[p[engine_a]/p[engine_b] for p in pairs.values() if set(p)=={engine_a,engine_b}]
                if not ratios:
                    continue
                med,lo,hi=statistics.median(ratios),min(ratios),max(ratios)
                xs.append(domain);mids.append(med);lows.append(med-lo);highs.append(hi-med)
                self.series.append({"kind":"paired_run_ratio","source_files":sorted({r["_source"] for r in chosen}),
                    "filters":filters,"metric":metric,"numerator_engine":engine_a,"denominator_engine":engine_b,
                    "value":med,"min":lo,"max":hi,"n":len(ratios)})
            ax.errorbar(xs,mids,yerr=[lows,highs],fmt="o-",capsize=4,color=TEAL,linewidth=2)
            ax.axhline(1,color=SLATE,linestyle="--",linewidth=1)
            ax.set_xscale("log",base=2)
            ax.set_xlabel("Configured price domain (ticks)")
        axes[0].set(ylabel="Bitmap / legacy throughput ratio",title="Above 1 = higher throughput")
        axes[1].set(ylabel="Bitmap / legacy run p99 ratio",title="Below 1 = lower run p99")
        sizes=', '.join(f'{n:,}' for n in sorted({r['commands'] for r in rows}))
        self.finish(fig,"09-fok-controlled-ablation",f"Sparse FOK; depth={ablation_depth}; commands/run: {sizes}. Commands, seed and run paired; bitmap price search in both engines. Dots = median ratios; bars = min–max.")

    def variation(self):
        if not self.profiles:
            return
        fig,axes=self.figure("Individual captures make run variation visible",panels=2,height=6.2)
        for i,engine in enumerate(self.engine_order):
            for ax,metric,scale in zip(axes,("commands_per_second","p99_ns"),(1e6,1)):
                for j,profile in enumerate(self.profiles):
                    filters=dict(self.cohort,profile=profile,engine=engine)
                    rows=selected(self.data.runs,**filters)
                    if not rows:
                        continue
                    rows.sort(key=lambda r:(r["seed"],r["run"]))
                    offset=(i-(len(self.engine_order)-1)/2)*.18
                    jitter=[(k-(len(rows)-1)/2)*min(.009,.07/max(1,len(rows))) for k in range(len(rows))]
                    ax.scatter([j+offset+v for v in jitter],[r[metric]/scale for r in rows],s=15,color=COLORS[engine],alpha=.7)
                    stats=self.metric(self.data.runs,metric,filters)
                    ax.plot([j+offset-.065,j+offset+.065],[stats[0]/scale]*2,color=COLORS[engine],linewidth=2.5)
                    for r in rows:
                        self.series.append({"kind":"run_metric","source_files":[r["_source"]],"filters":dict(filters,seed=r["seed"],run=r["run"]),
                            "metric":metric,"aggregation":"median","value":r[metric],"min":r[metric],"max":r[metric],"n":1})
        for ax in axes:
            ax.set_xticks(range(len(self.profiles)),[p.replace("_","\n") for p in self.profiles])
        axes[0].set(ylabel="Throughput (million commands/s)",title="Each dot is a seed/run capture")
        axes[1].set(ylabel="Run p99 latency (ns)",title="Small horizontal line = median")
        axes[1].set_yscale("log")
        for engine in self.engine_order:
            axes[0].plot([],[],"o",color=COLORS[engine],label=LABELS[engine])
        axes[0].legend(frameon=False,fontsize=8)
        self.finish(fig,"10-run-variation",self.cohort_caption()+" Jitter separates dots; horizontal positions carry no numeric meaning. Diagnostic timing includes clock overhead.")

    def book_state(self):
        rows=selected(self.data.depth,profile="balanced",**self.cohort)
        if not rows:
            return
        seed=min(r["seed"] for r in rows)
        rows=selected(rows,seed=seed)
        sequences=sorted({r["sequence"] for r in rows})
        if len(sequences)>3:
            sequences=[sequences[0],sequences[len(sequences)//2],sequences[-1]]
        fig,axes=self.figure("The replay leaves a concrete order book",panels=len(sequences),height=6.2,width=12)
        for ax,sequence in zip(axes,sequences):
            for side,color in (("buy",TEAL),("sell",CORAL)):
                # Serializer accepts canonical lowercase sides only. Harness may use Buy/Sell.
                candidates=[r for r in rows if str(r["side"]).lower() in (side,"bid" if side=="buy" else "ask") and r["sequence"]==sequence]
                candidates.sort(key=lambda r:r["price"])
                if candidates:
                    source_side=candidates[0]["side"]
                    self.csv_series(rows,"price",("quantity","orders"),{"sequence":sequence,"side":source_side})
                    x=[r["price"] for r in candidates];y=[r["quantity"] for r in candidates]
                    ax.step(x,y,where="mid",color=color,label="Bids" if side=="buy" else "Asks",linewidth=1.3)
                    ax.fill_between(x,y,step="mid",alpha=.12,color=color)
            ax.set(title=f"After {sequence:,} measured commands",xlabel="Price (integer ticks)",ylabel="Resting quantity (units)")
            ax.ticklabel_format(axis="x",style="plain",useOffset=False)
        axes[0].legend(frameon=False)
        self.finish(fig,"11-book-depth-snapshots",f"Balanced replay; depth={self.depth}, domain={self.domain:,}, seed={seed}. Exact reference-book price-level snapshots; preload already present at sequence zero.")

    def timeline(self):
        rows=selected(self.data.timeline,profile="balanced",**self.cohort)
        if not rows:
            return
        seed=min(r["seed"] for r in rows)
        rows=self.csv_series(rows,"sequence",("live_orders","resting_quantity","trade_count"),{"seed":seed})
        fig,axes=self.figure("Follow the state throughout one deterministic replay",panels=1,nrows=3,height=8)
        for ax,key,label,color in zip(axes,("live_orders","resting_quantity","trade_count"),
            ("Live orders (count)","Resting quantity (units)","Cumulative maker fills (count)"),(TEAL,SLATE,CORAL)):
            ax.plot([r["sequence"] for r in rows],[r[key] for r in rows],color=color,linewidth=1.8)
            ax.set(ylabel=label)
            ax.ticklabel_format(axis="x",style="plain",useOffset=False)
        axes[-1].set_xlabel("Measured command sequence (preload excluded)")
        self.finish(fig,"12-book-state-trajectory",f"Balanced replay; depth={self.depth}, domain={self.domain:,}, seed={seed}. Untimed reference observer; snapshots sampled at deterministic command boundaries.")

    def composition(self):
        if not self.profiles:
            return
        fig,axes=self.figure("What was actually measured?",panels=2,height=6)
        actions=["limit","market","cancel","replace"]
        for ax,fields,colors in zip(axes,(actions,["applied","rejected"]),
            ([TEAL,SLATE,CORAL,"#b594c2"],[TEAL,CORAL])):
            bottoms=[0]*len(self.profiles)
            for field,color in zip(fields,colors):
                fractions=[]
                for profile in self.profiles:
                    filters=dict(self.cohort,profile=profile,engine="fast-bitmap")
                    rows=selected(self.data.diagnostics,**filters)
                    key="action" if field in actions else "result"
                    numerator=sum(r["count"] for r in rows if r[key]==field)
                    denominator=sum(r["count"] for r in rows)
                    fractions.append(numerator/denominator*100 if denominator else 0)
                    self.series.append({"kind":"frequency_fraction","source_files":sorted({r["_source"] for r in rows}),
                        "table":"diagnostics","filters":filters,"field":key,"category":field,
                        "value":numerator/denominator if denominator else 0,"numerator":numerator,"denominator":denominator})
                ax.bar(range(len(self.profiles)),fractions,bottom=bottoms,color=color,label=field,width=.65)
                bottoms=[a+b for a,b in zip(bottoms,fractions)]
            ax.set_xticks(range(len(self.profiles)),[p.replace("_","\n") for p in self.profiles])
            ax.set(ylabel="Measured commands (%)",ylim=(0,100))
            ax.legend(frameon=False,ncol=2,fontsize=9)
        axes[0].set_title("Action mix from captured frequency counts")
        axes[1].set_title("Applied versus rejected outcomes")
        self.finish(fig,"13-measured-command-mix",self.cohort_caption()+" Bitmap diagnostic captures; each command contributes exactly once. Pooling repeated runs preserves workload proportions.")

    def queue_study(self):
        rows=self.data.queues
        if not rows:
            return
        works=sorted({int(r.get("consumer_work",0)) for r in rows})
        def recorded(row,key,record=True):
            value=row
            for part in key.split("."):
                value=value[part]
            if not isinstance(value,(int,float)) or not math.isfinite(value):
                raise DataError(f"invalid SPSC metric: {key}")
            if record:
                self.series.append({"kind":"json_metric","source":row["_source"],"metric":key,"value":value})
            return value
        def aggregate(chosen,key,denominator=None):
            values=[recorded(r,key,False)/(recorded(r,denominator,False) if denominator else 1) for r in chosen]
            mid,lo,hi=statistics.median(values),min(values),max(values)
            sample_counts=sorted({r.get('latency_samples',0) for r in chosen})
            filters={"consumer_work":chosen[0].get("consumer_work",0),"queue_capacity":chosen[0]["queue_capacity"],
                     "latency_samples":sample_counts[0] if len(sample_counts)==1 else sample_counts}
            series={"kind":"json_ratio_aggregate" if denominator else "json_aggregate",
                "source_files":sorted({r['_source'] for r in chosen}),"filters":filters,
                "aggregation":"median","value":mid,"min":lo,"max":hi,"n":len(values)}
            if denominator:
                series.update(numerator_metric=key,denominator_metric=denominator)
            else:
                series['metric']=key
            self.series.append(series)
            return mid,lo,hi
        fig,axes=self.figure("A queue changes latency as well as throughput",panels=2,height=5.8)
        palette=[TEAL,CORAL,SLATE,"#b594c2","#6ba88d"]
        for i,work in enumerate(works):
            for ax,metric,scale,latency in zip(axes,("commands_per_second","enqueue_attempt_to_core_process_ns.p99"),(1e6,1000),(False,True)):
                chosen=[r for r in rows if int(r.get("consumer_work",0))==work and bool(r.get("latency_samples",0))==latency]
                by_capacity=defaultdict(list)
                for row in chosen:
                    by_capacity[row['queue_capacity']].append(row)
                xs,ys,lo,hi=[],[],[],[]
                for capacity,group in sorted(by_capacity.items()):
                    median,minimum,maximum=aggregate(group,metric)
                    xs.append(capacity);ys.append(median/scale);lo.append((median-minimum)/scale);hi.append((maximum-median)/scale)
                if xs:
                    ax.errorbar(xs,ys,yerr=[lo,hi],fmt="o-",capsize=3,color=palette[i%len(palette)],label=f"consumer work={work:,}",markersize=5)
                ax.set_xscale("log",base=2)
                ax.set_xlabel("SPSC capacity (slots)")
        axes[0].set(ylabel="Throughput (million commands/s)",title="Zero-clock-sample throughput captures")
        axes[1].set(ylabel="Enqueue-attempt → core-process p99 (µs)",title="Separate sampled latency captures")
        axes[1].set_yscale("log")
        axes[0].legend(frameon=False,fontsize=9)
        self.finish(fig,"14-queue-capacity-envelope","Synthetic two-thread pipeline; bounded arithmetic consumer work. Medians with observed min–max bars. Queue residence/retry time included; throughput and latency captured separately.")
        if not all("producer_full_retries" in r and "consumer_empty_retries" in r for r in rows):
            warnings.warn("queue retry counters unavailable; backpressure figure omitted")
            return
        fig,axes=self.figure("Backpressure has a measurable signature",panels=2,height=5.8)
        for i,work in enumerate(works):
            chosen=[r for r in rows if int(r.get("consumer_work",0))==work and not r.get("latency_samples",0)]
            by_capacity=defaultdict(list)
            for row in chosen:
                by_capacity[row['queue_capacity']].append(row)
            for ax,key in zip(axes,("producer_full_retries","consumer_empty_retries")):
                xs,ys,lo,hi=[],[],[],[]
                for capacity,group in sorted(by_capacity.items()):
                    median,minimum,maximum=aggregate(group,key,'commands')
                    xs.append(capacity);ys.append(median);lo.append(median-minimum);hi.append(maximum-median)
                if xs:
                    ax.errorbar(xs,ys,yerr=[lo,hi],fmt="o-",capsize=3,color=palette[i%len(palette)],label=f"consumer work={work:,}")
                ax.set_xscale("log",base=2);ax.set_yscale("symlog",linthresh=.01)
                ax.set(xlabel="SPSC capacity (slots)",ylabel="Retry attempts / measured command")
        axes[0].set_title("Producer found queue full")
        axes[1].set_title("Consumer found queue empty")
        axes[0].legend(frameon=False,fontsize=9)
        self.finish(fig,"15-queue-backpressure","Zero-clock-sample captures; medians with min–max bars. Retried attempts reflect busy-spin scheduling, not dropped orders or occupancy. Axis linear near zero, logarithmic above 0.01.")

    def strategy(self):
        if not self.data.trace:
            return
        rows=self.csv_series(self.data.trace,"step",("mid_ticks","inventory_units","cash_tick_units","pnl_tick_units","max_abs_inventory_units"),{})
        fig,axes=self.figure("A synthetic market maker: marks, inventory and cash",panels=1,nrows=4,height=10)
        steps=[r["step"] for r in rows]
        for ax,key,label,color in zip(axes,("mid_ticks","inventory_units","cash_tick_units","pnl_tick_units"),
            ("Mid-price (ticks)","Inventory (units)","Cash (tick × units)","Mark-to-market P&L (tick × units)"),(SLATE,TEAL,SLATE,CORAL)):
            ax.plot(steps,[r[key] for r in rows],color=color,linewidth=1.1)
            ax.set_ylabel(label)
            ax.ticklabel_format(axis="y",style="plain",useOffset=False)
            if key in ("inventory_units","pnl_tick_units"):
                ax.axhline(0,color="#94a3b8",linestyle="--",linewidth=.7)
        axes[-1].set_xlabel("Synthetic flow step")
        self.finish(fig,"16-strategy-accounting","Captured seeded simulation. P&L = cash + inventory × mid at every mark. No fees, realistic queue-position model, historical-market validation, or live trading.")

    def render(self):
        self.distribution();self.workload_overview();self.action_tails();self.fill_latency()
        self.fill_level_heatmap();self.scaling();self.fok_ablation();self.variation()
        self.book_state();self.timeline();self.composition();self.queue_study();self.strategy()
        self.data.unchanged()
        manifest={"schema_version":SCHEMA_VERSION,"method":"Exact frequency nearest-rank pooled quantiles; run summaries use arithmetic medians; bars are observed min/max.",
            "minimum_conditional_samples":MIN_CELL,"cohort":self.cohort,
            "sources":[{"path":path,"sha256":digest} for path,digest in sorted(self.data.sources.items())],"plots":self.plots}
        relative=lambda path:os.path.relpath(path,self.outdir.resolve())
        for source in manifest['sources']:
            source['path']=relative(source['path'])
        for plot in manifest['plots']:
            plot['path']=relative(plot['path'])
            plot['sources']=[relative(path) for path in plot['sources']]
            for series in plot['series']:
                if 'source' in series:
                    series['source']=relative(series['source'])
                if 'source_files' in series:
                    series['source_files']=[relative(path) for path in series['source_files']]
        (self.outdir/"plot-manifest.json").write_text(json.dumps(manifest,indent=2,allow_nan=False)+"\n",encoding="utf-8")
        return manifest


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("study-root","core-root","strategy-trace","spsc-root"):
        parser.add_argument("--"+name,type=Path)
    parser.add_argument("--outdir",type=Path,required=True)
    parser.add_argument("--png",action="store_true",help="Also render PNGs for local visual QA")
    args=parser.parse_args(argv)
    try:
        dataset=Dataset(args.study_root,args.core_root,args.strategy_trace,args.spsc_root)
        if not dataset.sources:
            raise DataError("no captured measurement sources")
        manifest=Renderer(dataset,args.outdir,args.png).render()
    except (DataError,OSError,KeyError,json.JSONDecodeError) as exc:
        print(f"plotting failed: {exc}",file=sys.stderr)
        return 1
    print(json.dumps({"plots":len(manifest["plots"]),"manifest":str((args.outdir/"plot-manifest.json").resolve())}))
    return 0


if __name__=="__main__":
    sys.exit(main())
