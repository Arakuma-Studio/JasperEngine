#!/usr/bin/env python3
"""What takes the space in a native library (docs/specs/S30-shipping.md, shipped size): the
symbols of an unstripped library (a player runtime's symbols/, the build's own output) grouped by
the source directory their debug information names: engine/libs/<library>, engine/core/<area>,
engine/renders/<backend>, engine/platform/<platform>, the C++ standard library's templates, and
the rest. Code and data symbols count (nm types t/T, d/D, b/B, r/R, w/W, v/V, u).

    python3 tools/players/lib_size.py <library.so> [--nm NM] [--top 40] [--json out.json]
    python3 tools/players/lib_size.py --from-nm <nm-output.txt> ...   (the output of
        nm --print-size --size-sort --line-numbers --demangle <library>)

NM defaults to llvm-nm, then nm. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

LINE = re.compile(r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([A-Za-z])\s+(.*?)(?:\t(\S+?):\d+)?$")
KINDS = set("tTdDbBrRwWvVu")


def group(path: str | None) -> str:
    """The group a source path belongs to."""
    if not path:
        return "(no line information)"
    p = path.replace("\\", "/")
    if "/c++/v1/" in p or "/include/c++/" in p or "/libcxx/" in p:
        return "C++ standard library (templates)"
    if "/sysroot/" in p or p.startswith("/usr/include/"):
        return "system headers"
    for marker, depth in (("engine/libs/", 1), ("engine/core/", 1), ("engine/renders/", 1), ("engine/platform/", 1),
                          ("engine/player/", 0), ("engine/project/", 0), ("/project/", 0), ("editor/", 0)):
        at = p.find(marker)
        if at >= 0:
            rest = p[at + len(marker):].split("/")
            if depth == 0 or len(rest) <= 1:
                return marker.strip("/") if depth == 0 else marker + "(top level)"
            # the libraries a library vendors (shadercompiler/libs/glslang): their own group
            if len(rest) > 3 and rest[1] == "libs" and marker == "engine/libs/":
                return marker + rest[0] + "/" + rest[2]
            return marker + rest[0]
    if "/.cxx/" in p or "/build/" in p:
        return "generated (build tree)"
    return "other: " + "/".join(p.split("/")[-3:-1])


def parse(lines) -> dict[str, list[int]]:
    """{group: [bytes, symbols]}"""
    groups: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    for line in lines:
        m = LINE.match(line.rstrip("\n"))
        if not m or m.group(3) not in KINDS:
            continue
        size = int(m.group(2), 16)
        g = groups[group(m.group(5))]
        g[0] += size
        g[1] += 1
    return dict(groups)


def run_nm(library: Path, nm: str | None) -> list[str]:
    tool = nm or shutil.which("llvm-nm") or shutil.which("nm")
    if not tool:
        sys.exit("no nm: pass --nm")
    out = subprocess.run([tool, "--print-size", "--size-sort", "--line-numbers", "--demangle", str(library)],
                         capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        sys.exit(f"{tool} failed: {out.stderr[-2000:]}")
    return out.stdout.splitlines()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("library", nargs="?", type=Path)
    ap.add_argument("--from-nm", type=Path, help="read nm's output instead of running it")
    ap.add_argument("--nm", help="the nm to run (default llvm-nm, then nm)")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    if args.from_nm:
        lines = args.from_nm.read_text(errors="replace").splitlines()
    elif args.library:
        lines = run_nm(args.library, args.nm)
    else:
        ap.error("a library or --from-nm")
    groups = parse(lines)
    total = sum(v[0] for v in groups.values()) or 1
    ranked = sorted(groups.items(), key=lambda kv: -kv[1][0])
    print(f"{'bytes':>12} {'share':>6} {'symbols':>8}  group")
    for name, (size, count) in ranked[:args.top]:
        print(f"{size:>12,} {100.0 * size / total:>5.1f}% {count:>8,}  {name}")
    rest = ranked[args.top:]
    if rest:
        print(f"{sum(v[0] for _, v in rest):>12,} {100.0 * sum(v[0] for _, v in rest) / total:>5.1f}% "
              f"{sum(v[1] for _, v in rest):>8,}  ({len(rest)} more groups)")
    print(f"{total:>12,} 100.0% {sum(v[1] for v in groups.values()):>8,}  total (symbol sizes)")
    if args.json:
        args.json.write_text(json.dumps({name: {"bytes": v[0], "symbols": v[1]} for name, v in ranked}, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
