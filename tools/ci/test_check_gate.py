#!/usr/bin/env python3
"""Unit tests of tools/ci/check_gate.py (python3 tools/ci/test_check_gate.py).

The needs JSON is what `toJSON(needs)` gives the Check job: {"<job>": {"result": ..., "outputs": {}}}.
"""

from __future__ import annotations

import importlib.util
import io
import json
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
        for n in names:
            out.append({"lane": n, "status": "success", "sha": "", "commands": [], "failures": [], "artifacts": [],
                        "tests": {"ran": ran.get(n, 50 if name in ("unit", "asan", "htmlui", "mcp") else 0),
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
        # a group nothing reads runs nothing (D3)
        p = plan(event="merge_group", off=tuple(n for n in LANES if n != "changes"))
        status, out = self.gate(needs_for(p), p, reports_for(p))
        self.assertEqual(status, 0, out)

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

    def test_cli_exit_codes(self):
        p = plan()
        (self.dir / "needs.json").write_text(json.dumps(needs_for(p, linux="failure")))
        (self.dir / "lanes.json").write_text(json.dumps(p))
        cmd = [sys.executable, str(HERE / "check_gate.py"), "--needs", str(self.dir / "needs.json"),
               "--lanes", str(self.dir / "lanes.json"), "--reports", str(self.dir / "none")]
        self.assertEqual(subprocess.run(cmd, capture_output=True).returncode, 1)
        (self.dir / "needs.json").write_text("not json")
        self.assertEqual(subprocess.run(cmd, capture_output=True).returncode, 2)


if __name__ == "__main__":
    unittest.main()
