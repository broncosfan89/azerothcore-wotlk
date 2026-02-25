#!/usr/bin/env python3
"""
Patch WotLK Item.dbc by cloning known source entries into custom IDs.

Use case:
- Source entries: 35570..35578 (Utgarde Keep heroic items)
- Target entries: 59001..59009 (Mythic custom clones)
"""

from __future__ import annotations

import argparse
import os
import struct
from typing import Dict, List, Tuple


MAPPING: Tuple[Tuple[int, int], ...] = (
    (35570, 59001),
    (35571, 59002),
    (35572, 59003),
    (35573, 59004),
    (35574, 59005),
    (35575, 59006),
    (35576, 59007),
    (35577, 59008),
    (35578, 59009),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Patch Item.dbc with custom item ID clones.")
    parser.add_argument("--input", required=True, help="Path to input Item.dbc")
    parser.add_argument("--output", required=True, help="Path to output Item.dbc")
    parser.add_argument("--in-place", action="store_true", help="Also overwrite the input file with patched output")
    return parser.parse_args()


def read_dbc(path: str) -> Tuple[bytes, int, int, int, int, List[Tuple[int, ...]]]:
    with open(path, "rb") as f:
        header = f.read(20)
        if len(header) != 20:
            raise RuntimeError("Invalid DBC header length")

        magic, record_count, field_count, record_size, string_size = struct.unpack("<4s4I", header)
        if magic != b"WDBC":
            raise RuntimeError(f"Unexpected magic {magic!r}, expected b'WDBC'")
        if field_count != 8 or record_size != 32:
            raise RuntimeError(
                f"Unexpected Item.dbc layout: field_count={field_count}, record_size={record_size}"
            )

        raw_records = f.read(record_count * record_size)
        if len(raw_records) != record_count * record_size:
            raise RuntimeError("Truncated DBC records")

        records: List[Tuple[int, ...]] = []
        for i in range(record_count):
            off = i * record_size
            records.append(struct.unpack("<8I", raw_records[off : off + record_size]))

        string_block = f.read(string_size)
        if len(string_block) != string_size:
            raise RuntimeError("Truncated DBC string block")

    return magic, record_count, field_count, record_size, string_size, records


def patch_records(records: List[Tuple[int, ...]]) -> List[Tuple[int, ...]]:
    by_id: Dict[int, Tuple[int, ...]] = {rec[0]: rec for rec in records}

    # Ensure all source rows exist.
    missing = [src for src, _ in MAPPING if src not in by_id]
    if missing:
        raise RuntimeError(f"Source entries missing in Item.dbc: {missing}")

    target_ids = {dst for _, dst in MAPPING}
    kept = [rec for rec in records if rec[0] not in target_ids]

    # Clone source records to target IDs.
    for src, dst in MAPPING:
        src_rec = by_id[src]
        cloned = (dst,) + src_rec[1:]
        kept.append(cloned)

    kept.sort(key=lambda r: r[0])
    return kept


def write_dbc(path: str, records: List[Tuple[int, ...]]) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(struct.pack("<4s4I", b"WDBC", len(records), 8, 32, 1))
        for rec in records:
            f.write(struct.pack("<8I", *rec))
        # Item.dbc has no string data in this client build; keep a 1-byte null block.
        f.write(b"\x00")


def main() -> int:
    args = parse_args()

    _, _, _, _, _, records = read_dbc(args.input)
    patched = patch_records(records)
    write_dbc(args.output, patched)

    if args.in_place:
        write_dbc(args.input, patched)

    print(f"Patched Item.dbc written to: {args.output}")
    if args.in_place:
        print(f"Patched input in place: {args.input}")
    print("Added/updated entries: 59001..59009 (cloned from 35570..35578)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
