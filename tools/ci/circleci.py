#!/usr/bin/env python3
"""The advisory lanes on CircleCI (docs/testing.md, "CircleCI"): .circleci/config.yml's setup job plans
them with tools/ci/plan_lanes.py, as ci.yml's `changes` job does, and continues the pipeline with the
jobs this script writes; each job runs the same tools/build.sh, tools/ci/mcp_shard.sh and
tools/smoke-test.sh commands as ci.yml's lanes, in the same GHCR images, on a 4-vCPU machine.

    python3 -I tools/ci/circleci.py warm-key                 the daily cache warm's stamp key (main)
    python3 -I tools/ci/circleci.py plan                     the setup job: plan, write the continuation
    python3 -I tools/ci/circleci.py continue                 the setup job: POST it (CIRCLE_CONTINUATION_KEY)
    python3 -I tools/ci/circleci.py prepare [--image T...]   a job's start: the tested merge, the images
    python3 -I tools/ci/circleci.py budget --until MIN [--case NAME] -- COMMAND
        COMMAND (bash) until MIN minutes after the job's start: CircleCI's free plan stops a job at 60
    python3 -I tools/ci/circleci.py ccache mark|pack|unpack|save|unhide --lane LANE
    python3 -I tools/ci/circleci.py junit                    the JUnit files store_test_results reads
    python3 -I tools/ci/circleci.py gtest-args --lane L --binary B --shard K/N [-- ARGS]   (in the image)
        ARGS with --gtest_filter narrowed to shard K of N, balanced by tools/ci/circleci-suite-seconds.json
    python3 -I tools/ci/circleci.py suite-seconds --lane L [--parts N] XML...
        that file's lane from googletest reports (GitHub's test-results-<lane> artifacts)

What runs (`plan`): a pull request's (or any branch's) lanes as ci.yml plans a pull request, over the
merge of its head into main, which every job rebuilds byte for byte; a change to the CircleCI lanes
themselves (.circleci/, this script) runs every lane whole, as a .github/ change does on GitHub. A
push to main warms the build caches at most once a day (WARM_KEY). Tags and the rest run nothing.

Every job fits CircleCI's 60 minutes: builds stop at BUILD_UNTIL and save what they compiled (a cold
cache then needs a rerun, not a bigger machine), the long test binaries run in shards (FULL_SHARDS),
and the MCP shards are planned to MCP_SHARD_SECONDS. Standard library only; exit 1 on a failure.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
import shlex
import shutil
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from xml.sax.saxutils import escape, quoteattr

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))

PLAN_DIR = Path("build/ci-plan")
STATE = Path("build/ci-circleci")
WORKSPACE = STATE / "ws"
CCACHE = Path("build/.cache/ccache")
REGISTRY = "ghcr.io/arakuma-studio"
MACHINE = {"image": "ubuntu-2404:2026.11.1"}
RESOURCE_CLASS = "large"            # 4 vCPU, 15 GB, the free plan's largest Linux VM
# minutes after a job's first step: a build stops here and saves its progress; tests stop at
# TEST_UNTIL; CircleCI's free plan ends every job at 60 (the setup, the cache save and the upload fit
# in what is left)
BUILD_UNTIL = 47
TEST_UNTIL = 55
CACHE_RETENTION = "3d"
# a run saves its lane's cache when it compiled at least this many objects (or a build was cut)
SAVE_MISSES = 50
# what the CI lanes cap each cache to (ci.yml's ccache-restore max-size); the smoke lane is three jobs here
CAPS = {"linux": "0.95G", "unit-core": "0.4G", "unit-editor": "0.85G", "asan-core": "0.8G", "asan-editor": "2.2G",
        "htmlui": "0.85G", "smoke-players": "0.8G", "smoke-post-processing": "0.8G", "smoke-ecs-collision": "0.6G"}
# shards of a whole test binary (the tests of GitHub's runs 45037289790, 45121239260: unit core 64-84 min, ASan
# core 79-89, ASan editor 45-47, HTML UI 79-113, unit editor 5); a filtered run takes its share
# trees whose cold build passes BUILD_UNTIL on 4 vCPU compile in this many part jobs first (cold, CircleCI:
# unit core 47 min, unit editor and ASan editor past 2 x 47, ASan core 34 warm)
PARTS = {"unit-core": 2, "unit-editor": 3, "asan-core": 2, "asan-editor": 3}
FULL_SHARDS = {"unit-core": 4, "unit-editor": 1, "asan-core": 4, "asan-editor": 2, "htmlui": 6}
TEST_DIRS = {"core": "engine/tests/core", "editor": "editor/tests", "htmlui": "engine/tests/htmlui"}
MCP_SHARD_SECONDS = 35 * 60         # the planned length of an MCP shard (tools/mcp/shard.py's estimate)
MCP_MAX_SHARDS = 16
MIN_STEP_SECONDS = 60               # a step with less time than this left in the job does not start
# a change to these runs every lane whole: no selection sees what they change
OWN_PATHS = (".circleci/", "tools/ci/circleci.py")
BINARIES = {"unit-core": "build/linux/core-tests/tests/core/jasperengine-core-tests",
            "asan-core": "build/linux/core-tests-address-undefined/tests/core/jasperengine-core-tests",
            "unit-editor": "build/linux/editor-tests/jasperengine-editor-tests",
            "asan-editor": "build/linux/editor-tests-address-undefined/jasperengine-editor-tests",
            "htmlui": "build/linux/htmlui-tests-glcore/tests/htmlui/jasperengine-htmlui-tests"}
# each suite's seconds in GitHub's runs (`suite-seconds`), which the shards are balanced by; a suite over
# SPLIT_SECONDS is dealt test by test
SUITE_SECONDS = HERE / "circleci-suite-seconds.json"
SPLIT_SECONDS = 300.0
MERGE_IDENTITY = {"GIT_AUTHOR_NAME": "CircleCI advisory", "GIT_AUTHOR_EMAIL": "advisory@circleci.invalid",
                  "GIT_COMMITTER_NAME": "CircleCI advisory", "GIT_COMMITTER_EMAIL": "advisory@circleci.invalid"}
LANE_ENV = {"KEEP_GOING": "1", "JASPERENGINE_PCH": "OFF", "JASPERENGINE_DEBUG_INFO": "lines", "CCACHE_MAXSIZE": "0"}


# ------------------------------------------------------------------------------------- helpers

def git(*args: str, env: dict | None = None, check: bool = True) -> str:
    r = subprocess.run(["git", *args], cwd=str(ROOT), capture_output=True, text=True, errors="replace",
                       env={**os.environ, **(env or {})})
    if check and r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()[-500:]}")
    return r.stdout.strip()


def export(**values: str) -> None:
    """Sets variables for the job's later steps ($BASH_ENV, which every step's bash sources)."""
    path = os.environ.get("BASH_ENV")
    lines = "".join(f"export {k}={shlex.quote(v)}\n" for k, v in values.items())
    if path:
        with open(path, "a") as f:
            f.write(lines)
    else:
        print(lines, end="")


def image_tag(target: str = "linux") -> str:
    return subprocess.run([str(ROOT / "tools/build.sh"), "image-tag", target], capture_output=True, text=True,
                          check=True).stdout.strip()


def make_merge(main: str, head: str) -> str:
    """The commit ci.yml tests on a pull request (refs/pull/N/merge: main first, then the head), made the
    same in every job: one identity, and the head's committer date."""
    date = git("show", "-s", "--format=%ct", head)
    env = {**MERGE_IDENTITY, "GIT_AUTHOR_DATE": f"{date} +0000", "GIT_COMMITTER_DATE": f"{date} +0000"}
    git("checkout", "-q", "--detach", main)
    r = subprocess.run(["git", "merge", "-q", "--no-ff", "--no-edit", "-m", f"advisory: merge {head[:12]} into {main[:12]}",
                        head], cwd=str(ROOT), capture_output=True, text=True, env={**os.environ, **env})
    if r.returncode != 0:
        subprocess.run(["git", "merge", "--abort"], cwd=str(ROOT), capture_output=True)
        raise RuntimeError(f"{head[:12]} does not merge into main {main[:12]} (resolve the conflict): {r.stdout.strip()[-500:]}")
    return git("rev-parse", "HEAD")


def count_suites(binary: str) -> int:
    import check_changed as cc
    suites = set()
    for path in (ROOT / TEST_DIRS[binary]).rglob("*.cpp"):
        suites.update(m.group(2) for m in cc.TEST_DEF.finditer(path.read_text(errors="replace")))
    return max(1, len(suites))


def shards_for(lane: str, gtest_filter: str, total_suites: int) -> int:
    """A whole binary runs in FULL_SHARDS[lane] shards; a filter of n suites in its share of them."""
    full = FULL_SHARDS[lane]
    positive = gtest_filter.split("-", 1)[0]
    if not positive or positive == "*":
        return full
    # (check_changed.suite_filter writes each suite three ways: S.*, */S.* and S/*.*)
    picked = len({p.replace("*/", "").split("/")[0].split(".")[0] for p in positive.split(":") if p})
    return max(1, min(full, math.ceil(full * picked / total_suites)))


# ------------------------------------------------------------------------------------- the plan

def mode_of(event: str, branch: str, stamped: bool) -> tuple[str, str]:
    if not branch:
        return "none", "a tag: the advisory lanes test pull requests"
    if event == "push" and branch == "main":
        if stamped:
            return "none", "main: today's cache warm ran already (one a day, WARM_KEY)"
        return "warm", "main: the daily cache warm (the build lanes, build only)"
    return "pr", f"{branch}: the pull request's lanes"


def reshard_mcp(plan_dir: Path, shards: list[str]) -> list[str]:
    """More shards than plan_lanes.py chose until the longest is planned under MCP_SHARD_SECONDS (a
    CircleCI job stops at 60 min), or another one would not shorten it."""
    mcp = plan_dir / "mcp-plan"
    if not shards or not (mcp / "plan.json").is_file():
        return shards
    estimate = json.loads((mcp / "plan.json").read_text())["estimate_seconds"]
    n = len(shards)
    while estimate > MCP_SHARD_SECONDS and n < MCP_MAX_SHARDS:
        trial = mcp.parent / "mcp-plan-trial"
        shutil.rmtree(trial, ignore_errors=True)
        command = [sys.executable, "-I", str(ROOT / "tools/mcp/shard.py"), "plan", "--tests", str(mcp / "tests.txt"),
                   "--out", str(trial), "--chunk-seconds", "600", "--timings", str(ROOT / "tools/mcp/ci-timings.json")]
        for k in range(1, n + 2):
            command += ["--shard", f"{k}:2"]
        r = subprocess.run(command, cwd=str(ROOT), capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"tools/mcp/shard.py plan of {n + 1} shards: {r.stderr.strip()[-500:]}")
        shutil.copy(mcp / "tests.txt", trial / "tests.txt")
        longer = json.loads((trial / "plan.json").read_text())["estimate_seconds"]
        if longer >= estimate:
            shutil.rmtree(trial)
            break
        shutil.rmtree(mcp)
        trial.rename(mcp)
        n, estimate = n + 1, longer
    print(f"circleci: {n} MCP shard(s), the longest planned at {estimate / 60:.0f} min")
    return [str(k) for k in range(1, n + 1)]


def cmd_warm_key(_args) -> int:
    print(f"{dt.datetime.now(dt.timezone.utc):%Y%m%d}-{image_tag()}")
    return 0


def cmd_plan(_args) -> int:
    event = os.environ.get("PIPELINE_EVENT", "")
    branch = os.environ.get("PIPELINE_BRANCH", "")
    stamped = (STATE / "warm-stamp" / "done").is_file()
    mode, why = mode_of(event, branch, stamped)
    head = git("rev-parse", "HEAD")
    meta = {"mode": mode, "why": why, "event": event, "branch": branch, "head": head, "main": "", "merge": head,
            "pipeline": os.environ.get("CIRCLE_PIPELINE_ID", ""), "warm_key": (STATE / "warm-key").read_text().strip()
            if (STATE / "warm-key").is_file() else ""}
    lanes = None
    if mode == "pr":
        git("fetch", "-q", "--no-tags", "origin", "+refs/heads/main:refs/remotes/origin/main")
        meta["main"] = git("rev-parse", "refs/remotes/origin/main")
        meta["merge"] = make_merge(meta["main"], head)
        files = git("diff", "--name-only", "--no-renames", meta["main"], meta["merge"]).splitlines()
        own = [f for f in files if f.startswith(OWN_PATHS)]
        planned = "full" if own else "pull_request"
        if own:
            meta["why"] += f"; {own[0]} changed (the CircleCI lanes): every lane whole"
        inputs = Path("build/ci-inputs")
        subprocess.run([sys.executable, "-I", "tools/ci/plan_inputs.py", "fetch", "--out", str(inputs)], cwd=str(ROOT), check=True)
        args = [sys.executable, "-I", "tools/ci/plan_lanes.py", "--event", planned, "--base", meta["main"], "--head", "HEAD",
                "--out-dir", str(PLAN_DIR), "--repo", "Arakuma-Studio/JasperEngine"]
        if (inputs / "deps").is_dir():
            args += ["--deps-from", str(inputs / "deps")]
        if (inputs / "coverage-map.json").is_file():
            args += ["--coverage-map", str(inputs / "coverage-map.json")]
        if (inputs / "manifest.json").is_file():
            args += ["--inputs-manifest", str(inputs / "manifest.json")]
        subprocess.run(args, cwd=str(ROOT), check=True)
        lanes = json.loads((PLAN_DIR / "lanes.json").read_text())
        if lanes["lanes"]["mcp"]["run"]:
            lanes["mcp"]["shards"] = reshard_mcp(PLAN_DIR, lanes["mcp"]["shards"])
    PLAN_DIR.mkdir(parents=True, exist_ok=True)
    totals = {b: count_suites(b) for b in TEST_DIRS}
    config = continuation(meta, lanes, totals)
    (PLAN_DIR / "circleci.json").write_text(json.dumps(meta, indent=1) + "\n")
    STATE.mkdir(parents=True, exist_ok=True)
    (STATE / "continue.json").write_text(json.dumps(config, indent=1) + "\n")
    print(f"circleci: {mode}: {meta['why']}")
    for name in config["workflows"]["advisory"]["jobs"]:
        (key, entry), = name.items()
        print(f"  {entry['name']}" + (f" (after {', '.join(entry['requires'])})" if entry.get("requires") else ""))
    return 0


def cmd_continue(_args) -> int:
    body = json.dumps({"continuation-key": os.environ["CIRCLE_CONTINUATION_KEY"],
                       "configuration": (STATE / "continue.json").read_text()}).encode()
    request = urllib.request.Request("https://circleci.com/api/v2/pipeline/continue", data=body,
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=60) as r:
        print(f"circleci: continued ({r.status}) {r.read().decode()[:200]}")
    return 0


# ------------------------------------------------------------------------------- the continuation

def continuation(meta: dict, lanes: dict | None, totals: dict[str, int]) -> dict:
    """The jobs of the pipeline (JSON, which CircleCI reads as YAML): every name advisory/<lane>."""
    jobs: dict[str, dict] = {}
    flow: list[dict] = []
    env = {**LANE_ENV, "ADVISORY_MODE": meta["mode"], "ADVISORY_MAIN": meta["main"], "ADVISORY_HEAD": meta["head"],
           "ADVISORY_MERGE": meta["merge"]}

    def add(key: str, steps: list, requires: tuple[str, ...] = (), saves: bool = False, machine: bool = True) -> str:
        job = {"environment": dict(env), "steps": steps}
        if machine:
            job.update(machine=MACHINE, resource_class=RESOURCE_CLASS)
        else:
            job.update(docker=[{"image": "cimg/base:current"}], resource_class="small")
        if saves:
            job["retention"] = {"caches": CACHE_RETENTION}
        jobs[key] = job
        entry = {"name": f"advisory/{key}"}
        if requires:
            entry["requires"] = [f"advisory/{r}" for r in requires]
        flow.append({key: entry})
        return key

    if meta["mode"] == "pr" and lanes:
        pr_jobs(lanes, totals, add)
    elif meta["mode"] == "warm":
        warm_jobs(meta, add)
    if not jobs:
        jobs["nothing"] = {"type": "no-op"}
        flow.append({"nothing": {"name": "advisory/nothing-to-run"}})
    return {"version": 2.1, "jobs": jobs, "workflows": {"advisory": {"jobs": flow}}}


def start(images: tuple[str, ...] = ("linux",), plan: bool = False, workspace: bool = False) -> list:
    steps = [{"checkout": {}},
             {"run": {"name": "The tested commit and the build images",
                      "command": "python3 -I tools/ci/circleci.py prepare " + " ".join(f"--image {i}" for i in images)}}]
    if plan:
        steps.append({"restore_cache": {"name": "The plan", "keys": ["v1-plan-{{ .Environment.CIRCLE_PIPELINE_ID }}"]}})
    if workspace:
        steps.append({"attach_workspace": {"at": str(WORKSPACE)}})
    return steps


def restore(lane: str) -> list:
    return [{"restore_cache": {"name": f"Restore ccache ({lane})",
                               "keys": [f"v1-ccache-{lane}-{{{{ checksum \"{STATE}/image-tag\" }}}}-"]}},
            {"run": {"name": "Mark the restored cache", "command": f"python3 -I tools/ci/circleci.py ccache mark --lane {lane}"}}]


def save(lane: str, only_cut: bool = False) -> list:
    """only_cut: a part job's cache is saved only when its build was stopped (the job that joins the parts saves)."""
    return [{"run": {"name": "Trim the cache to the run's working set, or keep it", "when": "always",
                     "command": f"python3 -I tools/ci/circleci.py ccache save --lane {lane}{' --only-cut' if only_cut else ''}"}},
            {"save_cache": {"name": f"Save ccache ({lane})", "when": "always", "paths": [str(CCACHE)],
                            "key": f"v1-ccache-{lane}-{{{{ checksum \"{STATE}/image-tag\" }}}}-{{{{ .BuildNum }}}}"}},
            {"run": {"name": "Back to the job's cache", "when": "always",
                     "command": f"python3 -I tools/ci/circleci.py ccache unhide --lane {lane}"}}]


def step(name: str, command: str, until: int, when: str | None = None) -> dict:
    run = {"name": name, "no_output_timeout": "30m",
           "command": f"python3 -I tools/ci/circleci.py budget --until {until} --case {shlex.quote(name)} -- {shlex.quote(command)}"}
    if when:
        run["when"] = when
    return {"run": run}


def results() -> list:
    return [{"run": {"name": "JUnit results", "when": "always", "command": "python3 -I tools/ci/circleci.py junit"}},
            {"store_test_results": {"path": str(STATE / "junit")}},
            {"store_artifacts": {"path": "build/test-results", "destination": "test-results"}}]


def pack(lane: str) -> list:
    return [{"run": {"name": "The objects this build compiled, for its shards",
                     "command": f"python3 -I tools/ci/circleci.py ccache pack --lane {lane}"}},
            {"persist_to_workspace": {"root": str(WORKSPACE), "paths": [f"ccache-{lane}.tar"]}}]


def unpack(lane: str) -> dict:
    return {"run": {"name": "Add the build job's objects", "command": f"python3 -I tools/ci/circleci.py ccache unpack --lane {lane}"}}


def flt_arg(name: str, value: str) -> str:
    return f"{name}={shlex.quote('--gtest_filter=' + value)} " if value else ""


def gtest_command(lane: str, gtest_filter: str, command: str, variable: str):
    """The test command of shard k of n: the lane's own command, a shard with the filter `gtest-args` cuts
    from the built binary's listing by the suites' measured seconds (tools/build.sh's UNIT_SHARD cuts by
    test count, which put every *GpuSlow suite in one shard)."""
    def test(k: int, n: int) -> str:
        if n == 1:
            return f"{flt_arg(variable, gtest_filter)}{command}"
        listing = f" -- {shlex.quote('--gtest_filter=' + gtest_filter)}" if gtest_filter else ""
        return (f"RUN=0 {command} && args=$(tools/build.sh exec linux python3 -I tools/ci/circleci.py gtest-args --lane {lane} "
                f"--binary {BINARIES[lane]} --shard {k}/{n}{listing}) && {variable}=\"$args\" {command}")
    return test


def parts_of(add, lane: str, build: str) -> tuple[tuple[str, ...], list]:
    """A tree whose cold build does not fit a job (PARTS) compiles in part jobs first (UNIT_BUILD_PART, as
    ci.yml's unit-editor parts): their jobs, and the steps that add their objects to the job that joins them."""
    n = PARTS.get(lane, 1)
    if n == 1:
        return (), []
    keys = tuple(add(f"{lane}-part-{k}", start() + restore(lane)
                     + [step(f"Compile part {k} of {n}", f"UNIT_BUILD_PART={k}/{n} {build}", BUILD_UNTIL),
                        {"run": {"name": "The objects this part compiled", "when": "always",
                                 "command": f"python3 -I tools/ci/circleci.py ccache pack --lane {lane} --part {k}"}},
                        {"persist_to_workspace": {"root": str(WORKSPACE), "paths": [f"ccache-{lane}-part-{k}.tar"]}}]
                     + save(lane, only_cut=True) + results(), saves=True) for k in range(1, n + 1))
    return keys, [{"attach_workspace": {"at": str(WORKSPACE)}},
                  {"run": {"name": "Add the parts' objects", "command": f"python3 -I tools/ci/circleci.py ccache unpack --lane {lane} --parts {n}"}}]


def tree_jobs(add, lane: str, build: str, test, shards: int, extra=lambda k: ([], [])) -> None:
    """A test binary: one job when it runs in one shard, else a build job and a job per shard."""
    parts, join = parts_of(add, lane, build)
    if shards == 1:
        before, after = extra(1)
        add(lane, start() + restore(lane) + join + [step("Build", build, BUILD_UNTIL)] + save(lane)
            + before + [step("Tests", test(1, 1), TEST_UNTIL)] + after + results(), requires=parts, saves=True)
        return
    # (the save decides first: the shards restore a saved cache, and get only an unsaved run's objects through
    # the workspace, which CircleCI keeps 15 days)
    decide, *rest = save(lane)
    key = add(f"{lane}-build", start() + restore(lane) + join + [step("Build", build, BUILD_UNTIL), decide] + pack(lane) + rest
              + results(), requires=parts, saves=True)
    for k in range(1, shards + 1):
        before, after = extra(k)
        add(f"{lane}-{k}", start(workspace=True) + restore(lane)[:1] + [unpack(lane)] + before
            + [step(f"Tests, shard {k} of {shards}", test(k, shards), TEST_UNTIL)] + after + results(), requires=(key,))


def pr_jobs(lanes: dict, totals: dict[str, int], add) -> None:
    run = {name: entry["run"] for name, entry in lanes["lanes"].items()}
    if run["unit"]:
        for suite in ("core", "editor"):
            if not lanes["unit_run"][suite]:
                continue
            f = lanes["unit_filter"][suite]
            lane = f"unit-{suite}"
            base = f"UNIT_SUITE={suite} "

            def core_extra(k, suite=suite):
                # ci.yml's unit (core): the presets' option errors, and the picking suites on the scalar path (S26 AC2)
                if suite != "core" or k != 1:
                    return [], []
                results_dir = "build/test-results"
                scalar = (f"if [[ -f {results_dir}/core.xml ]]; then mv {results_dir}/core.xml {results_dir}/core.main.xml; fi; status=0; "
                          "JASPER_PICK_SIMD=scalar UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=MeshBvh.*:SceneBvh.*:MeshRaycast.*:PickCache.*' "
                          "WERROR=1 tools/build.sh unit-tests || status=1; "
                          f"if [[ -f {results_dir}/core.xml ]]; then mv {results_dir}/core.xml {results_dir}/core-picking-scalar.xml; fi; "
                          f"if [[ -f {results_dir}/core.main.xml ]]; then mv {results_dir}/core.main.xml {results_dir}/core.xml; fi; exit $status")
                return ([step("CMake presets and option errors", "tools/build.sh exec linux tools/ci/check-presets.sh", TEST_UNTIL)],
                        [step("Picking on the scalar path", scalar, TEST_UNTIL, when="always")])

            tree_jobs(add, lane, f"RUN=0 {base}WERROR=1 tools/build.sh unit-tests",
                      gtest_command(lane, f, f"{base}WERROR=1 tools/build.sh unit-tests", "UNIT_TEST_ARGS"),
                      shards_for(lane, f, totals[suite]), core_extra)
    if run["asan"]:
        for suite in ("core", "editor"):
            if not lanes["asan_run"][suite]:
                continue
            f = lanes["asan_filter"][suite] or "*"
            # the *GpuSlow suites run under ASan in the nightly (ci.yml's asan job)
            f = f"{f}:*GpuSlow*" if "-" in f else f"{f}-*GpuSlow*"
            lane = f"asan-{suite}"
            base = f"SANITIZE=address,undefined UNIT_SUITE={suite} "
            tree_jobs(add, lane, f"RUN=0 {base}tools/build.sh unit-tests",
                      gtest_command(lane, f, f"{base}tools/build.sh unit-tests", "UNIT_TEST_ARGS"), shards_for(lane, f, totals[suite]))
    if run["htmlui"]:
        f = lanes["htmlui_filter"]
        command = "JASPER_HTMLUI_PERF_SLACK=3 tools/build.sh htmlui-tests"
        tree_jobs(add, "htmlui", f"RUN=0 {command}", gtest_command("htmlui", f, command, "HTMLUI_TEST_ARGS"),
                  shards_for("htmlui", f, totals["htmlui"]))
    if run["mcp"] or run["smoke"]:
        add("editor", start() + restore("linux") + [step("Build the editor", "SKIP_ENGINE=1 WERROR=1 tools/build.sh linux", BUILD_UNTIL)]
            + [{"run": {"name": "Pack the editor install",
                        "command": f"mkdir -p {WORKSPACE} && tools/ci/editor_install.sh pack build/linux/editor-install {WORKSPACE}/editor-install.tar.zst"}},
               {"persist_to_workspace": {"root": str(WORKSPACE), "paths": ["editor-install.tar.zst"]}}]
            + save("linux") + results(), saves=True)
    install = {"run": {"name": "Unpack the editor install",
                       "command": f"tools/ci/editor_install.sh unpack {WORKSPACE}/editor-install.tar.zst build/linux"}}
    if run["mcp"]:
        for shard in lanes["mcp"]["shards"]:
            add(f"mcp-{shard}", start(plan=True, workspace=True) + [install]
                + [step(f"MCP shard {shard}", f"tools/ci/mcp_shard.sh {shard} {PLAN_DIR}", TEST_UNTIL)] + results(),
                requires=("editor",))
    if run["smoke"]:
        add("smoke-players", start(images=("runtime", "linux"), workspace=True) + [install] + restore("smoke-players")
            + [step("Player runtime (Linux, OpenGL, full, release)",
                    "PLAYER_VARIANTS=full PLAYER_BACKENDS=glcore PLAYER_FLAVOURS=release tools/build.sh players", BUILD_UNTIL)]
            + save("smoke-players")
            + [step("Prebuilt smoke test, unified-lighting", "JASPER_DOCKER_IMAGE=\"$JASPER_IMAGE_RUNTIME\" SMOKE_METHOD=prebuilt "
                    "SMOKE_DIR=build/smoke-unified-lighting tools/smoke-test.sh tools/fixtures/unified-lighting", TEST_UNTIL),
               step("Prebuilt smoke test, a project from each template", "SMOKE_METHOD=prebuilt tools/ci/smoke-templates.sh",
                    TEST_UNTIL, when="always")]
            + smoke_artifacts() + results(), requires=("editor",), saves=True)
        for fixture in ("post-processing", "ecs-collision"):
            lane = f"smoke-{fixture}"
            add(lane, start(workspace=True) + [install] + restore(lane)
                + [step(f"Compile smoke test, {fixture}", f"SMOKE_METHOD=compile SMOKE_DIR=build/smoke-{fixture} "
                        f"tools/smoke-test.sh tools/fixtures/{fixture}", BUILD_UNTIL + 5)]
                + save(lane) + smoke_artifacts() + results(), requires=("editor",), saves=True)


def smoke_artifacts() -> list:
    return [{"run": {"name": "Smoke screenshots and logs", "when": "always",
                     "command": "mkdir -p build/smoke-artifacts && for f in build/smoke-*/*.png build/smoke-*/*.log; do "
                                "[ -f \"$f\" ] && cp \"$f\" build/smoke-artifacts/\"$(basename \"$(dirname \"$f\")\")-$(basename \"$f\")\"; done; true"}},
            {"store_artifacts": {"path": "build/smoke-artifacts", "destination": "smoke"}}]


def warm_jobs(meta: dict, add) -> None:
    """main, once a day: each test tree's build, saved when it compiled something (SAVE_MISSES)."""
    trees = {"linux": "SKIP_ENGINE=1 WERROR=1 tools/build.sh linux",
             "unit-core": "RUN=0 UNIT_SUITE=core WERROR=1 tools/build.sh unit-tests",
             "unit-editor": "RUN=0 UNIT_SUITE=editor WERROR=1 tools/build.sh unit-tests",
             "asan-core": "RUN=0 SANITIZE=address,undefined UNIT_SUITE=core tools/build.sh unit-tests",
             "asan-editor": "RUN=0 SANITIZE=address,undefined UNIT_SUITE=editor tools/build.sh unit-tests",
             "htmlui": "RUN=0 JASPER_HTMLUI_PERF_SLACK=3 tools/build.sh htmlui-tests"}
    keys = []
    for lane, command in trees.items():
        parts, join = parts_of(lambda key, *a, **k: add(f"warm-{key}", *a, **k), lane, command)
        keys.append(add(f"warm-{lane}", start() + restore(lane) + join + [step("Build", command, BUILD_UNTIL)] + save(lane)
                        + results(), requires=parts, saves=True))
    stamp = str(STATE / "warm-stamp")
    add("warm-done", [{"run": {"name": "Today's stamp", "command": f"mkdir -p {stamp} && date -u > {stamp}/done && "
                                                                  f"echo {shlex.quote(meta['warm_key'])} > {STATE}/warm-key"}},
                      {"save_cache": {"name": "Save the stamp", "paths": [stamp],
                                      "key": f"v1-warm-stamp-{{{{ checksum \"{STATE}/warm-key\" }}}}"}}],
        requires=tuple(keys), machine=False)


# ------------------------------------------------------------------------------------- in a job

def cmd_prepare(args) -> int:
    export(ADVISORY_JOB_START=str(int(time.time())))
    if os.environ.get("ADVISORY_MODE") == "pr":
        main, head, merge = os.environ["ADVISORY_MAIN"], os.environ["ADVISORY_HEAD"], os.environ["ADVISORY_MERGE"]
        git("fetch", "-q", "--no-tags", "origin", "+refs/heads/main:refs/remotes/origin/main")
        if subprocess.run(["git", "cat-file", "-e", f"{main}^{{commit}}"], cwd=str(ROOT)).returncode != 0:
            git("fetch", "-q", "--no-tags", "origin", main)
        made = make_merge(main, head)
        print(f"circleci: testing {made[:12]}, {head[:12]} merged into main {main[:12]}"
              + ("" if made == merge else f" (the plan's was {merge[:12]}: same tree, other bytes)"))
    STATE.mkdir(parents=True, exist_ok=True)
    (STATE / "image-tag").write_text(image_tag() + "\n")
    if not shutil.which("zstd"):    # (tools/ci/editor_install.sh packs with it)
        subprocess.run("sudo apt-get update -qq && sudo apt-get install -y -qq zstd", shell=True, check=True)
    for target in args.image:
        (STATE / "image.env").write_text("")
        env = {**os.environ, "REGISTRY": REGISTRY, "GITHUB_ENV": str(STATE / "image.env"), "GITHUB_OUTPUT": str(STATE / "image.out")}
        subprocess.run([str(ROOT / ".github/actions/image/image.sh"), target], cwd=str(ROOT), env=env, check=True)
        values = dict(line.split("=", 1) for line in (STATE / "image.env").read_text().splitlines() if "=" in line)
        export(**values)
    return 0


def kill_containers() -> None:
    ids = subprocess.run(["docker", "ps", "-q"], capture_output=True, text=True).stdout.split()
    if ids:
        subprocess.run(["docker", "kill", *ids], capture_output=True)


def record(case: str, status: str, seconds: float, detail: str = "") -> None:
    STATE.mkdir(parents=True, exist_ok=True)
    with open(STATE / "steps.jsonl", "a") as f:
        f.write(json.dumps({"case": case, "status": status, "seconds": round(seconds, 1), "detail": detail}) + "\n")


def cmd_budget(args) -> int:
    command = " ".join(args.command)
    started = float(os.environ.get("ADVISORY_JOB_START") or time.time())
    deadline = started + args.until * 60
    print(f"circleci: {command}\ncircleci: (stopped {max(0, deadline - time.time()) / 60:.0f} min from now, "
          f"{args.until} min into the job)", flush=True)
    t0 = time.time()
    if deadline - t0 < MIN_STEP_SECONDS:
        record(args.case, "fail", 0, "no time left in the job")
        print(f"circleci: no time left for this step ({args.until} min into the job)", file=sys.stderr)
        return 1
    proc = subprocess.Popen(["bash", "-eo", "pipefail", "-c", command], cwd=str(ROOT), start_new_session=True)
    try:
        status = proc.wait(timeout=deadline - t0)
    except subprocess.TimeoutExpired:
        kill_containers()
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
        (STATE / "cut").write_text(f"{args.case}\n")
        detail = (f"stopped {args.until} min into the job: CircleCI's free plan ends a job at 60 min. A build saves what it "
                  "compiled to the lane's cache, so a cold cache finishes on a rerun of the workflow")
        record(args.case, "fail", time.time() - t0, detail)
        print(f"circleci: {detail}", file=sys.stderr)
        return 124
    record(args.case, "pass" if status == 0 else "fail", time.time() - t0, "" if status == 0 else f"exit {status}")
    return status


# ------------------------------------------------------------------------------------- the cache

def ccache_in_image(*args: str, cap: str = "0") -> subprocess.CompletedProcess:
    return subprocess.run(["docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}", "-v", f"{ROOT / CCACHE}:/ccache",
                           "-e", "CCACHE_DIR=/ccache", "-e", f"CCACHE_MAXSIZE={cap}", os.environ["JASPER_DOCKER_IMAGE"],
                           "ccache", *args], capture_output=True, text=True)


def misses() -> int:
    stats = dict(line.split("\t", 1) for line in ccache_in_image("--print-stats").stdout.splitlines() if "\t" in line)
    return int(stats.get("cache_miss", "0") or 0)


def cmd_ccache(args) -> int:
    lane = args.lane
    hidden = ROOT / f"{CCACHE}.unsaved"
    tar = WORKSPACE / (f"ccache-{lane}-part-{args.part}.tar" if args.part else f"ccache-{lane}.tar")
    if args.action == "mark":
        (ROOT / CCACHE).mkdir(parents=True, exist_ok=True)
        marker = STATE / f"ccache-{lane}.marker"
        marker.touch()
        size = subprocess.run(["du", "-sh", str(ROOT / CCACHE)], capture_output=True, text=True).stdout.split("\t")[0]
        ccache_in_image("--zero-stats")
        subprocess.run([sys.executable, "-I", "tools/ci/unit_split.py", "snapshot", "--ccache", str(CCACHE),
                        "--out", str(STATE / f"ccache-{lane}.before.json")], cwd=str(ROOT), check=True)
        export(JASPER_CCACHE_LANE=lane, JASPER_CCACHE_CAP=CAPS[lane], JASPER_CCACHE_MARKER=str(ROOT / marker),
               JASPER_CCACHE_START=str(int(time.time())), JASPER_CCACHE_RESTORED_SIZE=size)
        print(f"circleci: restored {size} of ccache ({lane})")
        return 0
    if args.action == "pack":
        WORKSPACE.mkdir(parents=True, exist_ok=True)
        if not hidden.is_dir() and not args.part:
            # saved: the shards restore it from the cache
            subprocess.run(["tar", "-cf", str(tar), "-T", "/dev/null"], check=True)
            print("circleci: the cache is saved; the shards restore it")
            return 0
        return subprocess.run([sys.executable, "-I", "tools/ci/unit_split.py", "pack", "--ccache", str(hidden if hidden.is_dir() else CCACHE),
                               "--before", str(STATE / f"ccache-{lane}.before.json"), "--out", str(tar)], cwd=str(ROOT)).returncode
    if args.action == "unpack":
        (ROOT / CCACHE).mkdir(parents=True, exist_ok=True)
        tars = [WORKSPACE / f"ccache-{lane}-part-{k}.tar" for k in range(1, args.parts + 1)] if args.parts else [tar]
        for t in tars:
            if t.is_file():
                # (-m: written now, so the cache save counts them as this run's)
                subprocess.run(["tar", "-m", "-C", str(ROOT / CCACHE), "-xf", str(t)], check=True)
                print(f"circleci: added {t.stat().st_size >> 20} MiB of objects ({t.name})")
            elif args.parts:
                print(f"circleci: no {t.name}: this job compiles that part's objects")
        return 0
    if args.action == "unhide":
        if hidden.is_dir() and not (ROOT / CCACHE).exists():
            hidden.rename(ROOT / CCACHE)
        return 0
    # save: the run's working set cut to the cap when it compiled enough, the whole cache when a build was
    # cut (what it has not reached yet stays), else nothing (save_cache skips a missing path)
    if not os.environ.get("JASPER_CCACHE_LANE") or not (ROOT / CCACHE).is_dir():
        print("circleci: no cache restored in this job: nothing to save")
        return 0
    cut = (STATE / "cut").is_file()
    compiled = misses()
    decision = cut or (compiled >= SAVE_MISSES and not args.only_cut)
    print(f"circleci: {compiled} compile(s) missed the cache{', a build was cut' if cut else ''}: "
          + ("saving" if decision else "not saving (a part job saves only a stopped build)" if args.only_cut
             else f"not saving (under {SAVE_MISSES})"))
    if decision and not cut:
        env = {**os.environ, "LANE": lane, "EVENT": "push", "REF": "refs/heads/main", "RUNNER_OS": "Linux",
               "GITHUB_OUTPUT": str(STATE / "trim.out")}
        r = subprocess.run([str(ROOT / ".github/actions/ccache-save/trim.sh")], cwd=str(ROOT), env=env)
        decision = r.returncode == 0 and "save=true" in (STATE / "trim.out").read_text()
    elif cut:
        print(ccache_in_image("--show-stats").stdout)
    if not decision:
        (ROOT / CCACHE).rename(hidden)
    return 0


# ------------------------------------------------------------------------------------- the shards

def weighted_filter(suites: dict[str, list[str]], base: str | None, k: int, n: int, seconds: dict[str, float]) -> str:
    """The filter of shard k of n over the listed tests (`--gtest_filter=<base>`, in the binary's order):
    n runs of consecutive pieces of about equal seconds, a piece a suite, or a test of a suite over
    SPLIT_SECONDS. Consecutive, because tests that leave state behind for later ones exist: interleaved
    shards failed WaterBoatsGpu and EffectPerfTest, which pass after their neighbours in the whole run.
    A suite without a time costs its tests at the binary's median seconds a test. The shard runs what
    the filter's positive part and no other shard selects, so a new test runs somewhere."""
    runnable = {s: [t for t in tests if not t.startswith("DISABLED_")]
                for s, tests in suites.items() if not s.startswith("DISABLED_")}
    runnable = {s: tests for s, tests in runnable.items() if tests}
    if not runnable:
        raise ValueError(f"the filter {base or '*'} selects no test")
    pos, _, neg = (base or "*").partition("-")
    if n == 1:
        return base or "*"
    rates = sorted(seconds[s] / len(runnable[s]) for s in runnable if s in seconds)
    rate = rates[len(rates) // 2] if rates else 1.0
    items: list[tuple[float, str]] = []
    for suite, tests in runnable.items():
        cost = seconds.get(suite, rate * len(tests))
        if cost > SPLIT_SECONDS and len(tests) > 1:
            items += [(cost / len(tests), f"{suite}.{t}") for t in tests]
        else:
            items.append((cost, f"{suite}.*"))
    total = sum(c for c, _ in items)
    loads = [0.0] * n
    owner: dict[str, int] = {}
    done, shard = 0.0, 0
    for cost, pattern in items:
        # the next shard starts once this one holds its share (the piece's middle past the boundary)
        while shard < n - 1 and done + cost / 2 > total * (shard + 1) / n:
            shard += 1
        owner[pattern] = shard
        loads[shard] += cost
        done += cost
    exclude = [p for p, i in owner.items() if i != k - 1]
    print(f"circleci: shard {k} of {n}: {sum(1 for i in owner.values() if i == k - 1)} piece(s), about "
          f"{loads[k - 1] / 60:.0f} min (shards: {', '.join(f'{x / 60:.0f}' for x in loads)})", file=sys.stderr)
    negative = [p for p in neg.split(":") if p] + exclude
    return f"{pos or '*'}-{':'.join(negative)}"


def cmd_gtest_args(args) -> int:
    """In the image: the test arguments of a shard (unit_split.py args' interface, weighted)."""
    sys.path.insert(0, str(HERE))
    import unit_split as us
    k, n = us.parse_part(args.shard)
    rest, base = us.split_filter(args.test_args)
    listing = [args.binary, "--gtest_list_tests"] + ([f"--gtest_filter={base}"] if base is not None else [])
    # (an ASan binary's leak check at exit is not this listing's business)
    r = subprocess.run(listing, capture_output=True, text=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    if r.returncode != 0:
        print(f"circleci: {' '.join(listing)} failed ({r.returncode}): {r.stderr.strip()[-2000:]}", file=sys.stderr)
        return 1
    seconds = json.loads(SUITE_SECONDS.read_text()).get(args.lane, {}) if SUITE_SECONDS.is_file() else {}
    print(" ".join(rest + [f"--gtest_filter={weighted_filter(us.parse_listing(r.stdout), base, k, n, seconds)}"]))
    return 0


def cmd_suite_seconds(args) -> int:
    """Each suite's seconds in googletest XML reports of a lane (the most of the reports), into SUITE_SECONDS."""
    import xml.etree.ElementTree as ET
    data = json.loads(SUITE_SECONDS.read_text()) if SUITE_SECONDS.is_file() else {}
    runs: list[dict[str, float]] = []
    for path in args.xml:
        run: dict[str, float] = {}
        for suite in ET.parse(path).getroot().iter("testsuite"):
            if int(suite.get("tests", "0")) > 0:
                run[suite.get("name")] = run.get(suite.get("name"), 0.0) + float(suite.get("time", "0"))
        runs.append(run)
    # (a binary's parts are separate reports of one run: --parts N sums each N in turn)
    merged: dict[str, float] = {}
    for i in range(0, len(runs), args.parts):
        one: dict[str, float] = {}
        for run in runs[i:i + args.parts]:
            for name, t in run.items():
                one[name] = one.get(name, 0.0) + t
        for name, t in one.items():
            merged[name] = max(merged.get(name, 0.0), t)
    data[args.lane] = {name: round(t, 1) for name, t in sorted(merged.items())}
    tmp = SUITE_SECONDS.with_name(SUITE_SECONDS.name + ".tmp")
    tmp.write_text(json.dumps(data, indent=0, sort_keys=True) + "\n")
    os.replace(tmp, SUITE_SECONDS)
    print(f"circleci: {len(merged)} suites of {args.lane}, {sum(merged.values()) / 60:.0f} min")
    return 0


# ------------------------------------------------------------------------------------- results

def junit_cases(suite: str, cases: list[dict]) -> str:
    failures = sum(1 for c in cases if c["status"] == "fail")
    skipped = sum(1 for c in cases if c["status"] == "skip")
    body = []
    for c in cases:
        classname, _, name = c["name"].rpartition("::")
        attrs = f'classname={quoteattr(classname or suite)} name={quoteattr(name)} time="{c.get("seconds", 0)}"'
        detail = escape(str(c.get("detail") or ""))
        if c["status"] == "fail":
            body.append(f"  <testcase {attrs}><failure message=\"failed\">{detail}</failure></testcase>")
        elif c["status"] == "skip":
            body.append(f"  <testcase {attrs}><skipped message={quoteattr(str(c.get('detail') or ''))}/></testcase>")
        else:
            body.append(f"  <testcase {attrs}/>")
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name={quoteattr(suite)} tests="{len(cases)}" '
            f'failures="{failures}" skipped="{skipped}">\n' + "\n".join(body) + "\n</testsuite>\n")


def cmd_junit(_args) -> int:
    out = STATE / "junit"
    out.mkdir(parents=True, exist_ok=True)
    results = Path("build/test-results")
    copied = 0
    for xml in sorted(results.glob("*.xml")) if results.is_dir() else []:
        shutil.copy(xml, out / xml.name)
        copied += 1
    mcp = results / "mcp.json"
    if mcp.is_file():
        data = json.loads(mcp.read_text())
        cases = list(data.get("results", []))
        cases += [{"name": f"{c}::(crashed)", "status": "fail", "detail": "the chunk crashed", "seconds": 0}
                  for c in data.get("crashed", [])]
        (out / "mcp.xml").write_text(junit_cases("mcp", cases))
        copied += 1
    steps = STATE / "steps.jsonl"
    if steps.is_file():
        cases = [{"name": f"{os.environ.get('CIRCLE_JOB', 'advisory')}::{s['case']}", "status": s["status"],
                  "seconds": s["seconds"], "detail": s["detail"]} for s in map(json.loads, steps.read_text().splitlines())]
        (out / "steps.xml").write_text(junit_cases("steps", cases))
        copied += 1
    print(f"circleci: {copied} JUnit file(s) in {out}")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("warm-key")
    sub.add_parser("plan")
    sub.add_parser("continue")
    p = sub.add_parser("prepare")
    p.add_argument("--image", action="append", default=[])
    b = sub.add_parser("budget")
    b.add_argument("--until", type=float, required=True)
    b.add_argument("--case", default="step")
    b.add_argument("command", nargs=argparse.REMAINDER)
    c = sub.add_parser("ccache")
    c.add_argument("action", choices=("mark", "pack", "unpack", "save", "unhide"))
    c.add_argument("--lane", required=True, choices=sorted(CAPS))
    c.add_argument("--part", type=int, default=0, help="pack: a part job's objects")
    c.add_argument("--parts", type=int, default=0, help="unpack: every part job's objects")
    c.add_argument("--only-cut", action="store_true", help="save: only when a build was stopped (a part job)")
    sub.add_parser("junit")
    g = sub.add_parser("gtest-args")
    g.add_argument("--lane", required=True, choices=sorted(FULL_SHARDS))
    g.add_argument("--binary", required=True)
    g.add_argument("--shard", required=True)
    g.add_argument("test_args", nargs="*")
    t = sub.add_parser("suite-seconds")
    t.add_argument("--lane", required=True, choices=sorted(FULL_SHARDS))
    t.add_argument("--parts", type=int, default=1, help="reports of one run (the editor's two parts)")
    t.add_argument("xml", nargs="+")
    args = ap.parse_args(argv)
    if args.cmd == "budget":
        args.command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if args.cmd == "gtest-args":
        args.test_args = args.test_args[1:] if args.test_args[:1] == ["--"] else args.test_args
    os.chdir(ROOT)
    handlers = {"warm-key": cmd_warm_key, "plan": cmd_plan, "continue": cmd_continue, "prepare": cmd_prepare,
                "budget": cmd_budget, "ccache": cmd_ccache, "junit": cmd_junit, "gtest-args": cmd_gtest_args,
                "suite-seconds": cmd_suite_seconds}
    try:
        return handlers[args.cmd](args)
    except (RuntimeError, subprocess.CalledProcessError, OSError, KeyError) as e:
        print(f"circleci: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
