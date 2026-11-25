"""tools/ci/plan_lanes.py: CI's `changes` job. Which lanes of .github/workflows/ci.yml run, with which
unit test filters and which MCP shards (CI design §8.2; contract C1).

    python3 tools/ci/plan_lanes.py --event pull_request --base <sha> --head <sha> --out-dir build/ci-plan \\
        [--deps-from DIR] [--coverage-map FILE] [--test-seconds FILE] [--github-output FILE] \\
        [--advisory "unit asan htmlui mcp smoke windows-editor-tests"] \\
        [--newest <sha>] [--previous-runs FILE --run-id N]

It plans through `tools/check_changed.py --plan-json` (the per-commit gate's own selection), then
applies CI's stricter rules:

- every changed path on check-changed's CI_NOTHING_READS list (or no change): no lane runs (D3);
- anything else builds: every platform lane and lint run;
- smoke runs for what a game's export reads (SMOKE_PATHS);
- asan, on a pull request: the unit lane's selection when a path the sanitizers see changed
  (SANITIZER_INPUTS, a .github/ change included), every test of both binaries when its own settings
  or definition did (ASAN_SETTINGS, ci.yml's asan job or env), else nothing: the advisory run on main
  runs it after the merge;
- macos and ios, on a pull request: only when an Apple-only input changed (APPLE_INPUTS); ios in the
  merge queue too (the nightly builds it for every change); macos in the queue, a push, full runs and
  the nightly for every change that builds;
- MCP runs the planned files that are in the CI suite (run_tests.ci_files()); the rest are listed
  under not_in_ci with their reason. A planned whole suite is the CI suite;
- windows_parts, the windows lane's matrix: on a pull request, a dispatch, in the merge queue and the advisory
  run, engine-d3d11 and engine-vulkan only for their own inputs (windows_legs); every leg otherwise;
- merge_group: every lane, unit and HTML UI unfiltered, MCP planned over the group's diff (D1), but
  the test lanes and test runs --advisory names (ci.yml's ADVISORY_LANES): they cannot block a merge,
  so the queue does not wait for them (listed under advisory_off; a test run of a job that still
  builds, under build_only too). Lint and the platform builds stay whatever the list says;
- push: the build lanes (caches and plan inputs), and smoke when a game's export reads the change, so its
  Compile builds' ccache stays warm; not ios, which keeps no cache entry (on push it would take a macOS
  runner for nothing); the test lanes build only (build_only; ASan's two binaries too, each keeping a
  ccache entry). When --advisory names a test lane, dispatch_advisory asks ci.yml to dispatch the
  advisory run, which runs those tests. A push run waits ci.yml's admission delay first, and steps aside
  (no lane) when main moved on by then with a path that builds (--newest: main's head after the delay;
  the newer push's run builds and saves the caches). It plans against the head of the last push run that
  finished (--previous-runs, `gh run list` JSON, without --run-id's own), not github.event.before: a
  push run waiting for the one before it is dropped when a newer one comes, and its change would go
  unbuilt;
- advisory (workflow_dispatch with advisory: true, which a push dispatches): what the merge queue left out
  for being advisory, as the queue would run it, and what that needs (linux for the editor install of
  mcp and smoke, the windows editor leg for its test run); nothing else. Planned against the head of
  the last advisory run that finished (--previous-runs; ADVISORY_TITLE names them), so every merged
  change is tested once however many pushes one run covers; --base is unused. With no such run: every
  advisory lane, the whole MCP CI suite (fail open);
- full (workflow_dispatch with full: true): every lane, the whole MCP CI suite;
- nightly: as full, and MCP also runs the CI_EXCLUDED files CI can run (NIGHTLY_LEFT_OUT says which
  it cannot);
- fail open: no plan, or a .github/ change, runs every lane (MCP: the whole CI suite); check-changed's
  own widenings (a missing deps dump or map) are kept and said, in `fallbacks`.
- shadow (merge_group, advisory, push, full, nightly): lanes.json's `shadow` is what a pull request's
  selection of the run's diff would run (full and nightly: the diff since --shadow-base), which
  tools/ci/shadow.py judges the run's failures against;
- rollback: --full-selection (ci.yml passes it when the repository variable CI_FULL_SELECTION is
  true) runs a PR's, the queue's or the advisory run's every lane whole for any change that builds.

MCP shards: `tools/mcp/shard.py plan` over `run_tests.py --list` of the files, by the committed
timings of CI's PR and merge-queue shards (tools/mcp/ci-timings.json, refitted daily), then for the
files they lack the per-test seconds of earlier runs (--test-seconds, the nightly's
`mcp-test-seconds.json`; the nightly plans by those alone: its shards build the games a PR's skip),
SHARD_WORKERS editors per shard; as many shards as each shorten the lane by more than the setup they
cost (SHARD_SETUP_SECONDS); at most MAX_PLANNED_SHARDS, except for full and nightly.

Writes <out-dir>/lanes.json, <out-dir>/check-changed.json (the plan) and <out-dir>/mcp-plan/
(plan-<shard>.json, tests.txt). Exit 0 whenever lanes.json was written, widened or not; 2 on
unusable arguments.
"""

from __future__ import annotations

import argparse
import dataclasses
import importlib.util
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
sys.path.insert(0, str(TOOLS))
import check_changed as cc  # noqa: E402

EVENTS = ("pull_request", "merge_group", "push", "workflow_dispatch", "full", "nightly", "advisory")
LANES = ("changes", "lint", "linux", "unit", "asan", "htmlui", "mcp", "smoke", "web", "android", "windows", "macos", "ios")
PLATFORM_LANES = ("linux", "web", "android", "windows", "macos", "ios")
# what one more MCP shard costs a runner besides its tests: job start to the shard's step and after it
# (checkout, disk, the image, the editor install), median 236 + 6 s over 46 shards of runs 44666496445,
# 44735818671, 44745111854, 44753600317, 44764139781, 44799402546
SHARD_SETUP_SECONDS = 240.0
SHARD_WORKERS = 2           # tools/ci/mcp_shard.sh runs -j 2 on a 4-vCPU runner
CHUNK_SECONDS = 600.0       # an independent file's chunks (shard.py plan --chunk-seconds)
COMMITTED_TIMINGS = "tools/mcp/ci-timings.json"
MAX_PLANNED_SHARDS = 8      # a pull request's or the merge queue's MCP lane (CI design §16.2)
MAX_SHARDS = 256            # a workflow's matrix limit: full and nightly take as many as they need
FILTER_LIMIT = 60000        # a longer --gtest_filter runs everything instead (an argument's size)
# the unit lane's jobs per test binary: the editor tests' build is most of their lane (110 of 123 min,
# run 43737549764), so two jobs each compile half its objects, take the other's through the cache and
# run half its suites (tools/ci/unit_split.py)
UNIT_PARTS = {"core": 1, "editor": 2}
# the test lanes the merge queue leaves to the advisory run while ADVISORY_LANES names them (and the
# test runs reported apart from their job, report.TEST_REPORTS)
QUEUE_DEFERRABLE = ("unit", "asan", "htmlui", "mcp", "smoke")
# the start of an advisory run's name (ci.yml run-name), by which the next one finds what it tested
ADVISORY_TITLE = "Advisory tests of "
# what a game's export and launch read (the smoke lane): check-changed's compile and saved-output
# families, the engine, the templates and fixtures, the smoke scripts, and what builds everything
SMOKE_PATHS = ("engine/*", *cc.COMPILE_PATHS, *cc.SAVED_OUTPUT_PATHS, "editor/resources/templates/*", "tools/fixtures/*",
               "tools/smoke-test.sh", "tools/ci/smoke_*", "tools/ci/parity.sh", "tools/mcp/nav_smoke.py", *cc.ALL_TRIGGERS)
# the CI_EXCLUDED files the nightly cannot run either (the ones needing the uncommitted default
# fixture are found by run_tests.default_fixture_reason)
NIGHTLY_LEFT_OUT = {
    "test_sound_library_pipeline.py": "needs the pinned download cache (JASPER_SOUND_CACHE), which CI has not",
    "test_sound_library_semantic.py": "needs the semantic model cache (JASPER_SOUND_CACHE), which CI has not",
    "test_pixel_fonts_export.py": "reads a SMOKE_METHOD=compile smoke run of tools/fixtures/pixel-fonts made first on "
                                  "the same machine, which an MCP shard does not make",
    "test_pixel_fx_export.py": "reads SMOKE_METHOD=compile smoke runs of tools/fixtures/pixel-fx-game made first on "
                               "the same machine (PFX_SMOKE_DIRS), which an MCP shard does not make",
}
# the asan lane on a pull request: what the sanitizers can see is compiled code (C/C++, and the .inc
# files it includes) and what builds and runs it (CMake and its presets, the image, tools/build.sh, the
# vendored libraries, the test runner and gate). CI's own scripts, docs, MCP tests, assets and test
# data change no instrumented code: the advisory run on main runs ASan over them after the merge
SANITIZER_INPUTS = (*(f"*{ext}" for ext in (*cc.CXX, ".inc", ".cmake")), "*CMakeLists.txt", "CMakePresets.json",
                    "engine/cmake/*", "docker/*", "tools/build.sh", "tools/with-xvfb.sh", "tools/ci/gtest-gate.py",
                    "libs/*", "engine/libs/*", "editor/libs/*")
# the lane's own settings and definition (ci.yml's asan job and the workflow's env, definition_change):
# no selection sees what they change, so every test of both binaries runs under them
ASAN_SETTINGS = ("tools/sanitizers/*",)
WORKFLOW = ".github/workflows/ci.yml"
ASAN_JOB = "asan"
# the macos and ios lanes on a pull request: what only an Apple build reads. macOS runners are the scarce
# ones (5 for the organization; jobs waited a median of 2 h, up to 10, on 2026-11-23), and the merge queue
# is the gate: it, a push to main, full runs and the nightly build both for every change that builds, so
# nothing reaches main without them. A PR runs them for the Apple platform code, the Xcode project and the
# scripts that sync it (lint checks its source lists: tools/test_xcode_sync_sources.py), the Metal shader
# stubs, and the lanes' own scripts (tools/ci/macos.sh, its common part, retry.sh, the report action); not
# for a ci.yml change alone. The merge queue builds ios by the same rule: it is a compile check with the
# Apple compiler the queue's macos lane runs on every change, and nightly.yml's ios job builds it every night
APPLE_INPUTS = ("engine/platform/apple/*", "engine/platform/mac/*", "engine/workspaces/xcode/*", "tools/xcode_sync_*",
                "tools/test_xcode_sync_sources.py", "engine/shaders/msl21*", "tools/ci/macos.sh",
                "tools/ci/platform-common.sh", "tools/ci/retry.sh", ".github/actions/report/*")
APPLE_LANES = ("macos", "ios")
# the macos lane in the merge queue: what an Apple build alone reads, more widely than a pull request's
# rule (APPLE_INPUTS) since the queue is the last gate before main: Objective-C sources, Mac and Apple named
# sources and resources, the vendored sokol (its Metal backend), and by content (macos_inputs): a C/C++ file
# whose text names an Apple macro (MACOS_TEXT), a CMake file whose changed lines or their context name an
# Apple setting (MACOS_CMAKE), ci.yml's macos job or env. macOS caps the queue: 5 hosted runners and je-mac,
# a macos job a group. A push to main runs the whole lane when the queue left it out (macos_full), the
# nightly always: a mac-only break shows on main right after the merge
MACOS_PATHS = (*APPLE_INPUTS, "*.mm", "*.m", "*/Mac[A-Z]*", "*Mac.cpp", "*Mac.h", "*/Apple*", "editor/resources/macos/*",
               "engine/libs/sokol/*", "tools/ci/macos.sh", ".github/actions/ccache-restore/*", ".github/actions/ccache-save/*")
MACOS_TEXT = re.compile(r"__APPLE__|TARGET_OS_|SOKOL_METAL|__OBJC__")
MACOS_CMAKE = re.compile(r"\bAPPLE\b|\bIOS\b|CMAKE_OSX|Darwin|XCODE|macos|metal", re.I)
MACOS_JOB = "macos"
# the repository variable that runs a pull request's blocking lanes only (off), as the merge queue does, while
# GitHub's 60 concurrent jobs are the bottleneck: its advisory test lanes run on main after the merge anyway
PR_ADVISORY_VARIABLE = "PR_ADVISORY_LANES"
# the repository variable that turns the selection off (ci.yml passes --full-selection when it is "true")
FULL_SELECTION_VARIABLE = "CI_FULL_SELECTION"
TEST_NAME = re.compile(r"^test_\w+\.py::\w+$")
# run_tests.py --list through uv and the harness's pinned packages (tools/ci/test_plan_lanes.py's
# scratch test files import nothing, and list without them)
LIST_WITH_UV = True


# ---------------------------------------------------------------------------------------- inputs

def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


# (TEST_REPORTS: the test runs reported apart from their job)
report = load_module(HERE / "report.py", "plan_lanes_report")


def git(root: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=str(root), capture_output=True, text=True, errors="replace")


def resolve(root: Path, ref: str) -> str | None:
    r = git(root, "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}")
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else None


def run_check_changed(root: Path, base: str, out: Path, deps_from: Path | None,
                      coverage_map: Path | None) -> tuple[dict | None, str]:
    """check-changed's plan of root's HEAD against base, or None and why there is none."""
    command = [sys.executable, "-I", str(TOOLS / "check_changed.py"), "--root", str(root), "--base", base,
               "--plan-json", str(out)]
    if deps_from:
        command += ["--deps-from", str(deps_from)]
    # (no --coverage-map: no map, whatever this machine keeps; check-changed then plans the CI suite)
    env = dict(os.environ, JASPER_MCP_COVERAGE_MAP=str(coverage_map or out.parent / "no-coverage-map.json"))
    r = subprocess.run(command, cwd=str(root), env=env, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        return None, f"tools/check_changed.py --plan-json exited {r.returncode}: {(r.stderr or r.stdout).strip()[-300:]}"
    try:
        return json.loads(out.read_text()), ""
    except (OSError, ValueError) as e:
        return None, f"tools/check_changed.py wrote no readable plan ({e})"


def mcp_suites(root: Path) -> tuple[list[str], dict[str, str], dict[str, str]]:
    """The CI suite's files; the files it leaves out with the reason of each; and of those, the ones
    the nightly cannot run either, with the reason."""
    run_tests = load_module(root / "tools" / "mcp" / "run_tests.py", "plan_lanes_run_tests")
    tests = root / "tools" / "mcp" / "tests"
    files, _problems, excluded = run_tests.ci_files(tests)
    left_out = dict(excluded)
    nightly_out = {}
    for name, why in excluded:
        # (ci_files gives a CI_EXCLUDED file's own reason only: it may need the fixture as well)
        fixture = run_tests.default_fixture_reason(tests / name)
        if fixture:
            nightly_out[name] = f"needs the default fixture project, which CI has no copy of ({fixture})"
        elif name in NIGHTLY_LEFT_OUT:
            nightly_out[name] = NIGHTLY_LEFT_OUT[name]
    return [f.name for f in files], left_out, nightly_out


def list_tests(root: Path, files: list[str]) -> tuple[list[str] | None, str]:
    """`run_tests.py --list` of the files (it imports them: some make their tests as they load, which
    no static reading sees), through the harness's pinned packages."""
    paths = [str(root / "tools" / "mcp" / "tests" / f) for f in files]
    uv = shutil.which("uv") or str(Path.home() / ".local" / "bin" / "uv")
    if LIST_WITH_UV and Path(uv).is_file():
        command = [uv, "run", "--quiet", "--no-project", "--with-requirements", str(root / "tools/mcp/requirements.txt"),
                   "python", str(root / "tools/mcp/run_tests.py"), "--list", *paths]
    else:
        command = [sys.executable, str(root / "tools/mcp/run_tests.py"), "--list", *paths]
    r = subprocess.run(command, cwd=str(root), capture_output=True, text=True, errors="replace")
    names = [line.strip() for line in r.stdout.splitlines() if TEST_NAME.match(line.strip())]
    if r.returncode != 0 or not names:
        return None, f"run_tests.py --list exited {r.returncode} with {len(names)} test(s): {r.stderr.strip()[-300:]}"
    return names, ""


def shard_count(shard, tests: list[str], times: dict[str, float], tests_dir: Path, cap: int) -> int:
    """The fewest shards of SHARD_WORKERS that no more shards (up to cap) beat by more than the
    setup the added ones cost (SHARD_SETUP_SECONDS each): past that a shard costs a runner more than
    it saves the lane, and once the longest chunk sets the length it saves nothing."""
    chunks = shard.make_chunks(tests, times, CHUNK_SECONDS, tests_dir)
    total = sum(c.seconds for c in chunks)
    longest = max((c.seconds for c in chunks), default=0.0)
    # (past this many the longest chunk alone is the length)
    upper = max(1, min(cap, math.ceil(total / (SHARD_WORKERS * max(longest, 1.0))) + 1))

    def estimate(n: int) -> float:
        shards = [shard.parse_shard(f"{k}:{SHARD_WORKERS}") for k in range(1, n + 1)]
        shard.assign([dataclasses.replace(c) for c in chunks], shards)
        return max(max(s.finish) for s in shards)

    estimates = {n: estimate(n) for n in range(1, upper + 1)}
    for n in range(1, upper + 1):
        if all(estimates[n] - estimates[m] <= (m - n) * SHARD_SETUP_SECONDS for m in range(n + 1, upper + 1)):
            return n
    return upper


def readable(root: Path, paths: list[Path]) -> tuple[list[Path], list[str]]:
    """The timings files that parse, and a fallback for each that does not."""
    good, fallbacks = [], []
    for p in paths:
        try:
            json.loads(p.read_text())
            good.append(p)
        except (OSError, ValueError) as e:
            name = p.relative_to(root) if p.is_relative_to(root) else p
            fallbacks.append(f"MCP timings {name} left out ({type(e).__name__}: {e}): the shards are planned without it")
    return good, fallbacks


def plan_shards(root: Path, tests: list[str], test_seconds: Path | None, cap: int, out: Path,
                committed: bool = True) -> tuple[list[str], str, list[str]]:
    """The shards, a note on them, and the fallbacks taken (a timings file left out)."""
    shard = load_module(root / "tools" / "mcp" / "shard.py", "plan_lanes_shard")
    paths, fallbacks = readable(root, [p for p in (test_seconds, root / COMMITTED_TIMINGS if committed else None)
                                       if p and p.is_file()])
    times = shard.load_timings(paths)
    n = shard_count(shard, tests, times, root / "tools" / "mcp" / "tests", cap)
    out.mkdir(parents=True, exist_ok=True)
    (out / "tests.txt").write_text("\n".join(tests) + "\n")
    command = [sys.executable, "-I", str(root / "tools/mcp/shard.py"), "plan", "--tests", str(out / "tests.txt"),
               "--out", str(out), "--chunk-seconds", str(CHUNK_SECONDS)]
    if paths:
        command += ["--timings", *map(str, paths)]
    for k in range(1, n + 1):
        command += ["--shard", f"{k}:{SHARD_WORKERS}"]
    r = subprocess.run(command, cwd=str(root), capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        return [], f"tools/mcp/shard.py plan exited {r.returncode}: {r.stderr.strip()[-300:]}", fallbacks
    estimate = json.loads((out / "plan.json").read_text())["estimate_seconds"]
    sources = " and ".join(([COMMITTED_TIMINGS] if root / COMMITTED_TIMINGS in paths else []) +
                           (["the recorded test seconds"] if test_seconds in paths else []))
    return [str(k) for k in range(1, n + 1)], (f"{len(tests)} MCP test(s) in {n} shard(s) of {SHARD_WORKERS} editors, "
                                               f"the longest estimated at {estimate / 60:.0f} min, by "
                                               f"{sources or f'no timings ({shard.DEFAULT_SECONDS:.0f} s a test)'}"), fallbacks


# ---------------------------------------------------------------------------------------- the rules

def suite_choice(value) -> tuple[bool, str]:
    """check-changed's selection of one test binary ("all" or suites): whether it runs, its filter."""
    if value == "all":
        return True, ""
    if value:
        return True, cc.suite_filter(list(value))
    return False, ""


def decide(event: str, plan: dict | None, plan_error: str, ci: list[str], left_out: dict[str, str],
           nightly_out: dict[str, str], advisory=(), superseded_by: tuple[str, str] | None = None,
           full_selection: bool = False, asan_definition: str = "", pr_advisory: bool = True,
           macos_inputs: str | None = None) -> dict:
    """The lanes of an event, from check-changed's plan (None: there is none, plan_error says why),
    in LANES order; advisory: ADVISORY_LANES; superseded_by: a newer push to main, and a path it
    changed that builds (newer_push). full_selection (the rollback switch, FULL_SELECTION_VARIABLE):
    a pull request, the merge queue or the advisory run runs every lane whole for any change that builds.
    asan_definition: the asan lane's definition changed, and where (definition_change). macos_inputs:
    the first macOS-only input of the change (macos_inputs; "": none, so the queue leaves macos out; None:
    not known, so it runs). pr_advisory
    (PR_ADVISORY_VARIABLE): False runs a pull request's blocking lanes only, as the merge queue does."""
    result = _decide("merge_group" if event == "advisory" else event, plan, plan_error, ci, left_out, nightly_out,
                     full_selection, asan_definition, macos_inputs)
    result["advisory_off"] = []
    result["build_only"] = []
    result["unbuilt"] = []
    result["dispatch_advisory"] = False
    if event == "advisory":
        keep_advisory_only(result, set(advisory))
    elif event == "merge_group":
        leave_advisory_out(result, set(advisory), unbuilt=True)
    elif event == "pull_request" and not pr_advisory:
        leave_advisory_out(result, set(advisory), f"a pull request while {PR_ADVISORY_VARIABLE} is off")
    elif event == "push" and superseded_by:
        sha, path = superseded_by
        for name in LANES[1:]:
            result["lanes"][name] = {"run": False, "why": f"push to main: superseded by {sha[:7]} ({path} changed), "
                                                          "pushed within the admission delay: its run builds and saves the caches"}
        result["mcp"] = {"mode": "none", "files": [], "not_in_ci": []}
    elif event == "push":
        lanes = result["lanes"]
        result["build_only"] = [n for n in ("unit", "asan", "htmlui") if lanes[n]["run"]]
        result["build_only"] += [t for t, job in report.TEST_REPORTS.items() if lanes[job]["run"]]
        result["dispatch_advisory"] = (any(e["run"] for n, e in lanes.items() if n != "changes")
                                       and bool(set(advisory) & {*QUEUE_DEFERRABLE, *report.TEST_REPORTS}))
    result["lanes"] = {name: result["lanes"][name] for name in LANES}
    return result


def leave_advisory_out(result: dict, advisory: set[str], who: str = "the merge queue", unbuilt: bool = False) -> None:
    """The merge queue (or a pull request while PR_ADVISORY_VARIABLE is off): the advisory test lanes and
    test runs planned off, so its Check waits only for what can block the merge (they run in the
    advisory run a push to main dispatches). A plan that runs nothing keeps nothing."""
    lanes = result["lanes"]
    if not any(entry["run"] for name, entry in lanes.items() if name != "changes"):
        return
    for name in QUEUE_DEFERRABLE:
        if name in advisory and lanes[name]["run"]:
            lanes[name] = {"run": False, "why": f"{who}: advisory (ADVISORY_LANES), so it cannot block the "
                                                "merge; it runs in the advisory run a push to main dispatches"}
            result["advisory_off"].append(name)
    if "mcp" in result["advisory_off"]:
        result["mcp"] = {"mode": "none", "files": [], "not_in_ci": []}
    for test_run, job in report.TEST_REPORTS.items():
        if test_run in advisory and lanes[job]["run"]:
            result["advisory_off"].append(test_run)
            result["build_only"].append(test_run)
            # unbuilt: its binary is not even built (the merge queue: an MSVC-only compile error in a test
            # shows on main after the merge, in the advisory run, which builds and runs it)
            if unbuilt:
                result["unbuilt"].append(test_run)


def keep_advisory_only(result: dict, advisory: set[str]) -> None:
    """The advisory run (result: the merge queue's plan): the advisory test lanes and test runs as the
    queue would run them, linux when mcp or smoke runs (the editor install), a test run's job; the rest
    ran in the queue. A plan that runs nothing keeps its reasons."""
    lanes = result["lanes"]
    if not any(entry["run"] for name, entry in lanes.items() if name != "changes"):
        return
    queue = dict(lanes)
    for name in LANES[1:]:
        lanes[name] = {"run": False, "why": "the advisory run: not advisory (ADVISORY_LANES), so the merge queue ran it"}
    for name in QUEUE_DEFERRABLE:
        if name in advisory:
            lanes[name] = {"run": queue[name]["run"], "why": "the advisory run: advisory (ADVISORY_LANES), so not run in "
                                                             f"the merge queue: {queue[name]['why']}"}
    if "mcp" not in advisory:
        result["mcp"] = {"mode": "none", "files": [], "not_in_ci": []}
    if lanes["mcp"]["run"] or lanes["smoke"]["run"]:
        lanes["linux"] = {"run": True, "why": "the advisory run: the editor install the mcp and smoke lanes run"}
    for test_run, job in report.TEST_REPORTS.items():
        if test_run in advisory and queue[job]["run"]:
            lanes[job] = {"run": True, "why": f"the advisory run: for its test run {test_run}, advisory (ADVISORY_LANES), "
                                              "so not run in the merge queue"}


def _decide(event: str, plan: dict | None, plan_error: str, ci: list[str], left_out: dict[str, str],
            nightly_out: dict[str, str], full_selection: bool = False, asan_definition: str = "",
            macos_inputs: str | None = None) -> dict:
    lanes: dict[str, dict] = {}
    fallbacks: list[str] = []
    result = {"lanes": lanes, "unit_filter": {"core": "", "editor": ""}, "unit_run": {"core": False, "editor": False},
              "asan_filter": {"core": "", "editor": ""}, "asan_run": {"core": False, "editor": False},
              "htmlui_filter": "", "mcp": {"mode": "none", "files": [], "not_in_ci": []}, "fallbacks": fallbacks,
              "queue_off": [], "macos_full": False}

    def lane(name: str, run: bool, why: str) -> None:
        lanes[name] = {"run": run, "why": why}

    def tests_dir_path(name: str) -> str:
        return f"tools/mcp/tests/{name}"

    def mcp_whole(nightly: bool = False) -> None:
        files = sorted(set(ci) | ({n for n in left_out if n not in nightly_out} if nightly else set()))
        out = nightly_out if nightly else left_out
        result["mcp"] = {"mode": "nightly" if nightly else "ci", "files": [tests_dir_path(f) for f in files],
                         "not_in_ci": [{"file": tests_dir_path(n), "reason": w} for n, w in sorted(out.items())]}

    def everything(why: str, nightly: bool = False) -> None:
        for name in LANES:
            lane(name, True, why if name != "changes" else "plans the lanes")
        result["unit_run"] = {"core": True, "editor": True}
        result["asan_run"] = {"core": True, "editor": True}
        mcp_whole(nightly)

    def asan_scope() -> None:
        """A pull request's asan lane: the unit lane's selection when the sanitizers can see the change
        (SANITIZER_INPUTS), every test when the lane's settings or definition changed, else nothing."""
        own = [f for f in files if cc.match(f, ASAN_SETTINGS)] + ([asan_definition] if asan_definition else [])
        seen = [f for f in files if cc.match(f, SANITIZER_INPUTS)]
        if own:
            result["asan_run"], result["asan_filter"] = {"core": True, "editor": True}, {"core": "", "editor": ""}
            lane("asan", True, f"{own[0]} changed (the lane's settings or definition): every unit test under ASan and UBSan")
        elif not seen:
            result["asan_run"], result["asan_filter"] = {"core": False, "editor": False}, {"core": "", "editor": ""}
            lane("asan", False, "no path the sanitizers see changed (C/C++, CMake, the image, tools/build.sh, vendored "
                                "libraries): the advisory run on main runs ASan after the merge")
        elif lanes["unit"]["run"]:
            result["asan_run"], result["asan_filter"] = dict(result["unit_run"]), dict(result["unit_filter"])
            lane("asan", True, f"the unit lane's tests under ASan and UBSan ({seen[0]} changed)")
        else:
            lane("asan", False, lanes["unit"]["why"])

    def queue_macos() -> None:
        """The merge queue's macos lane: only for a macOS-only input (MACOS_PATHS, macos_inputs)."""
        why = macos_inputs if macos_inputs is not None else "(the inputs were not read)"
        why = why or next((f for f in files if cc.match(f, MACOS_PATHS)), "")
        if why:
            lane("macos", True, f"{why} changed (what an Apple build alone reads)")
        else:
            lane("macos", False, "no macOS-only input changed (MACOS_PATHS, Apple macros or CMake settings): the "
                                 "push to main runs the whole lane, the nightly too")
            result["queue_off"].append("macos")

    def apple_scope(names=APPLE_LANES) -> None:
        """A pull request's macos and ios lanes, the merge queue's ios: only when an Apple-only input
        changed (APPLE_INPUTS)."""
        apple = [f for f in files if cc.match(f, APPLE_INPUTS)]
        for name in names:
            if apple:
                lane(name, True, f"{apple[0]} changed (what only an Apple build reads)")
            elif event == "merge_group":
                lane(name, False, "no Apple-only input changed (APPLE_INPUTS): the macos lane compiles the change with "
                                  "the same Apple compiler, and the nightly builds the Xcode project for iOS")
                result["queue_off"].append(name)
            else:
                lane(name, False, "no Apple-only input changed (APPLE_INPUTS): macOS runners are the scarce ones, "
                                  "and the merge queue builds it for every change before it reaches main")

    lane("changes", True, "plans the lanes")
    if event in ("full", "nightly"):
        everything(f"{'workflow_dispatch with full: true' if event == 'full' else 'the nightly'}: every lane, "
                   "unit and HTML UI unfiltered, the whole MCP CI suite"
                   f"{' and the CI_EXCLUDED files CI can run' if event == 'nightly' else ''}", event == "nightly")
        return result
    files = plan["files"] if plan else None
    if plan:
        fallbacks.extend(plan.get("fallbacks", []))
    else:
        fallbacks.append(f"no plan ({plan_error}): every lane")
    building = [f for f in files if not cc.ci_reads_nothing(f)] if files is not None else ["(no plan)"]
    if not building:
        why = ("no file changed" if not files else
               f"every changed path ({len(files)}) is one nothing reads (CI_NOTHING_READS): no lane runs")
        for name in LANES[1:]:
            lane(name, False, why)
        return result
    first = building[0]
    if event == "push":
        for name in PLATFORM_LANES:
            lane(name, True, f"push to main: the build keeps the caches warm ({first} changed)")
        for name in ("unit", "htmlui"):
            lane(name, True, "push to main: build only (the caches and the deps dumps); its tests ran in the merge queue")
        lane("asan", True, "push to main: build only (each binary's ccache entry); its tests ran in the merge queue")
        for name in ("lint", "mcp"):
            lane(name, False, "push to main: its tests ran in the merge queue")
        # only main saves caches: the games' Compile builds would otherwise never find a warm ccache
        smoke = [f for f in files if cc.match(f, SMOKE_PATHS)] if files is not None else ["(no diff)"]
        lane("smoke", bool(smoke), f"push to main: the games' builds keep the smoke cache warm ({smoke[0]} changed)"
             if smoke else "push to main: no path a game's export reads changed")
        lane("ios", False, "push to main: keeps no cache entry, so a build would warm nothing; the merge queue ran it")
        # the queue left macos out without a macOS-only input (queue_macos): the push runs its tests too
        apple = macos_inputs if macos_inputs is not None else "(the inputs were not read)"
        apple = apple or next((f for f in files if cc.match(f, MACOS_PATHS)), "") if files is not None else "(no diff)"
        result["macos_full"] = not apple
        result["unit_run"] = {"core": True, "editor": True}
        result["asan_run"] = {"core": True, "editor": True}
        return result
    github = [f for f in files if f.startswith(".github/")] if files is not None else []
    if files is None:
        everything("no plan: every lane (fail open)")
        return result
    pr_like = event in ("pull_request", "workflow_dispatch")
    if github:
        fallbacks.append(f"{github[0]} changed: every lane, the whole MCP CI suite")
        everything(f"{github[0]} changed (CI itself): every lane")
        if pr_like and not full_selection:
            asan_scope()
            apple_scope()
        elif event == "merge_group" and not full_selection:
            apple_scope(("ios",))
            queue_macos()
        return result
    if full_selection:
        fallbacks.append(f"the repository variable {FULL_SELECTION_VARIABLE} is true: every lane, every test (the selection's rollback)")
        everything(f"{FULL_SELECTION_VARIABLE} is true: every lane, unfiltered")
        return result
    for name in PLATFORM_LANES + ("lint",):
        lane(name, True, f"{first} changed (anything outside CI_NOTHING_READS builds)")
    queue = event == "merge_group"
    if pr_like:
        apple_scope()
    elif queue:
        apple_scope(("ios",))
        queue_macos()
    # unit and ASan: the gtest suites that see the change (the merge queue: all)
    unit = plan.get("unit", {})
    if queue:
        result["unit_run"] = {"core": True, "editor": True}
        lane("unit", True, "the merge queue: every unit test")
    else:
        why = []
        for name in ("core", "editor"):
            run, flt = suite_choice(unit if unit == "all" else unit.get(name, []))
            if len(flt) > FILTER_LIMIT:
                fallbacks.append(f"the {name} filter is {len(flt)} characters: every {name} unit test")
                flt = ""
            result["unit_run"][name] = run
            result["unit_filter"][name] = flt
            if run:
                why.append(f"{name}: {'all' if not flt else str(len(unit[name])) + ' suite(s)'}")
        run = any(result["unit_run"].values())
        lane("unit", run, ("the change's unit tests (" + "; ".join(why) + ")") if run else "no unit test sees the change")
    if queue:
        result["asan_run"] = dict(result["unit_run"])
        lane("asan", True, "the merge queue: every unit test under ASan and UBSan")
    else:
        asan_scope()
    # HTML UI
    everything_trigger = [f for f in files if cc.match(f, cc.ALL_TRIGGERS)]
    if queue or everything_trigger:
        lane("htmlui", True, "the merge queue: every HTML UI test" if queue else f"{everything_trigger[0]} changed: every HTML UI test")
    else:
        run, flt = suite_choice(unit if unit == "all" else unit.get("htmlui", []))
        if len(flt) > FILTER_LIMIT:
            fallbacks.append(f"the HTML UI filter is {len(flt)} characters: every HTML UI test")
            flt = ""
        result["htmlui_filter"] = flt
        lane("htmlui", run, ("the change's HTML UI tests" + (" (all)" if not flt else "")) if run else "no HTML UI test sees the change")
    # smoke
    smoke = [f for f in files if cc.match(f, SMOKE_PATHS)]
    lane("smoke", queue or bool(smoke), "the merge queue: every game export" if queue else
         f"{smoke[0]} changed (what a game's export reads)" if smoke else "nothing a game's export reads changed")
    # MCP: the plan's files that are in the CI suite
    mcp = plan.get("mcp", [])
    ci_set = set(ci)
    if mcp in ("all", "ci"):
        if mcp == "all":
            fallbacks.append("the plan is the whole MCP suite: in CI, the whole CI suite")
        mcp_whole()
        if mcp == "ci":   # (what the CI suite leaves out of what the plan named)
            named = {s["file"] for s in plan.get("mcp_selected", [])}
            result["mcp"]["not_in_ci"] = [n for n in result["mcp"]["not_in_ci"] if n["file"].rsplit("/", 1)[-1] in named]
        lane("mcp", True, "the whole MCP CI suite" + (" (the plan widened: see fallbacks)" if mcp == "ci" else ""))
    else:
        chosen = [s["file"] for s in mcp if s["file"] in ci_set]
        result["mcp"] = {"mode": "planned" if chosen else "none", "files": [tests_dir_path(f) for f in sorted(chosen)],
                         "not_in_ci": [{"file": tests_dir_path(s["file"]),
                                        "reason": left_out.get(s["file"], "not a test file of this tree")}
                                       for s in mcp if s["file"] not in ci_set]}
        lane("mcp", bool(chosen), f"{len(chosen)} planned MCP file(s) of the CI suite" if chosen else
             "the planned MCP files are not in the CI suite (not_in_ci)" if mcp else "no MCP test sees the change")
    return result


# ---------------------------------------------------------------------------------------- selection

SELECTION_LISTED = 40   # reasons listed per test lane in the job summary (all are in lanes.json)


def _widening(fallbacks: list[str], words: tuple[str, ...], default: str) -> str:
    hits = [f for f in fallbacks if any(w in f for w in words)]
    return "; ".join(hits) or default


def selection(event: str, plan: dict | None, result: dict, ci: list[str]) -> dict:
    """What each test lane runs and why: per unit binary and for MCP, its mode (all, suites or files,
    none) and the reason of each selected suite or file by kind (check_changed.REASON_KINDS); a whole
    suite is `full` (the event runs everything) or a `fallback` (a widening, said why)."""
    full = event in ("merge_group", "advisory", "full", "nightly", "push")
    fallbacks = result["fallbacks"]
    lanes = result["lanes"]
    reasons = (plan or {}).get("unit_reasons", {})
    unit = {}
    for binary in ("core", "editor", "htmlui"):
        if binary == "htmlui":
            run, flt, lane_why = lanes["htmlui"]["run"], result["htmlui_filter"], lanes["htmlui"]["why"]
            queue_full = event in ("full", "nightly", "merge_group", "advisory") or "every HTML UI test" in lane_why
        else:
            run, flt, lane_why = result["unit_run"][binary], result["unit_filter"][binary], lanes["unit"]["why"]
            queue_full = full
        if not run:
            unit[binary] = {"mode": "none", "kind": "", "why": lane_why if not lanes["unit"]["run"] or binary == "htmlui"
                            else "no unit test sees the change", "suites": []}
        elif not flt:
            unit[binary] = {"mode": "all", "kind": "full" if queue_full and event != "pull_request" else "fallback",
                            "why": lane_why if full else _widening(fallbacks, ("unit test", "HTML UI test", f"{binary} filter",
                                                                               ".github/", "no plan"), lane_why),
                            "suites": []}
        else:
            names = (plan or {}).get("unit", {}).get(binary, []) if isinstance((plan or {}).get("unit"), dict) else []
            known = reasons.get(binary, {})
            unit[binary] = {"mode": "suites", "kind": "", "why": lane_why, "suites": [
                {"suite": n, "kind": known.get(n, ["dependency", ""])[0], "why": known.get(n, ["", "sees the change"])[1]}
                for n in names]}
    mcp = result["mcp"]
    picked = {f.rsplit("/", 1)[-1] for f in mcp["files"]}
    if mcp["mode"] in ("ci", "nightly"):
        files_sel = {"mode": mcp["mode"], "kind": "full" if event in ("full", "nightly") else "fallback",
                     "why": lanes["mcp"]["why"] if event in ("full", "nightly") else
                     _widening(fallbacks, ("MCP", "CI suite", "map", ".github/", "no plan"), lanes["mcp"]["why"]),
                     "files": []}
    else:
        listed = plan.get("mcp_selected") or (plan.get("mcp") if isinstance(plan.get("mcp"), list) else []) if plan else []
        files_sel = {"mode": mcp["mode"], "kind": "", "why": lanes["mcp"]["why"], "files": [
            {"file": e["file"], "kind": e.get("kind", "dependency"), "why": e.get("reason", "")}
            for e in listed if e["file"] in picked]}
    files_sel["of"] = len(ci)
    return {"unit": unit, "mcp": files_sel}


def _kinds(entries: list[dict]) -> str:
    counts: dict[str, int] = {}
    for e in entries:
        counts[e["kind"]] = counts.get(e["kind"], 0) + 1
    return ", ".join(f"{n} {k}" for k, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])))


def selection_markdown(sel: dict, fallbacks: list[str], manifest: dict) -> str:
    """The job summary's account of the selection: a row per test lane, then each reason."""
    lines = ["### Test selection", "", "| Lane | Selected | Why |", "|---|---|---|"]
    titles = {"core": "unit (core)", "editor": "unit (editor)", "htmlui": "HTML UI"}
    for binary, entry in sel["unit"].items():
        if entry["mode"] == "suites":
            picked, why = f"{len(entry['suites'])} suites", _kinds(entry["suites"])
        else:
            picked, why = entry["mode"], (f"{entry['kind']}: {entry['why']}" if entry["kind"] else entry["why"])
        lines.append(f"| {titles[binary]} | {picked} | {why.replace('|', '/')} |")
    m = sel["mcp"]
    if m["mode"] in ("ci", "nightly"):
        picked, why = f"all {m['of']} files", f"{m['kind']}: {m['why']}"
    elif m["mode"] == "planned":
        picked, why = f"{len(m['files'])} of {m['of']} files", _kinds(m["files"])
    else:
        picked, why = "none", m["why"]
    if m.get("tests"):
        picked += f" ({m['tests']} tests)"
    lines.append(f"| MCP | {picked} | {why.replace('|', '/')} |")
    entries = [f"- `{e['file']}` ({e['kind']}): {e['why']}" for e in m["files"]]
    for binary, entry in sel["unit"].items():
        entries += [f"- `{e['suite']}` ({titles[binary]}, {e['kind']}): {e['why']}" for e in entry["suites"]]
    if entries:
        lines += ["", f"<details><summary>Why each was selected ({len(entries)})</summary>", ""]
        lines += entries[:SELECTION_LISTED]
        if len(entries) > SELECTION_LISTED:
            lines.append(f"- ... and {len(entries) - SELECTION_LISTED} more (the `plan` artifact's lanes.json lists all)")
        lines += ["", "</details>"]
    if fallbacks:
        lines += ["", "Widened: " + "; ".join(fallbacks)]
    inputs = []
    for name, label in (("coverage-map", "coverage map"), ("deps/core-tests", "core deps dump"),
                        ("deps/editor-tests", "editor deps dump"), ("deps/htmlui-tests", "HTML UI deps dump")):
        made = manifest.get(name)
        if made:
            inputs.append(f"{label} of {str(made.get('commit', '?'))[:12]} (run {made.get('run', '?')}, "
                          f"published {(time.time() - float(made.get('created', 0))) / 86400:.1f} days ago)")
    if manifest:
        lines += ["", "Plan inputs (ci-plan-inputs): " + ("; ".join(inputs) or "none")]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------- shadow

# the events that run more than a pull request would: their lanes.json carries a PR's selection of
# their diff (tools/ci/shadow.py judges their failures against it)
SHADOW_EVENTS = ("merge_group", "advisory", "push", "full", "nightly")


def shadow_selection(plan: dict, ci: list[str], left_out: dict[str, str], nightly_out: dict[str, str],
                     asan_definition: str = "") -> dict:
    """What a pull request with plan's diff would run: per unit binary "all" or its suites, the same
    under ASan, and the MCP files ("all": the whole CI suite). Without the rollback switch: it measures
    the shortcut."""
    r = decide("pull_request", plan, "", ci, left_out, nightly_out, asan_definition=asan_definition)
    named = plan.get("unit") if isinstance(plan.get("unit"), dict) else {}

    def chosen(run: bool, flt: str, binary: str):
        return [] if not run else "all" if not flt else sorted(named.get(binary, []))

    unit = {}
    for binary in ("core", "editor", "htmlui"):
        run = r["lanes"]["htmlui"]["run"] if binary == "htmlui" else r["unit_run"][binary]
        unit[binary] = chosen(run, r["htmlui_filter"] if binary == "htmlui" else r["unit_filter"][binary], binary)
    asan = {b: chosen(r["lanes"]["asan"]["run"] and r["asan_run"][b], r["asan_filter"][b], b) for b in ("core", "editor")}
    mode = r["mcp"]["mode"]
    mcp = "all" if mode in ("ci", "nightly") else sorted(f.rsplit("/", 1)[-1] for f in r["mcp"]["files"])
    return {"base": plan.get("base", ""), "files": plan.get("files") or [], "fallbacks": r["fallbacks"], "unit": unit,
            "asan": asan, "mcp": mcp}


def plan_shadow(event: str, plan: dict | None, error: str, shadow_base: str | None, compute) -> tuple[dict | None, str]:
    """The PR-style plan a fuller run is judged against: its own diff's (the queue, the advisory run, a
    push), or for a run with no diff of its own (full, nightly) the diff since shadow_base
    (compute(base) plans it)."""
    if event not in SHADOW_EVENTS:
        return None, "a pull request runs its own selection: nothing to shadow"
    if event in ("full", "nightly"):
        if not shadow_base:
            return None, "no --shadow-base: no diff to plan a pull request's selection of"
        plan, error = compute(shadow_base)
    return (plan, "") if plan else (None, f"no PR-style plan ({error})")


def job_blocks(text: str, job: str = ASAN_JOB) -> list[tuple[int, int]]:
    """The 1-based line ranges of ci.yml that define a lane: its job and the workflow's env (the
    builds' settings), without the comments and blank lines that close a block (the next one's). Empty
    when the job is not found."""
    lines = text.splitlines()
    blocks = []
    for start_re, end_re in ((rf"^  {job}:\s*(#.*)?$", r"^(  [A-Za-z0-9_-]+:\s*(#.*)?|\S.*)$"),
                             (r"^env:\s*(#.*)?$", r"^\S.*$")):
        start = next((i for i, line in enumerate(lines) if re.match(start_re, line)), None)
        if start is None:
            if "env" not in start_re:
                return []
            continue
        end = next((i for i in range(start + 1, len(lines)) if re.match(end_re, lines[i])), len(lines))
        while end > start + 1 and (not lines[end - 1].strip() or lines[end - 1].lstrip().startswith("#")):
            end -= 1
        blocks.append((start + 1, end))
    return blocks


def definition_change(root: Path, base: str, files: list[str], job: str = ASAN_JOB) -> str:
    """Where the diff since base changes a lane's definition in ci.yml (job_blocks), or "" when it
    does not. A workflow it cannot read counts as changed (fail open)."""
    if WORKFLOW not in files:
        return ""
    changed = f"{WORKFLOW} (the {job} job or the workflow's env)"
    old = git(root, "show", f"{base}:{WORKFLOW}")
    diff = git(root, "diff", "-U0", "--no-renames", base, "HEAD", "--", WORKFLOW)
    try:
        new = (root / WORKFLOW).read_text()
    except OSError:
        return changed if diff.returncode == 0 else f"{WORKFLOW} (unreadable)"
    if diff.returncode != 0:
        return f"{WORKFLOW} (no diff: {diff.stderr.strip()[-120:]})"
    old_blocks = job_blocks(old.stdout, job) if old.returncode == 0 else []
    new_blocks = job_blocks(new, job)
    if not new_blocks or (old.returncode == 0 and not old_blocks):
        return f"{WORKFLOW} (no {job} job found)"

    def touches(first: int, count: int, blocks) -> bool:
        return count > 0 and any(first <= end and first + count - 1 >= start for start, end in blocks)

    for o_first, o_count, n_first, n_count in cc.parse_hunks(diff.stdout).get(WORKFLOW, []):
        if touches(o_first, o_count, old_blocks) or touches(n_first, n_count, new_blocks):
            return changed
    return ""


def macos_inputs(root: Path, base: str, files: list[str]) -> str:
    """The first macOS-only input of the diff since base, said, or "": a path (MACOS_PATHS), a C/C++ source
    whose text, before or after, names an Apple macro (MACOS_TEXT), a CMake file whose changed lines or their
    context name an Apple setting (MACOS_CMAKE), or ci.yml's macos job or env (definition_change)."""
    for f in files:
        if cc.match(f, MACOS_PATHS):
            return f
    sources = (*cc.CXX, ".inc", ".c", ".m", ".mm")
    for f in files:
        if not f.endswith(sources):
            continue
        texts = [git(root, "show", f"{base}:{f}").stdout]
        try:
            texts.append((root / f).read_text(errors="replace"))
        except OSError:
            pass
        m = next((MACOS_TEXT.search(t) for t in texts if MACOS_TEXT.search(t)), None)
        if m:
            return f"{f} (it names {m.group(0)})"
    for f in files:
        if f.endswith(("CMakeLists.txt", ".cmake", "CMakePresets.json")):
            hunks = git(root, "diff", "-U10", base, "HEAD", "--", f).stdout
            m = MACOS_CMAKE.search("\n".join(l for l in hunks.splitlines() if not l.startswith(("+++", "---"))))
            if m:
                return f"{f} (a change near {m.group(0)})"
    return definition_change(root, base, files, MACOS_JOB)


def newer_push(root: Path, head: str, newest: str | None) -> tuple[str, str] | None:
    """main's newer head and the first path after head that builds, when there is one: that push's run
    then builds and saves the caches. None while main is at head, when the newer commits touch only
    paths nothing reads (that run plans nothing, so head's must build), or newest is unknown here."""
    newest_sha = resolve(root, newest) if newest else None
    head_sha = resolve(root, head)
    if not newest_sha or not head_sha or newest_sha == head_sha:
        return None
    r = git(root, "diff", "--name-only", head_sha, newest_sha)
    if r.returncode != 0:
        return None
    building = [f for f in r.stdout.splitlines() if f and not cc.ci_reads_nothing(f)]
    return (newest_sha, building[0]) if building else None


def previous_run(runs, run_id: int | None, title: str = "") -> tuple[str, int] | None:
    """The head and id of the newest run in runs (`gh run list --json databaseId,headSha,status,
    conclusion,createdAt,displayTitle`) other than run_id, named starting with title, that ran to its
    end: a cancelled one was dropped while it waited (or stopped), so it planned or tested nothing."""
    if not isinstance(runs, list):
        return None
    done = [r for r in runs if isinstance(r, dict) and r.get("databaseId") != run_id and r.get("status") == "completed"
            and r.get("conclusion") != "cancelled" and str(r.get("displayTitle", "")).startswith(title)
            and isinstance(r.get("headSha"), str)
            and re.fullmatch(r"[0-9a-f]{40}", r["headSha"]) and isinstance(r.get("databaseId"), int)]
    done.sort(key=lambda r: str(r.get("createdAt", "")), reverse=True)
    return (done[0]["headSha"], done[0]["databaseId"]) if done else None


def load_runs(path: Path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------------------------------- the windows legs

# the windows lane's legs that a pull request, a dispatch, the merge queue and the advisory run build only
# for their own inputs. Each compiles the engine and its core tests as engine-glcore does, for one more
# backend, and runs no test (the runner has no GPU), in 56-97 min (merge-queue runs 45115998605 to
# 45216542055; windows-compile waits for every leg): all it can catch is code that compiles for its backend
# alone. Generic engine code rarely breaks one backend alone, and engine-glcore compiles it with the same
# MSVC; the nightly (nightly.yml windows-engine) builds both legs every night, whatever changed. A push and
# a full run keep every leg (on a push engine-vulkan saves the Vulkan SDK's cache)
WINDOWS_BACKEND_LEGS = ("engine-d3d11", "engine-vulkan")
WINDOWS_SCOPED = ("pull_request", "workflow_dispatch", "merge_group", "advisory")
# what every backend leg reads that compiles or translates differently per backend: the sokol renderer and
# its vendored library, the shaders, their stubs, compilers and translators (glslang, SPIRV-Cross), the
# shader data, variants and pools, the Windows platform code, the engine's presets and CMake modules (the
# toolchain), and the lanes' own scripts and report action (their job in ci.yml: definition_change)
WINDOWS_BACKEND_INPUTS = ("engine/renders/*", "engine/libs/sokol/*", "engine/libs/shadercompiler/*", "engine/shaderlib/*",
                          "engine/shaders/*", "engine/core/shader/*", "engine/core/pool/ShaderPool*",
                          "engine/core/render/ShaderRender*", "engine/platform/win/*", "engine/CMakePresets.json",
                          "engine/cmake/*", "tools/ci/windows.sh", "tools/ci/platform-common.sh", ".github/actions/report/*")
WINDOWS_LEG_INPUTS = {"engine-d3d11": (), "engine-vulkan": ("engine/platform/common/VulkanContext*",)}
# and a changed C/C++ file the legs compile (the engine; the procedural material sources the core tests
# build, engine/tests/core/CMakeLists.txt) whose text has code of one backend: a sokol backend guard (its
# other branch is another backend's), or the leg's own API. Read at plan time, so a new backend-specific
# file needs no entry here. The other platforms' code is no Windows leg's (engine/platform/win and
# VulkanContext are inputs above)
BACKEND_GUARD = r"\bSOKOL_(?:GLCORE|GLES3|D3D11|METAL|VULKAN|WGPU|DUMMY_BACKEND)\b"
WINDOWS_LEG_MARKERS = {"engine-d3d11": re.compile(rf"{BACKEND_GUARD}|(?i:d3d1[01]|dxgi)"),
                       "engine-vulkan": re.compile(rf"{BACKEND_GUARD}|(?i:vulkan|spir-?v)|\bVK_|\b[Vv]k[A-Z]")}
MARKED_PATHS = ("engine/*", "editor/procmat/*")
UNMARKED_PATHS = ("engine/platform/*", "engine/workspaces/*")
MARKED_TYPES = tuple(e for e in cc.CXX if e not in (".mm", ".m"))
# a CMake file of those (engine/CMakeLists.txt's source lists change with most features): only a changed
# line that names the backend or Windows, or that an if(), function() or macro() naming one encloses; its
# other lines are every backend's, which engine-glcore configures
CMAKE_TYPES = (".cmake", "CMakeLists.txt")
CMAKE_WINDOWS = re.compile(r"(?i:\bWIN32\b|\bMSVC\b|GRAPHIC_BACKEND|windows)")
CMAKE_BLOCK = re.compile(r"^\s*(if|elseif|else|endif|function|endfunction|macro|endmacro)\s*\((.*)", re.I)
WINDOWS_JOB = "windows"


def cmake_scopes(text: str) -> list[str]:
    """Each line of a CMake file after the conditions and names of the blocks around it."""
    stack: list[str] = []
    out = []
    for line in text.splitlines():
        m = CMAKE_BLOCK.match(line)
        word = m.group(1).lower() if m else ""
        if word in ("if", "function", "macro"):
            stack.append(m.group(2))
        elif word in ("elseif", "else") and stack:
            stack[-1] += " " + m.group(2)
        out.append(" ".join([*stack, line]))
        if word.startswith("end") and stack:
            stack.pop()
    return out


def cmake_changes(root: Path, base: str | None, files: list[str]) -> dict[str, list[str]] | None:
    """The changed lines of each CMake file since base, each after its blocks' conditions (cmake_scopes;
    both sides of the diff), or None when there is no diff to read."""
    if not files:
        return {}
    if not base:
        return None
    diff = git(root, "diff", "-U0", "--no-renames", base, "HEAD", "--", *files)
    if diff.returncode != 0:
        return None
    changed = {}
    for f, hunks in cc.parse_hunks(diff.stdout).items():
        old = git(root, "show", f"{base}:{f}").stdout
        try:
            new = (root / f).read_text(errors="replace")
        except OSError:
            new = ""
        old_lines, new_lines = cmake_scopes(old), cmake_scopes(new)
        changed[f] = [*(old_lines[i - 1] for o, oc, _n, _nc in hunks for i in range(o, o + oc) if 0 < i <= len(old_lines)),
                      *(new_lines[i - 1] for _o, _oc, n, nc in hunks for i in range(n, n + nc) if 0 < i <= len(new_lines))]
    return changed


def windows_inputs(root: Path, files: list[str], definition: str = "", base: str | None = None) -> dict[str, str]:
    """Each backend leg's first changed input (WINDOWS_BACKEND_INPUTS, WINDOWS_LEG_INPUTS, a file
    WINDOWS_LEG_MARKERS marks, a CMake line CMAKE_WINDOWS or the markers mark) and why, or "" for none;
    definition: the windows job's or the workflow's env changed (definition_change), which every leg reads.
    The CMake lines are the diff since base (none: every line of the file, fail open)."""
    texts: dict[str, str] = {}

    def text(f: str) -> str:
        if f not in texts:
            try:
                texts[f] = (root / f).read_text(errors="replace")
            except OSError:   # (deleted: nothing of it is compiled any more)
                texts[f] = ""
        return texts[f]

    ours = [f for f in files if cc.match(f, MARKED_PATHS) and not cc.match(f, UNMARKED_PATHS)]
    marked = [f for f in ours if f.endswith(MARKED_TYPES)]
    cmake = [f for f in ours if f.endswith(CMAKE_TYPES) and not cc.match(f, WINDOWS_BACKEND_INPUTS)]
    lines = cmake_changes(root, base, cmake)
    if lines is None:
        lines = {f: [text(f)] for f in cmake}
    found = {}
    for leg in WINDOWS_BACKEND_LEGS:
        path = next((f for f in files if cc.match(f, (*WINDOWS_BACKEND_INPUTS, *WINDOWS_LEG_INPUTS[leg]))), None)
        names = next((f for f in marked if WINDOWS_LEG_MARKERS[leg].search(text(f))), None)
        names = names or next((f for f in cmake if any(CMAKE_WINDOWS.search(line) or WINDOWS_LEG_MARKERS[leg].search(line)
                                                     for line in lines.get(f, []))), None)
        found[leg] = (f"{definition} changed" if definition else f"{path} changed" if path else
                      f"{names} changed (in code of the backend or of Windows)" if names else "")
    return found


def windows_legs(event: str, plan: dict | None, result: dict, inputs: dict[str, str], full_selection: bool = False,
                 forced=()) -> list[str]:
    """The windows lane's matrix (WINDOWS_PARTS): without the backend legs whose inputs did not change
    (inputs: windows_inputs) on a WINDOWS_SCOPED event, unless there is no plan (fail open), the rollback
    switch is on or a dispatch forces the leg onto a runner (forced: its jobs); each backend leg's reason
    joins the lane's why. Empty when the lane does not run."""
    lane = result["lanes"]["windows"]
    if not lane["run"]:
        return []
    if event not in WINDOWS_SCOPED or plan is None or full_selection:
        return list(WINDOWS_PARTS)
    off = [leg for leg in WINDOWS_BACKEND_LEGS if not inputs.get(leg) and f"windows-{leg}" not in forced]
    notes = [f"{leg}: {inputs[leg]}" for leg in WINDOWS_BACKEND_LEGS if inputs.get(leg)]
    if off:
        notes.append(f"not {' or '.join(off)}: no input of theirs changed (WINDOWS_BACKEND_INPUTS), the nightly builds them")
    if notes:
        lane["why"] += "; " + "; ".join(notes)
    return [p for p in WINDOWS_PARTS if p not in off]


# ---------------------------------------------------------------------------------------- runners

# where a job runs (ci.yml's runs-on reads lanes.json runs_on by job: a lane's name, or <lane>-<entry> for a
# matrix job: unit-editor-1, asan-core, mcp-3, windows-engine-d3d11): GitHub's hosted runner of its OS,
# unless an overflow runner takes it (docs/testing.md "Overflow runners")
HOSTED = {"linux": "ubuntu-latest", "windows": "windows-latest", "macos": "macos-latest"}
# the overflow runners, by label: their OS and the jobs each may take, best first (each runs one at a
# time). je-win (8 cores, 7 GB) takes the engine legs; the editor's MSVC build does not fit
STATE_MAX_AGE = 300         # a runner state older than this (s) means no overflow runner
OVERFLOW = {
    "je-mac": ("macos", ("macos", "ios")),
    "je-linux": ("linux", ("linux", "asan-editor", "unit-editor-1", "unit-editor-2", "asan-core", "smoke", "htmlui",
                           "android", "web", "unit-core", "lint")),
    "je-win": ("windows", ("windows-engine-d3d11", "windows-engine-glcore")),
}
# the jobs of one plan a runner takes (it runs them one after the other): the Mac builds ios in 12 min
# (run 45104498209), so the plan's macos and ios both beat a saturated hosted queue there
TAKES = {"je-mac": 2}
# where tools/ci/runner_dispatch.py publishes the state every minute: a data branch (one parentless commit
# of state.json), which `changes` fetches as it plans. Not a repository variable: a run reads `vars` as it
# is created, and its `changes` job can wait an hour for a hosted runner (run 45109730961 read one 891 s old)
STATE_BRANCH = "ci-runner-state"
DISABLED_VARIABLE = "SELF_HOSTED_DISABLED"
WINDOWS_PARTS = ("editor", "engine-glcore", "engine-d3d11", "engine-vulkan")


def planned_jobs(doc: dict) -> dict[str, str]:
    """Every job the plan runs, with its OS, by its runs_on name."""
    lanes = doc["lanes"]
    jobs: dict[str, str] = {}
    for name in ("lint", "linux", "htmlui", "smoke", "web", "android"):
        if lanes[name]["run"]:
            jobs[name] = "linux"
    if lanes["unit"]["run"]:
        jobs.update({f"unit-{j}": "linux" for j in doc["unit_jobs"]})
    jobs.update({f"asan-{j}": "linux" for j in doc["asan_jobs"]})
    if lanes["mcp"]["run"]:
        jobs.update({f"mcp-{k}": "linux" for k in doc["mcp"]["shards"]})
    if lanes["windows"]["run"]:
        # (a plan without windows_parts: every leg)
        jobs.update({f"windows-{p}": "windows" for p in (*doc.get("windows_parts", WINDOWS_PARTS), "compile")})
    for name in ("macos", "ios"):
        if lanes[name]["run"]:
            jobs[name] = "macos"
    return jobs


def lane_of(job: str) -> str:
    return next((n for n in LANES if job == n or job.startswith(n + "-")), job)


def trusted(trigger: str, head_repo: str, repo: str) -> str:
    """"" when the run may use an overflow runner, else why not: a push, the merge queue, a dispatch, or
    a pull request from a branch of this repository; never a fork's (its code would run on a person's
    machine)."""
    if trigger in ("push", "merge_group", "workflow_dispatch"):
        return ""
    if trigger == "pull_request":
        return "" if repo and head_repo == repo else f"a pull request from {head_repo or 'an unknown repository'}, not {repo}"
    return f"a {trigger or 'unknown'} event"


def parse_force(text: str) -> tuple[dict[str, str], list[str]]:
    """The self_hosted dispatch input: `<job>=<label> ...`; the entries it cannot use, said."""
    force, bad = {}, []
    for item in text.replace(",", " ").split():
        job, _, label = item.partition("=")
        if label in OVERFLOW:
            force[job] = label
        else:
            bad.append(f"{item}: not <job>=<{'|'.join(OVERFLOW)}>")
    return force, bad


def restrict_to_forced(result: dict, force: dict[str, str]) -> None:
    """A dispatch that forces jobs onto overflow runners runs only their lanes, and those whatever the
    change (it tests the runners: a PR-like plan leaves macos and ios out without an Apple input)."""
    keep = {lane_of(job) for job in force}
    for name in LANES[1:]:
        if name in keep and not result["lanes"][name]["run"] and name in (*PLATFORM_LANES, "lint"):
            result["lanes"][name] = {"run": True, "why": "workflow_dispatch with self_hosted: a forced job's lane"}
        elif name not in keep and result["lanes"][name]["run"]:
            result["lanes"][name] = {"run": False, "why": "workflow_dispatch with self_hosted: only the forced jobs' lanes run"}
    if "mcp" not in keep:
        result["mcp"] = {"mode": "none", "files": [], "not_in_ci": []}
    if "unit" not in keep:
        result["unit_run"] = {"core": False, "editor": False}
    if "asan" not in keep:
        result["asan_run"] = {"core": False, "editor": False}


def route(jobs: dict[str, str], state, now: float, trigger: str, head_repo: str, repo: str, attempt: int,
          disabled: str = "", force: dict[str, str] | None = None, run_id: str = "") -> tuple[dict[str, str], list[str]]:
    """runs_on of each planned job and why. A job goes to an overflow runner only when the run may use one
    (trusted), on its first attempt (a rerun, which the watchdog makes when a runner went away, stays
    hosted), when state (STATE_BRANCH's state.json: tools/ci/runner_dispatch.py, at most STATE_MAX_AGE old) says
    the hosted runners of its OS are saturated (the merge queue, the critical path: macOS as soon as a
    hosted macOS job waits) and the runner is online, idle and fit, and it is not in disabled
    (SELF_HOSTED_DISABLED: labels, or `all`); each runner takes TAKES jobs (default one). force (a dispatch's
    self_hosted input, first attempt only) puts those jobs on those runners whatever the state says."""
    runs_on = {job: HOSTED[os_] for job, os_ in jobs.items()}
    why = trusted(trigger, head_repo, repo)
    if why:
        return runs_on, [f"hosted only: {why}"]
    # a run the watchdog moved off a runner that went away (the state's hosted_only) stays on GitHub's
    # runners, as does a forced dispatch's rerun; its rerun for an idle runner routes again
    held = {str(r) for r in state.get("hosted_only") or []} if isinstance(state, dict) else set()
    if attempt > 1 and (not run_id or str(run_id) in held or force):
        return runs_on, [f"hosted only: attempt {attempt} (a rerun after a runner went away stays on GitHub's runners)"]
    if force:
        notes = []
        for job, label in sorted(force.items()):
            if trigger != "workflow_dispatch":
                notes.append(f"{job}={label} ignored: only a workflow_dispatch forces a runner")
            elif job not in jobs or jobs[job] != OVERFLOW[label][0]:
                notes.append(f"{job}={label} ignored: no {OVERFLOW[label][0]} job {job} in this plan")
            else:
                runs_on[job] = label
                notes.append(f"{job} on {label}: forced (self_hosted)")
        return runs_on, notes
    off = set(disabled.replace(",", " ").split())
    if "all" in off:
        return runs_on, [f"hosted only: {DISABLED_VARIABLE} is all"]
    t = state.get("t") if isinstance(state, dict) else None
    if not isinstance(t, (int, float)) or not -60 <= now - t <= STATE_MAX_AGE:
        return runs_on, ["hosted only: " + ("no runner state" if t is None else
                                            f"the runner state is {now - t:.0f} s old (over {STATE_MAX_AGE})")]
    notes = []
    for label, (os_, order) in OVERFLOW.items():
        runner = (state.get("runners") or {}).get(label) or {}
        hosted = (state.get("os") or {}).get(os_) or {}
        candidates = [j for j in order if j in jobs]
        if not candidates:
            continue
        if label in off:
            notes.append(f"{label}: off ({DISABLED_VARIABLE})")
        elif not (hosted.get("saturated") or (trigger == "merge_group" and os_ == "macos" and hosted.get("queued", 0) > 0)):
            notes.append(f"{label}: hosted {os_} runners not saturated ({hosted.get('running', 0)} running, "
                         f"{hosted.get('queued', 0)} queued)")
        elif not runner.get("online"):
            notes.append(f"{label}: offline")
        elif runner.get("busy"):
            notes.append(f"{label}: busy")
        elif not runner.get("fit", True):
            notes.append(f"{label}: unfit ({runner.get('why', 'no reason given')})")
        else:
            for job in candidates[:TAKES.get(label, 1)]:
                runs_on[job] = label
                notes.append(f"{job} on {label}: hosted {os_} {'saturated' if hosted.get('saturated') else 'has a job waiting'} "
                             f"({hosted.get('running', 0)} running, {hosted.get('queued', 0)} queued), {label} idle")
    return runs_on, notes


def load_state(path: Path | None):
    try:
        return json.loads(path.read_text()) if path else None
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------------------------------- output

def unit_jobs(unit_run: dict) -> list[dict]:
    """The unit lane's matrix: a job per planned test binary, or per part of one (part K/N, other: the
    part whose compiled objects it waits for; two parts only)."""
    jobs = []
    for suite in ("core", "editor"):
        if not unit_run.get(suite):
            continue
        n = UNIT_PARTS[suite]
        if n == 1:
            jobs.append({"job": suite, "suite": suite, "part": "", "other": ""})
            continue
        for k in range(1, n + 1):
            jobs.append({"job": f"{suite}-{k}", "suite": suite, "part": f"{k}/{n}", "other": str(3 - k) if n == 2 else ""})
    return jobs


def unit_job_names(unit_run: dict) -> list[str]:
    return [j["job"] for j in unit_jobs(unit_run)]


def asan_jobs(asan_run: dict) -> list[dict]:
    """The asan lane's matrix: a job per planned test binary (in one job the cold core build, its tests
    and the editor's build left the editor's tests no time: merge-queue runs 44582666848, 44487933891)."""
    return [{"job": suite, "suite": suite} for suite in ("core", "editor") if asan_run.get(suite)]


def asan_job_names(asan_run: dict) -> list[str]:
    return [j["job"] for j in asan_jobs(asan_run)]


def planned_asan_run(lanes: dict, asan_run: dict) -> dict:
    """The binaries the asan lane runs (asan_run: the unit lane's, or both for its own settings), when it
    runs at all."""
    return asan_run if lanes.get("asan", {}).get("run") else {}


def github_output(doc: dict, unit_run: dict) -> str:
    lines = [f"full={'true' if doc['full'] else 'false'}"]
    lines += [f"run_{name}={'true' if entry['run'] else 'false'}" for name, entry in doc["lanes"].items()]
    lines += [f"unit_filter_core={doc['unit_filter']['core']}", f"unit_filter_editor={doc['unit_filter']['editor']}",
              f"unit_run_core={'true' if unit_run['core'] else 'false'}",
              f"unit_run_editor={'true' if unit_run['editor'] else 'false'}",
              # the unit lane's matrix: one job per planned test binary, the editor's in parts, side by side
              f"unit_jobs={json.dumps(unit_jobs(unit_run), separators=(',', ':'))}",
              # the asan lane's matrix: one job per test binary it runs
              f"asan_jobs={json.dumps(asan_jobs(planned_asan_run(doc['lanes'], doc['asan_run'])), separators=(',', ':'))}",
              # its filters: the unit lane's, or every test for the lane's own settings and definition
              f"asan_filter_core={doc['asan_filter']['core']}",
              f"asan_filter_editor={doc['asan_filter']['editor']}",
              f"htmlui_filter={doc['htmlui_filter']}", f"mcp_shards={json.dumps(doc['mcp']['shards'], separators=(',', ':'))}",
              # the windows lane's matrix: its legs (windows_legs)
              f"windows_parts={json.dumps(doc.get('windows_parts', []), separators=(',', ':'))}",
              # the jobs that run but skip a test run of theirs (lanes or report.TEST_REPORTS)
              f"build_only={json.dumps(doc.get('build_only', []), separators=(',', ':'))}",
              # the test runs whose binary is not built either (the merge queue's windows-editor-tests)
              f"unbuilt={json.dumps(doc.get('unbuilt', []), separators=(',', ':'))}",
              # push: the queue left macos out, so this run runs its tests too
              f"macos_full={'true' if doc.get('macos_full') else 'false'}",
              # push: ci.yml dispatches the advisory run
              f"dispatch_advisory={'true' if doc.get('dispatch_advisory') else 'false'}",
              # each job's runs-on (route)
              f"runs_on={json.dumps(doc.get('runs_on', {}), separators=(',', ':'))}"]
    return "\n".join(lines) + "\n"


def write_selection(doc: dict, event: str, plan: dict | None, result: dict, ci: list[str], tests: list[str] | None,
                    manifest_path: Path | None, out: Path) -> None:
    """lanes.json's selection, and <out>/selection.md for the job summary."""
    sel = selection(event, plan, result, ci)
    if tests and doc["mcp"]["shards"]:
        sel["mcp"]["tests"] = len(tests)
    doc["selection"] = sel
    try:
        manifest = json.loads(manifest_path.read_text()) if manifest_path else {}
    except (OSError, ValueError):
        manifest = {}
    (out / "selection.md").write_text(selection_markdown(sel, result["fallbacks"], manifest))


def head_checkout(head: str) -> tuple[Path, Path | None, str]:
    """The checkout of head: this one when it is at head, else a scratch worktree (removed after)."""
    here = resolve(cc.ROOT, "HEAD")
    want = resolve(cc.ROOT, head)
    if want is None:
        return cc.ROOT, None, f"--head {head} is no commit here"
    if want == here:
        return cc.ROOT, None, ""
    scratch = Path(tempfile.mkdtemp(prefix="plan-lanes-")) / "tree"
    r = git(cc.ROOT, "worktree", "add", "--quiet", "--detach", str(scratch), want)
    if r.returncode != 0:
        return cc.ROOT, None, f"no worktree of {head}: {r.stderr.strip()[-200:]}"
    return scratch, scratch, ""


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--event", required=True, choices=EVENTS)
    ap.add_argument("--base", required=True, help="the commit the change is against (a PR's base, the queue's base_sha)")
    ap.add_argument("--head", required=True, help="the commit planned (CI: the checkout's HEAD)")
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--deps-from", type=Path, help="the deps dumps of main (tools/ci/dump_deps.sh)")
    ap.add_argument("--coverage-map", type=Path, help="the nightly's coverage map (none: the CI suite for C++ changes)")
    ap.add_argument("--test-seconds", type=Path, help="per-test seconds of earlier runs (mcp-test-seconds.json)")
    ap.add_argument("--github-output", type=Path, help="append the job outputs here ($GITHUB_OUTPUT)")
    ap.add_argument("--advisory", default="", help="ci.yml's ADVISORY_LANES: the test lanes that cannot fail Check")
    ap.add_argument("--shadow-base", help="full and nightly: also plan a pull request's selection of the diff since "
                    "this commit (lanes.json shadow, tools/ci/shadow.py)")
    ap.add_argument("--full-selection", action="store_true",
                    help=f"the rollback switch ({FULL_SELECTION_VARIABLE}): a PR or the queue runs every test of every lane")
    ap.add_argument("--inputs-manifest", type=Path, help="the plan inputs' manifest.json (tools/ci/plan_inputs.py fetch)")
    ap.add_argument("--newest", help="push: main's head after the admission delay (a newer push supersedes this one)")
    ap.add_argument("--previous-runs", type=Path, help="push, advisory: `gh run list` JSON of the event's earlier runs on the branch")
    ap.add_argument("--run-id", type=int, help="this run's id, left out of --previous-runs")
    ap.add_argument("--runner-state", type=Path, help=f"state.json of the branch {STATE_BRANCH} (tools/ci/runner_dispatch.py)")
    ap.add_argument("--pr-advisory", default="on", help=f"{PR_ADVISORY_VARIABLE}: off runs a pull request's blocking lanes only")
    ap.add_argument("--runner-disabled", default="", help=f"{DISABLED_VARIABLE}: overflow runner labels to leave alone, or all")
    ap.add_argument("--trigger", default="", help="github.event_name (an overflow runner takes no fork's pull request)")
    ap.add_argument("--head-repo", default="", help="a pull request's head repository (github.event.pull_request.head.repo.full_name)")
    ap.add_argument("--repo", default="", help="github.repository")
    ap.add_argument("--run-attempt", type=int, default=1, help="github.run_attempt (a rerun stays hosted)")
    ap.add_argument("--self-hosted", default="", help="a dispatch's self_hosted input: <job>=<label> ...: those jobs' lanes "
                    "only, forced onto those runners on the first attempt")
    args = ap.parse_args(argv)

    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    root, scratch, head_error = head_checkout(args.head)
    try:
        base = args.base
        notes: list[str] = []
        stage = {"push": ("push", ""), "advisory": ("advisory", ADVISORY_TITLE)}.get(args.event)
        previous = previous_run(load_runs(args.previous_runs), args.run_id, stage[1]) \
            if stage and args.previous_runs else None
        found = bool(previous and resolve(root, previous[0]))
        if found:
            base = previous[0]
            notes.append(f"planned against {base[:7]}, the head of run {previous[1]}, the last {stage[0]} run that finished")
        plan, error = None, head_error
        if args.event == "advisory" and not found:
            error = error or "no earlier advisory run of this branch finished (--previous-runs): every advisory lane"
        elif args.event not in ("full", "nightly") and not head_error:
            plan, error = run_check_changed(root, base, out / "check-changed.json",
                                            args.deps_from.resolve() if args.deps_from else None,
                                            args.coverage_map.resolve() if args.coverage_map else None)
        superseded = newer_push(cc.ROOT, args.head, args.newest) if args.event == "push" else None
        ci, left_out, nightly_out = mcp_suites(root)
        deps_from = args.deps_from.resolve() if args.deps_from else None
        cmap = args.coverage_map.resolve() if args.coverage_map else None
        shadow_plan, shadow_why = plan_shadow(
            args.event, plan, error, None if head_error else args.shadow_base,
            lambda base: run_check_changed(root, base, out / "shadow-check-changed.json", deps_from, cmap))
        def definition(p: dict | None, job: str = ASAN_JOB) -> str:
            return definition_change(root, p.get("base") or base, p.get("files") or [], job) if p else ""

        mac = macos_inputs(root, plan.get("base") or base, plan.get("files") or []) if plan else None
        result = decide(args.event, plan, error, ci, left_out, nightly_out, args.advisory.split(),
                        superseded_by=superseded, full_selection=args.full_selection, asan_definition=definition(plan),
                        pr_advisory=args.pr_advisory.strip().lower() != "off", macos_inputs=mac)
        force, bad_force = parse_force(args.self_hosted)
        notes += [f"self_hosted {b}" for b in bad_force]
        # (a rerun keeps the lanes, on GitHub's runners: route)
        if force and args.trigger == "workflow_dispatch":
            restrict_to_forced(result, force)
        shards: list[str] = []
        tests: list[str] | None = None
        if result["mcp"]["mode"] != "none":
            names = [f.rsplit("/", 1)[-1] for f in result["mcp"]["files"]]
            tests, why = list_tests(root, names)
            cap = MAX_SHARDS if args.event in ("full", "nightly") else MAX_PLANNED_SHARDS
            if tests:
                shards, why, timing_fallbacks = plan_shards(root, tests, args.test_seconds.resolve() if args.test_seconds
                                                            else None, cap, out / "mcp-plan", args.event != "nightly")
                result["fallbacks"] += timing_fallbacks
            if shards:
                notes.append(why)
            else:
                # (a shard with no plan file: tools/ci/mcp_shard.sh fails it, and says why: no silent green)
                shards = ["1"]
                result["fallbacks"].append(f"the MCP shards could not be planned ({why}): the mcp lane fails")
        # (an advisory run with no earlier one has no base: --base is a placeholder there)
        doc = {"event": args.event, "full": args.event in ("full", "nightly"),
               "base": None if args.event == "advisory" and not found else resolve(root, base) or base,
               "head": resolve(root, "HEAD") or args.head, "lanes": result["lanes"], "unit_filter": result["unit_filter"],
               "unit_run": result["unit_run"], "unit_jobs": unit_job_names(result["unit_run"]),
               "asan_run": result["asan_run"], "asan_filter": result["asan_filter"],
               "asan_jobs": asan_job_names(planned_asan_run(result["lanes"], result["asan_run"])),
               "htmlui_filter": result["htmlui_filter"],
               "windows_parts": windows_legs(args.event, plan, result, windows_inputs(
                   root, plan["files"] if plan else [], definition(plan, WINDOWS_JOB), (plan or {}).get("base") or base),
                   args.full_selection, force if args.trigger == "workflow_dispatch" else ()),
               "mcp": dict(result["mcp"], shards=shards), "fallbacks": result["fallbacks"], "notes": notes,
               "macos_full": result.get("macos_full", False),
               "advisory_off": result["advisory_off"], "build_only": result["build_only"], "unbuilt": result["unbuilt"], "queue_off": result["queue_off"],
               "superseded_by": superseded[0] if superseded else None,
               "dispatch_advisory": result["dispatch_advisory"],
               "files": plan["files"] if plan else None,
               "self_tests": plan["self_tests"] if plan else [],
               "checks": {"modules_off": plan["modules_off"] if plan else "", "navigation_off": bool(plan and plan["navigation_off"])}}
        doc["runs_on"], doc["routing"] = route(planned_jobs(doc), load_state(args.runner_state), time.time(), args.trigger,
                                               args.head_repo, args.repo, args.run_attempt, args.runner_disabled, force,
                                               run_id=str(args.run_id or ""))
        write_selection(doc, args.event, plan, result, ci, tests, args.inputs_manifest, out)
        doc["shadow"] = shadow_selection(shadow_plan, ci, left_out, nightly_out, definition(shadow_plan)) if shadow_plan else None
        doc["shadow_why"] = "" if shadow_plan else shadow_why
        tmp = out / "lanes.json.tmp"
        tmp.write_text(json.dumps(doc, indent=1) + "\n")
        os.replace(tmp, out / "lanes.json")
        if args.github_output:
            with args.github_output.open("a") as f:
                f.write(github_output(doc, result["unit_run"]))
        width = max(len(n) for n in doc["lanes"])
        for name, entry in doc["lanes"].items():
            print(f"{name:<{width}}  {'run ' if entry['run'] else 'skip'}  {entry['why']}")
        print(f"mcp: {doc['mcp']['mode']}, {len(doc['mcp']['files'])} file(s), shards {doc['mcp']['shards']}, "
              f"{len(doc['mcp']['not_in_ci'])} not in CI")
        for line in doc["fallbacks"]:
            print(f"fallback: {line}")
        for line in notes:
            print(f"note: {line}")
        for line in doc["routing"]:
            print(f"runner: {line}")
        sh = doc["shadow"]
        print("shadow: " + (f"a PR's selection of {len(sh['files'])} file(s) against {str(sh['base'])[:12]}: "
                            + ", ".join(f"{k} {v if v == 'all' else len(v)}" for k, v in [*sh["unit"].items(), ("mcp", sh["mcp"])])
                            if sh else doc["shadow_why"]))
        return 0
    finally:
        if scratch is not None:
            git(cc.ROOT, "worktree", "remove", "--force", str(scratch))
            shutil.rmtree(scratch.parent, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
