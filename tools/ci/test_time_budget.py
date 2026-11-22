#!/usr/bin/env python3
"""Unit tests of tools/ci/time_budget.py and its use by tools/ci/report.py
(python3 -I tools/ci/test_time_budget.py). Fixtures are written by the tests themselves."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load(name: str, file: str):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


ttb = load("ci_time_budget", "time_budget.py")
report = load("ci_report_for_budget", "report.py")

BUDGET = {
    "tolerance": 1.5,
    "suites": {
        "core": {"test_cap": 10, "total": 100},
        "editor": {"test_cap": 5, "total": 50},
        "mcp": {"test_cap": 60, "file_cap": 120},
    },
    "tests": {
        "core": {"Slow.Big": {"seconds": 40, "reason": "renders every variant"}},
        "mcp": {"test_a.py::test_long": {"seconds": 100, "reason": "a whole library"}},
    },
    "files": {"mcp": {"test_a.py": {"seconds": 200, "reason": "one editor for the library"}}},
}


def gtest_xml(path: Path, cases: dict[str, float], disabled: tuple = ()) -> Path:
    rows = []
    for name, seconds in cases.items():
        suite, test = name.split(".")
        rows.append(f'<testsuite name="{suite}"><testcase name="{test}" classname="{suite}" status="run" result="completed" '
                    f'time="{seconds}"/></testsuite>')
    for name in disabled:
        suite, test = name.split(".")
        rows.append(f'<testsuite name="{suite}"><testcase name="{test}" classname="{suite}" status="notrun" result="suppressed" '
                    f'time="0"/></testsuite>')
    path.write_text('<?xml version="1.0"?><testsuites>' + "".join(rows) + "</testsuites>")
    return path


def mcp_json(path: Path, results: dict[str, tuple[str, float]]) -> Path:
    path.write_text(json.dumps({"results": [{"name": n, "status": s, "seconds": t} for n, (s, t) in results.items()]}))
    return path


class Measure(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def test_suite_follows_the_lane(self):
        self.assertEqual(ttb.suite_of("unit-core"), "core")
        self.assertEqual(ttb.suite_of("unit-editor-2"), "editor")
        self.assertEqual(ttb.suite_of("htmlui"), "htmlui")
        self.assertEqual(ttb.suite_of("mcp-7"), "mcp")
        self.assertEqual(ttb.suite_of("mcp"), "mcp")
        # sanitizer and platform lanes run the same tests slower: no budget of theirs
        self.assertIsNone(ttb.suite_of("asan-core"))
        self.assertIsNone(ttb.suite_of("windows-editor-tests"))

    def test_gtest_times_leave_out_disabled_tests(self):
        xml = gtest_xml(self.tmp / "core.xml", {"Math.Add": 0.5, "Slow.Big": 30.0}, disabled=("Math.DISABLED_Old",))
        self.assertEqual(ttb.measure([xml]), {"Math.Add": 0.5, "Slow.Big": 30.0})

    def test_mcp_times_leave_out_skips(self):
        js = mcp_json(self.tmp / "mcp.json", {"test_a.py::test_x": ("pass", 2.0), "test_a.py::test_y": ("skip", 0.0),
                                               "test_b.py::test_z": ("fail", 9.0)})
        # the per-test seconds file of the same run is no second source
        (self.tmp / "mcp-test-seconds.json").write_text(json.dumps({"test_a.py::test_x": 2.0}))
        self.assertEqual(ttb.measure([js, self.tmp / "mcp-test-seconds.json"]), {"test_a.py::test_x": 2.0, "test_b.py::test_z": 9.0})


class Check(unittest.TestCase):
    def test_under_every_cap_is_quiet(self):
        self.assertEqual(ttb.check(BUDGET, "core", {"Math.Add": 9.0, "Slow.Big": 59.0}), [])

    def test_a_new_test_over_the_cap_is_named_with_the_way_out(self):
        over = ttb.check(BUDGET, "core", {"Math.Add": 11.0})
        self.assertEqual([(f["kind"], f["name"], f["seconds"], f["budget"]) for f in over], [("test", "Math.Add", 11.0, 10)])

    def test_a_recorded_test_may_grow_by_the_tolerance_only(self):
        self.assertEqual(ttb.check(BUDGET, "core", {"Slow.Big": 60.0}), [])
        over = ttb.check(BUDGET, "core", {"Slow.Big": 61.0})
        self.assertEqual([(f["name"], f["budget"]) for f in over], [("Slow.Big", 40)])

    def test_mcp_files_have_a_cap_of_their_own(self):
        # three quick tests of one file over its cap together
        over = ttb.check(BUDGET, "mcp", {"test_b.py::test_1": 50.0, "test_b.py::test_2": 50.0, "test_b.py::test_3": 50.0})
        self.assertEqual([(f["kind"], f["name"], f["seconds"]) for f in over], [("file", "test_b.py", 150.0)])
        self.assertEqual(ttb.check(BUDGET, "mcp", {"test_a.py::test_long": 140.0, "test_a.py::test_x": 50.0}), [])

    def test_totals_count_only_complete_runs(self):
        lanes = [{"suite": "core", "seconds": 160, "complete": True}, {"suite": "editor", "seconds": 40, "complete": False}]
        over = ttb.check_totals(BUDGET, lanes)
        self.assertEqual([(f["kind"], f["name"], f["seconds"], f["budget"]) for f in over], [("total", "core", 160, 100)])

    def test_the_editor_parts_add_up_to_its_total(self):
        lanes = [{"suite": "editor", "seconds": 40, "complete": True}, {"suite": "editor", "seconds": 40, "complete": True}]
        self.assertEqual([(f["name"], f["seconds"]) for f in ttb.check_totals(BUDGET, lanes)], [("editor", 80)])

    def test_the_message_says_how_to_raise_a_budget_with_a_reason(self):
        text = "\n".join(ttb.render(ttb.check(BUDGET, "core", {"Math.Add": 11.0}), "core"))
        self.assertIn("Math.Add", text)
        self.assertIn("tools/ci/time_budget.py allow core Math.Add 11", text)
        self.assertIn("--reason", text)


class Edit(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.path = self.tmp / "budget.json"
        self.path.write_text(json.dumps(BUDGET))

    def run_cli(self, *args) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-I", str(HERE / "time_budget.py"), *args, "--budget", str(self.path)],
                              capture_output=True, text=True)

    def test_allow_needs_a_reason(self):
        r = self.run_cli("allow", "core", "Math.Add", "20")
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(json.loads(self.path.read_text()), BUDGET)

    def test_allow_records_the_seconds_and_the_reason(self):
        r = self.run_cli("allow", "core", "Math.Add", "20", "--reason", "sums every matrix")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads(self.path.read_text())["tests"]["core"]["Math.Add"], {"seconds": 20, "reason": "sums every matrix"})
        r = self.run_cli("allow", "mcp", "test_c.py", "500", "--reason", "one editor", "--file")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads(self.path.read_text())["files"]["mcp"]["test_c.py"]["seconds"], 500)

    def test_ratchet_lowers_what_got_cheaper_and_drops_what_fits_the_cap(self):
        xml = gtest_xml(self.tmp / "core.xml", {"Slow.Big": 8.0})
        r = self.run_cli("ratchet", "--lane", "unit-core", str(xml))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("Slow.Big", json.loads(self.path.read_text())["tests"]["core"])
        self.path.write_text(json.dumps(BUDGET))
        gtest_xml(xml, {"Slow.Big": 16.0})
        self.run_cli("ratchet", "--lane", "unit-core", str(xml))
        self.assertEqual(json.loads(self.path.read_text())["tests"]["core"]["Slow.Big"]["seconds"], 20)
        # what did not get cheaper by half keeps its budget (a noisy runner is no saving)
        self.path.write_text(json.dumps(BUDGET))
        gtest_xml(xml, {"Slow.Big": 30.0})
        self.run_cli("ratchet", "--lane", "unit-core", str(xml))
        self.assertEqual(json.loads(self.path.read_text())["tests"]["core"]["Slow.Big"]["seconds"], 40)

    def test_check_exits_1_on_a_finding(self):
        xml = gtest_xml(self.tmp / "core.xml", {"Math.Add": 11.0})
        self.assertEqual(self.run_cli("check", "--lane", "unit-core", str(xml)).returncode, 1)
        gtest_xml(xml, {"Math.Add": 1.0})
        self.assertEqual(self.run_cli("check", "--lane", "unit-core", str(xml)).returncode, 0)


class CommittedBudget(unittest.TestCase):
    def test_every_entry_has_seconds_and_a_reason(self):
        budget = ttb.load_budget(ttb.BUDGET_FILE)
        self.assertEqual(ttb.problems(budget), [])
        self.assertTrue(budget["suites"]["mcp"]["test_cap"] > 0)

    def test_a_reasonless_entry_is_a_problem(self):
        bad = json.loads(json.dumps(BUDGET))
        bad["tests"]["core"]["Slow.Big"]["reason"] = ""
        self.assertEqual(len(ttb.problems(bad)), 1)


class ReportIntegration(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.results = self.tmp / "results"
        self.results.mkdir()

    def collect(self, lane: str, command: str) -> dict:
        env = {"JASPER_CI_STATUS": "success", "JASPER_CI_REPRO": command}
        return report.collect(lane, self.results, self.results / "ci-report" / f"{lane}.json", env=env)

    def test_collect_keeps_the_lane_total_and_what_went_over(self):
        gtest_xml(self.results / "core.xml", {"Math.Add": 1.0, "Math.Huge": 400.0})
        data = self.collect("unit-core", "UNIT_SUITE=core WERROR=1 tools/build.sh unit-tests")
        self.assertEqual(data["time"]["suite"], "core")
        self.assertEqual(data["time"]["seconds"], 401)
        self.assertTrue(data["time"]["complete"])
        self.assertEqual([f["name"] for f in data["time"]["over"]], ["Math.Huge"])

    def test_a_filtered_run_is_no_total(self):
        gtest_xml(self.results / "core.xml", {"Math.Add": 1.0})
        data = self.collect("unit-core", "UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=Math.*' tools/build.sh unit-tests")
        self.assertFalse(data["time"]["complete"])

    def test_render_shows_the_findings_as_advisory_and_check_ignores_them(self):
        gtest_xml(self.results / "core.xml", {"Math.Huge": 400.0})
        data = self.collect("unit-core", "UNIT_SUITE=core WERROR=1 tools/build.sh unit-tests")
        reports = {"unit-core": report.sanitise_report(data)}
        text = report.render(reports, report.load_lanes(None), "https://example.invalid/run", "0123456789abcdef", [])
        self.assertIn("Test time", text)
        self.assertIn("Math.Huge", text)
        self.assertIn("advisory", text)

    def test_a_malformed_time_block_is_dropped(self):
        data = {"lane": "unit-core", "status": "success", "time": {"suite": "core", "seconds": "x", "over": [{"name": 5}]}}
        clean = report.sanitise_report(data)
        self.assertEqual(clean["time"]["seconds"], 0)
        self.assertEqual(clean["time"]["over"], [])


if __name__ == "__main__":
    unittest.main()
