#!/usr/bin/env python3
"""Verify Ledger's core benchmark evidence offline using only the standard library."""
import argparse
import csv
import json
import re
import sys
from collections import defaultdict
from decimal import Decimal, localcontext
from pathlib import Path


class VerificationError(ValueError):
    """An evidence file is malformed or contradicts another evidence file."""


ENGINES = {"reference", "fast-bitmap", "fast-linear"}
RUN_COLUMNS = (
    "engine", "run", "commands", "throughput_total_ns", "commands_per_second",
    "p50_ns", "p99_ns", "p999_ns", "max_ns", "rejected_commands", "events",
    "trades", "event_checksum", "trade_checksum",
)
HISTOGRAM_COLUMNS = ("engine", "run", "latency_ns", "count")
SUMMARY_KEYS = {
    "schema_version", "commands", "seed", "input", "runs", "warmup_commands",
    "config", "mix", "generated_peak_live_orders", "generated_empty_book_cancels",
    "generated_capacity_ioc_limits", "machine", "compiler", "clock", "method", "engines",
}
ENGINE_KEYS = {
    "median_commands_per_second", "median_p50_ns", "median_p99_ns",
    "median_p999_ns", "median_run_max_ns", "max_ns", "rejected_commands",
    "live_orders_final", "events", "trades", "event_checksum", "trade_checksum",
}


def require(condition, message):
    if not condition:
        raise VerificationError(message)


def exact_keys(value, expected, name):
    require(isinstance(value, dict), f"{name}: expected an object")
    require(set(value) == set(expected), f"{name}: unexpected or missing fields")


def integer(value, name, minimum=0):
    require(type(value) is int and value >= minimum, f"{name}: expected integer >= {minimum}")
    require(value <= 2**64 - 1, f"{name}: exceeds uint64 range")
    return value


def csv_integer(value, name, minimum=0):
    require(isinstance(value, str) and re.fullmatch(r"[0-9]+", value) is not None,
            f"{name}: expected a decimal integer")
    require(len(value) <= 20, f"{name}: exceeds uint64 decimal width")
    return integer(int(value), name, minimum)


def positive_decimal(value, name):
    require(isinstance(value, (int, Decimal)) and not isinstance(value, bool),
            f"{name}: expected a number")
    number = Decimal(value)
    require(number.is_finite() and number > 0, f"{name}: expected a finite positive number")
    require(number <= Decimal(2**64 - 1) * Decimal(10**9), f"{name}: exceeds uint64 command/time bounds")
    return number


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        require(key not in value, f"JSON: duplicate field {key}")
        value[key] = item
    return value


def invalid_constant(value):
    raise VerificationError(f"JSON: non-finite constant {value}")


def read_summary(path):
    with Path(path).open(encoding="utf-8") as stream:
        summary = json.load(stream, parse_float=Decimal, parse_constant=invalid_constant,
                            object_pairs_hook=unique_object)
    exact_keys(summary, SUMMARY_KEYS, "summary")
    require(summary["schema_version"] == 1 and type(summary["schema_version"]) is int,
            "summary: unsupported schema_version")
    for key in ("commands", "runs"):
        integer(summary[key], key, 1)
    require(summary["commands"] <= 5_000_000, "commands: exceeds benchmark's 5000000-command limit")
    require(summary["runs"] <= 31 and summary["runs"] % 2 == 1,
            "runs: benchmark requires an odd count in [1, 31]")
    require(summary["input"] is None or isinstance(summary["input"], str), "input: expected string or null")
    generated_fields = ("seed", "generated_peak_live_orders", "generated_empty_book_cancels",
                        "generated_capacity_ioc_limits")
    if summary["input"] is None:
        for key in generated_fields:
            integer(summary[key], key)
    else:
        require(bool(summary["input"]), "input: expected a nonempty feed path")
        for key in generated_fields:
            require(summary[key] is None, f"{key}: input replay requires null generator metadata")
    integer(summary["warmup_commands"], "warmup_commands")
    require(summary["warmup_commands"] <= summary["commands"], "warmup_commands: exceeds measured stream size")
    require(isinstance(summary["method"], str) and bool(summary["method"]), "method: expected nonempty text")
    exact_keys(summary["config"], {"min_price", "max_price", "max_orders"}, "config")
    for key in summary["config"]:
        integer(summary["config"][key], f"config.{key}", 1)
    require(summary["config"]["min_price"] <= summary["config"]["max_price"], "config: inverted price range")
    require(summary["config"]["max_price"] <= 2**63 - 1, "config: price exceeds signed int64 range")
    require(summary["config"]["max_price"] - summary["config"]["min_price"] <= 999_999,
            "config: price domain exceeds 1000000 ticks")
    require(summary["config"]["max_orders"] <= 1_000_000, "config: capacity exceeds 1000000 orders")
    exact_keys(summary["mix"], {"limit", "cancel", "market", "replace"}, "mix")
    for key, value in summary["mix"].items():
        integer(value, f"mix.{key}")
    require(sum(summary["mix"].values()) == summary["commands"], "mix: counts do not sum to commands")
    validate_metadata(summary)
    exact_keys(summary["engines"], ENGINES, "engines")
    for engine, metrics in summary["engines"].items():
        exact_keys(metrics, ENGINE_KEYS, engine)
        for key, value in metrics.items():
            if key == "median_commands_per_second":
                positive_decimal(value, f"{engine}.{key}")
            else:
                integer(value, f"{engine}.{key}")
    return summary


def validate_metadata(summary):
    machine = summary["machine"]
    exact_keys(machine, {"system", "cpu_model", "logical_cpus", "affinity_cpus", "memory", "governor_cpu0"}, "machine")
    integer(machine["logical_cpus"], "machine.logical_cpus", 1)
    for field in ("system", "cpu_model", "affinity_cpus", "memory", "governor_cpu0"):
        require(isinstance(machine[field], str), f"machine.{field}: expected text")
    compiler = summary["compiler"]
    exact_keys(compiler, {"version", "flags", "optimization_enabled", "ndebug"}, "compiler")
    for field in ("version", "flags"):
        require(isinstance(compiler[field], str), f"compiler.{field}: expected text")
    for field in ("optimization_enabled", "ndebug"):
        require(type(compiler[field]) is bool, f"compiler.{field}: expected boolean")
    clock = summary["clock"]
    exact_keys(clock, {"steady", "period_numerator", "period_denominator", "median_empty_pair_ns"}, "clock")
    require(type(clock["steady"]) is bool and clock["steady"], "clock.steady: requires a steady clock")
    for field in ("period_numerator", "period_denominator"):
        integer(clock[field], f"clock.{field}", 1)
    integer(clock["median_empty_pair_ns"], "clock.median_empty_pair_ns")


def read_csv(path, columns):
    with Path(path).open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        require(reader.fieldnames == list(columns), f"{Path(path).name}: unexpected CSV header")
        rows = list(reader)
    require(bool(rows), f"{Path(path).name}: empty CSV")
    for index, row in enumerate(rows, 2):
        require(None not in row and all(value is not None for value in row.values()),
                f"{Path(path).name}:{index}: inconsistent column count")
    return rows


def expected_runs(summary):
    return {(engine, run) for engine in ENGINES for run in range(1, summary["runs"] + 1)}


def read_runs(path, summary):
    runs = {}
    for row in read_csv(path, RUN_COLUMNS):
        engine = row["engine"]
        require(engine in ENGINES, f"runs: unknown engine {engine}")
        run = csv_integer(row["run"], "run", 1)
        key = (engine, run)
        require(key not in runs, f"runs: duplicate engine/run {key}")
        values = {}
        for field in RUN_COLUMNS[2:]:
            if field == "commands_per_second":
                try:
                    values[field] = positive_decimal(Decimal(row[field]), f"{key}.{field}")
                except ArithmeticError as error:
                    raise VerificationError(f"{key}.{field}: invalid decimal") from error
            else:
                values[field] = csv_integer(row[field], f"{key}.{field}")
        require(values["commands"] == summary["commands"], f"{key}: command count differs from summary")
        require(values["throughput_total_ns"] > 0, f"{key}: elapsed time must be positive")
        runs[key] = values
    require(set(runs) == expected_runs(summary), "runs: incomplete or unexpected engine/run set")
    return runs


def nearest_rank(histogram, numerator, denominator):
    """Return ceil(N * fraction)'s sample without expanding frequency counts."""
    require(0 < numerator <= denominator, "percentile: invalid fraction")
    total = sum(histogram.values())
    require(total > 0, "percentile: empty histogram")
    rank = (total * numerator + denominator - 1) // denominator
    cumulative = 0
    for latency, count in sorted(histogram.items()):
        cumulative += count
        if cumulative >= rank:
            return latency
    raise VerificationError("percentile: inconsistent histogram")


def read_histograms(path, summary, runs):
    histograms = defaultdict(dict)
    for row in read_csv(path, HISTOGRAM_COLUMNS):
        engine = row["engine"]
        require(engine in ENGINES, f"histogram: unknown engine {engine}")
        key = (engine, csv_integer(row["run"], "histogram.run", 1))
        require(key in runs, f"histogram: unexpected engine/run {key}")
        latency = csv_integer(row["latency_ns"], f"{key}.latency_ns")
        count = csv_integer(row["count"], f"{key}.count", 1)
        require(latency not in histograms[key], f"histogram: duplicate bin {key}/{latency}")
        histograms[key][latency] = count
    require(set(histograms) == expected_runs(summary), "histogram: incomplete engine/run set")
    for key, histogram in histograms.items():
        require(sum(histogram.values()) == summary["commands"], f"{key}: histogram count differs from commands")
        for field, numerator, denominator in (("p50_ns", 50, 100), ("p99_ns", 99, 100),
                                               ("p999_ns", 999, 1000)):
            require(nearest_rank(histogram, numerator, denominator) == runs[key][field],
                    f"{key}: {field} differs from exact frequencies")
        require(max(histogram) == runs[key]["max_ns"], f"{key}: maximum differs from exact frequencies")
    return dict(histograms)


def upper_median(values):
    """Mirror the C++ reporter's sorted[n/2], including even run counts."""
    require(bool(values), "median: empty sequence")
    return sorted(values)[len(values) // 2]


def close_decimal(actual, expected, name):
    # The C++ stream records 12 significant figures, so allow serialization
    # rounding only; this is not a measurement error or confidence tolerance.
    with localcontext() as context:
        context.prec = 80
        tolerance = max(abs(expected) * Decimal("1e-10"), Decimal("0.000001"))
        require(abs(actual - expected) <= tolerance, f"{name}: inconsistent numeric value")


def verify_metrics(summary, runs):
    with localcontext() as context:
        context.prec = 80
        for key, values in runs.items():
            expected_rate = Decimal(values["commands"]) * 10**9 / values["throughput_total_ns"]
            close_decimal(values["commands_per_second"], expected_rate, f"{key}.commands_per_second")
            for field in ("p50_ns", "p99_ns", "p999_ns", "max_ns"):
                require(values[field] > 0, f"{key}.{field}: latency metric must be positive")
        for engine in sorted(ENGINES):
            selected = [values for (candidate, _), values in runs.items() if candidate == engine]
            metrics = summary["engines"][engine]
            rate = upper_median([value["commands_per_second"] for value in selected])
            close_decimal(Decimal(metrics["median_commands_per_second"]), rate, f"{engine}.median throughput")
            for metric, field in (("median_p50_ns", "p50_ns"), ("median_p99_ns", "p99_ns"),
                                  ("median_p999_ns", "p999_ns"), ("median_run_max_ns", "max_ns")):
                require(metrics[metric] == upper_median([value[field] for value in selected]),
                        f"{engine}.{metric}: differs from run median")
            require(metrics["max_ns"] == max(value["max_ns"] for value in selected),
                    f"{engine}.max_ns: differs from run maximum")
        engines = summary["engines"]
        bitmap_rate = Decimal(engines["fast-bitmap"]["median_commands_per_second"])
        linear_rate = Decimal(engines["fast-linear"]["median_commands_per_second"])
        reference_rate = Decimal(engines["reference"]["median_commands_per_second"])
        return {
            "bitmap_reference_throughput_ratio": bitmap_rate / reference_rate,
            "bitmap_linear_throughput_improvement_percent": (bitmap_rate / linear_rate - 1) * 100,
            "bitmap_linear_p99_reduction_percent": (
                Decimal(engines["fast-linear"]["median_p99_ns"] - engines["fast-bitmap"]["median_p99_ns"])
                / engines["fast-linear"]["median_p99_ns"] * 100),
            "bitmap_linear_p999_reduction_percent": (
                Decimal(engines["fast-linear"]["median_p999_ns"] - engines["fast-bitmap"]["median_p999_ns"])
                / engines["fast-linear"]["median_p999_ns"] * 100),
        }


def verify_accounting(summary, runs):
    """Cross-check reported execution counts/checksums and final live state."""
    fields = ("rejected_commands", "events", "trades", "event_checksum", "trade_checksum")
    reference = summary["engines"]["reference"]
    for engine, metrics in summary["engines"].items():
        require(metrics["rejected_commands"] <= summary["commands"], f"{engine}: rejects exceed commands")
        require(metrics["trades"] <= metrics["events"], f"{engine}: trades exceed events")
        require(metrics["live_orders_final"] <= summary["config"]["max_orders"],
                f"{engine}: final live orders exceed configured capacity")
        if summary["input"] is None:
            require(metrics["live_orders_final"] <= summary["generated_peak_live_orders"],
                    f"{engine}: final live orders exceed generated peak")
        for field in (*fields, "live_orders_final"):
            require(metrics[field] == reference[field], f"{engine}.{field}: differs from reference")
    if summary["input"] is None:
        require(summary["generated_peak_live_orders"] <= summary["config"]["max_orders"],
                "generated peak live orders exceed configured capacity")
    for key, values in runs.items():
        engine = key[0]
        for field in fields:
            require(values[field] == summary["engines"][engine][field],
                    f"{key}.{field}: differs from engine summary")


def verify_directory(directory):
    directory = Path(directory)
    summary = read_summary(directory / "summary.json")
    runs = read_runs(directory / "runs.csv", summary)
    histograms = read_histograms(directory / "latency_histogram.csv", summary, runs)
    verify_accounting(summary, runs)
    derived = verify_metrics(summary, runs)
    return {
        "verified_commands_per_run": summary["commands"],
        "verified_engine_runs": len(runs),
        "verified_latency_samples": sum(sum(values.values()) for values in histograms.values()),
        "derived": {key: format(value, ".12g") for key, value in sorted(derived.items())},
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path,
                        default=Path(__file__).resolve().parents[1] / "bench/results/core-v2",
                        help="Directory containing summary.json, runs.csv and latency_histogram.csv")
    args = parser.parse_args(argv)
    try:
        report = verify_directory(args.directory)
    except (OSError, ValueError, ArithmeticError) as error:
        print(f"benchmark verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
