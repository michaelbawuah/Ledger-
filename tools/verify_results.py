#!/usr/bin/env python3
"""Verify Ledger's core benchmark evidence offline using only the standard library."""
import csv
import json
import re
from decimal import Decimal
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
    return value


def csv_integer(value, name, minimum=0):
    require(isinstance(value, str) and re.fullmatch(r"[0-9]+", value) is not None,
            f"{name}: expected a decimal integer")
    return integer(int(value), name, minimum)


def positive_decimal(value, name):
    require(isinstance(value, (int, Decimal)) and not isinstance(value, bool),
            f"{name}: expected a number")
    number = Decimal(value)
    require(number.is_finite() and number > 0, f"{name}: expected a finite positive number")
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
    for key in ("seed", "warmup_commands", "generated_peak_live_orders",
                "generated_empty_book_cancels", "generated_capacity_ioc_limits"):
        integer(summary[key], key)
    require(summary["input"] is None or isinstance(summary["input"], str), "input: expected string or null")
    require(isinstance(summary["method"], str) and bool(summary["method"]), "method: expected nonempty text")
    exact_keys(summary["config"], {"min_price", "max_price", "max_orders"}, "config")
    for key in summary["config"]:
        integer(summary["config"][key], f"config.{key}", 1)
    require(summary["config"]["min_price"] <= summary["config"]["max_price"], "config: inverted price range")
    exact_keys(summary["mix"], {"limit", "cancel", "market", "replace"}, "mix")
    for key, value in summary["mix"].items():
        integer(value, f"mix.{key}")
    require(sum(summary["mix"].values()) == summary["commands"], "mix: counts do not sum to commands")
    exact_keys(summary["engines"], ENGINES, "engines")
    for engine, metrics in summary["engines"].items():
        exact_keys(metrics, ENGINE_KEYS, engine)
        for key, value in metrics.items():
            if key == "median_commands_per_second":
                positive_decimal(value, f"{engine}.{key}")
            else:
                integer(value, f"{engine}.{key}")
    return summary


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
