"""Tests of tools/ci/shadow.py (python3 -I tools/ci/test_shadow.py; stdlib only).

A run that ran more than a PR would have (the merge queue's unit tests, the nightly's MCP suite):
each failed test is judged against the PR-style selection lanes.json's shadow holds, a failure the
selection left out is a miss unless the history saw it fail already (pre-existing or flaky), a run
with no change attributes nothing; the records merge into the history once each, and the report adds
them up.
"""

from __future__ import annotations

import io
import json
import shutil
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import shadow as sh  # noqa: E402

SHADOW = {"base": "b" * 40, "files": ["engine/core/Foo.cpp"], "fallbacks": [],
          "unit": {"core": ["Foo", "Bar"], "editor": "all", "htmlui": []},
          "mcp": ["test_a.py"]}


def report(lane: str, failures: list[tuple[str, str]], ran: int = 10, status: str = "failure") -> dict:
    return {"lane": lane, "status": status, "tests": {"ran": ran, "failed": len(failures), "skipped": 0, "disabled": 0},
            "failures": [{"kind": kind, "title": title, "location": "", "excerpt": "", "repro": ""} for kind, title in failures]}


class Judge(unittest.TestCase):
    def test_suite_names(self):
        self.assertTrue(sh.unit_selected(["Foo"], "Foo.Adds"))
        self.assertTrue(sh.unit_selected(["Foo"], "Prefix/Foo.Adds/0"))      # value-parameterised
        self.assertTrue(sh.unit_selected(["Foo"], "Foo/0.Adds"))             # typed
        self.assertTrue(sh.unit_selected(["Foo"], "Foo.Adds crashed"))
        self.assertFalse(sh.unit_selected(["Foo"], "FooBar.Adds"))
        self.assertTrue(sh.unit_selected("all", "Anything.X"))
        self.assertFalse(sh.unit_selected([], "Foo.Adds"))

    def test_scopes(self):
        self.assertEqual([sh.scope(x) for x in ("unit-core", "unit-editor-2", "asan-editor", "asan-core", "htmlui",
                                                 "mcp-7", "linux", "windows-editor-tests")],
                         ["core", "editor", "editor", "core", "htmlui", "mcp", None, None])

    def test_misses_and_what_was_caught(self):
        reports = [report("unit-core", [("test", "Foo.Adds"), ("test", "Baz.Breaks"), ("build", "error: x")]),
                   report("unit-editor-1", [("test", "Any.Thing")]),
                   report("mcp-3", [("test", "test_a.py::test_one"), ("test", "test_b.py::test_two")]),
                   report("linux", [("test", "Lint.Thing")]),
                   report("unit-editor-2", [], status="success")]
        rec = sh.judge({"event": "merge_group", "head": "h" * 40, "shadow": SHADOW}, reports, [], run_id="7", now=1000.0)
        self.assertEqual(rec["misses"], [{"scope": "core", "lane": "unit-core", "test": "Baz.Breaks"},
                                         {"scope": "mcp", "lane": "mcp-3", "test": "test_b.py::test_two"}])
        self.assertEqual(rec["caught"], 3)
        self.assertEqual(rec["selected"], {"core": 2, "editor": "all", "htmlui": 0, "mcp": 1})
        self.assertEqual(rec["ran"], {"core": 10, "editor": 20, "mcp": 10})
        self.assertEqual((rec["run"], rec["event"], rec["time"]), ("7", "merge_group", 1000.0))
        md = sh.markdown(rec)
        self.assertIn("2 miss", md)
        self.assertIn("`Baz.Breaks`", md)

    def test_an_asan_failure_is_judged_by_the_prs_asan_selection(self):
        # a PR runs ASan only for what the sanitizers see (tools/ci/plan_lanes.py SANITIZER_INPUTS)
        reports = [report("asan-core", [("test", "Foo.Adds")]), report("unit-core", [("test", "Foo.Adds")])]
        shadow = dict(SHADOW, asan={"core": [], "editor": []})
        rec = sh.judge({"event": "advisory", "head": "h", "shadow": shadow}, reports, [], now=1000.0)
        self.assertEqual((rec["caught"], rec["misses"]), (1, [{"scope": "core", "lane": "asan-core", "test": "Foo.Adds"}]))
        rec = sh.judge({"event": "advisory", "head": "h", "shadow": dict(SHADOW, asan={"core": ["Foo"], "editor": []})},
                       reports, [], now=1000.0)
        self.assertEqual((rec["caught"], rec["misses"]), (2, []))
        # a shadow without an ASan selection (an older lanes.json): the unit lane's
        rec = sh.judge({"event": "advisory", "head": "h", "shadow": SHADOW}, reports, [], now=1000.0)
        self.assertEqual((rec["caught"], rec["misses"]), (2, []))

    def test_a_failure_the_history_saw_is_not_a_miss(self):
        history = [{"run": "5", "time": 900.0, "misses": [], "known": [],
                    "failures": [{"scope": "core", "test": "Baz.Breaks"}]}]
        rec = sh.judge({"event": "merge_group", "head": "h", "shadow": SHADOW},
                       [report("unit-core", [("test", "Baz.Breaks")])], history, run_id="6", now=1000.0)
        self.assertEqual((rec["misses"], rec["known"]), ([], [{"scope": "core", "lane": "unit-core", "test": "Baz.Breaks"}]))
        # older than the window: a miss again
        rec = sh.judge({"event": "merge_group", "head": "h", "shadow": SHADOW},
                       [report("unit-core", [("test", "Baz.Breaks")])], history, run_id="6",
                       now=900.0 + sh.KNOWN_WINDOW_DAYS * 86400 + 1)
        self.assertEqual(len(rec["misses"]), 1)

    def test_only_what_the_run_ran_whole_is_judged(self):
        # the merge queue runs its planned MCP files, the PR selection itself: nothing to learn there
        lanes = {"event": "merge_group", "head": "h", "shadow": SHADOW, "unit_run": {"core": True, "editor": True},
                 "unit_filter": {"core": "", "editor": "Foo.*"}, "lanes": {"htmlui": {"run": False}},
                 "mcp": {"mode": "planned"}}
        rec = sh.judge(lanes, [report("unit-core", [("test", "Baz.Breaks")]), report("unit-editor-1", [("test", "Baz.Breaks")]),
                               report("mcp-1", [("test", "test_b.py::test_two")])], [], run_id="4", now=1.0)
        self.assertEqual(rec["scopes"], ["core"])
        self.assertEqual([m["lane"] for m in rec["misses"]], ["unit-core"])
        self.assertEqual(rec["ran"], {"core": 10})
        # the merge queue leaves an advisory unit lane out (it runs on the push to main): not judged there
        lanes["lanes"]["unit"] = {"run": False}
        self.assertNotIn("core", sh.whole_scopes(lanes))

    def test_a_run_that_ran_no_narrowed_test_judges_nothing(self):
        # (a push to main builds the test trees without running them)
        rec = sh.judge({"event": "push", "head": "h", "shadow": SHADOW},
                       [report("unit-core", [], ran=0, status="success"), report("linux", [("build", "x")])], [], run_id="3", now=1.0)
        self.assertFalse(rec["judged"])
        self.assertIn("ran no test", rec["why"])

    def test_no_change_attributes_nothing_and_no_shadow_judges_nothing(self):
        rec = sh.judge({"event": "nightly", "head": "h", "shadow": dict(SHADOW, files=[])},
                       [report("mcp-1", [("test", "test_b.py::test_two")])], [], run_id="8", now=1.0)
        self.assertEqual((rec["misses"], len(rec["unattributed"])), ([], 1))
        rec = sh.judge({"event": "nightly", "head": "h", "shadow": None, "shadow_why": "no --shadow-base"},
                       [report("mcp-1", [("test", "test_b.py::test_two")])], [], run_id="9", now=1.0)
        self.assertEqual((rec["misses"], rec["judged"]), ([], False))
        self.assertIn("no --shadow-base", sh.markdown(rec))


class Files(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="shadow-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_main(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with redirect_stdout(out):
            code = sh.main(list(argv))
        return code, out.getvalue()

    def test_check_reads_the_reports_and_writes_a_record_and_the_summary(self):
        lanes = self.tmp / "lanes.json"
        lanes.write_text(json.dumps({"event": "merge_group", "head": "h" * 40, "shadow": SHADOW}))
        reports = self.tmp / "reports"
        (reports / "ci-report-unit-core").mkdir(parents=True)
        (reports / "ci-report-unit-core" / "unit-core.json").write_text(json.dumps(report("unit-core", [("test", "Baz.Breaks")])))
        (reports / "junk.json").write_text("[1, 2]")
        summary = self.tmp / "summary.md"
        code, out = self.run_main("check", "--lanes", str(lanes), "--reports", str(reports), "--run-id", "42",
                                  "--run-url", "https://example.invalid/42", "--out", str(self.tmp / "rec.json"),
                                  "--summary", str(summary))
        self.assertEqual(code, 0, out)
        rec = json.loads((self.tmp / "rec.json").read_text())
        self.assertEqual((rec["run"], len(rec["misses"])), ("42", 1))
        self.assertIn("Baz.Breaks", summary.read_text())

    def test_merge_adds_each_run_once_and_report_adds_up(self):
        history = self.tmp / "shadow.jsonl"
        history.write_text(json.dumps({"run": "1", "time": 1.0, "event": "merge_group", "judged": True, "caught": 2,
                                       "misses": [], "known": [], "failures": [], "unattributed": []}) + "\n")
        new = []
        for run, misses in (("1", []), ("2", [{"scope": "mcp", "lane": "mcp-1", "test": "test_b.py::t"}])):
            p = self.tmp / f"r{run}.json"
            p.write_text(json.dumps({"run": run, "time": float(run), "event": "nightly", "judged": True, "caught": 1,
                                     "misses": misses, "known": [], "failures": [], "unattributed": []}))
            new.append(str(p))
        code, out = self.run_main("merge", "--history", str(history), "--out", str(self.tmp / "add.jsonl"), *new)
        self.assertEqual(code, 0, out)
        added = [json.loads(x) for x in (self.tmp / "add.jsonl").read_text().splitlines()]
        self.assertEqual([r["run"] for r in added], ["2"])
        history.write_text(history.read_text() + (self.tmp / "add.jsonl").read_text())
        code, out = self.run_main("report", "--history", str(history))
        self.assertEqual(code, 0, out)
        self.assertIn("| 2 | 3 | 1 |", out)
        self.assertIn("test_b.py::t", out)

    def test_since(self):
        history = self.tmp / "shadow.jsonl"
        code, out = self.run_main("since", "--history", str(history), "--days", "3")
        self.assertEqual(code, 0)
        self.assertLess(abs(time.time() - 3 * 86400 - time.mktime(time.strptime(out.strip(), "%Y-%m-%dT%H:%M:%SZ")) + time.timezone), 120)
        history.write_text(json.dumps({"run": "1", "time": 86400.0 * 365}) + "\n")
        code, out = self.run_main("since", "--history", str(history))
        # (a day before the newest record: a run that ended after it started earlier; merge drops repeats)
        self.assertEqual(out.strip(), "1970-12-31T00:00:00Z")


if __name__ == "__main__":
    unittest.main(verbosity=2)
