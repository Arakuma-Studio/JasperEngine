"""The MCP suite in pieces: chunks (one editor each) and shards (one server each), and one report.

A file's tests share one editor and run in source order, so a file is one piece of work, unless it
declares TESTS_INDEPENDENT = "why" (each of its tests sets up what it needs and runs correctly in
an editor of its own, in any order), or TESTS_INDEPENDENT = ("why", ["test_x_*"]) for some of its
tests: then those may be split into chunks, each run by its own worker process and editor (the
file's other tests stay together, in order, in a chunk of their own). That is what keeps one long file (an hour of software-rendered
examples) from being the whole run's wall time.

    # the chunks of one server's run (run_tests.py --split-seconds 600 does this itself)
    # the shards of a run across servers (je-remote/mcp-shard.sh drives it):
    python3 tools/mcp/shard.py plan --tests all-tests.txt --timings LOG_OR_JSON... \\
        --shard A:7 --shard H:8:gpu --shard B:6 --pin-builds A --out DIR
    python3 tools/mcp/shard.py check --tests all-tests.txt DIR/plan.json      # the union check
    python3 tools/mcp/shard.py merge --plan DIR/plan.json DIR/mcp-*.json      # one report, exit code

The union check: every test `run_tests.py --list` lists (MCP_SUITE=all) is in exactly one chunk
of exactly one shard, and no chunk names a test that does not exist; `merge` checks the same of
the results (every planned test reported once, nothing else), so no test is lost or run twice.

Timings come from earlier runs: their logs (the runner's `PASS file::test (12.3s)` lines) or their
--json results; the last file given wins for a test. A test with no time counts DEFAULT_SECONDS.
Longest processing time first: chunks sorted by time, each to the worker (of any shard it may run
on) that finishes it first. Pinned chunks go to their shard only: the files that build games
(PREBUILT_PLAYERS, or a compile: BUILD_FILES) to --pin-builds, where the shared player cache and
the compile's ccache are warm. A shard marked gpu (H, the real-GPU lane) also runs the tests
tools/mcp/gpu_lane.json classes "gpu" on its GPU (docs/testing.md, "The real-GPU lane"); every
other test there runs on llvmpipe as everywhere.
"""

from __future__ import annotations

import argparse
import ast
import fnmatch
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESTS = HERE / "tests"
GPU_LANE = HERE / "gpu_lane.json"
DEFAULT_SECONDS = 30.0
# an editor's start, its project copy and its close: what a chunk costs besides its tests
CHUNK_OVERHEAD = 20.0
# the files that compile a game (method "compile", docs/testing.md "Compiled and prebuilt game builds")
BUILD_FILES = ("test_build.py", "test_html_ui_effects_export.py", "test_hub.py", "test_busy.py", "test_lifecycle_export.py")
LINE = re.compile(r"^(PASS|FAIL|SKIP) (\S+?\.py)::(\S+) \(([\d.]+)s\)")


# ---------------------------------------------------------------------------- inputs

def module_value(path: Path, name: str):
    """A module-level NAME = <constant or list of constants> of a test file (None if absent)."""
    for node in ast.parse(path.read_text(encoding="utf-8"), str(path)).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            try:
                return ast.literal_eval(node.value)
            except ValueError:
                return None
    return None


def independent_patterns(path: Path) -> list[str]:
    """The tests of a file that may run in editors of their own (TESTS_INDEPENDENT): ["*"] for a
    file that declares TESTS_INDEPENDENT = "why", the patterns of TESTS_INDEPENDENT = ("why",
    ["test_x_*", ...]) (those tests use nothing the file's other tests leave, nor do the others use
    theirs), [] for a file without it."""
    value = module_value(path, "TESTS_INDEPENDENT")
    if isinstance(value, str) and value.strip():
        return ["*"]
    if isinstance(value, (tuple, list)) and len(value) == 2 and isinstance(value[0], str) and value[0].strip() \
            and isinstance(value[1], (tuple, list)) and all(isinstance(p, str) for p in value[1]):
        return list(value[1])
    return []


def independent(path: Path) -> bool:
    return bool(independent_patterns(path))


def builds_games(path: Path) -> bool:
    return path.name in BUILD_FILES or bool(module_value(path, "PREBUILT_PLAYERS"))


def load_tests(path: Path) -> list[str]:
    """`run_tests.py --list` output: file::test lines, in run order."""
    return [line.strip() for line in path.read_text().splitlines() if "::" in line and not line.startswith("run_tests")]


def load_timings(paths: list[Path]) -> dict[str, float]:
    times: dict[str, float] = {}
    for p in paths:
        text = p.read_text(errors="replace")
        if p.suffix == ".json":
            data = json.loads(text)
            if isinstance(data, dict) and "results" in data:
                for r in data["results"]:
                    times[r["name"]] = float(r["seconds"])
            elif isinstance(data, dict):   # {name: seconds}
                times.update({k: float(v) for k, v in data.items() if "::" in k})
            continue
        for line in text.splitlines():
            m = LINE.match(line)
            if m:
                times[f"{m.group(2)}::{m.group(3)}"] = float(m.group(4))
    return times


def gpu_classes(path: Path = GPU_LANE) -> dict[str, str]:
    """file or file::test -> "gpu" (its checks hold on any GPU), "llvmpipe" (they do not) ..."""
    if not path.is_file():
        return {}
    data = json.loads(path.read_text())
    return {name: entry["class"] for name, entry in data.get("tests", {}).items()}


def gpu_ok(name: str, classes: dict[str, str]) -> bool:
    file = name.split("::")[0]
    return classes.get(name, classes.get(file)) == "gpu"


# ---------------------------------------------------------------------------- chunks

@dataclass
class Chunk:
    file: str
    tests: list[str]          # test names (without the file)
    seconds: float
    pin: str = ""             # the shard it must run on ("" any)
    gpu: bool = False         # it may run on a gpu shard's GPU
    lane: str = ""            # "gpu" once placed on a gpu shard and run on its GPU
    shard: str = ""
    index: int = 0
    of: int = 1

    def names(self) -> list[str]:
        return [f"{self.file}::{t}" for t in self.tests]

    def to_json(self) -> dict:
        return {"file": self.file, "tests": self.tests, "seconds": round(self.seconds, 1), "lane": self.lane,
                "chunk": f"{self.index + 1}/{self.of}"}


def make_chunks(tests: list[str], times: dict[str, float], chunk_seconds: float, tests_dir: Path = TESTS,
                pin_builds: str = "", classes: dict[str, str] | None = None) -> list[Chunk]:
    """The suite's chunks: a file whole, or an independent file's tests in consecutive groups of
    about chunk_seconds (a longer test alone)."""
    classes = classes or {}
    by_file: dict[str, list[str]] = {}
    for name in tests:
        file, _, test = name.partition("::")
        by_file.setdefault(file, []).append(test)
    chunks: list[Chunk] = []
    for file, names in by_file.items():
        path = tests_dir / file
        seconds = [times.get(f"{file}::{t}", DEFAULT_SECONDS) for t in names]
        pin = pin_builds if pin_builds and path.is_file() and builds_games(path) else ""
        groups: list[list[int]] = []
        patterns = independent_patterns(path) if path.is_file() else []
        free = [i for i, t in enumerate(names) if any(fnmatch.fnmatchcase(t, p) for p in patterns)]
        if free and sum(seconds) > chunk_seconds:
            # the tests that depend on each other: one chunk, in order; the independent ones in
            # consecutive groups of about chunk_seconds (a longer test alone)
            rest = [i for i in range(len(names)) if i not in free]
            if rest:
                groups.append(rest)
            current: list[int] = []
            for i in free:
                if current and sum(seconds[j] for j in current) + seconds[i] > chunk_seconds:
                    groups.append(current)
                    current = []
                current.append(i)
            groups.append(current)
        else:
            groups = [list(range(len(names)))]
        for k, group in enumerate(groups):
            members = [names[i] for i in group]
            chunks.append(Chunk(file=file, tests=members, seconds=CHUNK_OVERHEAD + sum(seconds[i] for i in group),
                                pin=pin, gpu=all(gpu_ok(f"{file}::{t}", classes) for t in members),
                                index=k, of=len(groups)))
    return chunks


# ---------------------------------------------------------------------------- shards

@dataclass
class Shard:
    name: str
    workers: int
    gpu: bool = False
    speed: float = 1.0        # a test's time there is its reference time x speed
    gpu_speed: float = 1.0    # a gpu-lane test's time on its GPU, x its reference time
    finish: list[float] = field(default_factory=list)
    chunks: list[Chunk] = field(default_factory=list)


def parse_shard(spec: str) -> Shard:
    """NAME:WORKERS[:gpu][:speed=F][:gpu_speed=F]"""
    parts = spec.split(":")
    if len(parts) < 2 or not parts[1].isdigit() or int(parts[1]) < 1:
        raise SystemExit(f"shard.py: --shard NAME:WORKERS[:gpu][:speed=F][:gpu_speed=F], not {spec!r}")
    shard = Shard(parts[0], int(parts[1]))
    for p in parts[2:]:
        if p == "gpu":
            shard.gpu = True
        elif p.startswith("speed="):
            shard.speed = float(p[6:])
        elif p.startswith("gpu_speed="):
            shard.gpu_speed = float(p[10:])
        else:
            raise SystemExit(f"shard.py: unknown shard option {p!r} in {spec!r}")
    shard.finish = [0.0] * shard.workers
    return shard


def assign(chunks: list[Chunk], shards: list[Shard]) -> None:
    """Longest processing time first: each chunk to the worker that would finish it first."""
    names = {s.name for s in shards}
    for c in chunks:
        if c.pin and c.pin not in names:
            raise SystemExit(f"shard.py: {c.file} is pinned to {c.pin}, which is no shard")
    for c in sorted(chunks, key=lambda c: (-c.seconds, c.file, c.index)):
        best = None
        for s in shards:
            if c.pin and s.name != c.pin:
                continue
            lane = "gpu" if (s.gpu and c.gpu) else ""
            cost = c.seconds * (s.gpu_speed if lane else s.speed)
            w = min(range(s.workers), key=lambda i: s.finish[i])
            end = s.finish[w] + cost
            if best is None or end < best[0]:
                best = (end, s, w, lane)
        end, s, w, lane = best
        s.finish[w] = end
        c.shard, c.lane = s.name, lane
        s.chunks.append(c)


def plan(tests: list[str], times: dict[str, float], shards: list[Shard], chunk_seconds: float,
         pin_builds: str = "", tests_dir: Path = TESTS, classes: dict[str, str] | None = None) -> dict:
    chunks = make_chunks(tests, times, chunk_seconds, tests_dir, pin_builds, classes)
    assign(chunks, shards)
    out = {"tests": len(tests), "chunk_seconds": chunk_seconds, "shards": {}}
    for s in shards:
        s.chunks.sort(key=lambda c: -c.seconds)
        out["shards"][s.name] = {
            "workers": s.workers, "gpu": s.gpu, "estimate_seconds": round(max(s.finish or [0.0]), 1),
            "test_seconds": round(sum(c.seconds for c in s.chunks), 1),
            "chunks": [c.to_json() for c in s.chunks],
        }
    out["estimate_seconds"] = max(v["estimate_seconds"] for v in out["shards"].values())
    return out


# ---------------------------------------------------------------------------- checks and the report

def planned_names(plan_data: dict) -> list[str]:
    return [f"{c['file']}::{t}" for s in plan_data["shards"].values() for c in s["chunks"] for t in c["tests"]]


def union_problems(expected: list[str], planned: list[str], what: str = "planned") -> list[str]:
    """Every expected test exactly once in `planned`, and nothing else."""
    problems = []
    seen: dict[str, int] = {}
    for n in planned:
        seen[n] = seen.get(n, 0) + 1
    want = set(expected)
    if len(want) != len(expected):
        problems.append(f"the test list itself names {len(expected) - len(want)} test(s) twice")
    for n in expected:
        if n not in seen:
            problems.append(f"MISSING {n} (not {what})")
    for n, k in seen.items():
        if k > 1:
            problems.append(f"DUPLICATE {n} ({what} {k} times)")
        if n not in want:
            problems.append(f"UNKNOWN {n} ({what}, but not in the suite)")
    return problems


def merge(plan_data: dict, results: list[dict], allow_skip: list[str]) -> tuple[int, str]:
    """One report of the shards' --json results, in the runner's summary format; its exit code."""
    lines = []
    all_results = [r for res in results for r in res.get("results", [])]
    crashed = [c for res in results for c in res.get("crashed", [])]
    problems = union_problems(planned_names(plan_data), [r["name"] for r in all_results], "reported")
    n = {s: sum(1 for r in all_results if r["status"] == s) for s in ("pass", "fail", "skip")}
    unexpected_skips = [r for r in all_results if r["status"] == "skip" and not any(a in r["name"] for a in allow_skip)]
    lines.append("== summary")
    for r in all_results:
        if r["status"] == "fail":
            lines.append(f"FAIL {r['name']}")
    for r in unexpected_skips:
        lines.append(f"SKIP {r['name']}  {r.get('detail', '')[:200]}")
    for c in crashed:
        lines.append(f"CRASH {c}")
    lines += problems
    wall = max((res.get("seconds", 0.0) for res in results), default=0.0)
    lines.append(f"{n['pass']} passed, {n['fail']} failed, {n['skip']} skipped"
                 f"{', ' + str(len(crashed)) + ' crashed' if crashed else ''} in {wall:.1f}s"
                 f" (the longest of {len(results)} shard(s); {sum(r['seconds'] for r in all_results) / 3600:.2f} test-hours)")
    if problems:
        lines.append(f"shard.py: the union check failed: {len(problems)} problem(s)")
    bad = n["fail"] or crashed or problems or unexpected_skips or not all_results
    return (1 if bad else 0), "\n".join(lines)


# ---------------------------------------------------------------------------- command line

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan", help="split the suite into shards")
    p.add_argument("--tests", type=Path, required=True, help="run_tests.py --list output (MCP_SUITE=all)")
    p.add_argument("--timings", type=Path, nargs="*", default=[], help="earlier runs' logs or --json results")
    p.add_argument("--shard", action="append", required=True, help="NAME:WORKERS[:gpu][:speed=F][:gpu_speed=F]")
    p.add_argument("--pin-builds", default="", help="the shard that runs the files that build games")
    p.add_argument("--chunk-seconds", type=float, default=600.0, help="an independent file's chunk size")
    p.add_argument("--out", type=Path, required=True, help="plan.json and plan-<shard>.json go here")
    c = sub.add_parser("check", help="the union check of a plan against the suite's test list")
    c.add_argument("--tests", type=Path, required=True)
    c.add_argument("plan", type=Path)
    m = sub.add_parser("merge", help="one report of the shards' results")
    m.add_argument("--plan", type=Path, required=True)
    m.add_argument("--allow-skip", action="append", default=[], help="a skip whose name contains this is expected")
    m.add_argument("results", type=Path, nargs="+")
    args = ap.parse_args(argv)

    if args.cmd == "plan":
        tests = load_tests(args.tests)
        if not tests:
            print(f"shard.py: no tests in {args.tests}", file=sys.stderr)
            return 2
        shards = [parse_shard(s) for s in args.shard]
        data = plan(tests, load_timings(args.timings), shards, args.chunk_seconds, args.pin_builds,
                    classes=gpu_classes())
        problems = union_problems(tests, planned_names(data))
        if problems:
            print("\n".join(problems), file=sys.stderr)
            return 1
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "plan.json").write_text(json.dumps(data, indent=1))
        for name, s in data["shards"].items():
            (args.out / f"plan-{name}.json").write_text(json.dumps({"shard": name, "chunks": s["chunks"]}, indent=1))
            print(f"shard {name}: {s['workers']} workers, {len(s['chunks'])} chunks, "
                  f"{sum(len(c['tests']) for c in s['chunks'])} tests, {s['test_seconds'] / 3600:.2f} test-hours, "
                  f"estimate {s['estimate_seconds'] / 60:.1f} min")
        print(f"plan: {len(tests)} tests in {sum(len(s['chunks']) for s in data['shards'].values())} chunks, "
              f"estimate {data['estimate_seconds'] / 60:.1f} min; union check: every test exactly once")
        return 0
    if args.cmd == "check":
        tests = load_tests(args.tests)
        problems = union_problems(tests, planned_names(json.loads(args.plan.read_text())))
        print("\n".join(problems) if problems else f"union check: {len(tests)} tests, each in exactly one chunk")
        return 1 if problems else 0
    results = []
    for path in args.results:
        try:
            results.append(json.loads(path.read_text()))
        except (OSError, ValueError) as e:
            print(f"shard.py: {path}: {e}", file=sys.stderr)
            results.append({"results": [], "crashed": [f"{path.name} (no results: {e})"], "seconds": 0.0})
    code, report = merge(json.loads(args.plan.read_text()), results, args.allow_skip)
    print(report)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
