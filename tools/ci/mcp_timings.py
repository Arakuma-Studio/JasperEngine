"""tools/ci/mcp_timings.py: the committed timings the MCP shards are planned by (tools/mcp/ci-timings.json),
and their refit from CI's own runs (.github/workflows/mcp-timings-refit.yml, daily).

    python3 tools/ci/mcp_timings.py fetch --repo OWNER/NAME --runs 20 --out DIR    # needs gh
    python3 tools/ci/mcp_timings.py refit --results DIR [--timings tools/mcp/ci-timings.json] [--write]
    python3 tools/ci/mcp_timings.py check [--timings tools/mcp/ci-timings.json]

fetch: the `mcp.json` of every `test-results-mcp-<k>` artifact of the newest ci.yml runs of this
repository's own branches (pull_request, merge_group, workflow_dispatch; not a fork's, not the
nightly's, whose shards build real games), until DIR holds RUNS runs: DIR/<run>/<artifact>.json.

refit: one sample per run of each file (its tests' seconds summed, when that run reported as many
tests of it as the newest run did: a crashed chunk's are not all there) and of each test of a
TESTS_INDEPENDENT file. Every reported test counts, whatever its status: a PR shard skips the game
builds, so their skip is what they cost there, and a failing test costs its shard what it took (the
mcp lane is advisory, and a few tests fail in every run: only passes would leave their files
unmeasured). A value is the median of its samples after the outliers (over OUTLIER_RATIO times or
under 1/OUTLIER_RATIO of the median, and more than OUTLIER_FLOOR seconds off: a timeout, a crash)
are dropped, and only from MIN_SAMPLES samples or more: one run says nothing. It replaces the
committed value only outside the band (BAND of it, at least BAND_SECONDS), or when a file's test
count changed. A file no longer in tools/mcp/tests goes, and so does a test no run reported (its
file's share covers it). Without --write it only says what would change; exit 0 either way, 2 on
bad input. --write rewrites the file only when an entry changed.

check: the file is there, parses, and names only files of tools/mcp/tests (tools/ci/test_mcp_timings.py
runs it in the lint lane).

How tools/mcp/shard.py reads the file: docs/testing.md, "How the MCP shards are balanced".
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
TESTS = ROOT / "tools" / "mcp" / "tests"
TIMINGS = ROOT / "tools" / "mcp" / "ci-timings.json"
sys.path.insert(0, str(ROOT / "tools" / "mcp"))
import shard  # noqa: E402

FORMAT = 1
MIN_SAMPLES = 2
OUTLIER_RATIO = 3.0
OUTLIER_FLOOR = 30.0
BAND = 0.25
BAND_SECONDS = 15.0
EVENTS = ("pull_request", "merge_group", "workflow_dispatch")
ARTIFACT = re.compile(r"^test-results-mcp-\d+$")
ABOUT = ("Seconds of the MCP CI suite's files (their tests summed) and of each test of a TESTS_INDEPENDENT file, "
         "medians of CI's pull_request and merge-queue shards. tools/mcp/shard.py plans the shards by them. "
         "Written by tools/ci/mcp_timings.py refit (the daily mcp-timings-refit workflow): do not edit by hand.")


# ---------------------------------------------------------------------------------------- fetch

def gh(*args: str) -> str:
    r = subprocess.run(["gh", *args], capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise SystemExit(f"mcp_timings: gh {' '.join(args[:3])}: {r.stderr.strip()[-300:]}")
    return r.stdout


def wanted(run: dict, repo: str) -> bool:
    """A ci.yml run whose MCP shards time what a PR's do: a PR, the queue or a dispatch, of a branch
    of this repository (a fork's run could upload anything)."""
    return run.get("event") in EVENTS and (run.get("head_repository") or {}).get("full_name") == repo


def fetch(repo: str, runs: int, out: Path, scan: int = 200) -> list[str]:
    """The newest `runs` ci.yml runs of the repository's own branches that left MCP results."""
    got: list[str] = []
    page = 1
    seen = 0
    while len(got) < runs and seen < scan:
        data = json.loads(gh("api", f"repos/{repo}/actions/workflows/ci.yml/runs?status=completed&per_page=50&page={page}"))
        batch = data.get("workflow_runs", [])
        if not batch:
            break
        for run in batch:
            seen += 1
            if len(got) >= runs:
                break
            if not wanted(run, repo):
                continue
            arts = json.loads(gh("api", f"repos/{repo}/actions/runs/{run['id']}/artifacts?per_page=100"))
            mcp = [a for a in arts.get("artifacts", []) if ARTIFACT.match(a["name"]) and not a.get("expired")]
            if not mcp:
                continue
            target = out / str(run["id"])
            target.mkdir(parents=True, exist_ok=True)
            for a in mcp:
                if (target / f"{a['name']}.json").is_file():
                    continue
                with tempfile.TemporaryDirectory() as tmp:
                    archive = Path(tmp) / "a.zip"
                    with archive.open("wb") as f:
                        r = subprocess.run(["gh", "api", f"repos/{repo}/actions/artifacts/{a['id']}/zip"], stdout=f)
                    if r.returncode != 0:
                        continue
                    with zipfile.ZipFile(archive) as z:
                        if "mcp.json" in z.namelist():
                            (target / f"{a['name']}.json").write_bytes(z.read("mcp.json"))
            if any(target.glob("*.json")):
                got.append(str(run["id"]))
                print(f"run {run['id']} ({run['event']}, {run['created_at']}): {len(list(target.glob('*.json')))} shard(s)")
            else:
                shutil.rmtree(target, ignore_errors=True)
        page += 1
    return got


# ---------------------------------------------------------------------------------------- refit

def load_runs(results: Path) -> dict[str, list[dict]]:
    """run id -> its shards' results (the runner's --json `results` rows)."""
    runs: dict[str, list[dict]] = {}
    for d in sorted((p for p in results.iterdir() if p.is_dir() and p.name.isdigit()), key=lambda p: int(p.name)):
        rows: list[dict] = []
        for f in sorted(d.glob("*.json")):
            try:
                rows += json.loads(f.read_text()).get("results", [])
            except (OSError, ValueError, AttributeError):
                print(f"mcp_timings: {f}: unreadable, left out", file=sys.stderr)
        if rows:
            runs[d.name] = rows
    return runs


def samples(runs: dict[str, list[dict]], independent: set[str]) -> tuple[dict[str, list[tuple[float, int]]], dict[str, list[float]]]:
    """Per file, oldest run first: (seconds, tests) of each run that reported as many of its tests as
    the newest did; per test of an independent file: its seconds in each run."""
    files: dict[str, list[tuple[float, int]]] = {}
    tests: dict[str, list[float]] = {}
    for run in sorted(runs, key=int):
        by_file: dict[str, list[dict]] = {}
        for r in runs[run]:
            by_file.setdefault(r["name"].split("::")[0], []).append(r)
        for file, rs in by_file.items():
            if file in independent:
                for r in rs:
                    tests.setdefault(r["name"], []).append(float(r["seconds"]))
            files.setdefault(file, []).append((sum(float(r["seconds"]) for r in rs), len(rs)))
    newest = {file: got[-1][1] for file, got in files.items()}
    return {file: [g for g in got if g[1] == newest[file]] for file, got in files.items()}, tests


def robust(values: list[float]) -> float | None:
    """The median after the outliers go; None from fewer than MIN_SAMPLES."""
    if len(values) < MIN_SAMPLES:
        return None
    m = statistics.median(values)
    kept = [v for v in values if abs(v - m) <= OUTLIER_FLOOR or m / OUTLIER_RATIO <= v <= m * OUTLIER_RATIO]
    if len(kept) < MIN_SAMPLES:
        return None
    return round(statistics.median(kept), 1)


def outside_band(new: float, old: float) -> bool:
    return abs(new - old) > max(BAND * old, BAND_SECONDS)


def empty() -> dict:
    return {"about": ABOUT, "format": FORMAT, "runs": [], "default_test_seconds": shard.DEFAULT_SECONDS,
            "files": {}, "tests": {}}


def load(path: Path) -> dict:
    if not path.is_file():
        return empty()
    data = json.loads(path.read_text())
    if not isinstance(data, dict) or data.get("format") != FORMAT:
        raise SystemExit(f"mcp_timings: {path} is not format {FORMAT}")
    return data


def refit(old: dict, runs: dict[str, list[dict]], tests_dir: Path | None = None) -> tuple[dict, list[str]]:
    """The refitted timings and what changed (empty: keep the committed file)."""
    tests_dir = tests_dir or TESTS
    present = {p.name for p in tests_dir.glob("test_*.py")}
    independent = {f for f in present if shard.independent(tests_dir / f)}
    file_samples, test_samples = samples(runs, independent)
    files = {k: dict(v) for k, v in old.get("files", {}).items()}
    tests = dict(old.get("tests", {}))
    changes: list[str] = []
    for file in sorted(set(files) - present):
        changes.append(f"- {file} (no longer in the suite)")
        del files[file]
    for name in sorted(n for n in tests if n.split("::")[0] not in independent):
        changes.append(f"- {name} (its file is gone or no longer TESTS_INDEPENDENT)")
        del tests[name]
    for name in sorted(n for n in tests if n not in test_samples):
        changes.append(f"- {name} (no run reported it)")
        del tests[name]
    for file, got in sorted(file_samples.items()):
        if file not in present:
            continue
        new = robust([s for s, _ in got])
        if new is None:
            continue
        count = got[-1][1]
        before = files.get(file)
        if before is None:
            changes.append(f"+ {file} {new:.0f}s ({count} tests)")
        elif before["tests"] != count or outside_band(new, before["seconds"]):
            changes.append(f"~ {file} {before['seconds']:.0f}s -> {new:.0f}s ({before['tests']} -> {count} tests)")
        else:
            continue
        files[file] = {"seconds": new, "tests": count}
    for name, got in sorted(test_samples.items()):
        if name.split("::")[0] not in present:
            continue
        new = robust(got)
        if new is None:
            continue
        before = tests.get(name)
        if before is None or outside_band(new, before):
            changes.append(f"{'+' if before is None else '~'} {name} {'' if before is None else f'{before:.0f}s -> '}{new:.0f}s")
            tests[name] = new
    total = sum(v["seconds"] for v in files.values())
    count = sum(v["tests"] for v in files.values())
    default = round(total / count, 1) if count else shard.DEFAULT_SECONDS
    data = {"about": ABOUT, "format": FORMAT, "runs": sorted(runs, key=int) if changes else old.get("runs", []),
            "default_test_seconds": default if changes else old.get("default_test_seconds", default),
            "files": dict(sorted(files.items())), "tests": dict(sorted(tests.items()))}
    return data, changes


def dump(data: dict) -> str:
    """One entry a line: the standing PR's diff names what moved."""
    lines = ["{"]
    for key in ("about", "format", "runs", "default_test_seconds"):
        lines.append(f" {json.dumps(key)}: {json.dumps(data[key])},")
    for key, close in (("files", ","), ("tests", "")):
        entries = [f"  {json.dumps(k)}: {json.dumps(v, separators=(', ', ': '))}" for k, v in data[key].items()]
        lines.append(f" {json.dumps(key)}: {{" + ("\n" + ",\n".join(entries) + "\n }" if entries else "}") + close)
    lines.append("}")
    return "\n".join(lines) + "\n"


def problems(data: dict, tests_dir: Path | None = None) -> list[str]:
    tests_dir = tests_dir or TESTS
    present = {p.name for p in tests_dir.glob("test_*.py")}
    out = []
    for file, v in data.get("files", {}).items():
        if file not in present:
            out.append(f"{file}: no such test file (tools/ci/mcp_timings.py refit drops it)")
        if not (isinstance(v, dict) and isinstance(v.get("seconds"), (int, float)) and isinstance(v.get("tests"), int)):
            out.append(f"{file}: not {{seconds, tests}}: {v!r}")
    for name, v in data.get("tests", {}).items():
        if "::" not in name or name.split("::")[0] not in present:
            out.append(f"{name}: no such test file")
        if not isinstance(v, (int, float)):
            out.append(f"{name}: not seconds: {v!r}")
    return out


# ---------------------------------------------------------------------------------------- command line

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="the MCP results of the newest CI runs (gh)")
    f.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    f.add_argument("--runs", type=int, default=20)
    f.add_argument("--out", type=Path, required=True)
    r = sub.add_parser("refit", help="robust medians of the fetched runs against the committed file")
    r.add_argument("--results", type=Path, required=True)
    r.add_argument("--timings", type=Path, default=TIMINGS)
    r.add_argument("--write", action="store_true", help="rewrite --timings when an entry changed")
    c = sub.add_parser("check", help="the committed file parses and names only test files that exist")
    c.add_argument("--timings", type=Path, default=TIMINGS)
    args = ap.parse_args(argv)

    if args.cmd == "fetch":
        if not args.repo:
            print("mcp_timings: --repo OWNER/NAME (or GITHUB_REPOSITORY)", file=sys.stderr)
            return 2
        args.out.mkdir(parents=True, exist_ok=True)
        got = fetch(args.repo, args.runs, args.out)
        print(f"fetched {len(got)} run(s) into {args.out}")
        return 0 if got else 1
    if args.cmd == "check":
        try:
            found = problems(load(args.timings)) if args.timings.is_file() else [f"{args.timings}: missing"]
        except (OSError, ValueError, SystemExit) as e:
            found = [str(e)]
        print("\n".join(found) if found else f"{args.timings}: fine")
        return 1 if found else 0
    if not args.results.is_dir():
        print(f"mcp_timings: no results directory {args.results}", file=sys.stderr)
        return 2
    runs = load_runs(args.results)
    if not runs:
        print(f"mcp_timings: no MCP results under {args.results}", file=sys.stderr)
        return 2
    data, changes = refit(load(args.timings), runs)
    print(f"{len(runs)} run(s); {len(changes)} change(s) outside the band")
    for line in changes:
        print(line)
    if changes and args.write:
        tmp = args.timings.with_suffix(".tmp")
        tmp.write_text(dump(data))
        os.replace(tmp, args.timings)
        print(f"wrote {args.timings}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
