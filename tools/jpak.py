#!/usr/bin/env python3
"""Inspects and unpacks a .jpak content pack (docs/specs/S30-shipping.md D6), for tests and people:

    python3 tools/jpak.py list content.jpak            # path, size, offset of every entry
    python3 tools/jpak.py verify content.jpak          # every entry's FNV-1a 64 against the index
    python3 tools/jpak.py extract content.jpak DIR     # the files, as the pack's paths say

Layout (little-endian): "JPAK\\0\\0\\0\\0", u32 version (1), u32 count, u64 index bytes; then per entry
u32 path length, the UTF-8 path, u64 offset, u64 size, u64 fnv1a64 of the bytes; then the blobs,
each at a 16-byte aligned offset. Paths are relative, '/'-separated, without '..' (an entry that
would escape DIR is refused). Exit 1 on a damaged pack. Standard library only.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path, PurePosixPath

MAGIC = b"JPAK\0\0\0\0"
VERSION = 1
HEADER = 24


class PackError(Exception):
    pass


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def read_index(data: bytes) -> list[tuple[str, int, int, int]]:
    if len(data) < HEADER or data[:8] != MAGIC:
        raise PackError("not a .jpak (bad magic)")
    version, count, index_bytes = struct.unpack_from("<IIQ", data, 8)
    if version != VERSION:
        raise PackError(f"version {version}, this tool reads {VERSION}")
    if HEADER + index_bytes > len(data):
        raise PackError("the index runs past the end of the file")
    entries = []
    at = HEADER
    for _ in range(count):
        if at + 4 > HEADER + index_bytes:
            raise PackError("the index is shorter than its count")
        (length,) = struct.unpack_from("<I", data, at)
        at += 4
        path = data[at:at + length].decode("utf-8")
        at += length
        offset, size, digest = struct.unpack_from("<QQQ", data, at)
        at += 24
        if offset % 16 or offset + size > len(data) or offset < HEADER + index_bytes:
            raise PackError(f"{path}: its bytes are outside the file or not 16-byte aligned")
        parts = PurePosixPath(path).parts
        if not path or path.startswith("/") or "\\" in path or ".." in parts or "." in parts:
            raise PackError(f"{path!r}: not a legal pack path")
        entries.append((path, offset, size, digest))
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=["list", "verify", "extract"])
    parser.add_argument("pack", type=Path)
    parser.add_argument("dest", type=Path, nargs="?")
    args = parser.parse_args()
    data = args.pack.read_bytes()
    try:
        entries = read_index(data)
        if args.action == "list":
            for path, offset, size, _ in entries:
                print(f"{size:12d}  {offset:12d}  {path}")
            print(f"{len(entries)} entries")
        elif args.action == "verify":
            bad = [path for path, offset, size, digest in entries if fnv1a64(data[offset:offset + size]) != digest]
            for path in bad:
                print(f"damaged: {path}", file=sys.stderr)
            if bad:
                return 1
            print(f"ok: {len(entries)} entries")
        else:
            if args.dest is None:
                parser.error("extract needs DIR")
            root = args.dest.resolve()
            for path, offset, size, digest in entries:
                blob = data[offset:offset + size]
                if fnv1a64(blob) != digest:
                    raise PackError(f"{path}: its bytes do not match the index")
                target = (root / path).resolve()
                if root not in target.parents:
                    raise PackError(f"{path}: escapes {root}")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(blob)
            print(f"extracted {len(entries)} entries into {root}")
    except PackError as error:
        print(f"jpak: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
