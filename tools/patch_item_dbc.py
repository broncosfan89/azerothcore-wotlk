#!/usr/bin/env python3
"""
Patch WotLK Item.dbc by cloning source entries into Mythic IDs.
"""

from __future__ import annotations

import argparse
import os
import struct
from typing import Dict, List, Tuple


DEFAULT_SOURCE_ITEM_IDS: Tuple[int, ...] = (
    35570,
    35571,
    35572,
    35573,
    35574,
    35575,
    35576,
    35577,
    35578,
)
MYTHIC_MIN_LEVEL = 0
MYTHIC_MAX_LEVEL = 15
TARGET_ITEM_BASE = 70000000
TARGET_ITEM_STRIDE = 1000000


def load_source_ids(path: str | None) -> Tuple[int, ...]:
    if not path:
        return DEFAULT_SOURCE_ITEM_IDS

    source_ids: List[int] = []
    with open(path, "r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            source_ids.append(int(line))

    if not source_ids:
        raise RuntimeError(f"No source IDs loaded from {path}")

    return tuple(dict.fromkeys(source_ids))


def build_mapping(source_ids: Tuple[int, ...]) -> Tuple[Tuple[int, int], ...]:
    mapping: List[Tuple[int, int]] = []
    for level in range(MYTHIC_MIN_LEVEL, MYTHIC_MAX_LEVEL + 1):
        for source_id in source_ids:
            target_id = TARGET_ITEM_BASE + (level * TARGET_ITEM_STRIDE) + source_id
            mapping.append((source_id, target_id))
    return tuple(mapping)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Patch Item.dbc with custom item ID clones.")
    parser.add_argument("--input", required=True, help="Path to input Item.dbc")
    parser.add_argument("--output", required=True, help="Path to output Item.dbc")
    parser.add_argument("--source-list", help="Optional text file with one source item ID per line")
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


def patch_records(records: List[Tuple[int, ...]], mapping: Tuple[Tuple[int, int], ...]) -> List[Tuple[int, ...]]:
    by_id: Dict[int, Tuple[int, ...]] = {rec[0]: rec for rec in records}

    # Ensure all source rows exist.
    missing = [src for src, _ in mapping if src not in by_id]
    if missing:
        raise RuntimeError(f"Source entries missing in Item.dbc: {missing}")

    target_ids = {dst for _, dst in mapping}
    kept = [rec for rec in records if rec[0] not in target_ids]

    # Clone source records to target IDs.
    for src, dst in mapping:
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
    source_ids = load_source_ids(args.source_list)
    mapping = build_mapping(source_ids)

    _, _, _, _, _, records = read_dbc(args.input)
    patched = patch_records(records, mapping)
    write_dbc(args.output, patched)

    if args.in_place:
        write_dbc(args.input, patched)

    print(f"Patched Item.dbc written to: {args.output}")
    if args.in_place:
        print(f"Patched input in place: {args.input}")
    print(f"Added/updated entries for {len(source_ids)} source items across Mythic levels {MYTHIC_MIN_LEVEL}..{MYTHIC_MAX_LEVEL}")
    if args.source_list:
        print(f"Source list file: {args.source_list}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
