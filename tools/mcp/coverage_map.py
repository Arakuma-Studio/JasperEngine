"""The MCP suite's coverage map: which test files' editors executed each function of the engine and the
editor (what tools/check-changed selects MCP tests by).

Recorded by a full run of a coverage-instrumented editor, off the gate's critical path (a batch
landing's run, a quiet server):

    COVERAGE=1 SKIP_ENGINE=1 tools/build.sh linux             # build/linux/editor-coverage(-install)
    # each test file's (or chunk's) editors write their .gcda under <dir>/<file>-<n>/ (GCOV_PREFIX):
    JASPER_EDITOR=build/linux/editor-coverage-install/bin/jasperengine-editor \\
      JASPER_MCP_COVERAGE_DIR=build/coverage/runs JASPER_SLOW_TESTS=1 tools/mcp/run-tests.sh -j 4 --split-seconds 900 \\
      --json build/coverage/mcp.json
    python3 tools/mcp/coverage_map.py collect build/coverage/runs build/linux/editor-coverage \\
      --commit $(git rev-parse HEAD) --results build/coverage/mcp.json --out build/coverage/coverage-map.json

A run split across machines (the nightly's shards, .github/workflows/nightly.yml) collects each
machine's runs on its own and joins the maps (all of one commit):

    python3 tools/mcp/coverage_map.py merge map-1.json map-2.json ... --out coverage-map.json

A function counts for a test file when one of its objects' .gcda has a counter above zero in it. The
object's .gcno says where each function is written: its file (a header's for an inline function) and
lines, which is what lets check-changed go from a diff's hunks to the functions they touch.

The map: {"version": 2, "commit", "created", "tests": [names], "editor_tests": N (those that started an
editor), "editor_test_names" (those, by name), "seconds": {test: s}, "sources": {file: {"functions": [[first line, last line, hex mask of the
tests that executed it], ...]}}}.
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
GCNO_MAGIC = 0x67636e6f            # "gcno"
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


def gcno_functions(data: bytes) -> dict[int, tuple[str, int, int]]:
    """A .gcno (what GCC knows of one object's functions): ident -> (source file, first line, last
    line), the file the function is written in, a header's for an inline function. GCC 11's layout
    (the Linux image's): header, current directory, a flag, then records; lengths and strings in
    4-byte words."""
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
    found: dict[int, tuple[str, int, int]] = {}
    while pos + 8 <= len(data):
        tag, length = struct.unpack_from(order + "Ii", data, pos)
        pos += 8
        if tag == 0:
            break
        if tag == TAG_FUNCTION:
            ident, = struct.unpack_from(order + "I", data, pos)
            _, at = string(pos + 12)   # (past its checksums: the name)
            file, at = string(at + 4)  # (past "artificial")
            first, _, last, _ = struct.unpack_from(order + "IIII", data, at)
            found[ident] = (file if file.startswith("/") else os.path.join(cwd, file), first, last)
        pos += abs(length) * 4
    return found


def repo_relative(path: str, tree: Path) -> str | None:
    """A source's path in the repository, or None for one outside it (a system header)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    import check_changed
    relative = check_changed.normalize(path, tree)
    return None if relative.startswith("/") or relative.startswith("build/") else relative


def collect(runs: Path, tree: Path, read_gcno=None) -> dict:
    """runs/<file stem>-<n>/<the tree's path>/<object>.gcda and the tree's <object>.gcno -> the map."""
    if read_gcno is None:
        read_gcno = lambda relative: (tree / relative).read_bytes()   # noqa: E731
    functions: dict[str, dict[int, tuple[str, int, int]]] = {}
    tests = sorted({p.name.rsplit("-", 1)[0] + ".py" for p in runs.iterdir() if p.is_dir()})
    index = {t: i for i, t in enumerate(tests)}
    masks: dict[tuple[str, int, int], int] = {}
    executed = [0] * len(tests)
    files: dict[str, str | None] = {}
    prefix = str(tree).lstrip("/")
    for run in sorted(p for p in runs.iterdir() if p.is_dir()):
        bit = 1 << index[run.name.rsplit("-", 1)[0] + ".py"]
        base = run / prefix
        for directory, _, names in os.walk(base):
            for name in names:
                if not name.endswith(".gcda"):
                    continue
                path = Path(directory) / name
                relative = str(path.relative_to(base))[:-len(".gcda")]
                if relative not in functions:
                    try:
                        known = gcno_functions(read_gcno(relative + ".gcno"))
                    except OSError:
                        known = {}
                    functions[relative] = {}
                    for ident, (file, first, last) in known.items():   # every function, executed or not
                        if file not in files:
                            files[file] = repo_relative(file, tree)
                        if files[file]:
                            key = (files[file], first, last)
                            functions[relative][ident] = key
                            masks.setdefault(key, 0)
                try:
                    idents = gcda_functions(path.read_bytes())
                except OSError:
                    continue
                keys = functions[relative]
                for ident in idents:
                    if ident in keys:
                        masks[keys[ident]] |= bit
                executed[index[run.name.rsplit("-", 1)[0] + ".py"]] += len(idents)
    sources: dict[str, dict] = {}
    for (file, first, last), mask in sorted(masks.items()):
        entry = sources.setdefault(file, {"functions": []})
        entry["functions"].append([first, last, format(mask, "x")])
    editor_tests = [t for t, n in zip(tests, executed) if n >= MIN_EDITOR_FUNCTIONS]
    return {"version": 2, "tests": tests, "editor_tests": len(editor_tests), "editor_test_names": editor_tests,
            "sources": sources}


def merge(maps: list[dict]) -> dict:
    """One map of several runs' maps of one commit (a run split across machines): the tests joined,
    each function's mask renumbered onto them; a test in more than one (its chunks ran apart) adds
    its seconds and counts once."""
    if len({m.get("commit") for m in maps}) != 1 or len({m.get("version") for m in maps}) != 1:
        raise ValueError("the maps are of different commits or versions: "
                         + ", ".join(f"{m.get('commit')} v{m.get('version')}" for m in maps))
    tests = sorted({t for m in maps for t in m["tests"]})
    index = {t: i for i, t in enumerate(tests)}
    masks: dict[tuple[str, int, int], int] = {}
    seconds: dict[str, float] = {}
    editor_tests: set[str] = set()
    for m in maps:
        bits = [1 << index[t] for t in m["tests"]]
        for file, entry in m["sources"].items():
            for first, last, mask in entry["functions"]:
                value, joined = int(mask, 16), 0
                for i, bit in enumerate(bits):
                    if value >> i & 1:
                        joined |= bit
                masks[(file, first, last)] = masks.get((file, first, last), 0) | joined
        for test, s in m.get("seconds", {}).items():
            seconds[test] = round(seconds.get(test, 0.0) + s, 1)
        editor_tests.update(m.get("editor_test_names", []))
    sources: dict[str, dict] = {}
    for (file, first, last), mask in sorted(masks.items()):
        sources.setdefault(file, {"functions": []})["functions"].append([first, last, format(mask, "x")])
    return {"version": maps[0]["version"], "commit": maps[0]["commit"], "created": max(m.get("created", 0) for m in maps),
            "tests": tests, "editor_tests": len(editor_tests), "editor_test_names": sorted(editor_tests),
            "seconds": seconds, "sources": sources}


def write(data: dict, out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp")
    tmp.write_text(json.dumps(data))
    os.replace(tmp, out)
    n = sum(len(e["functions"]) for e in data["sources"].values())
    print(f"coverage map: {len(data['tests'])} test files, {data['editor_tests']} with an editor, {len(data['sources'])} "
          f"sources, {n} functions, {out.stat().st_size // 1024} KiB; {out}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect")
    c.add_argument("runs", type=Path, help="JASPER_MCP_COVERAGE_DIR of the run")
    c.add_argument("tree", type=Path, help="the instrumented editor's build tree (build/linux/editor-coverage)")
    c.add_argument("--commit", required=True)
    c.add_argument("--results", type=Path, help="the run's --json results: each test file's time (the map's seconds)")
    c.add_argument("--out", type=Path, required=True)
    m = sub.add_parser("merge", help="one map of a split run's maps (each a collect of its own runs)")
    m.add_argument("maps", type=Path, nargs="+")
    m.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    if args.cmd == "merge":
        try:
            data = merge([json.loads(p.read_text()) for p in args.maps])
        except (OSError, ValueError, KeyError) as e:
            print(f"coverage_map: cannot merge: {e}", file=sys.stderr)
            return 1
        if not data["editor_tests"]:
            print("coverage_map: no test file of the maps executed anything", file=sys.stderr)
            return 1
        write(data, args.out)
        return 0
    tree = args.tree.resolve()
    data = collect(args.runs, tree)
    if not data["editor_tests"]:
        print(f"coverage_map: no test file executed anything under {args.runs}", file=sys.stderr)
        return 1
    data.update(commit=args.commit, created=time.time(), seconds={})
    if args.results and args.results.is_file():
        for r in json.loads(args.results.read_text()).get("results", []):
            file = r["name"].split("::")[0]
            data["seconds"][file] = round(data["seconds"].get(file, 0.0) + r["seconds"], 1)
    write(data, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
