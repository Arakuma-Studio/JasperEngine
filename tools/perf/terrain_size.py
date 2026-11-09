#!/usr/bin/env python3
"""What the terrain engine adds to a player (docs/specs/T1-terrain-authoring.md R54, AC40;
docs/perf/terrain.md): the growth of a Linux glcore player's linked .text and .rodata, and of the web
`st` player's gzipped .wasm, between a base build and a head build.

    python3 tools/perf/terrain_size.py --base-players DIR --head-players DIR [--json out.json]
    python3 tools/perf/terrain_size.py --base-linux FILE --head-linux FILE [--base-web FILE --head-web FILE]

A players directory is what tools/players/build_linux_player.sh or `tools/build.sh players` writes
(linux-x64-glcore-release*/files/jasperengine-player, web-*/files/jasperengine-player-st.wasm); either
platform may be missing from both, and is then reported as not measured. The base is the merge base
(R54): build the same recipe on a copy of that commit, e.g. from the worktree root, inside the linux image:

    mkdir -p build/terrain-size/base-src && git archive <base> | tar -x -C build/terrain-size/base-src
    tools/build.sh exec linux bash -c 'cd build/terrain-size/base-src && tools/players/build_linux_player.sh \\
        --engine-version 0 glcore release /src/build/terrain-size/base-tree /src/build/terrain-size/base-players'
    tools/build.sh exec linux tools/players/build_linux_player.sh --engine-version 0 glcore release \\
        /src/build/terrain-size/head-tree /src/build/terrain-size/head-players

Prints each measure with its budget and exits 1 when one is over, 2 when nothing could be measured.

    python3 tools/perf/terrain_size.py --t1-share PLAYER.debug [--t1-share OTHER.debug]

counts T1's own share of one player instead: the .text and .rodata symbols whose debug line information
points into T1's sources (T1_SOURCES), from `nm -S -l` (binutils) on the player's split debug file, so a
player built without the runtime edit module (JASPERENGINE_TERRAIN_EDIT=OFF) and one with it can be
compared against R54's 96 KiB. Standard library and nm only.
"""

from __future__ import annotations

import argparse
import gzip
import json
import struct
import subprocess
import sys
from pathlib import Path

KIB = 1024
# R54: Linux glcore .text + .rodata, and the web st player gzipped
LINUX_BUDGET = 96 * KIB
WEB_BUDGET = 32 * KIB
SECTIONS = (".text", ".rodata")


# T1's sources (docs/specs/T1-terrain-authoring.md D-41): the operation engine, its stores, the surface
# query, the op JSON, the runtime edit targets and uploads, the Lua terrain bindings, the collider refresh
T1_SOURCES = (
    "engine/core/terrain/TerrainOps", "engine/core/terrain/OverlayMapStore", "engine/core/terrain/RawMapStore",
    "engine/core/terrain/TerrainMapStore", "engine/core/terrain/TerrainShape", "engine/core/terrain/TerrainStroke",
    "engine/core/terrain/TerrainPlacement", "engine/core/terrain/TerrainSurface", "engine/core/terrain/TerrainErosion",
    "engine/core/terrain/TerrainLayers", "engine/core/object/TerrainEdit", "engine/core/object/TerrainOpsJson",
    "engine/core/script/binding/TerrainClassesLua", "engine/core/physics/JoltTerrainRefresh",
    "engine/core/subsystem/MeshSystemTerrainEdit",
)


def t1_share(debug_file: Path) -> tuple[int, dict[str, int]]:
    """Bytes of .text/.rodata symbols defined in T1's sources, in all and by source."""
    run = subprocess.run(["nm", "-S", "-l", "--defined-only", str(debug_file)], capture_output=True, text=True, check=True)
    by_source: dict[str, int] = {}
    total = 0
    for line in run.stdout.splitlines():
        parts = line.split("\t")
        fields = parts[0].split()
        if len(fields) < 4 or len(parts) < 2 or fields[2] not in "tTrR":
            continue
        where = parts[1]
        for source in T1_SOURCES:
            if source in where:
                size = int(fields[1], 16)
                total += size
                by_source[source] = by_source.get(source, 0) + size
                break
    return total, by_source


def elf_sections(path: Path) -> dict[str, int]:
    """Section name -> size of an ELF file (32 or 64-bit, either byte order)."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF file")
    is64 = data[4] == 2
    order = "<" if data[5] == 1 else ">"
    if is64:
        shoff, = struct.unpack_from(order + "Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(order + "HHH", data, 0x3A)
    else:
        shoff, = struct.unpack_from(order + "I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(order + "HHH", data, 0x2E)

    def header(index: int) -> tuple[int, int, int]:
        base = shoff + index * shentsize
        if is64:
            name, _, _, _, offset, size = struct.unpack_from(order + "IIQQQQ", data, base)
        else:
            name, _, _, _, offset, size = struct.unpack_from(order + "IIIIII", data, base)
        return name, offset, size

    _, names_offset, _ = header(shstrndx)
    sections: dict[str, int] = {}
    for index in range(shnum):
        name, _, size = header(index)
        end = data.index(b"\0", names_offset + name)
        sections[data[names_offset + name:end].decode("ascii", "replace")] = size
    return sections


def linux_bytes(path: Path) -> int:
    sections = elf_sections(path)
    missing = [name for name in SECTIONS if name not in sections]
    if missing:
        raise ValueError(f"{path}: no {', '.join(missing)} section")
    return sum(sections[name] for name in SECTIONS)


def web_bytes(path: Path) -> int:
    return len(gzip.compress(path.read_bytes(), 9))


def find_linux(players: Path) -> Path | None:
    found = sorted(players.glob("linux-x64-glcore-release*/files/jasperengine-player"))
    return found[0] if found else None


def find_web(players: Path) -> Path | None:
    found = sorted(players.glob("web-*/files/jasperengine-player-st.wasm"))
    return found[0] if found else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base-players", type=Path)
    parser.add_argument("--head-players", type=Path)
    parser.add_argument("--base-linux", type=Path)
    parser.add_argument("--head-linux", type=Path)
    parser.add_argument("--base-web", type=Path)
    parser.add_argument("--head-web", type=Path)
    parser.add_argument("--json", type=Path, help="write the numbers here")
    parser.add_argument("--t1-share", type=Path, action="append", default=[], help="a player's .debug file: T1's own bytes")
    args = parser.parse_args()

    if args.t1_share:
        shares = {}
        over = 0
        for debug_file in args.t1_share:
            total, by_source = t1_share(debug_file)
            shares[str(debug_file)] = {"bytes": total, "by_source": by_source, "budget": LINUX_BUDGET}
            ok = total <= LINUX_BUDGET
            over += 0 if ok else 1
            print(f"  {'ok  ' if ok else 'FAIL'}  T1's share of {debug_file}: {total / KIB:.1f} KiB (budget {LINUX_BUDGET / KIB:.0f} KiB)")
            for source, size in sorted(by_source.items(), key=lambda item: -item[1]):
                print(f"          {size / KIB:8.1f} KiB  {source}")
        if args.json:
            args.json.write_text(json.dumps(shares, indent=2) + "\n")
        return 1 if over else 0

    pairs = {
        "linux": (args.base_linux, args.head_linux),
        "web": (args.base_web, args.head_web),
    }
    if args.base_players and args.head_players:
        if pairs["linux"] == (None, None):
            pairs["linux"] = (find_linux(args.base_players), find_linux(args.head_players))
        if pairs["web"] == (None, None):
            pairs["web"] = (find_web(args.base_players), find_web(args.head_players))

    measures = {
        "linux": ("Linux glcore player, .text + .rodata", linux_bytes, LINUX_BUDGET),
        "web": ("web st player, .wasm gzipped -9", web_bytes, WEB_BUDGET),
    }
    out: dict[str, dict] = {}
    over = 0
    measured = 0
    for key, (label, measure, budget) in measures.items():
        base, head = pairs[key]
        if not base or not head or not base.is_file() or not head.is_file():
            print(f"  not measured  {label}: base {base or '-'}, head {head or '-'}")
            continue
        before, after = measure(base), measure(head)
        growth = after - before
        ok = growth <= budget
        measured += 1
        over += 0 if ok else 1
        out[key] = {"base": before, "head": after, "growth": growth, "budget": budget, "base_file": str(base), "head_file": str(head)}
        print(f"  {'ok  ' if ok else 'FAIL'}  {label}: {before} -> {after} bytes, growth {growth / KIB:+.1f} KiB "
              f"(budget {budget / KIB:.0f} KiB)")
    if args.json:
        args.json.write_text(json.dumps(out, indent=2) + "\n")
    if not measured:
        print("nothing measured")
        return 2
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main())
