#!/usr/bin/env python3
"""Plot exact latency-frequency CSVs without expanding millions of samples."""
import argparse
import collections
import csv
import pathlib

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("latency.svg"))
    args = parser.parse_args()
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    histograms = collections.defaultdict(collections.Counter)
    runs = collections.defaultdict(collections.Counter)
    with args.csv.open(newline="") as stream:
        for row in csv.DictReader(stream):
            latency, count = int(row["latency_ns"]), int(row["count"])
            if latency < 0 or count <= 0:
                raise ValueError("latencies must be nonnegative and counts positive")
            histograms[row["engine"]][latency] += count
            runs[(row["engine"], int(row["run"]))][latency] += count
    if not histograms:
        raise ValueError("empty latency CSV")
    labels = {"reference": "STL reference", "fast-bitmap": "Fast bitmap", "fast-linear": "Fast linear"}
    colors = {"reference": "#64748b", "fast-bitmap": "#0e7490", "fast-linear": "#c2410c"}
    fig, (cdf, tails) = plt.subplots(1, 2, figsize=(11, 4.3), constrained_layout=True)
    percentiles = {}
    for engine, histogram in histograms.items():
        ordered = sorted(histogram.items())
        total = sum(histogram.values())
        cumulative = 0
        xs, ys = [], []
        for latency, count in ordered:
            cumulative += count
            xs.append(max(latency, 1))
            ys.append(100 * cumulative / total)
        cdf.step(xs, ys, where="post", label=labels.get(engine, engine), color=colors.get(engine))
        # Bars show median run quantiles, matching summary.json, rather than pooled quantiles.
        quantiles = []
        for fraction in (0.5, 0.99, 0.999):
            values = []
            for (candidate, _), run_hist in runs.items():
                if candidate != engine:
                    continue
                size = sum(run_hist.values())
                threshold = int(__import__("math").ceil(size * fraction))
                seen = 0
                for latency, count in sorted(run_hist.items()):
                    seen += count
                    if seen >= threshold:
                        values.append(latency)
                        break
            values.sort()
            quantiles.append(values[len(values) // 2])
        percentiles[engine] = quantiles
    cdf.set_xscale("log")
    cdf.set_xlabel("Per-command latency (ns; clock overhead included)")
    cdf.set_ylabel("Commands at or below latency (%)")
    cdf.set_ylim(0, 100.1)
    cdf.set_title("All recorded samples")
    cdf.legend(frameon=False)
    width = 0.23
    for index, (engine, values) in enumerate(percentiles.items()):
        tails.bar([x + (index - 1) * width for x in range(3)], values, width=width,
                  color=colors.get(engine), label=labels.get(engine, engine))
    tails.set_xticks([0, 1, 2], ["p50", "p99", "p99.9"])
    tails.set_ylabel("Median run latency (ns)")
    tails.set_title("Typical and tail latency")
    tails.set_yscale("log")
    for ax in (cdf, tails):
        ax.spines[["top", "right"]].set_visible(False)
        ax.grid(axis="y", alpha=0.15)
    fig.suptitle("Ledger matching engine — synthetic replay", fontweight="bold")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=160, metadata={"Description": "Synthetic CPU benchmark; no network or exchange latency."})
    print(args.out)

if __name__ == "__main__":
    main()
