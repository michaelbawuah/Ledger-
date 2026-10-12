#!/usr/bin/env python3
"""Generate reproducible version-1 feed parser seeds without native layouts."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


HEADER_SIZE = 32
RECORD_SIZE = 40
MAGIC = b"LGRFEED\0"
MAX_U64 = (1 << 64) - 1


def header(count: int) -> bytes:
    result = struct.pack("<8sHHHHQQ", MAGIC, 1, HEADER_SIZE, RECORD_SIZE, 0, count, 0)
    assert len(result) == HEADER_SIZE
    return result


def record(sequence: int = 1, order_id: int = 7, price: int = 100,
           quantity: int = 3, side: int = 0, action: int = 0, tif: int = 0) -> bytes:
    result = struct.pack("<QQqIBBBBQ", sequence, order_id, price, quantity,
                         side, action, tif, 0, 0)
    assert len(result) == RECORD_SIZE
    return result


def mutate(data: bytes, offset: int, value: int = 255) -> bytes:
    result = bytearray(data)
    result[offset] = value
    return bytes(result)


def make_seeds() -> dict[str, bytes]:
    seeds: dict[str, bytes] = {}
    basic_record = record()
    basic_header = header(1)
    mixed_records = [basic_record,
                     record(2, 8, 0, 2, 1, 1, 1),
                     record(3, 7, 101, 4, 0, 3, 2)]
    mixed = header(len(mixed_records)) + b"".join(mixed_records)

    seeds["feed_valid_empty"] = header(0)
    seeds["feed_valid_one"] = basic_header + basic_record
    seeds["feed_valid_mixed"] = mixed
    # Full feeds exercise preflight separately from standalone field decoders.
    for length in range(len(mixed)):
        seeds[f"feed_truncated_{length:03d}"] = mixed[:length]
    seeds["feed_trailing_byte"] = mixed + b"\0"
    seeds["feed_trailing_record"] = mixed + basic_record
    seeds["feed_invalid_last_enum"] = mutate(mixed, len(mixed) - RECORD_SIZE + 29)
    seeds["feed_invalid_last_reserved"] = mutate(mixed, len(mixed) - 1, 1)
    seeds["feed_count_short"] = header(2) + basic_record
    seeds["feed_count_long"] = header(0) + basic_record
    seeds["feed_count_maximum"] = header(MAX_U64)

    for length in range(HEADER_SIZE):
        seeds[f"header_truncated_{length:02d}"] = basic_header[:length]
    for offset in list(range(16)) + list(range(24, HEADER_SIZE)):
        seeds[f"header_corrupt_{offset:02d}"] = mutate(basic_header, offset)
    for count in (0, 1, 2, 20_000_000, 20_000_001, 1 << 63, MAX_U64):
        seeds[f"header_count_{count}"] = header(count)
    seeds["header_trailing_byte"] = basic_header + b"\0"

    for length in range(RECORD_SIZE):
        seeds[f"record_truncated_{length:02d}"] = basic_record[:length]
    for side in range(2):
        for action in range(4):
            for tif in range(3):
                seeds[f"record_valid_s{side}_a{action}_t{tif}"] = record(
                    side=side, action=action, tif=tif)
    for price in (-(1 << 63), -1, 0, (1 << 63) - 1):
        seeds[f"record_extreme_price_{price}"] = record(
            MAX_U64, MAX_U64, price, (1 << 32) - 1, 1, 3, 2)
    seeds["record_engine_semantic_errors"] = record(0, 0, -100, 0)
    for offset, first_invalid in ((28, 2), (29, 4), (30, 3)):
        for value in (first_invalid, 255):
            seeds[f"record_invalid_enum_{offset}_{value}"] = mutate(basic_record, offset, value)
    for offset in range(31, RECORD_SIZE):
        seeds[f"record_reserved_{offset:02d}"] = mutate(basic_record, offset, 1)
    seeds["record_trailing_byte"] = basic_record + b"\0"
    return seeds


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="Directory for deterministic .seed files")
    args = parser.parse_args()
    seeds = make_seeds()
    args.directory.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(seeds.items()):
        (args.directory / f"{name}.seed").write_bytes(data)
    print(f"Wrote {len(seeds)} seeds ({sum(map(len, seeds.values()))} bytes) to {args.directory}")


if __name__ == "__main__":
    main()
