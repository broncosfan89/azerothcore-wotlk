#!/usr/bin/env python3
"""
Patch WotLK Spell.dbc by cloning an existing spell row into a custom ID
and overriding client-facing text/icon fields.
"""

from __future__ import annotations

import argparse
import os
import struct
from typing import Dict, List, Tuple


SOURCE_SPELL_ID = 66690
TARGET_SPELL_ID = 770001
TARGET_ICON_ID = 37  # Flamestrike icon
TARGET_NAME = "Volcanic Eruption"
TARGET_SUBTEXT = ""
TARGET_DESCRIPTION = "Erupts volcanic fire at the target location, dealing Fire damage every 1 sec for 10 sec."
TARGET_AURA_DESCRIPTION = ""

FIELD_COUNT = 234
RECORD_SIZE = 936
LOCALE_COUNT = 16

IDX_ID = 0
IDX_SPELL_ICON = 133
IDX_ACTIVE_ICON = 134
IDX_NAME_START = 136
IDX_NAME_MASK = 152
IDX_SUBTEXT_START = 153
IDX_SUBTEXT_MASK = 169
IDX_DESC_START = 170
IDX_DESC_MASK = 186
IDX_AURA_DESC_START = 187
IDX_AURA_DESC_MASK = 203


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Patch Spell.dbc with a custom Volcanic Eruption spell row.")
    parser.add_argument("--input", required=True, help="Path to input Spell.dbc")
    parser.add_argument("--output", required=True, help="Path to output Spell.dbc")
    parser.add_argument("--in-place", action="store_true", help="Also overwrite the input file with patched output")
    return parser.parse_args()


def read_dbc(path: str) -> Tuple[int, int, int, bytes, List[List[int]], bytes]:
    with open(path, "rb") as handle:
        header = handle.read(20)
        if len(header) != 20:
            raise RuntimeError("Invalid DBC header length")

        magic, record_count, field_count, record_size, string_size = struct.unpack("<4s4I", header)
        if magic != b"WDBC":
            raise RuntimeError(f"Unexpected magic {magic!r}, expected b'WDBC'")
        if field_count != FIELD_COUNT or record_size != RECORD_SIZE:
            raise RuntimeError(
                f"Unexpected Spell.dbc layout: field_count={field_count}, record_size={record_size}"
            )

        raw_records = handle.read(record_count * record_size)
        if len(raw_records) != record_count * record_size:
            raise RuntimeError("Truncated DBC records")

        string_block = handle.read(string_size)
        if len(string_block) != string_size:
            raise RuntimeError("Truncated DBC string block")

    records: List[List[int]] = []
    unpack_fmt = "<" + "I" * FIELD_COUNT
    for i in range(record_count):
        off = i * record_size
        records.append(list(struct.unpack(unpack_fmt, raw_records[off : off + record_size])))

    return record_count, field_count, record_size, magic, records, string_block


def add_string(string_block: bytearray, cache: Dict[str, int], value: str) -> int:
    if not value:
        return 0

    if value in cache:
        return cache[value]

    offset = len(string_block)
    encoded = value.encode("utf-8") + b"\x00"
    string_block.extend(encoded)
    cache[value] = offset
    return offset


def patch_records(records: List[List[int]], original_strings: bytes) -> Tuple[List[List[int]], bytes]:
    by_id: Dict[int, List[int]] = {row[IDX_ID]: row for row in records}
    if SOURCE_SPELL_ID not in by_id:
        raise RuntimeError(f"Source spell {SOURCE_SPELL_ID} not found in Spell.dbc")

    source_row = by_id[SOURCE_SPELL_ID]
    target_row = source_row.copy()
    target_row[IDX_ID] = TARGET_SPELL_ID
    target_row[IDX_SPELL_ICON] = TARGET_ICON_ID
    target_row[IDX_ACTIVE_ICON] = 0

    string_block = bytearray(original_strings)
    if not string_block:
        string_block = bytearray(b"\x00")
    elif string_block[-1] != 0:
        string_block.append(0)

    string_cache: Dict[str, int] = {}
    name_off = add_string(string_block, string_cache, TARGET_NAME)
    subtext_off = add_string(string_block, string_cache, TARGET_SUBTEXT)
    desc_off = add_string(string_block, string_cache, TARGET_DESCRIPTION)
    aura_desc_off = add_string(string_block, string_cache, TARGET_AURA_DESCRIPTION)

    for i in range(LOCALE_COUNT):
        target_row[IDX_NAME_START + i] = 0
        target_row[IDX_SUBTEXT_START + i] = 0
        target_row[IDX_DESC_START + i] = 0
        target_row[IDX_AURA_DESC_START + i] = 0

    target_row[IDX_NAME_START] = name_off
    target_row[IDX_SUBTEXT_START] = subtext_off
    target_row[IDX_DESC_START] = desc_off
    target_row[IDX_AURA_DESC_START] = aura_desc_off

    target_row[IDX_NAME_MASK] = 0
    target_row[IDX_SUBTEXT_MASK] = 0
    target_row[IDX_DESC_MASK] = 0
    target_row[IDX_AURA_DESC_MASK] = 0

    replaced = False
    for i, row in enumerate(records):
        if row[IDX_ID] == TARGET_SPELL_ID:
            records[i] = target_row
            replaced = True
            break

    if not replaced:
        records.append(target_row)

    records.sort(key=lambda row: row[IDX_ID])
    return records, bytes(string_block)


def write_dbc(path: str, records: List[List[int]], string_block: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(struct.pack("<4s4I", b"WDBC", len(records), FIELD_COUNT, RECORD_SIZE, len(string_block)))
        pack_fmt = "<" + "I" * FIELD_COUNT
        for row in records:
            handle.write(struct.pack(pack_fmt, *row))
        handle.write(string_block)


def main() -> int:
    args = parse_args()
    _, _, _, _, records, string_block = read_dbc(args.input)
    patched_records, patched_strings = patch_records(records, string_block)

    write_dbc(args.output, patched_records, patched_strings)
    if args.in_place:
        write_dbc(args.input, patched_records, patched_strings)

    print(f"Patched Spell.dbc written to: {args.output}")
    if args.in_place:
        print(f"Patched input in place: {args.input}")
    print(f"Custom spell row: {TARGET_SPELL_ID} cloned from {SOURCE_SPELL_ID}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
