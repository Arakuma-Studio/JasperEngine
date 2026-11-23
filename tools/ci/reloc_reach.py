#!/usr/bin/env python3
"""Prints how much of x86-64's 2 GiB PC-relative reach an ELF64 binary's loaded image uses.

    python3 tools/ci/reloc_reach.py <binary>

The small code model reaches every symbol with a signed 32-bit offset from the code that uses it, so the
link fails ("relocation truncated to fit", lld's "out of range") once the span from the first section code
refers to (.rodata) to the end of the last (.bss) passes 2 GiB; the dynamic linker's tables (.rela.dyn,
.dynsym, ...) load too, but no code refers to them PC-relative. The asan (editor) lane prints this for its
test binary, the largest we link. Always exits 0 (a measurement, not a gate), 2 on bad arguments.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

SHF_ALLOC = 0x2
SHF_TLS = 0x400
# the loader's tables: string, symbol, hash, note, version and relocation sections
LOADER_TYPES = {2, 3, 4, 5, 7, 9, 11, 0x6FFFFFF6, 0x6FFFFFFD, 0x6FFFFFFE, 0x6FFFFFFF}
REACH = 2 ** 31


def sections(f) -> list[tuple[str, int, int, int, int]]:
    """(name, type, flags, address, size) of every section of a little-endian ELF64 file, read from its
    headers only (the binary can be several GB)."""
    header = f.read(64)
    if header[:4] != b"\x7fELF" or header[4] != 2 or header[5] != 1:
        raise ValueError("not a little-endian ELF64 file")
    shoff, = struct.unpack_from("<Q", header, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", header, 0x3A)
    f.seek(shoff)
    table = f.read(shentsize * shnum)
    headers = [struct.unpack_from("<IIQQQQ", table, i * shentsize) for i in range(shnum)]
    f.seek(headers[shstrndx][4])
    strtab = f.read(headers[shstrndx][5])
    return [(strtab[name:strtab.index(b"\0", name)].decode(errors="replace"), kind, flags, addr, size)
            for name, kind, flags, addr, _offset, size in headers]


def report(path: Path) -> str:
    with path.open("rb") as f:
        found = sections(f)
    loaded = [s for s in found
              if s[2] & SHF_ALLOC and not s[2] & SHF_TLS and s[4] and s[1] not in LOADER_TYPES]
    start = min(s[3] for s in loaded)
    end = max(s[3] + s[4] for s in loaded)
    span = end - start
    big = sorted(loaded, key=lambda s: -s[4])[:4]
    sizes = ", ".join(f"{s[0]} {s[4] / 2**20:,.0f} MiB" for s in big)
    return (f"{path.name}: code-referenced image {span / 2**30:.3f} GiB of the 2 GiB relocation reach, "
            f"{100 * (REACH - span) / REACH:.1f} % headroom ({sizes})")


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    path = Path(argv[0])
    try:
        print(report(path))
    except (OSError, ValueError, struct.error) as error:
        print(f"reloc_reach: {path}: {error}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
