#!/usr/bin/env python3
"""Tests of tools/ci/reloc_reach.py (python3 -I tools/ci/test_reloc_reach.py), on ELF64 headers written here:
only the file header, the section headers and their name table, as the script reads no more."""

from __future__ import annotations

import importlib.util
import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("reloc_reach", HERE / "reloc_reach.py")
reloc_reach = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = reloc_reach
spec.loader.exec_module(reloc_reach)

MIB = 2 ** 20
ALLOC, WRITE_ALLOC, TLS = 0x2, 0x3, 0x403


def elf(sections: list[tuple[str, int, int, int, int]]) -> bytes:
    """A little-endian ELF64 file with these (name, type, flags, address, size) sections after the null one."""
    names = b"\0" + b"".join(s[0].encode() + b"\0" for s in sections) + b".shstrtab\0"
    entries = [(0, 0, 0, 0, 0, 0)]
    offset = 1
    for name, kind, flags, addr, size in sections:
        entries.append((offset, kind, flags, addr, 0, size))
        offset += len(name) + 1
    strtab_at = 64
    shoff = strtab_at + len(names)
    entries.append((offset, 3, 0, 0, strtab_at, len(names)))
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", header, 0x28, shoff)
    struct.pack_into("<HHH", header, 0x3A, 64, len(entries), len(entries) - 1)
    table = b"".join(struct.pack("<IIQQQQ", *e) + bytes(64 - 40) for e in entries)
    return bytes(header) + names + table


class RelocReach(unittest.TestCase):
    def run_on(self, data: bytes) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "jasperengine-editor-tests"
            path.write_bytes(data)
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(reloc_reach.main([str(path)]), 0)
            return out.getvalue()

    def test_spans_rodata_to_bss_without_the_loaders_tables(self):
        text = self.run_on(elf([
            (".rela.dyn", 4, ALLOC, 0x1000, 900 * MIB),
            (".dynsym", 11, ALLOC, 900 * MIB + 0x1000, 10 * MIB),
            (".rodata", 1, ALLOC, 1024 * MIB, 100 * MIB),
            (".text", 1, 0x6, 1124 * MIB, 400 * MIB),
            (".tbss", 8, TLS, 1524 * MIB, 4000 * MIB),
            (".data", 1, WRITE_ALLOC, 1524 * MIB, 500 * MIB),
            (".bss", 8, WRITE_ALLOC, 2024 * MIB, 20 * MIB),
            (".debug_info", 1, 0, 0, 5000 * MIB),
        ]))
        # .rodata at 1024 MiB to the end of .bss at 2044 MiB: 1020 MiB of 2048
        self.assertIn("code-referenced image 0.996 GiB of the 2 GiB relocation reach, 50.2 % headroom", text)
        self.assertIn("(.data 500 MiB, .text 400 MiB, .rodata 100 MiB, .bss 20 MiB)", text)

    def test_past_the_reach_the_headroom_is_negative(self):
        text = self.run_on(elf([(".rodata", 1, ALLOC, 0, 1000 * MIB), (".data", 1, WRITE_ALLOC, 1000 * MIB, 1200 * MIB)]))
        self.assertIn("2.148 GiB of the 2 GiB relocation reach, -7.4 % headroom", text)

    def test_not_an_elf_file_is_said_and_passes(self):
        self.assertIn("not a little-endian ELF64 file", self.run_on(b"#!/bin/sh\n"))

    def test_bad_arguments(self):
        self.assertEqual(reloc_reach.main([]), 2)


if __name__ == "__main__":
    unittest.main()
