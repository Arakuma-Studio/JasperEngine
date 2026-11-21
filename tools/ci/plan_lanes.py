"""tools/ci/plan_lanes.py: CI's `changes` job. Which lanes of .github/workflows/ci.yml run, with which
unit test filters and which MCP shards (CI design §8.2; contract C1).

    python3 tools/ci/plan_lanes.py --event pull_request --base <sha> --head <sha> --out-dir build/ci-plan \\
        [--deps-from DIR] [--coverage-map FILE] [--test-seconds FILE] [--github-output FILE] \\
        [--advisory "unit asan htmlui mcp smoke windows-editor-tests"]

It plans through `tools/check_changed.py --plan-json` (the per-commit gate's own selection), then
applies CI's stricter rules:

- every changed path on check-changed's CI_NOTHING_READS list (or no change): no lane runs (D3);
- anything else builds: every platform lane and lint run;
- smoke runs for what a game's export reads (SMOKE_PATHS);
- MCP runs the planned files that are in the CI suite (run_tests.ci_files()); the rest are listed
  under not_in_ci with their reason. A planned whole suite is the CI suite;
- merge_group: every lane, unit and HTML UI unfiltered, MCP planned over the group's diff (D1), but
  the test lanes and test runs --advisory names (ci.yml's ADVISORY_LANES): they cannot block a merge,
  so the queue does not wait for them (listed under advisory_off; a test run of a job that still
  builds, under build_only too). Lint and the platform builds stay whatever the list says;
- push: the build lanes (caches and plan inputs), not ASan, which keeps no cache entry, and smoke when a
  game's export reads the change, so its Compile builds' ccache stays warm; the test lanes build only
  (build_only: their tests ran in the merge queue), but for the advisory ones the queue left out, which
  run as the queue would have run them;
- full (workflow_dispatch with full: true): every lane, the whole MCP CI suite;
- nightly: as full, and MCP also runs the CI_EXCLUDED files CI can run (NIGHTLY_LEFT_OUT says which
  it cannot);
- fail open: no plan, or a .github/ change, runs every lane (MCP: the whole CI suite); check-changed's
  own widenings (a missing deps dump or map) are kept and said, in `fallbacks`.

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
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
sys.path.insert(0, str(TOOLS))
import check_changed as cc  # noqa: E402

EVENTS = ("pull_request", "merge_group", "push", "workflow_dispatch", "full", "nightly")
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
# the test lanes the merge queue leaves to the push to main while ADVISORY_LANES names them (and the
# test runs reported apart from their job, report.TEST_REPORTS)
QUEUE_DEFERRABLE = ("unit", "asan", "htmlui", "mcp", "smoke")
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
           nightly_out: dict[str, str], advisory=()) -> dict:
    """The lanes of an event, from check-changed's plan (None: there is none, plan_error says why),
    in LANES order; advisory: ADVISORY_LANES."""
    result = _decide(event, plan, plan_error, ci, left_out, nightly_out)
    result["advisory_off"] = []
    result["build_only"] = []
    if event == "merge_group":
        leave_advisory_out(result, set(advisory))
    elif event == "push":
        run_advisory_on_push(result, set(advisory), _decide("merge_group", plan, plan_error, ci, left_out, nightly_out))
    result["lanes"] = {name: result["lanes"][name] for name in LANES}
    return result


def leave_advisory_out(result: dict, advisory: set[str]) -> None:
    """The merge queue: the advisory test lanes and test runs planned off, so its Check waits only for
    what can block the merge (they run on the push to main). A group that runs nothing keeps nothing."""
    lanes = result["lanes"]
    if not any(entry["run"] for name, entry in lanes.items() if name != "changes"):
        return
    for name in QUEUE_DEFERRABLE:
        if name in advisory and lanes[name]["run"]:
            lanes[name] = {"run": False, "why": "the merge queue: advisory (ADVISORY_LANES), so it cannot block the "
                                                "merge; it runs on the push to main"}
            result["advisory_off"].append(name)
    if "mcp" in result["advisory_off"]:
        result["mcp"] = {"mode": "none", "files": [], "not_in_ci": []}
    for test_run, job in report.TEST_REPORTS.items():
        if test_run in advisory and lanes[job]["run"]:
            result["advisory_off"].append(test_run)
            result["build_only"].append(test_run)


def run_advisory_on_push(result: dict, advisory: set[str], queue: dict) -> None:
    """The push to main: what the merge queue left out for being advisory, as the queue would have run
    it (queue: its plan without --advisory); the other test lanes and test runs build only."""
    lanes = result["lanes"]
    if not any(entry["run"] for name, entry in lanes.items() if name != "changes"):
        return
    for name in QUEUE_DEFERRABLE:
        if name in advisory:
            lanes[name] = {"run": queue["lanes"][name]["run"],
                           "why": f"push to main: advisory (ADVISORY_LANES), so not run in the merge queue: "
                                  f"{queue['lanes'][name]['why']}"}
        elif name in ("unit", "htmlui"):
            result["build_only"].append(name)
    if "mcp" in advisory:
        result["mcp"] = queue["mcp"]
    if advisory & set(QUEUE_DEFERRABLE):
        result["fallbacks"].extend(f for f in queue["fallbacks"] if f not in result["fallbacks"])
    for test_run, job in report.TEST_REPORTS.items():
        if lanes[job]["run"] and test_run not in advisory:
            result["build_only"].append(test_run)


def _decide(event: str, plan: dict | None, plan_error: str, ci: list[str], left_out: dict[str, str],
            nightly_out: dict[str, str]) -> dict:
    lanes: dict[str, dict] = {}
    fallbacks: list[str] = []
    result = {"lanes": lanes, "unit_filter": {"core": "", "editor": ""}, "unit_run": {"core": False, "editor": False},
              "htmlui_filter": "", "mcp": {"mode": "none", "files": [], "not_in_ci": []}, "fallbacks": fallbacks}

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
        mcp_whole(nightly)

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
        lane("asan", False, "push to main: ASan keeps no ccache entry (the 10 GB budget) and its tests ran in the merge queue")
        for name in ("lint", "mcp"):
            lane(name, False, "push to main: its tests ran in the merge queue")
        # only main saves caches: the games' Compile builds would otherwise never find a warm ccache
        smoke = [f for f in files if cc.match(f, SMOKE_PATHS)] if files is not None else ["(no diff)"]
        lane("smoke", bool(smoke), f"push to main: the games' builds keep the smoke cache warm ({smoke[0]} changed)"
             if smoke else "push to main: no path a game's export reads changed")
        result["unit_run"] = {"core": True, "editor": True}
        return result
    github = [f for f in files if f.startswith(".github/")] if files is not None else []
    if files is None:
        everything("no plan: every lane (fail open)")
        return result
    if github:
        fallbacks.append(f"{github[0]} changed: every lane, the whole MCP CI suite")
        everything(f"{github[0]} changed (CI itself): every lane")
        return result
    for name in PLATFORM_LANES + ("lint",):
        lane(name, True, f"{first} changed (anything outside CI_NOTHING_READS builds)")
    queue = event == "merge_group"
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
    lane("asan", lanes["unit"]["run"], "the unit lane's tests under ASan and UBSan" if lanes["unit"]["run"] else lanes["unit"]["why"])
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


def asan_jobs(unit_run: dict) -> list[dict]:
    """The asan lane's matrix: a job per planned test binary (in one job the cold core build, its tests
    and the editor's build left the editor's tests no time: merge-queue runs 44582666848, 44487933891)."""
    return [{"job": suite, "suite": suite} for suite in ("core", "editor") if unit_run.get(suite)]


def asan_job_names(unit_run: dict) -> list[str]:
    return [j["job"] for j in asan_jobs(unit_run)]


def planned_asan_run(lanes: dict, unit_run: dict) -> dict:
    """The binaries the asan lane runs: the unit lane's, when the asan lane runs at all."""
    return unit_run if lanes.get("asan", {}).get("run") else {}


def github_output(doc: dict, unit_run: dict) -> str:
    lines = [f"full={'true' if doc['full'] else 'false'}"]
    lines += [f"run_{name}={'true' if entry['run'] else 'false'}" for name, entry in doc["lanes"].items()]
    lines += [f"unit_filter_core={doc['unit_filter']['core']}", f"unit_filter_editor={doc['unit_filter']['editor']}",
              f"unit_run_core={'true' if unit_run['core'] else 'false'}",
              f"unit_run_editor={'true' if unit_run['editor'] else 'false'}",
              # the unit lane's matrix: one job per planned test binary, the editor's in parts, side by side
              f"unit_jobs={json.dumps(unit_jobs(unit_run), separators=(',', ':'))}",
              # the asan lane's matrix: one job per test binary it runs
              f"asan_jobs={json.dumps(asan_jobs(planned_asan_run(doc['lanes'], unit_run)), separators=(',', ':'))}",
              f"htmlui_filter={doc['htmlui_filter']}", f"mcp_shards={json.dumps(doc['mcp']['shards'], separators=(',', ':'))}",
              # the jobs that run but skip a test run of theirs (lanes or report.TEST_REPORTS)
              f"build_only={json.dumps(doc.get('build_only', []), separators=(',', ':'))}"]
    return "\n".join(lines) + "\n"


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
    args = ap.parse_args(argv)

    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    root, scratch, head_error = head_checkout(args.head)
    try:
        plan, error = None, head_error
        if args.event not in ("full", "nightly") and not head_error:
            plan, error = run_check_changed(root, args.base, out / "check-changed.json",
                                            args.deps_from.resolve() if args.deps_from else None,
                                            args.coverage_map.resolve() if args.coverage_map else None)
        ci, left_out, nightly_out = mcp_suites(root)
        result = decide(args.event, plan, error, ci, left_out, nightly_out, args.advisory.split())
        shards: list[str] = []
        notes: list[str] = []
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
        doc = {"event": args.event, "full": args.event in ("full", "nightly"), "base": resolve(root, args.base) or args.base,
               "head": resolve(root, "HEAD") or args.head, "lanes": result["lanes"], "unit_filter": result["unit_filter"],
               "unit_run": result["unit_run"], "unit_jobs": unit_job_names(result["unit_run"]),
               "asan_jobs": asan_job_names(planned_asan_run(result["lanes"], result["unit_run"])),
               "htmlui_filter": result["htmlui_filter"],
               "mcp": dict(result["mcp"], shards=shards), "fallbacks": result["fallbacks"], "notes": notes,
               "advisory_off": result["advisory_off"], "build_only": result["build_only"],
               "files": plan["files"] if plan else None,
               "self_tests": plan["self_tests"] if plan else [],
               "checks": {"modules_off": plan["modules_off"] if plan else "", "navigation_off": bool(plan and plan["navigation_off"])}}
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
        return 0
    finally:
        if scratch is not None:
            git(cc.ROOT, "worktree", "remove", "--force", str(scratch))
            shutil.rmtree(scratch.parent, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
