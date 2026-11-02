"""The MCP suite's coverage map: for each test file, the engine and editor sources its editors executed
(what tools/check-changed selects MCP tests by).

Recorded by a full run of a coverage-instrumented editor, off the gate's critical path (a batch
landing's run, a quiet server):

    COVERAGE=1 SKIP_ENGINE=1 tools/build.sh linux             # build/linux/editor-coverage(-install)
    # each test file's (or chunk's) editors write their .gcda under <dir>/<file>-<n>/ (GCOV_PREFIX):
    JASPER_EDITOR=build/linux/editor-coverage-install/bin/jasperengine-editor \\
      JASPER_MCP_COVERAGE_DIR=build/coverage/runs tools/mcp/run-tests.sh -j 6 --split-seconds 600
    python3 tools/mcp/coverage_map.py collect build/coverage/runs build/linux/editor-coverage \\
      --commit $(git rev-parse HEAD) --out build/coverage/coverage-map.json

A source counts as executed by a file when one of its objects' .gcda files has a counter above zero
(a translation unit: the .cpp and, through the editor tree's dependency database, what it includes).
The map: {"commit", "created", "tests": N, "files": {"test_x.py": ["engine/core/Foo.cpp", ...]}}.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

GCDA_MAGIC = 0x67636461            # "gcda"
TAG_FUNCTION = 0x01000000
TAG_COUNTER_BASE = 0x01a10000      # the arc counters


def gcda_executed(data: bytes) -> bool:
    """Whether a .gcda (GCC's coverage counts of one object) has a counter above zero. GCC 11 and
    older: record lengths in 4-byte words; GCC 12 and newer: in bytes (and a header checksum); a
    negative counter record length: that many counters, all zero, none written."""
    if len(data) < 12:
        return False
    for order in ("<", ">"):
        magic, = struct.unpack_from(order + "I", data, 0)
        if magic == GCDA_MAGIC:
            break
    else:
        return False
    version, = struct.unpack_from(order + "I", data, 4)
    # GCC's version "B04*" (11.4): its first character the major, '0' + n below 10, 'A' + n - 10 above
    first = chr((version >> 24) & 0xff)
    major = ord(first) - ord("A") + 10 if first.isalpha() else ord(first) - ord("0")
    bytes_lengths = major >= 12
    pos = 16 if bytes_lengths else 12   # GCC 12 added a checksum to the header
    while pos + 8 <= len(data):
        tag, length = struct.unpack_from(order + "Ii", data, pos)
        pos += 8
        if tag == 0:
            break
        if tag == TAG_COUNTER_BASE:   # the arc counters (what --coverage counts)
            if length < 0:
                continue   # all zero, none written
            size = length if bytes_lengths else length * 4
            for k in range(0, size - size % 8, 8):
                lo, hi = struct.unpack_from(order + "II", data, pos + k)
                if lo or hi:
                    return True
            pos += size
            continue
        if length < 0:
            continue
        pos += length if bytes_lengths else length * 4
    return False


def object_sources(tree: Path, deps_text: str | None = None) -> dict[str, list[str]]:
    """object (relative to the tree) -> its source and headers, repository-relative (ninja -t deps)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    import check_changed   # (its ninja deps parser)
    if deps_text is None:
        deps_text = subprocess.run(["ninja", "-C", str(tree), "-t", "deps"], capture_output=True, text=True).stdout
    return check_changed.parse_ninja_deps(deps_text, tree)


def collect(runs: Path, tree: Path, deps: dict[str, list[str]], prefix_in_run: str) -> dict[str, list[str]]:
    """runs/<file stem>-<n>/<prefix_in_run>/<object>.gcda -> {test file: [executed sources]}."""
    by_file: dict[str, set[str]] = {}
    for run in sorted(p for p in runs.iterdir() if p.is_dir()):
        test = run.name.rsplit("-", 1)[0] + ".py"
        executed = by_file.setdefault(test, set())
        base = run / prefix_in_run.lstrip("/")
        for directory, _, names in os.walk(base):
            for name in names:
                if not name.endswith(".gcda"):
                    continue
                path = Path(directory) / name
                try:
                    if not gcda_executed(path.read_bytes()):
                        continue
                except OSError:
                    continue
                obj = str(path.relative_to(base))[:-len(".gcda")] + ".o"
                sources = deps.get(obj)
                if sources:
                    executed.add(sources[0])
    return {k: sorted(v) for k, v in sorted(by_file.items())}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect")
    c.add_argument("runs", type=Path, help="JASPER_MCP_COVERAGE_DIR of the run")
    c.add_argument("tree", type=Path, help="the instrumented editor's build tree (build/linux/editor-coverage)")
    c.add_argument("--commit", required=True)
    c.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    tree = args.tree.resolve()
    deps = object_sources(tree)
    if not deps:
        print(f"coverage_map: no dependency database in {tree}", file=sys.stderr)
        return 1
    files = collect(args.runs, tree, deps, str(tree))
    if not files or not any(files.values()):
        print(f"coverage_map: no executed source under {args.runs}", file=sys.stderr)
        return 1
    data = {"commit": args.commit, "created": time.time(), "tree": str(tree), "tests": len(files), "files": files}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    tmp = args.out.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=1))
    os.replace(tmp, args.out)
    print(f"coverage map: {len(files)} test files, {len(set().union(*map(set, files.values())))} sources executed; {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
