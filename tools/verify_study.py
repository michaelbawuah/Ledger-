#!/usr/bin/env python3
"""Independently audit Ledger study CSV evidence and chart provenance offline."""
import argparse
from collections import Counter, defaultdict
import csv
from decimal import Decimal, localcontext
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys


class VerificationError(ValueError):
    """Malformed or internally inconsistent evidence."""


PREFIX = ("profile", "depth", "domain", "seed")
RUN_COLUMNS = PREFIX + ("engine", "run", "commands", "total_ns", "commands_per_second", "p50_ns", "p99_ns", "p999_ns", "max_ns", "applied", "rejected", "events", "trades", "checksum", "live_orders_final")
HISTOGRAM_COLUMNS = PREFIX + ("engine", "run", "latency_ns", "count")
DIAGNOSTICS_COLUMNS = PREFIX + ("engine", "run", "action", "result", "fills", "levels_emptied", "latency_ns", "count")
TIMELINE_COLUMNS = PREFIX + ("sequence", "live_orders", "resting_quantity", "best_bid", "best_ask", "trade_count")
DEPTH_COLUMNS = PREFIX + ("sequence", "side", "price", "quantity", "orders")
PROFILES = {"balanced", "cancel_heavy", "trade_storm", "replace_heavy", "sparse_fok", "rejection_stress"}
ENGINES = {"reference", "fast-linear", "fast-bitmap", "fast-fok-linear"}
ACTIONS = {"limit", "market", "cancel", "replace"}
QUANTILES = {"p50_ns": (1, 2), "p99_ns": (99, 100), "p999_ns": (999, 1000)}
U64 = 2**64 - 1
U32 = 2**32 - 1


def require(ok, message):
    if not ok:
        raise VerificationError(message)


def integer(value, name, minimum=0, maximum=U64):
    require(type(value) is int and minimum <= value <= maximum, f"{name}: expected integer in [{minimum}, {maximum}]")
    return value


def csv_integer(value, name, minimum=0, maximum=U64):
    require(isinstance(value, str) and re.fullmatch(r"[0-9]{1,20}", value), f"{name}: expected unsigned decimal integer")
    return integer(int(value), name, minimum, maximum)


def decimal(value, name, positive=False):
    require(not isinstance(value, bool) and isinstance(value, (str, int, float, Decimal)), f"{name}: expected numeric value")
    try:
        number = Decimal(str(value))
    except ArithmeticError as error:
        raise VerificationError(f"{name}: invalid decimal") from error
    require(number.is_finite() and number.copy_abs() <= Decimal(U64) * 10**9, f"{name}: invalid or out of range numeric value")
    require(not positive or number > 0, f"{name}: expected positive numeric value")
    return number


def close(actual, expected, name):
    # Twelve-significant-figure C++ serialization and normal chart JSON floats.
    # This allowance is only rounding; it is not measurement uncertainty.
    with localcontext() as ctx:
        ctx.prec = 80
        expected = decimal(expected, name)
        actual = decimal(actual, name)
        require(abs(actual - expected) <= max(abs(expected) * Decimal("1e-10"), Decimal("1e-6")), f"{name}: value disagrees with raw evidence")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"JSON: duplicate field {key}")
        result[key] = value
    return result


def invalid_constant(value):
    raise VerificationError(f"JSON: nonfinite constant {value}")


def read_json(path):
    with Path(path).open(encoding="utf-8") as stream:
        return json.load(stream, parse_float=Decimal, parse_constant=invalid_constant, object_pairs_hook=unique_object)


def required_fields(value, fields, name):
    require(isinstance(value, dict) and set(fields) <= set(value), f"{name}: missing required fields")


def text(value, name):
    require(isinstance(value, str) and value.strip(), f"{name}: expected nonempty text")


def read_summary(path):
    s = read_json(path)
    required_fields(s, ("schema_version", "profile", "commands", "seed", "depth", "domain", "runs", "preload_commands", "config", "engines", "machine", "compiler", "clock", "method", "results"), "summary")
    integer(s["schema_version"], "schema_version", 1, 1)
    require(s["profile"] in PROFILES, "summary: unknown profile")
    integer(s["commands"], "commands", 1, 1_000_000)
    integer(s["runs"], "runs", 1, 9)
    require(s["runs"] % 2 == 1, "runs: expected odd count")
    integer(s["seed"], "seed")
    integer(s["depth"], "depth", 1, 16384)
    integer(s["domain"], "domain", 2 * s["depth"], 1_000_000)
    integer(s["preload_commands"], "preload_commands")
    require(s["preload_commands"] == 2 * s["depth"], "preload: expected depth orders per side")
    require(isinstance(s["engines"], list) and bool(s["engines"]) and all(isinstance(e, str) and e in ENGINES for e in s["engines"]), "engines: invalid selected list")
    require(len(set(s["engines"])) == len(s["engines"]), "engines: duplicate selected engine")
    config = s["config"]
    require(isinstance(config, dict) and set(config) == {"min_price", "max_price", "max_orders"}, "config: unexpected fields")
    for name in config:
        integer(config[name], f"config.{name}", 1)
    require(config == {"min_price": 1, "max_price": s["domain"], "max_orders": max(4 * s["depth"], 1024)}, "config: differs from workload depth/domain")
    required_fields(s["machine"], ("os", "cpu_model", "hardware_threads", "affinity"), "machine")
    text(s["machine"]["os"], "machine.os")
    text(s["machine"]["cpu_model"], "machine.cpu_model")
    integer(s["machine"]["hardware_threads"], "machine.hardware_threads", 1)
    require(isinstance(s["machine"]["affinity"], str), "machine.affinity: expected text")
    required_fields(s["compiler"], ("version", "flags"), "compiler")
    text(s["compiler"]["version"], "compiler.version")
    require(isinstance(s["compiler"]["flags"], str), "compiler.flags: expected text")
    c = s["clock"]
    required_fields(c, ("name", "is_steady", "period_num", "period_den", "timer_pair_median_ns"), "clock")
    text(c["name"], "clock.name")
    require(c["is_steady"] is True, "clock: requires steady clock")
    integer(c["period_num"], "clock.period_num", 1)
    integer(c["period_den"], "clock.period_den", 1)
    integer(c["timer_pair_median_ns"], "clock.timer_pair_median_ns")
    method = s["method"]
    required_fields(method, ("throughput", "latency", "observer", "levels_emptied", "checksum_scope", "warmup_commands", "timer_overhead_subtracted", "engine_order", "timeline_sequence"), "method")
    for name in ("throughput", "latency", "observer", "levels_emptied", "checksum_scope", "engine_order", "timeline_sequence"):
        text(method[name], f"method.{name}")
    integer(method["warmup_commands"], "method.warmup_commands")
    require(method["timer_overhead_subtracted"] is False, "method: timer overhead must remain unsubtracted")
    require(isinstance(s["results"], dict) and set(s["results"]) == set(s["engines"]), "results: differs from selected engines")
    return s


def read_csv(path, columns):
    with Path(path).open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        require(reader.fieldnames == list(columns), f"{Path(path).name}: unexpected CSV header")
        rows = list(reader)
    require(bool(rows), f"{Path(path).name}: empty CSV")
    for i, row in enumerate(rows, 2):
        require(None not in row and all(v is not None for v in row.values()), f"{Path(path).name}:{i}: inconsistent column count")
    return rows


def typed_rows(path, columns, summary):
    rows = read_csv(path, columns)
    for row in rows:
        for field in columns:
            if field == "commands_per_second":
                row[field] = decimal(row[field], field, True)
            elif field not in ("profile", "engine", "action", "result", "side"):
                row[field] = csv_integer(row[field], field, 1 if field == "count" else 0)
        for field in PREFIX:
            require(row[field] == summary[field], f"{Path(path).name}: {field} differs from summary")
    return rows


def percentile(frequencies, numerator, denominator):
    """Find the integer cumulative-frequency rank without expanded samples."""
    total = sum(frequencies.values())
    require(total > 0 and 0 < numerator <= denominator, "percentile: invalid empty distribution or rank")
    wanted = (total * numerator + denominator - 1) // denominator
    seen = 0
    for value in sorted(frequencies):
        seen += frequencies[value]
        if seen >= wanted:
            return value
    raise VerificationError("percentile: inconsistent cumulative frequency")


def median(values):
    require(bool(values), "median: empty values")
    values = sorted(values)
    # Plot groups may pool seeds and therefore contain an even number of runs.
    middle = len(values) // 2
    return values[middle] if len(values) % 2 else (Decimal(values[middle - 1]) + Decimal(values[middle])) / 2


def run_key(row):
    return row["engine"], row["run"]


def read_runs(directory, s):
    rows = typed_rows(directory / "runs.csv", RUN_COLUMNS, s)
    runs = {}
    for row in rows:
        key = run_key(row)
        require(key not in runs, f"runs: duplicate {key}")
        require(row["commands"] == s["commands"] and row["total_ns"] > 0, f"{key}: inconsistent command count or elapsed time")
        require(row["applied"] + row["rejected"] == s["commands"], f"{key}: outcome counts do not sum to commands")
        require(row["trades"] <= row["events"] and row["live_orders_final"] <= s["config"]["max_orders"], f"{key}: impossible events or final live orders")
        with localcontext() as ctx:
            ctx.prec = 80
            close(row["commands_per_second"], Decimal(s["commands"]) * 10**9 / row["total_ns"], f"{key}.throughput")
        runs[key] = row
    expected = {(e, r) for e in s["engines"] for r in range(1, s["runs"] + 1)}
    require(set(runs) == expected, "runs: incomplete or unexpected engine/run set")
    same = ("applied", "rejected", "events", "trades", "checksum", "live_orders_final")
    baseline = next(iter(runs.values()))
    for key, row in runs.items():
        require(all(row[field] == baseline[field] for field in same), f"{key}: cross-engine/run deterministic outcomes disagree")
    for engine, result in s["results"].items():
        required_fields(result, ("median_commands_per_second", "median_p50_ns", "median_p99_ns", "median_p999_ns", "max_ns", *same, "final_stats"), f"results.{engine}")
        group = [r for (e, _), r in runs.items() if e == engine]
        close(result["median_commands_per_second"], median([r["commands_per_second"] for r in group]), f"{engine}.median throughput")
        for field in same:
            integer(result[field], f"results.{engine}.{field}")
            require(result[field] == baseline[field], f"{engine}.{field}: summary differs from runs")
        for field in QUANTILES:
            value = result[f"median_{field}"]
            integer(value, f"results.{engine}.median_{field}")
            require(value == median([r[field] for r in group]), f"{engine}.{field}: summary median differs")
        integer(result["max_ns"], f"results.{engine}.max_ns")
        require(result["max_ns"] == max(r["max_ns"] for r in group), f"{engine}.max_ns: summary maximum differs")
    return runs, rows


def verify_frequencies(directory, s, runs):
    hist_rows = typed_rows(directory / "histogram.csv", HISTOGRAM_COLUMNS, s)
    diagnostic_rows = typed_rows(directory / "diagnostics.csv", DIAGNOSTICS_COLUMNS, s)
    hist = defaultdict(Counter)
    for row in hist_rows:
        key = run_key(row)
        require(key in runs and row["latency_ns"] not in hist[key], "histogram: duplicate bin or unexpected run")
        hist[key][row["latency_ns"]] = row["count"]
    require(set(hist) == set(runs), "histogram: incomplete run set")
    for key, bins in hist.items():
        require(sum(bins.values()) == s["commands"], f"{key}: histogram count differs from commands")
        for field, (num, den) in QUANTILES.items():
            require(percentile(bins, num, den) == runs[key][field], f"{key}.{field}: differs from reconstructed frequency rank")
        require(max(bins) == runs[key]["max_ns"], f"{key}.max_ns: differs from histogram")
    conditional = defaultdict(Counter)
    semantics = defaultdict(Counter)
    seen = set()
    outcomes = defaultdict(Counter)
    fills = Counter()
    applied_replaces = Counter()
    noncancel_applied = Counter()
    for row in diagnostic_rows:
        key = run_key(row)
        require(key in runs, "diagnostics: unexpected run")
        require(row["action"] in ACTIONS and row["result"] in {"applied", "rejected"}, "diagnostics: invalid action/result")
        cell = (key, row["action"], row["result"], row["fills"], row["levels_emptied"], row["latency_ns"])
        require(cell not in seen, "diagnostics: duplicate conditional bin")
        seen.add(cell)
        action, result = row["action"], row["result"]
        n, f, empty = row["count"], row["fills"], row["levels_emptied"]
        require(f <= s["config"]["max_orders"], "diagnostics: fills exceed order pool")
        require(empty <= f + (1 if action in {"cancel", "replace"} else 0), "diagnostics: emptied levels exceed fills/cancel bound")
        require(result != "rejected" or (f == 0 and empty == 0), "diagnostics: rejected command changed the book")
        require(action != "cancel" or f == 0, "diagnostics: cancel generated trades")
        conditional[key][row["latency_ns"]] += n
        semantics[key][(action, result, f, empty)] += n
        outcomes[key][result] += n
        fills[key] += f * n
        if action == "replace" and result == "applied":
            applied_replaces[key] += n
        if action != "cancel" and result == "applied":
            noncancel_applied[key] += n
    require(set(conditional) == set(runs), "diagnostics: incomplete run set")
    base_semantics = next(iter(semantics.values()))
    for key, bins in conditional.items():
        require(bins == hist[key], f"{key}: diagnostic marginal differs from unconditional histogram")
        require(semantics[key] == base_semantics, f"{key}: diagnostic outcomes differ across deterministic repeats")
        require(fills[key] == runs[key]["trades"], f"{key}: weighted fills differ from trades")
        for result in ("applied", "rejected"):
            require(outcomes[key][result] == runs[key][result], f"{key}: diagnostic outcome totals disagree")
        base_events = s["commands"] + applied_replaces[key] + 3 * fills[key]
        require(base_events <= runs[key]["events"] <= base_events + noncancel_applied[key], f"{key}: impossible event accounting")
    return hist_rows, diagnostic_rows


def verify_snapshots(directory, s, runs):
    timeline = typed_rows(directory / "timeline.csv", TIMELINE_COLUMNS, s)
    depth = typed_rows(directory / "depth.csv", DEPTH_COLUMNS, s)
    by_sequence = {}
    previous = -1
    last_trades = 0
    for row in timeline:
        sequence = row["sequence"]
        require(previous < sequence <= s["commands"], "timeline: unordered/duplicate/out of range sequence")
        previous = sequence
        live, quantity = row["live_orders"], row["resting_quantity"]
        require(live <= s["config"]["max_orders"] and live <= quantity <= live * U32, "timeline: impossible resting orders/quantity")
        for field in ("best_bid", "best_ask"):
            require(row[field] == 0 or 1 <= row[field] <= s["domain"], f"timeline: {field} outside configured domain")
        require(not (row["best_bid"] and row["best_ask"]) or row["best_bid"] < row["best_ask"], "timeline: crossed resting book")
        require(live != 0 or row["best_bid"] == row["best_ask"] == 0, "timeline: nonzero best price on empty book")
        require(live == 0 or row["best_bid"] != 0 or row["best_ask"] != 0, "timeline: no best price on nonempty book")
        require(row["trade_count"] >= last_trades, "timeline: decreasing trade count")
        last_trades = row["trade_count"]
        by_sequence[sequence] = row
    cadence = max(1, s["commands"] // 100)
    expected = set(range(0, s["commands"] + 1, cadence)) | {s["commands"]}
    require(set(by_sequence) == expected, "timeline: missing/unexpected snapshot cadence")
    require(timeline[0]["live_orders"] == 2 * s["depth"] and timeline[0]["trade_count"] == 0, "timeline: preload identity differs")
    baseline = next(iter(runs.values()))
    require(timeline[-1]["live_orders"] == baseline["live_orders_final"], "timeline: final live orders disagree")
    require(timeline[-1]["trade_count"] == baseline["trades"], "timeline: final trade count disagrees")
    groups = defaultdict(list)
    seen = set()
    for row in depth:
        require(row["sequence"] in {0, s["commands"] // 2, s["commands"]}, "depth: unexpected snapshot sequence")
        require(row["side"] in {"buy", "sell"}, "depth: unknown side")
        require(1 <= row["price"] <= s["domain"], "depth: price outside domain")
        require(1 <= row["orders"] <= s["config"]["max_orders"] and row["orders"] <= row["quantity"] <= row["orders"] * U32, "depth: impossible quantity/orders")
        key = row["sequence"], row["side"], row["price"]
        require(key not in seen, "depth: duplicate price level")
        seen.add(key)
        groups[row["sequence"]].append(row)
    require(depth == sorted(depth, key=lambda row: (row["sequence"], 0 if row["side"] == "buy" else 1, -row["price"] if row["side"] == "buy" else row["price"])), "depth: snapshots/price levels not in canonical order")
    require(all(sum(row["orders"] for row in groups[0] if row["side"] == side) == s["depth"] for side in ("buy", "sell")), "depth: initial per-side depth differs")
    for sequence in {0, s["commands"] // 2, s["commands"]}:
        rows = groups[sequence]
        live = sum(row["orders"] for row in rows)
        quantity = sum(row["quantity"] for row in rows)
        require(live <= s["config"]["max_orders"], "depth: capacity exceeded")
        bids = [row["price"] for row in rows if row["side"] == "buy"]
        asks = [row["price"] for row in rows if row["side"] == "sell"]
        best_bid, best_ask = max(bids, default=0), min(asks, default=0)
        require(not (best_bid and best_ask) or best_bid < best_ask, "depth: crossed resting book")
        if sequence in by_sequence:
            row = by_sequence[sequence]
            require((live, quantity, best_bid, best_ask) == (row["live_orders"], row["resting_quantity"], row["best_bid"], row["best_ask"]), "depth: snapshot differs from timeline")
    stats_baseline = None
    for engine, result in s["results"].items():
        stats = result["final_stats"]
        require(isinstance(stats, dict) and set(stats) == {"submitted", "executed", "cancelled", "expired", "rejected", "trade_count"}, f"{engine}: invalid final_stats schema")
        for field, value in stats.items():
            integer(value, f"{engine}.final_stats.{field}")
        require(stats["submitted"] == 2 * stats["executed"] + stats["cancelled"] + stats["expired"] + timeline[-1]["resting_quantity"], f"{engine}: quantity conservation failed")
        require(stats["trade_count"] == baseline["trades"], f"{engine}: final_stats trade count failed")
        if stats_baseline is None:
            stats_baseline = stats
        require(stats == stats_baseline, f"{engine}: final quantity statistics differ")
    return timeline, depth


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_directory(directory):
    directory = Path(directory)
    s = read_summary(directory / "summary.json")
    runs, run_rows = read_runs(directory, s)
    hist, diagnostics = verify_frequencies(directory, s, runs)
    timeline, depth = verify_snapshots(directory, s, runs)
    sources = {name: sha256(directory / name) for name in ("summary.json", "runs.csv", "histogram.csv", "diagnostics.csv", "timeline.csv", "depth.csv")}
    report = {"directory": str(directory.resolve()), **{k: s[k] for k in PREFIX}, "commands_per_run": s["commands"], "engine_runs": len(runs), "latency_observations": s["commands"] * len(runs), "diagnostic_observations": sum(r["count"] for r in diagnostics), "trade_events_per_run": next(iter(runs.values()))["trades"], "timeline_snapshots": len(timeline), "depth_rows": len(depth), "source_sha256": sources}
    tables = {"runs.csv": run_rows, "histogram.csv": hist, "diagnostics.csv": diagnostics, "timeline.csv": timeline, "depth.csv": depth}
    return report, tables


def verify_study_root(root):
    root = Path(root)
    require(root.is_dir(), f"study root: missing directory {root}")
    paths = sorted(root.rglob("summary.json"))
    require(bool(paths), "study root: no summary.json files")
    reports, tables = [], {}
    identities = set()
    for path in paths:
        report, loaded = verify_directory(path.parent)
        identity = tuple(report[k] for k in PREFIX)
        require(identity not in identities, "study root: duplicate profile/depth/domain/seed study")
        identities.add(identity)
        reports.append(report)
        for name, rows in loaded.items():
            tables[(path.parent / name).resolve()] = rows
    return reports, tables


def matches(row, filters):
    for key, value in filters.items():
        require(key in row, f"plot: unknown filter {key}")
        if isinstance(value, list):
            if row[key] not in value:
                return False
        elif row[key] != value:
            return False
    return True


def verify_plot_manifest(path, tables):
    path = Path(path).resolve()
    m = read_json(path)
    required_fields(m, ("schema_version", "sources", "plots"), "plot manifest")
    integer(m["schema_version"], "plot schema_version", 1, 1)
    require(isinstance(m["sources"], list) and isinstance(m["plots"], list), "plot manifest: expected source/plot arrays")
    def resolve(name):
        require(isinstance(name, str) and bool(name), "plot: invalid source path")
        return (path.parent / name).resolve()
    sources = {}
    for source in m["sources"]:
        require(isinstance(source, dict) and set(source) == {"path", "sha256"}, "plot: invalid source entry")
        location = resolve(source["path"])
        require(location not in sources, "plot: duplicate source")
        require(isinstance(source["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", source["sha256"]), "plot: invalid source SHA")
        require(sha256(location) == source["sha256"], f"plot: source SHA mismatch {location}")
        sources[location] = source["sha256"]
    core_audits = {}
    for source in sources:
        if source.name == "latency_histogram.csv":
            script = Path(__file__).with_name("verify_results.py")
            spec = importlib.util.spec_from_file_location("ledger_core_verifier", script)
            verifier = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(verifier)
            try:
                core_report = verifier.verify_directory(source.parent)
                core_rows = verifier.read_csv(source, verifier.HISTOGRAM_COLUMNS)
            except ValueError as error:
                raise VerificationError(f"plot: core evidence failed independent audit: {error}") from error
            for row in core_rows:
                for field in ("run", "latency_ns", "count"):
                    row[field] = csv_integer(row[field], f"core.{field}", 1 if field == "count" else 0)
            tables[source] = core_rows
            core_audits[str(source.parent)] = {"verified_latency_samples": core_report["verified_latency_samples"], "source_sha256": {name: sha256(source.parent / name) for name in ("summary.json", "runs.csv", "latency_histogram.csv")}}
    checked = 0
    for plot in m["plots"]:
        required_fields(plot, ("path", "sources", "series"), "plot")
        require(resolve(plot["path"]).is_file(), "plot: missing generated figure")
        require(isinstance(plot["sources"], list) and isinstance(plot["series"], list), "plot: invalid source/series arrays")
        plot_sources = {resolve(p) for p in plot["sources"]}
        require(plot_sources <= set(sources), "plot: undeclared source")
        for series in plot["series"]:
            kind = series.get("kind")
            if kind in {"run_metric", "histogram_quantile", "paired_run_ratio", "histogram_distribution", "frequency_fraction"}:
                required_fields(series, ("source_files", "filters"), "plot series")
                require(isinstance(series["source_files"], list) and isinstance(series["filters"], dict), "plot: invalid files/filters")
                files = [resolve(p) for p in series["source_files"]]
                require(bool(files) and len(files) == len(set(files)) and set(files) <= plot_sources, "plot: duplicate/undeclared series sources")
                require(all(p in tables for p in files), "plot: study series must reference audited raw CSVs")
                rows = [r for p in files for r in tables[p] if matches(r, series["filters"])]
                require(bool(rows), "plot: empty selected series")
                if kind == "frequency_fraction":
                    require(series.get("table") == "diagnostics" and all(p.name == "diagnostics.csv" for p in files), "plot: wrong fraction source table")
                    require(series.get("field") in {"action", "result"}, "plot: invalid frequency fraction field")
                    numerator = sum(r["count"] for r in rows if r[series["field"]] == series.get("category"))
                    denominator = sum(r["count"] for r in rows)
                    integer(series.get("numerator"), "plot.numerator")
                    integer(series.get("denominator"), "plot.denominator", 1)
                    require((series["numerator"], series["denominator"]) == (numerator, denominator), "plot: fraction counts differ")
                    close(series.get("value"), Decimal(numerator) / denominator, "plot frequency fraction")
                    checked += 1
                    continue
                integer(series.get("n"), "plot.n", 1)
                if kind in {"histogram_quantile", "histogram_distribution"}:
                    require(series.get("table") in {"histogram", "diagnostics"}, "plot: invalid histogram table")
                    require(all(p.name in ({"histogram.csv", "latency_histogram.csv"} if series["table"] == "histogram" else {"diagnostics.csv"}) for p in files), "plot: wrong frequency source table")
                    bins = Counter()
                    for row in rows:
                        bins[row["latency_ns"]] += row["count"]
                    require(series["n"] == sum(bins.values()), "plot: frequency sample count differs")
                    if kind == "histogram_distribution":
                        values = series.get("values")
                        require(isinstance(values, list), "plot: distribution requires pairs")
                        for pair in values:
                            require(isinstance(pair, list) and len(pair) == 2, "plot: distribution requires value/count pairs")
                            integer(pair[0], "plot.latency_ns")
                            integer(pair[1], "plot.count", 1)
                        require(values == [[latency, bins[latency]] for latency in sorted(bins)], "plot: frequency distribution differs from raw bins")
                    else:
                        q = decimal(series.get("q"), "plot.q", True)
                        require(q <= 1, "plot: quantile greater than one")
                        num, den = q.as_integer_ratio()
                        integer(series.get("value"), "plot.quantile")
                        require(series["value"] == percentile(bins, num, den), "plot: quantile differs from raw frequency rank")
                else:
                    metric = series.get("metric")
                    require(metric in RUN_COLUMNS[6:] and metric != "checksum", "plot: invalid numeric run metric")
                    require(all(p.name == "runs.csv" for p in files), "plot: wrong run source table")
                    if kind == "run_metric":
                        require(series.get("aggregation") == "median", "plot: unsupported aggregation")
                        values = [r[metric] for r in rows]
                    else:
                        required_fields(series, ("numerator_engine", "denominator_engine"), "plot ratio")
                        require(series["numerator_engine"] != series["denominator_engine"], "plot: identical ratio engines")
                        pairs = defaultdict(dict)
                        for row in rows:
                            if row["engine"] in {series["numerator_engine"], series["denominator_engine"]}:
                                key = tuple(row[k] for k in PREFIX) + (row["run"],)
                                require(row["engine"] not in pairs[key], "plot: duplicate paired run")
                                pairs[key][row["engine"]] = row[metric]
                        require(bool(pairs) and all(set(v) == {series["numerator_engine"], series["denominator_engine"]} for v in pairs.values()), "plot: incomplete engine pairs")
                        values = []
                        for pair in pairs.values():
                            denominator = decimal(pair[series["denominator_engine"]], "plot denominator", True)
                            values.append(decimal(pair[series["numerator_engine"]], "plot numerator") / denominator)
                    require(series["n"] == len(values), "plot: run sample count differs")
                    for field, expected in (("value", median(values)), ("min", min(values)), ("max", max(values))):
                        require(field in series, f"plot: missing {field}")
                        close(series[field], expected, f"plot.{field}")
            elif kind == "csv_rows":
                required_fields(series, ("source", "x", "y", "filters", "values"), "plot CSV series")
                location = resolve(series["source"])
                require(location in plot_sources and location in tables, "plot: unaudited CSV series")
                require(isinstance(series["y"], list) and isinstance(series["filters"], dict) and isinstance(series["values"], list), "plot: invalid CSV series fields")
                columns = [series["x"], *series["y"]]
                rows = [r for r in tables[location] if matches(r, series["filters"])]
                require(all(set(columns) <= set(r) for r in rows), "plot: unknown CSV axis")
                expected = [[r[k] for k in columns] for r in rows]
                require(len(series["values"]) == len(expected), "plot: CSV plotted row count differs")
                for actual, original in zip(series["values"], expected):
                    require(isinstance(actual, list) and len(actual) == len(original), "plot: CSV row shape differs")
                    require(all(type(a) is type(b) and a == b for a, b in zip(actual, original)), "plot: CSV plotted values differ from raw rows")
            elif kind in {"json_aggregate", "json_ratio_aggregate"}:
                required_fields(series, ("source_files", "filters", "aggregation", "value", "min", "max", "n"), "plot JSON aggregate")
                require(isinstance(series["source_files"], list) and isinstance(series["filters"], dict), "plot: invalid JSON files/filters")
                files = [resolve(p) for p in series["source_files"]]
                require(bool(files) and len(files) == len(set(files)) and set(files) <= plot_sources, "plot: duplicate/undeclared JSON series sources")
                require(series["aggregation"] == "median", "plot: unsupported JSON aggregation")
                def dotted(value, metric):
                    require(isinstance(metric, str) and metric, "plot: invalid JSON metric path")
                    for field in metric.split("."):
                        require(isinstance(value, dict) and field in value, "plot: missing JSON metric path")
                        value = value[field]
                    return decimal(value, "plot JSON metric")
                values = []
                for source in files:
                    row = read_json(source)
                    require(isinstance(row, dict), "plot: JSON aggregate source must be an object")
                    if not matches(row, series["filters"]):
                        continue
                    if kind == "json_aggregate":
                        values.append(dotted(row, series.get("metric")))
                    else:
                        numerator = dotted(row, series.get("numerator_metric"))
                        denominator = dotted(row, series.get("denominator_metric"))
                        require(denominator > 0, "plot: JSON ratio denominator must be positive")
                        values.append(numerator / denominator)
                require(bool(values), "plot: no selected JSON aggregate sources")
                integer(series["n"], "plot.n", 1)
                require(series["n"] == len(values), "plot: JSON aggregate sample count differs")
                for field, expected in (("value", median(values)), ("min", min(values)), ("max", max(values))):
                    close(series[field], expected, f"plot JSON {field}")
            elif kind == "json_metric":
                required_fields(series, ("source", "metric", "value"), "plot JSON series")
                location = resolve(series["source"])
                require(location in plot_sources, "plot: undeclared JSON series")
                value = read_json(location)
                require(isinstance(series["metric"], str), "plot: invalid JSON metric path")
                for field in series["metric"].split("."):
                    require(isinstance(value, dict) and field in value, "plot: missing JSON metric path")
                    value = value[field]
                close(series["value"], value, "plot JSON value")
            else:
                raise VerificationError(f"plot: unsupported series kind {kind}")
            checked += 1
    return {"path": str(path), "source_files": len(sources), "figures": len(m["plots"]), "checked_series": checked, "sha256": sha256(path), "core_evidence_audits": core_audits}


LIMITATIONS = [
    "Internal consistency and SHA256 provenance do not establish independent hardware measurements or detect coordinated fabrication of all source files.",
    "Aggregate snapshots/checksums cannot independently reconstruct FIFO order identities or reexecute the command stream; engine snapshot equality is performed by the C++ harness.",
    "Diagnostic latency includes the separate observer pass and is not an isolated process() cycle count or allocator proof.",
    "Min/max run ranges describe observed variation; they are not confidence intervals or evidence of live exchange latency/profitability.",
    "No exchange timestamps or accepted/submitted per-command quantities are stored: timeline ordering is checked by sequence and conservation only at the final aggregate state.",
    "Ancillary json_metric figures are checked against hashed source values; their telemetry semantics require their own tests.",
]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--study-root", type=Path, required=True)
    parser.add_argument("--plot-manifest", type=Path)
    parser.add_argument("--out", type=Path, help="Optional audit JSON output file")
    args = parser.parse_args(argv)
    try:
        reports, tables = verify_study_root(args.study_root)
        report = {"schema_version": 1, "status": "verified", "studies": reports, "totals": {"study_directories": len(reports), "engine_runs": sum(r["engine_runs"] for r in reports), "latency_observations": sum(r["latency_observations"] for r in reports), "diagnostic_observations": sum(r["diagnostic_observations"] for r in reports)}, "limitations": LIMITATIONS}
        if args.plot_manifest:
            report["plot_manifest"] = verify_plot_manifest(args.plot_manifest, tables)
        serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(serialized, encoding="utf-8")
        print(serialized, end="")
    except (OSError, ValueError, ArithmeticError, KeyError, TypeError) as error:
        print(f"study verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
