"""The MCP suite's coverage map: for each test file, the engine and editor sources its editors executed
(what tools/check-changed selects MCP tests by).

Recorded by a full run of a coverage-instrumented editor, off the gate's critical path (a batch
landing's run, a quiet server):

    COVERAGE=1 SKIP_ENGINE=1 tools/build.sh linux             # build/linux/editor-coverage(-install)
    # each test file's (or chunk's) editors write their .gcda under <dir>/<file>-<n>/ (GCOV_PREFIX):
    JASPER_EDITOR=build/linux/editor-coverage-install/bin/jasperengine-editor \\
      JASPER_MCP_COVERAGE_DIR=build/coverage/runs JASPER_SLOW_TESTS=1 tools/mcp/run-tests.sh -j 4 --split-seconds 900
    python3 tools/mcp/coverage_map.py collect build/coverage/runs build/linux/editor-coverage \\
      --commit $(git rev-parse HEAD) --out build/coverage/coverage-map.json

A source file counts for a test file when its editors executed a function written in it (a counter
above zero in the object's .gcda; the object's .gcno says which file each function is in, a header's for
an inline function) that fewer than 80% of the files with an editor executed: start-up and static
registrations run in every editor and say nothing. A source only those reach is in no list, so
check-changed takes it as unmapped and adds the CI suite.
The map: {"commit", "created", "tests": N, "files": {"test_x.py": ["engine/core/Foo.cpp", ...]}}.
"""

from __future__ import annotations

import argparse
import collections
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
GCNO_MAGIC = 0x67636e6f            # "gcno"
GCNO_NO_PAYLOAD = (0x01410000,)    # the blocks record: a count, no words
COMMON_SHARE = 0.8                 # of the files with an editor: a function that many execute is any editor's
MIN_EDITOR_FUNCTIONS = 1000        # a file that executed fewer started no editor


def gcda_functions(data: bytes) -> set[int]:
    """The functions (their idents) of a .gcda (GCC's coverage counts of one object) with a counter
    above zero. GCC 11 and older: record lengths in 4-byte words; GCC 12 and newer: in bytes (and a
    header checksum); a negative counter record length: that many counters, all zero, none written."""
    hit: set[int] = set()
    if len(data) < 12:
        return hit
    for order in ("<", ">"):
        magic, = struct.unpack_from(order + "I", data, 0)
        if magic == GCDA_MAGIC:
            break
    else:
        return hit
    version, = struct.unpack_from(order + "I", data, 4)
    # GCC's version "B04*" (11.4): its first character the major, '0' + n below 10, 'A' + n - 10 above
    first = chr((version >> 24) & 0xff)
    major = ord(first) - ord("A") + 10 if first.isalpha() else ord(first) - ord("0")
    bytes_lengths = major >= 12
    pos = 16 if bytes_lengths else 12   # GCC 12 added a checksum to the header
    ident = None
    while pos + 8 <= len(data):
        tag, length = struct.unpack_from(order + "Ii", data, pos)
        pos += 8
        if tag == 0:
            break
        if length < 0:
            continue   # all zero, none written
        size = length if bytes_lengths else length * 4
        if tag == TAG_FUNCTION:
            ident = struct.unpack_from(order + "I", data, pos)[0] if size >= 4 else None
        elif tag == TAG_COUNTER_BASE and ident is not None:   # the arc counters (what --coverage counts)
            for k in range(0, size - size % 8, 8):
                lo, hi = struct.unpack_from(order + "II", data, pos + k)
                if lo or hi:
                    hit.add(ident)
                    break
        pos += size
    return hit


def gcda_executed(data: bytes) -> bool:
    return bool(gcda_functions(data))


def gcno_functions(data: bytes) -> dict[int, tuple[str, str]]:
    """A .gcno (what GCC knows of one object's functions): ident -> (name, source file), the file
    the function is written in, a header's for an inline function. GCC 8 to 11's layout: header,
    current directory, a flag, then records; strings are a length in words and their bytes."""
    if len(data) < 16:
        return {}
    for order in ("<", ">"):
        if struct.unpack_from(order + "I", data, 0)[0] == GCNO_MAGIC:
            break
    else:
        return {}

    def string(at: int) -> tuple[str, int]:
        words, = struct.unpack_from(order + "I", data, at)
        at += 4
        raw = data[at:at + words * 4]
        return raw.split(b"\0", 1)[0].decode("utf-8", "replace"), at + words * 4

    pos = 12
    cwd, pos = string(pos)
    pos += 4   # (support_unexecuted_blocks)
    found: dict[int, tuple[str, str]] = {}
    while pos + 8 <= len(data):
        tag, length = struct.unpack_from(order + "Ii", data, pos)
        pos += 8
        if tag == 0:
            break
        size = abs(length) * 4 if tag not in GCNO_NO_PAYLOAD else 0
        if tag == TAG_FUNCTION:
            ident, = struct.unpack_from(order + "I", data, pos)
            name, at = string(pos + 12)
            file, _ = string(at + 4)   # (past "artificial")
            found[ident] = (name, file if file.startswith("/") else os.path.join(cwd, file))
        pos += size
    return found


def object_sources(tree: Path, deps_text: str | None = None) -> dict[str, list[str]]:
    """object (relative to the tree) -> its source and headers, repository-relative (ninja -t deps)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    import check_changed   # (its ninja deps parser)
    if deps_text is None:
        deps_text = subprocess.run(["ninja", "-C", str(tree), "-t", "deps"], capture_output=True, text=True).stdout
    return check_changed.parse_ninja_deps(deps_text, tree)


def repo_relative(path: str, tree: Path) -> str | None:
    """A source's path in the repository, or None for one outside it (a system header)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    import check_changed
    relative = check_changed.normalize(path, tree)
    return None if relative.startswith("/") or relative.startswith("build/") else relative


def collect(runs: Path, tree: Path, read_gcno=None) -> dict[str, list[str]]:
    """runs/<file stem>-<n>/<the tree's path>/<object>.gcda and the tree's <object>.gcno ->
    {test file: [source files it executed]}, headers included, for an inline function runs in the
    file it is written in. What every editor runs (start-up, static registrations: a function at
    least COMMON_SHARE of the files with an editor executed) tells nothing of what a file tests, and
    counts for no file: a source only those reach is unmapped, and check-changed adds the CI suite."""
    if read_gcno is None:
        read_gcno = lambda relative: (tree / relative).read_bytes()   # noqa: E731
    functions: dict[str, dict[int, tuple[str, str]]] = {}
    executed: dict[str, set[tuple[str, str]]] = {}
    prefix = str(tree).lstrip("/")
    for run in sorted(p for p in runs.iterdir() if p.is_dir()):
        test = run.name.rsplit("-", 1)[0] + ".py"
        hits = executed.setdefault(test, set())
        base = run / prefix
        for directory, _, names in os.walk(base):
            for name in names:
                if not name.endswith(".gcda"):
                    continue
                path = Path(directory) / name
                relative = str(path.relative_to(base))[:-len(".gcda")]
                if relative not in functions:
                    try:
                        functions[relative] = gcno_functions(read_gcno(relative + ".gcno"))
                    except OSError:
                        functions[relative] = {}
                try:
                    idents = gcda_functions(path.read_bytes())
                except OSError:
                    continue
                known = functions[relative]
                hits.update(known[i] for i in idents if i in known)
    with_editor = [hits for hits in executed.values() if len(hits) >= MIN_EDITOR_FUNCTIONS]
    counts = collections.Counter(item for hits in with_editor for item in hits)
    limit = max(2, COMMON_SHARE * len(with_editor))
    common = {item for item, n in counts.items() if n >= limit}
    result: dict[str, list[str]] = {}
    relative: dict[str, str | None] = {}
    for test, hits in sorted(executed.items()):
        files = set()
        for _, file in hits - common:
            if file not in relative:
                relative[file] = repo_relative(file, tree)
            if relative[file]:
                files.add(relative[file])
        result[test] = sorted(files)
    return result


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
    files = collect(args.runs, tree)
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
