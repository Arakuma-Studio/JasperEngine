#!/usr/bin/env python3
"""Tests of tools/ci/circleci.py (python3 -I tools/ci/test_circleci.py; stdlib only, no build, no CircleCI).

The jobs it writes from a plan (a pull request's selection, a change to the CircleCI lanes, nothing to
run, the daily warm on main), the shard counts, the step budget that stops a command before
CircleCI's 60 minutes, and the JUnit files of an MCP shard and of the steps.
"""

from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
from argparse import Namespace
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import circleci as ci  # noqa: E402

TOTALS = {"core": 800, "editor": 800, "htmlui": 200}
META = {"mode": "pr", "why": "", "main": "a" * 40, "head": "b" * 40, "merge": "c" * 40, "warm_key": "20261123-tag"}


def lanes(run=(), unit=None, unit_filter=None, asan=None, asan_filter=None, htmlui_filter="", shards=()) -> dict:
    return {"lanes": {name: {"run": name in run, "why": ""} for name in ("unit", "asan", "htmlui", "mcp", "smoke")},
            "unit_run": unit or {"core": False, "editor": False}, "unit_filter": unit_filter or {"core": "", "editor": ""},
            "asan_run": asan or {"core": False, "editor": False}, "asan_filter": asan_filter or {"core": "", "editor": ""},
            "htmlui_filter": htmlui_filter, "mcp": {"shards": list(shards)}}


def names(config: dict) -> dict[str, list[str]]:
    """advisory/<job> -> the jobs it waits for."""
    out = {}
    for entry in config["workflows"]["advisory"]["jobs"]:
        (_, e), = entry.items()
        out[e["name"]] = e.get("requires", [])
    return out


def commands(job: dict) -> list[str]:
    return [s["run"]["command"] for s in job["steps"] if "run" in s]


class Jobs(unittest.TestCase):
    def test_whole_lanes(self):
        config = ci.continuation(META, lanes(run=("unit", "asan", "htmlui", "mcp", "smoke"), unit={"core": True, "editor": True},
                                             asan={"core": True, "editor": True}, shards=("1", "2", "3")), TOTALS)
        jobs = names(config)
        for k in range(1, 5):
            self.assertEqual(jobs[f"advisory/unit-core-{k}"], ["advisory/unit-core-build"])
            self.assertEqual(jobs[f"advisory/asan-core-{k}"], ["advisory/asan-core-build"])
        for k in range(1, 7):
            self.assertEqual(jobs[f"advisory/htmlui-{k}"], ["advisory/htmlui-build"])
        self.assertIn("advisory/unit-editor", jobs)
        self.assertEqual(jobs["advisory/asan-editor-2"], ["advisory/asan-editor-build"])
        self.assertEqual([j for j in jobs if j.startswith("advisory/mcp-")], ["advisory/mcp-1", "advisory/mcp-2", "advisory/mcp-3"])
        for j in ("advisory/mcp-2", "advisory/smoke-players", "advisory/smoke-post-processing", "advisory/smoke-ecs-collision"):
            self.assertEqual(jobs[j], ["advisory/editor"])
        # every machine job is the free plan's large VM; the shards' commands are the lanes' own
        for key, job in config["jobs"].items():
            self.assertEqual(job["resource_class"], "large", key)
        shard = " ".join(commands(config["jobs"]["asan-core-3"]))
        self.assertIn("gtest-args --lane asan-core --binary build/linux/core-tests-address-undefined/", shard)
        self.assertIn("--shard 3/4 -- ", shard)
        self.assertIn("--gtest_filter=*-*GpuSlow*", shard)
        self.assertIn('UNIT_TEST_ARGS="$args" SANITIZE=address,undefined UNIT_SUITE=core tools/build.sh unit-tests', shard)
        self.assertIn("tools/ci/mcp_shard.sh 2 build/ci-plan", " ".join(commands(config["jobs"]["mcp-2"])))
        self.assertIn("--shard 2/6", " ".join(commands(config["jobs"]["htmlui-2"])))

    def test_a_filtered_selection_takes_its_share_of_the_shards(self):
        core = ":".join(f"S{i}.*:*/S{i}.*:S{i}/*.*" for i in range(300))
        config = ci.continuation(META, lanes(run=("unit", "asan"), unit={"core": True, "editor": False},
                                             unit_filter={"core": core, "editor": ""}, asan={"core": True, "editor": False},
                                             asan_filter={"core": core, "editor": ""}), TOTALS)
        jobs = names(config)
        # 300 of 800 suites: ceil(4 * 300 / 800) = 2 shards
        self.assertEqual(sorted(j for j in jobs if j.startswith("advisory/unit-core")),
                         ["advisory/unit-core-1", "advisory/unit-core-2", "advisory/unit-core-build",
                          "advisory/unit-core-part-1", "advisory/unit-core-part-2"])
        # the build job joins the part jobs (UNIT_BUILD_PART), which save a cache only when stopped
        self.assertEqual(jobs["advisory/unit-core-build"], ["advisory/unit-core-part-1", "advisory/unit-core-part-2"])
        part = " ".join(commands(config["jobs"]["unit-core-part-2"]))
        self.assertIn("UNIT_BUILD_PART=2/2 RUN=0 UNIT_SUITE=core", part)
        self.assertIn("ccache save --lane unit-core --only-cut", part)
        self.assertIn("ccache unpack --lane unit-core --parts 2", " ".join(commands(config["jobs"]["unit-core-build"])))
        self.assertNotIn("advisory/unit-editor", jobs)
        self.assertNotIn("advisory/editor", jobs)
        asan = " ".join(commands(config["jobs"]["asan-core-1"]))
        self.assertIn(f"--gtest_filter={core}-*GpuSlow*", asan.replace("'\"'\"'", "'"))
        # the presets check and the scalar picking rerun ride with the first core shard only
        self.assertIn("check-presets.sh", " ".join(commands(config["jobs"]["unit-core-1"])))
        self.assertNotIn("check-presets.sh", " ".join(commands(config["jobs"]["unit-core-2"])))

    def test_one_shard_is_one_job(self):
        config = ci.continuation(META, lanes(run=("htmlui",), htmlui_filter="Golden.*:*/Golden.*:Golden/*.*"), TOTALS)
        self.assertEqual(list(names(config)), ["advisory/htmlui"])
        steps = commands(config["jobs"]["htmlui"])
        build = next(i for i, c in enumerate(steps) if "RUN=0" in c)
        save = next(i for i, c in enumerate(steps) if "ccache save" in c)
        tests = next(i for i, c in enumerate(steps) if "HTMLUI_TEST_ARGS" in c and "RUN=0" not in c)
        self.assertLess(build, save)
        self.assertLess(save, tests)

    def test_nothing_to_run(self):
        config = ci.continuation(META, lanes(), TOTALS)
        self.assertEqual(names(config), {"advisory/nothing-to-run": []})
        self.assertEqual(config["jobs"]["nothing"], {"type": "no-op"})

    def test_warm_builds_each_tree_and_stamps_the_day(self):
        config = ci.continuation(dict(META, mode="warm"), None, TOTALS)
        jobs = names(config)
        trees = [j for j in jobs if j != "advisory/warm-done" and "-part-" not in j]
        self.assertEqual(len(trees), 6)
        self.assertEqual(sorted(jobs["advisory/warm-done"]), sorted(trees))
        for key, job in config["jobs"].items():
            if key != "warm-done" and "-part-" not in key:
                self.assertTrue(any("RUN=0" in c or "tools/build.sh linux" in c for c in commands(job)), key)
                self.assertEqual(job["retention"], {"caches": ci.CACHE_RETENTION})
        self.assertIn("20261123-tag", " ".join(commands(config["jobs"]["warm-done"])))

    def test_modes(self):
        self.assertEqual(ci.mode_of("push", "main", False)[0], "warm")
        self.assertEqual(ci.mode_of("push", "main", True)[0], "none")
        self.assertEqual(ci.mode_of("pull_request", "fix/x", False)[0], "pr")
        self.assertEqual(ci.mode_of("push", "", False)[0], "none")

    def test_config_is_json(self):
        # CircleCI reads the continuation as YAML; JSON is YAML, and no `<<` (a 2.1 parameter) may appear
        text = json.dumps(ci.continuation(META, lanes(run=("unit", "mcp", "smoke"), unit={"core": True, "editor": True},
                                                      shards=("1",)), TOTALS))
        self.assertNotIn("<<", text)


class Shards(unittest.TestCase):
    def test_counts(self):
        self.assertEqual(ci.shards_for("unit-core", "", 800), 4)
        self.assertEqual(ci.shards_for("unit-core", "*-*GpuSlow*", 800), 4)
        self.assertEqual(ci.shards_for("unit-core", "A.*:*/A.*:A/*.*", 800), 1)
        self.assertEqual(ci.shards_for("unit-editor", "", 800), 1)
        many = ":".join(f"S{i}.*" for i in range(799))
        self.assertEqual(ci.shards_for("asan-core", many, 800), 4)


def matches(name: str, flt: str) -> bool:
    """googletest's filter: a positive and a negative list of fnmatch patterns."""
    import fnmatch
    pos, _, neg = flt.partition("-")
    def any_of(patterns):
        return any(fnmatch.fnmatchcase(name, p) for p in patterns.split(":") if p)
    return any_of(pos or "*") and not any_of(neg)


class WeightedShards(unittest.TestCase):
    SUITES = {"Slow": ["a", "b", "c", "d"], "Mid": ["x"], "Fast": ["p", "q"], "New": ["n"], "DISABLED_Off": ["z"]}
    SECONDS = {"Slow": 1200.0, "Mid": 250.0, "Fast": 10.0}

    def shard(self, k, n, base=None):
        with redirect_stderr(io.StringIO()):
            return ci.weighted_filter(self.SUITES, base, k, n, self.SECONDS)

    def test_the_shards_partition_the_tests(self):
        tests = [f"{s}.{t}" for s, ts in self.SUITES.items() if not s.startswith("DISABLED_") for t in ts]
        for n in (2, 3, 5):
            owners = {t: [k for k in range(1, n + 1) if matches(t, self.shard(k, n))] for t in tests}
            self.assertEqual({t: len(o) for t, o in owners.items()}, {t: 1 for t in tests}, n)

    def test_a_suite_over_the_split_is_dealt_by_test(self):
        # Slow (20 min) goes test by test: its tests span shards
        owners = [[k for k in range(1, 5) if matches(f"Slow.{t}", self.shard(k, 4))][0] for t in "abcd"]
        self.assertGreater(len(set(owners)), 1)

    def test_shards_are_runs_in_the_binary_order(self):
        # (a test may rely on what an earlier one left behind: each shard keeps its neighbours)
        order = [f"{s}.{t}" for s, ts in self.SUITES.items() if not s.startswith("DISABLED_") for t in ts]
        owners = [[k for k in range(1, 4) if matches(t, self.shard(k, 3))][0] for t in order]
        self.assertEqual(owners, sorted(owners))
        self.assertEqual(owners[0], 1)

    def test_keeps_the_base_negative(self):
        for k in (1, 2):
            self.assertTrue(self.shard(k, 2, "*-*Fast*").startswith("*-*Fast*"))
        self.assertEqual(self.shard(1, 1, "A.*"), "A.*")


class Budget(unittest.TestCase):
    def run_budget(self, until: float, command: str, started: float) -> tuple[int, str]:
        err = io.StringIO()
        with mock.patch.dict(os.environ, {"ADVISORY_JOB_START": str(started)}), mock.patch.object(ci, "MIN_STEP_SECONDS", 0.5), \
                mock.patch.object(ci, "kill_containers"), redirect_stdout(io.StringIO()), redirect_stderr(err):
            status = ci.cmd_budget(Namespace(until=until, case="case", command=[command]))
        return status, err.getvalue()

    def test_passes_and_records(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(ci, "STATE", Path(d)):
            status, _ = self.run_budget(10, "exit 0", time.time())
            self.assertEqual(status, 0)
            status, _ = self.run_budget(10, "exit 3", time.time())
            self.assertEqual(status, 3)
            rows = [json.loads(line) for line in (Path(d) / "steps.jsonl").read_text().splitlines()]
            self.assertEqual([r["status"] for r in rows], ["pass", "fail"])

    def test_cuts_at_the_deadline(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(ci, "STATE", Path(d)):
            t0 = time.time()
            # 1.5 s left in the job: the sleep is stopped, the cut is recorded for the cache save
            status, err = self.run_budget(1.0, "sleep 30", t0 - 58.5)
            self.assertEqual(status, 124)
            self.assertLess(time.time() - t0, 20)
            self.assertTrue((Path(d) / "cut").is_file())
            self.assertIn("60 min", err)

    def test_no_time_left(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(ci, "STATE", Path(d)):
            status, _ = self.run_budget(1.0, "exit 0", time.time() - 3600)
            self.assertEqual(status, 1)


class Junit(unittest.TestCase):
    def test_mcp_and_steps(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "build/test-results").mkdir(parents=True)
            (root / "build/test-results/core.xml").write_text("<testsuites/>")
            (root / "build/test-results/mcp.json").write_text(json.dumps({"results": [
                {"name": "test_a.py::test_one", "status": "pass", "detail": "", "seconds": 1.5},
                {"name": "test_a.py::test_two", "status": "fail", "detail": "assert <1> & 2", "seconds": 2},
                {"name": "test_b.py::test_three", "status": "skip", "detail": "needs X", "seconds": 0}], "crashed": ["test_c.py"]}))
            state = root / "build/ci-circleci"
            state.mkdir(parents=True)
            (state / "steps.jsonl").write_text(json.dumps({"case": "Build", "status": "pass", "seconds": 3.0, "detail": ""}) + "\n")
            cwd = os.getcwd()
            try:
                os.chdir(root)
                with mock.patch.object(ci, "STATE", Path("build/ci-circleci")), redirect_stdout(io.StringIO()):
                    ci.cmd_junit(None)
            finally:
                os.chdir(cwd)
            junit = state / "junit"
            self.assertTrue((junit / "core.xml").is_file())
            mcp = ET.parse(junit / "mcp.xml").getroot()
            self.assertEqual((mcp.get("tests"), mcp.get("failures"), mcp.get("skipped")), ("4", "2", "1"))
            cases = {c.get("name"): c for c in mcp.iter("testcase")}
            self.assertEqual(cases["test_two"].get("classname"), "test_a.py")
            self.assertEqual(cases["test_two"].find("failure").text, "assert <1> & 2")
            self.assertIsNotNone(cases["(crashed)"].find("failure"))
            steps = ET.parse(junit / "steps.xml").getroot()
            self.assertEqual(steps.get("tests"), "1")


if __name__ == "__main__":
    unittest.main()
