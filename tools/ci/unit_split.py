#!/usr/bin/env python3
"""Splits a unit test binary's build and run over CI jobs (docs/testing.md "CI", the unit lane).

    python3 tools/ci/unit_split.py objects --build-dir DIR --target TARGET --part K/N [--ninja NINJA]
        the object files of part K of N of TARGET's compile steps (`ninja -t commands`), one a line:
        sorted and dealt round-robin, so every job of the same configure gets the same parts; and
        DIR/unit-part.ninja to build them with: build.ninja without the libraries and executables CMake
        makes compiles and custom commands wait for (order-only), which would link everything first
    python3 tools/ci/unit_split.py args --binary BIN --shard K/N [-- TEST_ARGS...]
        TEST_ARGS with --gtest_filter narrowed to shard K of N of the tests it selects: whole suites,
        balanced by test count (single tests when there are fewer suites than shards; every shard
        runs them all when there are fewer tests), so no shard runs nothing
    python3 tools/ci/unit_split.py snapshot --ccache DIR --out JSON
        the cache's files and sizes before the part compiles
    python3 tools/ci/unit_split.py pack --ccache DIR --before JSON --out TAR [--objects LIST] [--github-output F]
        the ccache files the part added or grew (not those a hit only touched) into an uncompressed
        tar; outputs files, results (compiled objects) and objects (LIST's lines)
    python3 tools/ci/unit_split.py wait --repo R --run-id ID --artifact NAME --job NAME --started EPOCH
            --results N --objects M [--poll S] [--github-output F]
        waits for the other part's artifact (GitHub API through gh): found=true when it is there;
        found=false (compile the rest here) when that part has not started, ended without it, or
        is still compiling after as long as this part took, or when this part was warm (it compiled
        under a tenth of its objects: the other part's objects hit the restored cache here too)

A shard's filter is the planned filter (or *) minus the other shards' suites, so it never runs a
test the plan left out. Standard library only; exit 1 on a failure, 2 on unusable arguments.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tarfile
import time
from pathlib import Path

OUTPUT_RE = re.compile(r"(?:^|\s)-o\s+(\S+\.o)(?=\s|$)")
SUITE_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_/]*)\.(?:\s+#.*)?$")
TEST_RE = re.compile(r"^  ([A-Za-z_][A-Za-z0-9_/]*)(?:\s+#.*)?$")
# ccache's own files, not cached objects
NOT_OBJECTS = re.compile(r"(^|/)(stats|ccache\.conf|CACHEDIR\.TAG)$|\.lock$|(^|/)tmp/")
WARM_SHARE = 10            # a part that compiled under 1/10 of its objects was warm
NOT_STARTED = ("queued", "waiting", "pending", "requested")


class ApiError(Exception):
    pass


def parse_part(text: str) -> tuple[int, int]:
    m = re.fullmatch(r"([0-9]+)/([0-9]+)", text or "")
    if not m or not 1 <= int(m.group(1)) <= int(m.group(2)):
        raise ValueError(f"'{text}' is not K/N with 1 <= K <= N")
    return int(m.group(1)), int(m.group(2))


def compile_objects(commands: str) -> list[str]:
    """The object files the compile steps of `ninja -t commands` write (not the PCH, links or generators)."""
    out = set()
    for line in commands.splitlines():
        if " -c " not in line:
            continue
        m = OUTPUT_RE.search(line)
        if m:
            out.add(m.group(1))
    return sorted(out)


def part_ninja(build_ninja: str) -> str:
    """build.ninja whose edges wait for no library or executable they do not read: what a linker makes
    goes from every order-only input list (a target's custom commands wait for its link libraries, its
    objects for the executables it depends on); a tool a command runs is a real input and stays."""
    lines = build_ninja.splitlines()
    linked = set()
    for line in lines:
        if line.startswith("build ") and ": " in line:
            outs, _, rest = line[6:].partition(": ")
            if "_LINKER__" in rest.split(" ", 1)[0]:
                linked.update(o for o in outs.split() if o != "|")
    out = []
    for line in lines:
        if line.startswith("build ") and " || " in line:
            head, _, order = line.partition(" || ")
            keep = [n for n in order.split() if n not in linked]
            line = f"{head} || {' '.join(keep)}" if keep else head
        out.append(line)
    return "\n".join(out) + "\n"


def take_part(items: list[str], k: int, n: int) -> list[str]:
    return items[k - 1::n]


def parse_listing(text: str) -> dict[str, list[str]]:
    """`--gtest_list_tests`: suite -> its tests, in the binary's order (disabled ones included)."""
    suites: dict[str, list[str]] = {}
    suite = None
    for line in text.splitlines():
        m = SUITE_RE.match(line)
        if m:
            suite = m.group(1)
            suites.setdefault(suite, [])
            continue
        m = TEST_RE.match(line)
        if m and suite is not None:
            suites[suite].append(m.group(1))
    return {s: tests for s, tests in suites.items() if tests}


def split_filter(args: list[str]) -> tuple[list[str], str | None]:
    """The args without their --gtest_filter, and its value (googletest takes the last one)."""
    rest, flt = [], None
    for a in args:
        if a.startswith("--gtest_filter="):
            flt = a.split("=", 1)[1]
        else:
            rest.append(a)
    return rest, flt


def shard_filter(suites: dict[str, list[str]], base: str | None, k: int, n: int) -> str:
    """The filter of shard k of n over the tests `--gtest_filter=<base>` listed (suites)."""
    runnable = {s: [t for t in tests if not t.startswith("DISABLED_")]
                for s, tests in suites.items() if not s.startswith("DISABLED_")}
    runnable = {s: tests for s, tests in runnable.items() if tests}
    total = sum(len(tests) for tests in runnable.values())
    if total == 0:
        raise ValueError(f"the filter {base or '*'} selects no test")
    pos, _, neg = (base or "*").partition("-")
    if n == 1 or total < n:
        return base or "*"
    if len(runnable) >= n:
        loads = [0] * n
        owner: dict[str, int] = {}
        for s in sorted(runnable, key=lambda s: (-len(runnable[s]), s)):
            i = min(range(n), key=lambda i: (loads[i], i))
            owner[s] = i
            loads[i] += len(runnable[s])
        exclude = [f"{s}.*" for s in runnable if owner[s] != k - 1]
    else:
        tests = sorted(f"{s}.{t}" for s, ts in runnable.items() for t in ts)
        mine = set(take_part(tests, k, n))
        exclude = [t for t in tests if t not in mine]
    negative = [p for p in neg.split(":") if p] + exclude
    return f"{pos or '*'}-{':'.join(negative)}"


def snapshot(ccache: Path) -> dict[str, int]:
    """The cache's object files and their sizes (relative paths)."""
    out = {}
    for path in sorted(ccache.rglob("*")) if ccache.is_dir() else []:
        rel = path.relative_to(ccache).as_posix()
        if path.is_file() and not path.is_symlink() and not NOT_OBJECTS.search(rel):
            out[rel] = path.stat().st_size
    return out


def pack(ccache: Path, before: dict[str, int], out: Path) -> dict[str, int]:
    """The cache files added since the snapshot before, or grown (a manifest that took an entry), into
    out. Not by mtime: a hit refreshes its files' mtimes (ccache's LRU)."""
    files = [rel for rel, size in snapshot(ccache).items() if before.get(rel) != size]
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    with tarfile.open(tmp, "w") as t:
        for rel in files:
            t.add(ccache / rel, arcname=rel, recursive=False)
    os.replace(tmp, out)
    return {"files": len(files), "results": sum(1 for rel in files if rel.endswith("R"))}


def wait_for(api, repo: str, run_id: str, artifact: str, job: str, budget: float, poll: float,
             results: int, objects: int, now=time.time, sleep=time.sleep) -> tuple[bool, str]:
    """Whether artifact appeared (api(path) -> parsed JSON of a GitHub REST path), and why."""
    if objects > 0 and results * WARM_SHARE < objects:
        return False, (f"warm: this part compiled {results} of its {objects} objects, so the other part's "
                       "hit the restored cache here too: no wait")
    start = now()
    while True:
        waited = int(now() - start)
        try:
            found = api(f"repos/{repo}/actions/runs/{run_id}/artifacts?name={artifact}")
            if int(found.get("total_count", 0)) > 0:
                return True, f"{artifact} is there (waited {waited} s)"
            status = None
            page = 1
            while status is None:
                jobs = api(f"repos/{repo}/actions/runs/{run_id}/jobs?filter=latest&per_page=100&page={page}").get("jobs", [])
                status = next((j.get("status") for j in jobs if j.get("name") == job), None)
                if len(jobs) < 100:
                    break
                page += 1
        except (ApiError, ValueError, TypeError, AttributeError) as e:
            return False, f"the GitHub API failed ({e}): compile the rest here"
        if status is None or status in NOT_STARTED:
            return False, f"{job} has not started (waited {waited} s): compile the rest here"
        if status == "completed":
            return False, f"{job} ended without {artifact} (waited {waited} s): compile the rest here"
        if waited + poll > budget:
            return False, f"{job} is still compiling after {waited} s, about as long as this part took: compile the rest here"
        sleep(poll)


def gh_api(path: str) -> dict:
    r = subprocess.run(["gh", "api", path], capture_output=True, text=True)
    if r.returncode != 0:
        raise ApiError((r.stderr or r.stdout).strip().splitlines()[-1] if (r.stderr or r.stdout).strip() else f"gh exit {r.returncode}")
    return json.loads(r.stdout or "{}")


def write_outputs(path: Path | None, values: dict) -> None:
    if path:
        with path.open("a") as f:
            f.write("".join(f"{k}={v}\n" for k, v in values.items()))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    o = sub.add_parser("objects")
    o.add_argument("--build-dir", required=True)
    o.add_argument("--target", required=True)
    o.add_argument("--part", required=True)
    o.add_argument("--ninja", default="ninja")
    a = sub.add_parser("args")
    a.add_argument("--binary", required=True)
    a.add_argument("--shard", required=True)
    a.add_argument("test_args", nargs="*")
    sn = sub.add_parser("snapshot")
    sn.add_argument("--ccache", type=Path, required=True)
    sn.add_argument("--out", type=Path, required=True)
    p = sub.add_parser("pack")
    p.add_argument("--ccache", type=Path, required=True)
    p.add_argument("--before", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--objects", type=Path)
    p.add_argument("--github-output", type=Path)
    w = sub.add_parser("wait")
    w.add_argument("--repo", required=True)
    w.add_argument("--run-id", required=True)
    w.add_argument("--artifact", required=True)
    w.add_argument("--job", required=True)
    w.add_argument("--started", type=float, required=True, help="epoch seconds this part started compiling")
    w.add_argument("--results", type=int, default=0)
    w.add_argument("--objects", type=int, default=0)
    w.add_argument("--poll", type=float, default=20.0)
    w.add_argument("--github-output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "objects":
            k, n = parse_part(args.part)
            r = subprocess.run([args.ninja, "-C", args.build_dir, "-t", "commands", args.target], capture_output=True, text=True)
            if r.returncode != 0:
                print(f"unit_split: ninja -t commands {args.target} failed: {r.stderr.strip()}", file=sys.stderr)
                return 1
            objects = compile_objects(r.stdout)
            if not objects:
                print(f"unit_split: {args.target} in {args.build_dir} has no compile step", file=sys.stderr)
                return 1
            build = Path(args.build_dir)
            tmp = build / "unit-part.ninja.tmp"
            tmp.write_text(part_ninja((build / "build.ninja").read_text()))
            os.replace(tmp, build / "unit-part.ninja")
            print("\n".join(take_part(objects, k, n)))
            print(f"unit_split: part {k}/{n} of {args.target}: {len(take_part(objects, k, n))} of {len(objects)} objects",
                  file=sys.stderr)
            return 0
        if args.cmd == "args":
            k, n = parse_part(args.shard)
            rest, base = split_filter(args.test_args)
            listing = [args.binary, "--gtest_list_tests"] + ([f"--gtest_filter={base}"] if base is not None else [])
            r = subprocess.run(listing, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"unit_split: {' '.join(listing)} failed ({r.returncode}): {r.stderr.strip()[-2000:]}", file=sys.stderr)
                return 1
            try:
                flt = shard_filter(parse_listing(r.stdout), base, k, n)
            except ValueError as e:
                print(f"unit_split: no test to shard: {e}", file=sys.stderr)
                return 1
            print(" ".join(rest + [f"--gtest_filter={flt}"]))
            return 0
        if args.cmd == "snapshot":
            files = snapshot(args.ccache)
            args.out.write_text(json.dumps(files))
            print(f"unit_split: {len(files)} cache files before the part")
            return 0
        if args.cmd == "pack":
            got = pack(args.ccache, json.loads(args.before.read_text()), args.out)
            got["objects"] = len(args.objects.read_text().split()) if args.objects and args.objects.is_file() else 0
            print(f"unit_split: packed {got['files']} cache files ({got['results']} compiled objects of the part's "
                  f"{got['objects']}) into {args.out} ({args.out.stat().st_size // (1 << 20)} MiB)")
            write_outputs(args.github_output, got)
            return 0
        budget = max(0.0, time.time() - args.started)
        found, why = wait_for(gh_api, args.repo, args.run_id, args.artifact, args.job, budget, args.poll,
                              args.results, args.objects)
        print(f"unit_split: {why}")
        write_outputs(args.github_output, {"found": "true" if found else "false"})
        return 0
    except ValueError as e:
        print(f"unit_split: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
