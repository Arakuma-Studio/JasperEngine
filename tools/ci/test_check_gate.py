#!/usr/bin/env python3
"""Unit tests of tools/ci/check_gate.py (python3 tools/ci/test_check_gate.py).

The needs JSON is what `toJSON(needs)` gives the Check job: {"<job>": {"result": ..., "outputs": {}}}.
"""

from __future__ import annotations

import importlib.util
import io
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_gate", HERE / "check_gate.py")
check_gate = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = check_gate
spec.loader.exec_module(check_gate)

LANES = ("changes", "lint", "linux", "unit", "asan", "htmlui", "mcp", "smoke", "web", "android", "windows", "macos", "ios")


def plan(event="pull_request", full=False, off=(), shards=("1", "2")):
    return {"event": event, "full": full, "base": "a" * 40, "head": "b" * 40,
            "lanes": {n: {"run": n not in off, "why": "not needed" if n in off else "changed"} for n in LANES},
            "unit_filter": {"core": "", "editor": ""}, "htmlui_filter": "",
            "mcp": {"mode": "none" if "mcp" in off else "planned", "files": [], "not_in_ci": [],
                    "shards": [] if "mcp" in off else list(shards)}, "fallbacks": []}


def needs_for(lanes_json, **results):
    out = {}
    for name, lane in lanes_json["lanes"].items():
        out[name] = {"result": "success" if lane["run"] else "skipped", "outputs": {}}
    for name, result in results.items():
        out[name] = {"result": result, "outputs": {}}
    return out


def reports_for(lanes_json, ran=None):
    """A report per lane that ran; test lanes get a test count (ran overrides, per lane)."""
    ran = ran or {}
    out = []
    for name, lane in lanes_json["lanes"].items():
        if not lane["run"]:
            continue
        names = [f"mcp-{s}" for s in lanes_json["mcp"]["shards"]] if name == "mcp" else [name]
        if name == "windows" and lanes_json["event"] != "push":
            names.append("windows-editor-tests")
        for n in names:
            out.append({"lane": n, "status": "success", "sha": "", "commands": [], "failures": [], "artifacts": [],
                        "tests": {"ran": ran.get(n, 50 if name in ("unit", "asan", "htmlui", "mcp") or n == "windows-editor-tests" else 0),
                                  "failed": 0, "skipped": 0, "disabled": 0}, "timings": {"seconds": 1}, "disk": ""})
    return out


class GateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="check-gate-test-")
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def gate(self, needs, lanes_json, reports, advisory=""):
        needs_path = self.dir / "needs.json"
        needs_path.write_text(json.dumps(needs))
        lanes_path = self.dir / "lanes.json"
        if lanes_json is not None:
            lanes_path.write_text(json.dumps(lanes_json))
        elif lanes_path.exists():
            lanes_path.unlink()
        reports_dir = self.dir / "reports"
        reports_dir.mkdir(exist_ok=True)
        for old in reports_dir.rglob("*.json"):
            old.unlink()
        for r in reports:
            sub = reports_dir / f"ci-report-{r['lane']}"
            sub.mkdir(exist_ok=True)
            (sub / f"{r['lane']}.json").write_text(json.dumps(r))
        out = io.StringIO()
        status = check_gate.gate(needs_path, lanes_path, reports_dir, advisory.split(), out=out)
        return status, out.getvalue()

    def test_all_green_passes_and_prints_a_table(self):
        p = plan()
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^lane\s+result\s+verdict$")
        self.assertRegex(out, r"(?m)^unit\s+success\s+ok \(50 tests\)$")
        self.assertRegex(out, r"(?m)^mcp-2\s+success\s+ok \(50 tests\)$")
        self.assertIn("check: PASS", out)

    def test_skip_is_ok_only_when_the_plan_said_so(self):
        p = plan(off=("htmlui", "windows"))
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^htmlui\s+skipped\s+ok \(not planned: not needed\)$")
        needs = needs_for(p, linux="skipped")
        status, out = self.gate(needs, p, reports_for(p))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^linux\s+skipped\s+FAIL: skipped, but the plan needed it$")

    def test_skip_without_a_plan_fails(self):
        p = plan(off=("htmlui",))
        status, out = self.gate(needs_for(p), None, reports_for(p))
        self.assertEqual(status, 1, out)
        self.assertIn("no usable lanes.json", out)
        self.assertRegex(out, r"(?m)^htmlui\s+skipped\s+FAIL")

    def test_unknown_job_skipped_fails(self):
        p = plan()
        needs = needs_for(p)
        needs["players"] = {"result": "skipped", "outputs": {}}
        status, out = self.gate(needs, p, reports_for(p))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^players\s+skipped\s+FAIL: not in lanes\.json")

    def test_windows_compile_is_part_of_the_windows_lane(self):
        # its own job (the editor leg's install), not a lane of lanes.json: planned with windows
        p = plan(off=("windows",))
        needs = needs_for(p)
        needs["windows-compile"] = {"result": "skipped", "outputs": {}}
        status, out = self.gate(needs, p, reports_for(p))
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^windows-compile\s+skipped\s+ok \(not planned: not needed\)$")
        p = plan()
        needs = needs_for(p)
        needs["windows-compile"] = {"result": "skipped", "outputs": {}}
        status, out = self.gate(needs, p, reports_for(p))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^windows-compile\s+skipped\s+FAIL: skipped, but the plan needed it$")
        needs["windows-compile"] = {"result": "success", "outputs": {}}
        status, out = self.gate(needs, p, reports_for(p))
        self.assertEqual(status, 0, out)
        needs["windows-compile"] = {"result": "failure", "outputs": {}}
        status, out = self.gate(needs, p, reports_for(p))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^windows-compile\s+failure\s+FAIL$")

    def test_windows_editor_tests_report_on_their_own_and_may_be_advisory(self):
        # the editor leg's test run (report windows-editor-tests): its failure, a missing report or zero
        # tests fail Check unless ADVISORY_LANES names it; the leg's build still gates through its job
        p = plan()
        failing = {"lane": "windows-editor-tests", "status": "failure", "sha": "", "commands": [], "failures": [],
                   "artifacts": [], "tests": {"ran": 900, "failed": 3, "skipped": 0, "disabled": 0},
                   "timings": {"seconds": 1}, "disk": ""}
        status, out = self.gate(needs_for(p), p, reports_for(p) + [failing], advisory="windows-editor-tests")
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^windows-editor-tests\s+failure\s+advisory \(rollout\): failing$")
        status, out = self.gate(needs_for(p), p, reports_for(p) + [failing])
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^windows-editor-tests\s+failure\s+FAIL: failing$")
        passing = dict(failing, status="success", tests={"ran": 900, "failed": 0, "skipped": 0, "disabled": 0})
        status, out = self.gate(needs_for(p), p, [r for r in reports_for(p) if r["lane"] != "windows-editor-tests"] + [passing])
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^windows-editor-tests\s+success\s+ok \(900 tests\)$")
        # no report although the windows lane ran a pull request
        others = [r for r in reports_for(p) if r["lane"] != "windows-editor-tests"]
        status, out = self.gate(needs_for(p), p, others)
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^windows-editor-tests\s+missing\s+FAIL: no report, so no proof tests ran$")
        status, out = self.gate(needs_for(p), p, others, advisory="windows-editor-tests")
        self.assertEqual(status, 0, out)
        # nothing expected when the lane did not run, or on push (build only)
        p = plan(off=("windows",))
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertEqual(status, 0, out)
        self.assertNotIn("windows-editor-tests", out)
        p = plan(event="push")
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertNotIn("windows-editor-tests", out)

    def test_failure_and_cancelled_fail(self):
        p = plan()
        status, out = self.gate(needs_for(p, windows="failure"), p, reports_for(p))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^windows\s+failure\s+FAIL$")
        status, out = self.gate(needs_for(p, lint="cancelled"), p, reports_for(p))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^lint\s+cancelled\s+FAIL: cancelled$")

    def test_advisory_failure_is_listed_not_failing(self):
        p = plan()
        status, out = self.gate(needs_for(p, unit="failure", mcp="failure"), p, reports_for(p), advisory="unit asan mcp")
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^unit\s+failure\s+advisory \(rollout\): failing$")
        self.assertIn("advisory (rollout): failing: unit, mcp", out)

    def test_advisory_cancelled_still_fails(self):
        p = plan()
        status, out = self.gate(needs_for(p, mcp="cancelled"), p, reports_for(p), advisory="mcp")
        self.assertEqual(status, 1, out)

    def test_zero_tests_fail(self):
        p = plan()
        status, out = self.gate(needs_for(p), p, reports_for(p, ran={"unit": 0}))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^unit\s+success\s+FAIL: 0 tests ran$")

    def test_push_build_only_lanes_need_no_tests(self):
        # push to main builds the test trees (RUN=0) to warm caches; the queue already ran the tests
        p = plan(event="push", off=("mcp", "smoke"))
        status, out = self.gate(needs_for(p), p, reports_for(p, ran={"unit": 0, "asan": 0, "htmlui": 0}))
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^unit\s+success\s+ok \(build only on push\)$")

    def test_the_advisory_run_lists_its_failures(self):
        # the run a push to main dispatches: the advisory lanes' tests the merge queue left out, and the
        # linux and windows jobs they need; what the queue ran is planned off (tools/ci/plan_lanes.py)
        p = plan(event="advisory", off=("lint", "web", "android", "macos", "ios"))
        p["build_only"] = []
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^unit\s+success\s+ok \(50 tests\)$")
        self.assertRegex(out, r"(?m)^web\s+skipped\s+ok \(not planned: not needed\)$")
        self.assertRegex(out, r"(?m)^windows-editor-tests\s+success\s+ok \(50 tests\)$")
        advisory = "unit asan htmlui mcp smoke windows-editor-tests"
        reps = [r for r in reports_for(p, ran={"unit": 0}) if r["lane"] != "windows-editor-tests"]
        status, out = self.gate(needs_for(p, htmlui="failure"), p, reps, advisory=advisory)
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^unit\s+success\s+advisory \(rollout\): 0 tests ran$")
        self.assertRegex(out, r"(?m)^htmlui\s+failure\s+advisory \(rollout\): failing$")
        self.assertRegex(out, r"(?m)^windows-editor-tests\s+missing\s+advisory \(rollout\): no report, so no proof tests ran$")
        # a report that failed while its job succeeded (a step that continues on error) is failing
        reps = [dict(r, status="failure") if r["lane"] == "htmlui" else r for r in reports_for(p)]
        status, out = self.gate(needs_for(p), p, reps)
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^htmlui\s+success\s+FAIL: failing$")
        # the lanes it needs are not advisory: a linux failure fails Check
        status, out = self.gate(needs_for(p, linux="failure"), p, reports_for(p), advisory=advisory)
        self.assertEqual(status, 1, out)

    def test_a_superseded_push_runs_nothing_and_passes(self):
        p = plan(event="push", off=LANES[1:])
        p["build_only"] = []
        status, out = self.gate(needs_for(p), p, [])
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^linux\s+skipped\s+ok \(not planned: not needed\)$")

    def test_zero_test_unit_suite_fails(self):
        p = plan()
        p["unit_run"] = {"core": True, "editor": True}
        reps = [r for r in reports_for(p) if r["lane"] != "unit"]
        for suite, n in (("core", 40), ("editor", 0)):
            reps.append({"lane": f"unit-{suite}", "status": "success", "sha": "", "commands": [], "failures": [],
                         "artifacts": [], "tests": {"ran": n, "failed": 0, "skipped": 0, "disabled": 0}})
        status, out = self.gate(needs_for(p), p, reps)
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^unit-core\s+success\s+ok \(40 tests\)$")
        self.assertRegex(out, r"(?m)^unit-editor\s+success\s+FAIL: 0 tests ran$")

    def test_every_editor_part_must_run_tests(self):
        # the editor tests run in two jobs (unit-editor-1, -2): each must report tests, and pass
        p = plan()
        p["unit_jobs"] = ["core", "editor-1", "editor-2"]

        def reps(**ran):
            out = [r for r in reports_for(p) if r["lane"] != "unit"]
            for job, n in ran.items():
                out.append({"lane": f"unit-{job.replace('_', '-')}", "status": "success", "sha": "", "commands": [],
                            "failures": [], "artifacts": [], "tests": {"ran": n, "failed": 0, "skipped": 0, "disabled": 0}})
            return out

        status, out = self.gate(needs_for(p), p, reps(core=40, editor_1=30, editor_2=31))
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^unit-editor-2\s+success\s+ok \(31 tests\)$")
        status, out = self.gate(needs_for(p), p, reps(core=40, editor_1=30, editor_2=0))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^unit-editor-2\s+success\s+FAIL: 0 tests ran$")
        status, out = self.gate(needs_for(p), p, reps(core=40, editor_1=30))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^unit-editor-2\s+success\s+FAIL: no report, so no proof tests ran$")
        # a part that failed fails the unit job, which the matrix's result already says
        status, out = self.gate(needs_for(p, unit="failure"), p, reps(core=40, editor_1=30, editor_2=31))
        self.assertEqual(status, 1, out)

    def test_every_asan_job_must_run_tests(self):
        # ASan runs one job per test binary (asan-core, asan-editor): each planned one must report tests
        p = plan()
        p["asan_jobs"] = ["core", "editor"]

        def reps(**ran):
            out = [r for r in reports_for(p) if r["lane"] != "asan"]
            for job, n in ran.items():
                out.append({"lane": f"asan-{job}", "status": "success", "sha": "", "commands": [],
                            "failures": [], "artifacts": [], "tests": {"ran": n, "failed": 0, "skipped": 0, "disabled": 0}})
            return out

        status, out = self.gate(needs_for(p), p, reps(core=40, editor=30))
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^asan-editor\s+success\s+ok \(30 tests\)$")
        self.assertNotRegex(out, r"(?m)^asan\s")
        status, out = self.gate(needs_for(p), p, reps(core=40, editor=0))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^asan-editor\s+success\s+FAIL: 0 tests ran$")
        status, out = self.gate(needs_for(p), p, reps(editor=30))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^asan-core\s+success\s+FAIL: no report, so no proof tests ran$")
        # advisory during rollout: listed, not failing
        status, out = self.gate(needs_for(p), p, reps(core=40, editor=0), advisory="asan")
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^asan-editor\s+success\s+advisory \(rollout\): 0 tests ran$")
        # only the planned binary: core alone
        p["asan_jobs"] = ["core"]
        status, out = self.gate(needs_for(p), p, reps(core=40))
        self.assertEqual(status, 0, out)

    def test_zero_test_mcp_shard_fails(self):
        p = plan()
        status, out = self.gate(needs_for(p), p, reports_for(p, ran={"mcp-2": 0}))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^mcp-2\s+success\s+FAIL: 0 tests ran$")

    def test_missing_report_of_a_test_lane_fails(self):
        p = plan()
        reports = [r for r in reports_for(p) if r["lane"] != "mcp-1"]
        status, out = self.gate(needs_for(p), p, reports)
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^mcp-1\s+success\s+FAIL: no report, so no proof tests ran$")

    def test_advisory_zero_tests_does_not_fail(self):
        p = plan()
        status, out = self.gate(needs_for(p), p, reports_for(p, ran={"htmlui": 0}), advisory="htmlui")
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^htmlui\s+success\s+advisory \(rollout\): 0 tests ran$")

    def test_full_run_must_not_skip(self):
        p = plan(event="full", full=True, off=("htmlui",))
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^htmlui\s+skipped\s+FAIL: a full run skips no lane$")

    def test_merge_group(self):
        # the planned MCP selection may be empty in the queue
        p = plan(event="merge_group", off=("mcp",))
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertEqual(status, 0, out)
        # any other skipped lane in the queue fails, even when the plan says so
        p = plan(event="merge_group", off=("asan",))
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^asan\s+skipped\s+FAIL: the merge queue skips no lane$")
        # ios without an Apple-only input: planned off by rule (queue_off), which the gate reads
        p = dict(plan(event="merge_group", off=("ios",)), queue_off=["ios"])
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertEqual(status, 0, out)
        p = plan(event="merge_group", off=("ios",))
        self.assertEqual(self.gate(needs_for(p), p, reports_for(p))[0], 1)
        # (a lane the queue may skip only when the plan said so: not macos)
        p = dict(plan(event="merge_group", off=("macos",)), queue_off=["ios"])
        self.assertEqual(self.gate(needs_for(p), p, reports_for(p))[0], 1)
        # a group nothing reads runs nothing (D3)
        p = plan(event="merge_group", off=tuple(n for n in LANES if n != "changes"))
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertEqual(status, 0, out)

    def test_merge_group_skips_the_advisory_lanes_it_planned_off(self):
        # the queue leaves ADVISORY_LANES to the push to main (tools/ci/plan_lanes.py advisory_off): Check
        # waits for the blocking lanes only
        deferred = ("unit", "asan", "htmlui", "mcp", "smoke")
        advisory = "unit asan htmlui mcp smoke windows-editor-tests"
        p = plan(event="merge_group", off=deferred)
        p["advisory_off"] = [*deferred, "windows-editor-tests"]
        p["build_only"] = ["windows-editor-tests"]
        reps = [r for r in reports_for(p) if r["lane"] != "windows-editor-tests"]
        status, out = self.gate(needs_for(p), p, reps, advisory=advisory)
        self.assertEqual(status, 0, out)
        for name in deferred:
            self.assertRegex(out, rf"(?m)^{name}\s+skipped\s+ok \(not planned: not needed\)$")
        self.assertRegex(out, r"(?m)^windows-editor-tests\s+skipped\s+ok \(advisory: not run in the merge queue\)$")
        # a lane Check does not hold advisory: the queue skips it at its peril
        status, out = self.gate(needs_for(p), p, reps, advisory="unit htmlui mcp smoke windows-editor-tests")
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^asan\s+skipped\s+FAIL: the merge queue skips no lane$")
        # advisory, but planned off for another reason (or by a plan that names no reason)
        q = dict(p, advisory_off=["unit", "htmlui", "mcp", "smoke", "windows-editor-tests"])
        status, out = self.gate(needs_for(q), q, reps, advisory=advisory)
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^asan\s+skipped\s+FAIL: the merge queue skips no lane$")
        q = {k: v for k, v in p.items() if k not in ("advisory_off", "build_only")}
        status, out = self.gate(needs_for(q), q, reps, advisory=advisory)
        self.assertEqual(status, 1, out)
        # a blocking lane is never skipped for being "advisory"
        q = plan(event="merge_group", off=("linux",))
        q["advisory_off"] = ["linux"]
        status, out = self.gate(needs_for(q), q, reports_for(q), advisory=advisory)
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^linux\s+skipped\s+FAIL: the merge queue skips no lane$")
        # the editor leg's test run: skipped only while advisory and planned off for it
        for advisory_lanes, off in (("unit asan htmlui mcp smoke", p["advisory_off"]), (advisory, list(deferred))):
            q = dict(p, advisory_off=off)
            status, out = self.gate(needs_for(q), q, reps, advisory=advisory_lanes)
            self.assertEqual(status, 1, (advisory_lanes, out))
            self.assertRegex(out, r"(?m)^windows-editor-tests\s+skipped\s+FAIL: the merge queue skips no test run$")
        # nor in a full run
        q = plan(event="full", full=True)
        q["advisory_off"] = q["build_only"] = ["windows-editor-tests"]
        status, out = self.gate(needs_for(q), q, [r for r in reports_for(q) if r["lane"] != "windows-editor-tests"],
                                advisory=advisory)
        self.assertEqual(status, 1, out)
        self.assertRegex(out, r"(?m)^windows-editor-tests\s+skipped\s+FAIL: a full run skips no test run$")

    def test_changes_failure_fails(self):
        p = plan()
        status, out = self.gate(needs_for(p, changes="failure"), p, reports_for(p))
        self.assertEqual(status, 1, out)

    def test_report_job_does_not_gate(self):
        p = plan()
        needs = needs_for(p)
        needs["report"] = {"result": "failure", "outputs": {}}
        status, out = self.gate(needs, p, reports_for(p))
        self.assertEqual(status, 0, out)
        self.assertRegex(out, r"(?m)^report\s+failure\s+ok \(the comment, not a verdict\)$")

    def test_planned_lane_missing_from_needs_is_a_warning(self):
        p = plan()
        needs = needs_for(p)
        del needs["ios"]
        status, out = self.gate(needs, p, reports_for(p))
        self.assertEqual(status, 0, out)
        self.assertIn("warning: lanes.json plans ios, which Check does not need", out)

    def test_advisory_lanes_left_out_of_needs(self):
        # ci.yml's Check does not need the advisory lanes (it would not wait for them): no warning, and a
        # blocking lane still fails
        p = plan()
        advisory = "unit asan htmlui mcp smoke windows-editor-tests"
        needs = {n: "success" for n in LANES if n not in advisory.split()}
        rc, text = self.gate(needs, p, reports_for(p), advisory=advisory)
        self.assertEqual(rc, 0, text)
        self.assertNotIn("which Check does not need", text)
        rc, text = self.gate(dict(needs, linux="failure"), p, reports_for(p), advisory=advisory)
        self.assertEqual(rc, 1, text)

    def test_cli_exit_codes(self):
        p = plan()
        (self.dir / "needs.json").write_text(json.dumps(needs_for(p, linux="failure")))
        (self.dir / "lanes.json").write_text(json.dumps(p))
        cmd = [sys.executable, str(HERE / "check_gate.py"), "--needs", str(self.dir / "needs.json"),
               "--lanes", str(self.dir / "lanes.json"), "--reports", str(self.dir / "none")]
        self.assertEqual(subprocess.run(cmd, capture_output=True).returncode, 1)
        (self.dir / "needs.json").write_text("not json")
        self.assertEqual(subprocess.run(cmd, capture_output=True).returncode, 2)

    def test_results_only_needs(self):
        # ci.yml passes {"<job>": "<result>"}, not toJSON(needs)
        p = plan()
        needs = {n: "success" for n in LANES}
        rc, text = self.gate(needs, p, reports_for(p))
        self.assertEqual(rc, 0, text)
        rc, text = self.gate(dict(needs, web="failure"), p, reports_for(p))
        self.assertEqual(rc, 1, text)


MAX_ARG_STRLEN = 131072   # Linux: one environment string or argument (32 pages)
CI_YML = HERE.parent.parent / ".github" / "workflows" / "ci.yml"
EXPR = re.compile(r"\$\{\{(.*?)\}\}")


def workflow_jobs(text: str) -> dict[str, str]:
    body = text[text.index("\njobs:\n"):]
    parts = re.split(r"^  ([\w-]+):\n", body, flags=re.M)
    return dict(zip(parts[1::2], parts[2::2]))


def env_entries(block: str):
    """(key, value) of every entry of every env: map in a YAML block."""
    env_indent = None
    for line in block.splitlines():
        indent = len(line) - len(line.lstrip())
        if env_indent is not None and line.strip() and indent <= env_indent:
            env_indent = None
        if re.fullmatch(r"\s*env:\s*", line):
            env_indent = indent
        elif env_indent is not None and (m := re.fullmatch(r"\s*([A-Za-z_][\w]*): (.*)", line)):
            yield m.group(1), m.group(2)


class WorkflowEnvTest(unittest.TestCase):
    """Every environment value of ci.yml fits one environment string when the changes job's outputs are
    their largest (each filter FILTER_LIMIT long: a plan selecting every unit test, run 45088926257)."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(HERE))
        import plan_lanes
        cls.filter_limit = plan_lanes.FILTER_LIMIT
        cls.text = CI_YML.read_text(encoding="utf-8")
        cls.jobs = workflow_jobs(cls.text)
        outputs = re.search(r"^    outputs:\n((?:      .*\n)+)", cls.jobs["changes"], re.M).group(1)
        cls.outputs = {name: "x" * (cls.filter_limit if "filter" in name else 4096)
                       for name in re.findall(r"^      (\w+):", outputs, re.M)}

    def needs_of(self, job: str) -> list[str]:
        m = re.search(r"^    needs: (\[.*\]|[\w-]+)$", self.jobs[job], re.M)
        return [n.strip() for n in m.group(1).strip("[]").split(",")] if m else []

    def bound(self, job: str, expr: str) -> int:
        """An upper bound of the expression's value's length."""
        if re.search(r"toJSON\(\s*needs\s*\)", expr):
            # the runner's toJSON indents by 2
            return len(json.dumps({n: {"result": "cancelled", "outputs": self.outputs if n == "changes" else {}}
                                   for n in self.needs_of(job)}, indent=2))
        size = len(expr)
        for ref in re.findall(r"\b(?:needs|steps|github|inputs|vars|env|matrix|secrets)\.[\w.-]+", expr):
            m = re.fullmatch(r"needs\.changes\.outputs\.(\w+)", ref)
            size += len(self.outputs.get(m.group(1), "")) if m else 9 if ref.endswith(".result") else 1024
        return size

    def test_every_env_value_fits_one_environment_string(self):
        blocks = {"(workflow)": self.text[:self.text.index("\njobs:\n")], **self.jobs}
        checked = 0
        for job, block in blocks.items():
            for key, value in env_entries(block):
                size = len(EXPR.sub("", value)) + sum(self.bound(job, e) for e in EXPR.findall(value))
                checked += 1
                self.assertLess(size, MAX_ARG_STRLEN, f"{job}: env {key} can reach {size} bytes ({value[:80]})")
        self.assertGreater(checked, 50)

    def test_needs_maps_name_every_needed_job(self):
        # the gate and the report see only the jobs NEEDS names: a job added to needs must be added there
        for job in ("check", "report"):
            values = [v for k, v in env_entries(self.jobs[job]) if k == "NEEDS"]
            self.assertEqual(len(values), 1, job)
            m = re.fullmatch(r"\$\{\{ format\('\{\{(.*)\}\}', (.*)\) \}\}", values[0])
            self.assertTrue(m, f"{job}: NEEDS is not format('{{{{...}}}}', needs.<job>.result, ...)")
            keys = re.findall(r'"([\w-]+)":"\{(\d+)\}"', m.group(1))
            args = [a.strip() for a in m.group(2).split(",")]
            self.assertEqual([int(i) for _, i in keys], list(range(len(keys))), job)
            self.assertEqual(args, [f"needs.{k}.result" for k, _ in keys], job)
            self.assertEqual([k for k, _ in keys], self.needs_of(job), job)

    def test_check_needs_every_lane_but_the_advisory_ones(self):
        # Check waits only for what can fail it; a lane taken off ADVISORY_LANES must join its needs
        advisory = re.search(r"^  ADVISORY_LANES: \"([a-z0-9 _-]*)\"$", self.text, re.M).group(1).split()
        lanes = [n for n in self.needs_of("report") if n != "check"]
        import plan_lanes
        self.assertLessEqual(set(plan_lanes.LANES), set(lanes), "report must need every lane")
        self.assertEqual(self.needs_of("check"), [n for n in lanes if n not in advisory],
                         "check's needs: report's needs (but check) without ADVISORY_LANES, in that order")

    def test_filter_limit_fits_one_argument(self):
        # the unit, asan and HTML UI steps pass `printf %q` of --gtest_filter=<filter> in one argument:
        # %q at most doubles it
        self.assertLess(2 * (self.filter_limit + len("UNIT_TEST_ARGS=--gtest_filter=")) + 4096, MAX_ARG_STRLEN)


if __name__ == "__main__":
    unittest.main()
