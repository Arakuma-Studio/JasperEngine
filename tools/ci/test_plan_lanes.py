"""Tests of tools/ci/plan_lanes.py (python3 -I tools/ci/test_plan_lanes.py; stdlib only, no build).

The lane rules on synthetic check-changed plans (docs only, a building change, unit and HTML UI
filters, smoke paths, MCP restricted to the CI suite, merge_group, push, full, nightly, .github and
no plan), the job outputs, the shard count, and whole runs on a scratch repository with its own
run_tests.py and shard.py.
"""

from __future__ import annotations

import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import plan_lanes as pl  # noqa: E402

CI = ["test_a.py", "test_b.py", "test_c.py"]
LEFT_OUT = {"test_fixture.py": "needs the default fixture project, which CI has no copy of (x)",
            "test_long.py": "long-running (an hour)", "test_cache.py": "needs a download cache"}
NIGHTLY_OUT = {"test_fixture.py": LEFT_OUT["test_fixture.py"], "test_cache.py": "needs a download cache"}


def plan(files, builds=("linux",), unit=None, mcp=(), fallbacks=(), selected=None):
    mcp_list = mcp if isinstance(mcp, str) else [{"file": f, "reason": "r"} for f in mcp]
    return {"files": list(files), "builds": list(builds), "unit": unit or {"core": [], "editor": [], "htmlui": []},
            "mcp": mcp_list, "mcp_selected": [{"file": f, "reason": "r"} for f in (selected or [])],
            "fallbacks": list(fallbacks), "self_tests": [], "modules_off": "", "navigation_off": False}


# ci.yml's ADVISORY_LANES during the rollout
ADVISORY = ("unit", "asan", "htmlui", "mcp", "smoke", "windows-editor-tests")


def decide(event, p, error="", advisory=(), **extra):
    return pl.decide(event, p, error, CI, LEFT_OUT, NIGHTLY_OUT, **({"advisory": advisory} if advisory else {}), **extra)


def runs(result):
    return {name for name, entry in result["lanes"].items() if entry["run"]}


class Rules(unittest.TestCase):
    def test_every_lane_is_decided(self):
        for event in pl.EVENTS:
            for p in (plan(["engine/core/Foo.cpp"]), plan(["docs/specs/S1.md"]), None):
                r = decide(event, p, "no plan")
                self.assertEqual(set(r["lanes"]), set(pl.LANES), (event, p))
                self.assertTrue(all(entry["why"] for entry in r["lanes"].values()))

    def test_nothing_reads_runs_no_lane_on_a_pr_and_in_the_queue(self):
        for event in ("pull_request", "merge_group", "push", "workflow_dispatch"):
            for files in (["docs/specs/S1.md", "PIXEL-ART-LEDGER.md", "README.md"], []):
                r = decide(event, plan(files, builds=()))
                self.assertEqual(runs(r), {"changes"}, (event, files))
                self.assertEqual(r["mcp"]["mode"], "none")
                self.assertFalse(any(r["unit_run"].values()))
        self.assertIn("nothing reads", decide("pull_request", plan(["README.md"]))["lanes"]["lint"]["why"])

    def test_a_guide_doc_builds_every_platform(self):
        r = decide("pull_request", plan(["docs/mcp/guide/code.md"], mcp=["test_a.py"]))
        self.assertEqual(runs(r), {"changes", "lint", "mcp", *pl.PLATFORM_LANES})
        self.assertEqual(r["mcp"], {"mode": "planned", "files": ["tools/mcp/tests/test_a.py"], "not_in_ci": []})

    def test_a_pr_filters_the_unit_tests(self):
        r = decide("pull_request", plan(["engine/core/Foo.cpp"], builds=("linux", "unit-tests", "web"),
                                        unit={"core": ["Foo", "Bar"], "editor": [], "htmlui": []}))
        self.assertEqual(r["unit_run"], {"core": True, "editor": False})
        self.assertEqual(r["unit_filter"]["core"], "Foo.*:*/Foo.*:Foo/*.*:Bar.*:*/Bar.*:Bar/*.*")
        self.assertTrue(r["lanes"]["unit"]["run"] and r["lanes"]["asan"]["run"])
        self.assertFalse(r["lanes"]["htmlui"]["run"])
        self.assertTrue(r["lanes"]["smoke"]["run"])            # engine/: what a game's export reads
        r = decide("pull_request", plan(["editor/App.cpp"], unit={"core": [], "editor": [], "htmlui": []}))
        self.assertFalse(r["lanes"]["unit"]["run"] or r["lanes"]["asan"]["run"])
        self.assertFalse(r["lanes"]["smoke"]["run"])            # (not the export path)
        r = decide("pull_request", plan(["engine/x.cpp"], unit="all"))
        self.assertEqual((r["unit_run"], r["unit_filter"], r["htmlui_filter"], r["lanes"]["htmlui"]["run"]),
                         ({"core": True, "editor": True}, {"core": "", "editor": ""}, "", True))
        r = decide("pull_request", plan(["engine/core/htmlui/A.cpp"], unit={"core": [], "editor": [], "htmlui": ["Css"]}))
        self.assertEqual((r["lanes"]["htmlui"]["run"], r["htmlui_filter"]), (True, "Css.*:*/Css.*:Css/*.*"))

    def test_a_filter_too_long_runs_everything(self):
        suites = [f"Suite{i:05d}LongName" for i in range(3000)]
        r = decide("pull_request", plan(["engine/a.h"], unit={"core": suites, "editor": [], "htmlui": []}))
        self.assertEqual((r["unit_run"]["core"], r["unit_filter"]["core"]), (True, ""))
        self.assertTrue(any("core filter" in f for f in r["fallbacks"]))

    def test_smoke_paths(self):
        for f in ("editor/Exporter.cpp", "editor/resources/templates/2d/project.yaml", "tools/fixtures/x/a.scene",
                  "tools/smoke-test.sh", "CMakeLists.txt", "docker/linux.Dockerfile", "editor/stream/Scene.cpp"):
            self.assertTrue(decide("pull_request", plan([f]))["lanes"]["smoke"]["run"], f)
        self.assertFalse(decide("pull_request", plan(["editor/window/Inspector.cpp"]))["lanes"]["smoke"]["run"])

    def test_root_cmake_runs_html_ui_too(self):
        r = decide("pull_request", plan(["CMakeLists.txt"], unit="all", mcp="all"))
        self.assertTrue(r["lanes"]["htmlui"]["run"])
        self.assertEqual(r["mcp"]["mode"], "ci")

    def test_mcp_is_the_ci_suite(self):
        r = decide("pull_request", plan(["editor/a.cpp"], mcp=["test_a.py", "test_fixture.py", "test_gone.py"]))
        self.assertEqual(r["mcp"]["files"], ["tools/mcp/tests/test_a.py"])
        self.assertEqual(r["mcp"]["not_in_ci"], [
            {"file": "tools/mcp/tests/test_fixture.py", "reason": LEFT_OUT["test_fixture.py"]},
            {"file": "tools/mcp/tests/test_gone.py", "reason": "not a test file of this tree"}])
        r = decide("pull_request", plan(["editor/a.cpp"], mcp=["test_fixture.py"]))
        self.assertEqual((r["mcp"]["mode"], r["lanes"]["mcp"]["run"]), ("none", False))
        self.assertIn("not in the CI suite", r["lanes"]["mcp"]["why"])
        # the plan's CI suite: the whole of it, and what it named that CI leaves out
        r = decide("pull_request", plan(["editor/a.cpp"], mcp="ci", selected=["test_long.py", "test_a.py"],
                                        fallbacks=["no coverage map: the CI suite"]))
        self.assertEqual(r["mcp"]["mode"], "ci")
        self.assertEqual(len(r["mcp"]["files"]), 3)
        self.assertEqual([n["file"] for n in r["mcp"]["not_in_ci"]], ["tools/mcp/tests/test_long.py"])
        self.assertIn("no coverage map: the CI suite", r["fallbacks"])
        # the whole suite planned: the CI suite, everything it leaves out listed
        r = decide("pull_request", plan(["tools/mcp/harness.py"], mcp="all"))
        self.assertEqual((r["mcp"]["mode"], len(r["mcp"]["not_in_ci"])), ("ci", 3))

    def test_merge_group_runs_every_lane_unfiltered_and_plans_mcp(self):
        r = decide("merge_group", plan(["editor/App.cpp"], unit={"core": ["Foo"], "editor": [], "htmlui": []},
                                       mcp=["test_b.py"]))
        self.assertEqual(runs(r), set(pl.LANES))
        self.assertEqual((r["unit_filter"], r["htmlui_filter"]), ({"core": "", "editor": ""}, ""))
        self.assertEqual(r["unit_run"], {"core": True, "editor": True})
        self.assertEqual(r["mcp"]["files"], ["tools/mcp/tests/test_b.py"])
        # no MCP file planned: the lane stays off, every other runs
        r = decide("merge_group", plan(["editor/App.cpp"]))
        self.assertEqual(runs(r), set(pl.LANES) - {"mcp"})

    def test_merge_group_leaves_the_advisory_lanes_out(self):
        # an advisory lane cannot block a merge, so the queue does not wait hours for it (queue runs
        # of #124-#126: 217-276 min, ASan the longest, the slowest blocking job 96-137)
        p = plan(["editor/App.cpp"], unit={"core": ["Foo"], "editor": [], "htmlui": []}, mcp=["test_b.py"])
        r = decide("merge_group", p, advisory=ADVISORY)
        self.assertEqual(runs(r), {"changes", "lint", *pl.PLATFORM_LANES})
        self.assertEqual(r["advisory_off"], ["unit", "asan", "htmlui", "mcp", "smoke", "windows-editor-tests"])
        # the windows editor leg still builds, without its advisory test run
        self.assertEqual(r["build_only"], ["windows-editor-tests"])
        for name in ("unit", "asan", "htmlui", "mcp", "smoke"):
            self.assertIn("ADVISORY_LANES", r["lanes"][name]["why"], name)
        self.assertEqual(r["mcp"], {"mode": "none", "files": [], "not_in_ci": []})
        # it follows the list: a lane taken off it runs in the queue again
        r = decide("merge_group", p, advisory=("unit", "htmlui", "mcp", "smoke"))
        self.assertEqual(runs(r), set(pl.LANES) - {"unit", "htmlui", "mcp", "smoke"})
        self.assertEqual((r["advisory_off"], r["build_only"]), (["unit", "htmlui", "mcp", "smoke"], []))
        self.assertEqual(pl.asan_job_names(pl.planned_asan_run(r["lanes"], r["asan_run"])), ["core", "editor"])
        self.assertEqual(decide("merge_group", p, advisory=()), decide("merge_group", p))
        self.assertEqual(runs(decide("merge_group", p)), set(pl.LANES))
        # only the lanes the push to main runs instead: lint and the platform builds stay in the queue
        r = decide("merge_group", p, advisory=("lint", "linux", "windows", "mcp"))
        self.assertEqual(runs(r), set(pl.LANES) - {"mcp"})
        self.assertEqual(r["advisory_off"], ["mcp"])
        # however the queue widened: a .github change, no plan
        for q in (plan([".github/workflows/ci.yml"], builds=()), None):
            r = decide("merge_group", q, "exit 2", advisory=ADVISORY)
            self.assertEqual(runs(r), {"changes", "lint", *pl.PLATFORM_LANES}, q)
        # a group nothing reads runs nothing, and leaves nothing out for being advisory
        r = decide("merge_group", plan(["README.md"], builds=()), advisory=ADVISORY)
        self.assertEqual((runs(r), r["advisory_off"], r["build_only"]), ({"changes"}, [], []))

    def test_advisory_lanes_change_no_pull_request_and_no_full_run(self):
        for event, p in (("pull_request", plan(["engine/core/Foo.cpp"], unit="all", mcp=["test_a.py"])),
                         ("pull_request", plan([".github/workflows/ci.yml"])), ("full", None), ("nightly", None),
                         ("workflow_dispatch", plan(["editor/App.cpp"], mcp=["test_a.py"]))):
            r = decide(event, p, advisory=ADVISORY)
            self.assertEqual(r, decide(event, p), event)
            self.assertEqual((r["advisory_off"], r["build_only"]), ([], []), event)

    def test_push_runs_the_builds_only(self):
        r = decide("push", plan(["engine/core/Foo.cpp"], mcp=["test_a.py"]))
        self.assertEqual(runs(r), {"changes", "unit", "asan", "htmlui", "smoke", *pl.PLATFORM_LANES} - {"ios"})
        self.assertIn("build only", r["lanes"]["unit"]["why"])
        self.assertEqual(r["mcp"]["mode"], "none")
        # ASan keeps a ccache entry per binary (line tables only fit them in the budget), which only main saves
        self.assertIn("build only", r["lanes"]["asan"]["why"])
        self.assertEqual(pl.asan_job_names(pl.planned_asan_run(r["lanes"], r["asan_run"])), ["core", "editor"])
        self.assertEqual(r["build_only"], ["unit", "asan", "htmlui", "windows-editor-tests"])

    def test_push_builds_and_leaves_the_advisory_tests_to_the_advisory_run(self):
        # the advisory tests take 4.5 h (ASan): in the push run, every push held about 14 Linux jobs that
        # long and bursts overlapped (3 runs at once on 2026-11-20); the advisory run the push dispatches
        # takes them one at a time, the newest waiting
        p = plan(["editor/window/InspectorX.cpp"], unit={"core": ["Foo"], "editor": [], "htmlui": ["Css"]},
                 mcp=["test_a.py"])
        r = decide("push", p, advisory=ADVISORY)
        self.assertEqual(runs(r), {"changes", "unit", "asan", "htmlui", *pl.PLATFORM_LANES} - {"ios"})
        self.assertEqual((r["build_only"], r["advisory_off"]), (["unit", "asan", "htmlui", "windows-editor-tests"], []))
        self.assertEqual(r["mcp"]["mode"], "none")
        self.assertIn("build only", r["lanes"]["unit"]["why"])
        self.assertTrue(r["dispatch_advisory"])
        # no advisory test lane (ADVISORY_LANES emptied, or naming only lint), nothing that builds, or a
        # newer push supersedes it: no advisory run
        for q, adv, sup in ((p, (), None), (p, ("lint", "linux"), None), (plan(["README.md"], builds=()), ADVISORY, None),
                            (p, ADVISORY, ("c" * 40, "engine/a.cpp"))):
            self.assertFalse(decide("push", q, advisory=adv, superseded_by=sup)["dispatch_advisory"], (q["files"], adv, sup))
        self.assertTrue(decide("push", p, advisory=("windows-editor-tests",))["dispatch_advisory"])
        for event in ("pull_request", "merge_group", "full", "advisory"):
            self.assertFalse(decide(event, p, advisory=ADVISORY)["dispatch_advisory"], event)

    def test_the_advisory_run_runs_what_the_queue_left_out(self):
        p = plan(["editor/window/InspectorX.cpp"], unit={"core": ["Foo"], "editor": [], "htmlui": ["Css"]},
                 mcp=["test_a.py", "test_fixture.py"])
        r = decide("advisory", p, advisory=ADVISORY)
        # linux: the editor install the mcp and smoke lanes run; windows: the editor leg's test run
        self.assertEqual(runs(r), {"changes", "linux", "unit", "asan", "htmlui", "mcp", "smoke", "windows"})
        self.assertEqual((r["build_only"], r["advisory_off"]), ([], []))
        self.assertEqual((r["unit_run"], r["unit_filter"], r["htmlui_filter"]),
                         ({"core": True, "editor": True}, {"core": "", "editor": ""}, ""))
        self.assertEqual(r["mcp"], decide("merge_group", p)["mcp"])
        self.assertEqual(r["mcp"]["files"], ["tools/mcp/tests/test_a.py"])
        for name in ("unit", "asan", "htmlui", "mcp", "smoke"):
            self.assertIn("ADVISORY_LANES", r["lanes"][name]["why"], name)
        self.assertIn("every game export", r["lanes"]["smoke"]["why"])
        self.assertIn("windows-editor-tests", r["lanes"]["windows"]["why"])
        self.assertIn("editor install", r["lanes"]["linux"]["why"])
        for name in ("lint", "web", "android", "macos", "ios"):
            self.assertIn("the merge queue ran it", r["lanes"][name]["why"], name)
        # it follows the list
        for adv, want in ((("mcp",), {"changes", "linux", "mcp"}), (("unit",), {"changes", "unit"}),
                          (("windows-editor-tests",), {"changes", "windows"}), ((), {"changes"})):
            r = decide("advisory", p, advisory=adv)
            self.assertEqual(runs(r), want, adv)
            if "mcp" not in adv:
                self.assertEqual(r["mcp"]["mode"], "none", adv)
        # nothing that builds changed since the last advisory run: nothing runs
        self.assertEqual(runs(decide("advisory", plan(["README.md"], builds=()), advisory=ADVISORY)), {"changes"})
        # no plan (no earlier advisory run to diff against): every advisory lane, the whole MCP CI suite
        r = decide("advisory", None, "no earlier advisory run of main finished", advisory=ADVISORY)
        self.assertEqual(runs(r), {"changes", "linux", "unit", "asan", "htmlui", "mcp", "smoke", "windows"})
        self.assertEqual(r["mcp"]["mode"], "ci")
        self.assertTrue(any("no earlier advisory run" in f for f in r["fallbacks"]), r["fallbacks"])

    def test_the_previous_run_of_a_stage(self):
        def run(i, conclusion="success", title="CI", status="completed"):
            return {"databaseId": i, "headSha": f"{i:x}" * 40 if i < 16 else "e" * 40, "status": status,
                    "conclusion": conclusion, "displayTitle": title, "createdAt": f"2026-11-20T{i:02d}:00:00Z"}
        adv = pl.ADVISORY_TITLE + "main at " + "0" * 40
        runs_ = [run(9, "", adv, "in_progress"), run(8, "cancelled", adv), run(7), run(6, "failure", adv), run(5, "success", adv),
                 {"databaseId": 4, "headSha": "not a sha", "status": "completed", "conclusion": "success"}, "junk"]
        self.assertEqual(pl.previous_run(runs_, 9), ("7" * 40, 7))
        self.assertEqual(pl.previous_run(runs_, 9, title=pl.ADVISORY_TITLE), ("6" * 40, 6))
        self.assertIsNone(pl.previous_run(runs_[:2], 9))
        self.assertIsNone(pl.previous_run({"not": "a list"}, 9))

    def test_a_newer_push_that_builds_supersedes_this_one(self):
        # pushes to main come one per merge-queue group, some minutes apart (2026-11-15..20: 62 s, 3 and
        # 4 min of 20 gaps): the newest push's run builds and saves the caches, an older one steps aside
        p = plan(["engine/core/Foo.cpp"], mcp=["test_a.py"])
        r = decide("push", p, advisory=ADVISORY, superseded_by=("c" * 40, "editor/App.cpp"))
        self.assertEqual(runs(r), {"changes"})
        self.assertEqual(r["build_only"], [])
        for name in pl.LANES[1:]:
            self.assertIn("superseded by ccccccc", r["lanes"][name]["why"], name)
            self.assertIn("editor/App.cpp", r["lanes"][name]["why"], name)
        # only a push steps aside
        for event in ("pull_request", "merge_group", "workflow_dispatch", "full"):
            self.assertEqual(decide(event, p, superseded_by=("c" * 40, "a")), decide(event, p), event)

    def test_push_runs_no_lane_that_keeps_no_cache(self):
        # a push run is for the caches PRs restore: ios keeps no entry, so on push it would take a macOS
        # runner (of 5) for nothing; the merge queue ran it
        for files in (["engine/core/Foo.cpp"], ["editor/window/InspectorX.cpp"]):
            r = decide("push", plan(files, mcp=["test_a.py"]))
            self.assertFalse(r["lanes"]["ios"]["run"], files)
            self.assertIn("no cache", r["lanes"]["ios"]["why"])

    def test_push_warms_the_smoke_cache_when_an_export_reads_the_change(self):
        # only main saves caches, so a smoke lane that never runs on push never warms its ccache
        r = decide("push", plan(["engine/core/Foo.cpp"], mcp=["test_a.py"]))
        self.assertTrue(r["lanes"]["smoke"]["run"], r["lanes"]["smoke"])
        self.assertIn("cache", r["lanes"]["smoke"]["why"])
        r = decide("push", plan(["editor/window/InspectorX.cpp"], mcp=["test_a.py"]))
        self.assertFalse(r["lanes"]["smoke"]["run"], r["lanes"]["smoke"])

    def test_full_and_nightly(self):
        r = decide("full", None)
        self.assertEqual(runs(r), set(pl.LANES))
        self.assertEqual((r["mcp"]["mode"], len(r["mcp"]["files"])), ("ci", 3))
        self.assertEqual(len(r["mcp"]["not_in_ci"]), 3)
        r = decide("nightly", None)
        self.assertEqual(runs(r), set(pl.LANES))
        self.assertEqual(r["mcp"]["mode"], "nightly")
        self.assertEqual(r["mcp"]["files"], [f"tools/mcp/tests/{f}" for f in ("test_a.py", "test_b.py", "test_c.py", "test_long.py")])
        self.assertEqual(r["mcp"]["not_in_ci"], [{"file": f"tools/mcp/tests/{n}", "reason": w} for n, w in sorted(NIGHTLY_OUT.items())])

    def test_fail_open(self):
        for event in ("pull_request", "merge_group", "workflow_dispatch"):
            r = decide(event, None, "exit 2: no merge base")
            self.assertEqual(runs(r), set(pl.LANES), event)
            self.assertEqual(r["mcp"]["mode"], "ci")
            self.assertTrue(any("no merge base" in f for f in r["fallbacks"]))
        r = decide("push", None, "exit 2")
        self.assertNotIn("mcp", runs(r))
        # CI itself: every lane but ASan, which a workflow change outside its own job does not reach
        r = decide("pull_request", plan([".github/workflows/ci.yml"], builds=()))
        self.assertEqual(runs(r), set(pl.LANES) - {"asan"})
        self.assertEqual(r["mcp"]["mode"], "ci")
        self.assertTrue(any(".github/workflows/ci.yml" in f for f in r["fallbacks"]))

    def test_the_rollback_variable_selects_every_test(self):
        # CI_FULL_SELECTION=true (a repository variable): the selection is not trusted, every lane runs whole
        p = plan(["engine/core/Foo.cpp"], builds=("linux", "unit-tests"), unit={"core": ["Foo"], "editor": [], "htmlui": []},
                 mcp=["test_a.py"])
        for event in ("pull_request", "merge_group"):
            r = pl.decide(event, p, "", CI, LEFT_OUT, NIGHTLY_OUT, full_selection=True)
            self.assertEqual(runs(r), set(pl.LANES), event)
            self.assertEqual((r["unit_run"], r["unit_filter"], r["htmlui_filter"]),
                             ({"core": True, "editor": True}, {"core": "", "editor": ""}, ""))
            self.assertEqual((r["mcp"]["mode"], len(r["mcp"]["files"])), ("ci", 3))
            self.assertTrue(any("CI_FULL_SELECTION" in f for f in r["fallbacks"]), r["fallbacks"])
        # the advisory run: its lanes whole, the whole MCP CI suite
        r = pl.decide("advisory", p, "", CI, LEFT_OUT, NIGHTLY_OUT, advisory=ADVISORY, full_selection=True)
        self.assertEqual((r["unit_filter"], r["htmlui_filter"], r["mcp"]["mode"]), ({"core": "", "editor": ""}, "", "ci"))
        # prose nothing reads still runs nothing; push stays the build lanes
        r = pl.decide("pull_request", plan(["docs/specs/S1.md"], builds=()), "", CI, LEFT_OUT, NIGHTLY_OUT, full_selection=True)
        self.assertEqual(runs(r), {"changes"})
        r = pl.decide("push", p, "", CI, LEFT_OUT, NIGHTLY_OUT, full_selection=True)
        self.assertEqual(r["mcp"]["mode"], "none")

    def test_github_output(self):
        r = decide("pull_request", plan(["engine/core/Foo.cpp"], unit={"core": ["Foo"], "editor": [], "htmlui": []}))
        doc = {"full": False, "lanes": r["lanes"], "unit_filter": r["unit_filter"], "htmlui_filter": r["htmlui_filter"],
               "asan_run": r["asan_run"], "asan_filter": r["asan_filter"], "mcp": dict(r["mcp"], shards=["1", "2"])}
        lines = pl.github_output(doc, r["unit_run"]).splitlines()
        keys = [line.split("=", 1)[0] for line in lines]
        self.assertEqual(keys, ["full", *[f"run_{n}" for n in pl.LANES], "unit_filter_core", "unit_filter_editor",
                                "unit_run_core", "unit_run_editor", "unit_jobs", "asan_jobs", "asan_filter_core",
                                "asan_filter_editor", "htmlui_filter", "mcp_shards",
                                "build_only", "dispatch_advisory"])
        self.assertIn("dispatch_advisory=false", lines)
        self.assertIn("dispatch_advisory=true", pl.github_output(dict(doc, dispatch_advisory=True), r["unit_run"]).splitlines())
        self.assertIn("unit_filter_core=Foo.*:*/Foo.*:Foo/*.*", lines)
        self.assertIn('mcp_shards=["1","2"]', lines)
        self.assertIn("unit_run_editor=false", lines)
        # the unit lane's matrix: one job per planned test binary, the editor's in two
        self.assertIn('unit_jobs=[{"job":"core","suite":"core","part":"","other":""}]', lines)
        # the asan lane's matrix: one job per planned test binary
        self.assertIn('asan_jobs=[{"job":"core","suite":"core"}]', lines)
        self.assertIn("asan_filter_core=Foo.*:*/Foo.*:Foo/*.*", lines)
        doc["mcp"]["shards"] = []
        self.assertIn("mcp_shards=[]", pl.github_output(doc, r["unit_run"]).splitlines())
        # the jobs that build but skip a test run (fromJSON in ci.yml: a list even when empty)
        self.assertIn("build_only=[]", pl.github_output(doc, r["unit_run"]).splitlines())
        self.assertIn('build_only=["windows-editor-tests"]',
                      pl.github_output(dict(doc, build_only=["windows-editor-tests"]), r["unit_run"]).splitlines())
        both = [line for line in pl.github_output(doc, {"core": True, "editor": True}).splitlines() if line.startswith("unit_jobs=")]
        self.assertEqual(json.loads(both[0].split("=", 1)[1]), [
            {"job": "core", "suite": "core", "part": "", "other": ""},
            {"job": "editor-1", "suite": "editor", "part": "1/2", "other": "2"},
            {"job": "editor-2", "suite": "editor", "part": "2/2", "other": "1"}])
        doc["asan_run"] = {"core": True, "editor": True}
        both = [line for line in pl.github_output(doc, {"core": True, "editor": True}).splitlines() if line.startswith("asan_jobs=")]
        self.assertEqual(json.loads(both[0].split("=", 1)[1]), [{"job": "core", "suite": "core"}, {"job": "editor", "suite": "editor"}])
        # a lane that does not run has no jobs
        doc["lanes"] = dict(doc["lanes"], asan={"run": False, "why": "no unit test sees the change"})
        self.assertIn("asan_jobs=[]", pl.github_output(doc, {"core": True, "editor": True}).splitlines())

    def test_a_pr_runs_asan_only_for_what_the_sanitizers_see(self):
        # the asan lane costs 2.5-5 runner-hours a run; the advisory run on main still runs it after the merge.
        # The unit lane runs for these, ASan does not: CI's scripts, docs, MCP tests, a test's data, assets
        for f in ("tools/ci/report.py", "tools/mcp/requirements.txt", "tools/mcp/tests/test_a.py", "docs/testing.md",
                  "editor/tests/fixtures/titleart/fonts/Oswald-wght.woff2", "engine/shaderlib/lit.glsl",
                  "editor/resources/icons/play.png", "engine/core/script/binding/stubs/Physics.lua"):
            r = decide("pull_request", plan([f], unit="all", mcp=["test_a.py"]))
            self.assertTrue(r["lanes"]["unit"]["run"], f)
            self.assertFalse(r["lanes"]["asan"]["run"], f)
            self.assertIn("advisory run", r["lanes"]["asan"]["why"])
            self.assertEqual(pl.asan_job_names(pl.planned_asan_run(r["lanes"], r["asan_run"])), [], f)
        suites = {"core": ["Foo"], "editor": ["Bar"], "htmlui": []}
        for f in ("engine/core/Foo.cpp", "editor/window/Inspector.h", "engine/core/serialization/RuntimeFields.gen.inc",
                  "libs/woff2/src/woff2_common.cc", "engine/libs/sokol/sokol_gfx.h", "editor/libs/draco/README.md",
                  "engine/CMakeLists.txt", "engine/cmake/JasperSanitizers.cmake", "CMakePresets.json",
                  "docker/linux.Dockerfile", "tools/build.sh", "tools/ci/gtest-gate.py"):
            for event in ("pull_request", "workflow_dispatch"):
                r = decide(event, plan([f, "docs/testing.md"], unit=suites))
                self.assertTrue(r["lanes"]["asan"]["run"], (f, event))
                self.assertEqual((r["asan_run"], r["asan_filter"]), (r["unit_run"], r["unit_filter"]), f)
        # a C++ change no unit test sees: neither
        r = decide("pull_request", plan(["editor/App.cpp"]))
        self.assertFalse(r["lanes"]["asan"]["run"])
        self.assertEqual(r["lanes"]["asan"]["why"], r["lanes"]["unit"]["why"])

    def test_the_asan_lanes_own_settings_run_every_test_under_it(self):
        # no selection sees a suppression file or the lane's job: both binaries, every test
        for p, definition in ((plan(["tools/sanitizers/ubsan.supp"], builds=()), ""),
                              (plan(["tools/sanitizers/lsan.supp", "editor/App.cpp"], unit={"core": [], "editor": ["App"], "htmlui": []}), ""),
                              (plan([".github/workflows/ci.yml"], builds=()), ".github/workflows/ci.yml (the asan job)")):
            r = decide("pull_request", p, asan_definition=definition)
            self.assertTrue(r["lanes"]["asan"]["run"], p["files"])
            self.assertEqual((r["asan_run"], r["asan_filter"]), ({"core": True, "editor": True}, {"core": "", "editor": ""}))
            self.assertIn("the lane's settings or definition", r["lanes"]["asan"]["why"])
            self.assertEqual(pl.asan_job_names(pl.planned_asan_run(r["lanes"], r["asan_run"])), ["core", "editor"])
        self.assertFalse(decide("pull_request", plan(["tools/sanitizers/ubsan.supp"], builds=()))["lanes"]["unit"]["run"])

    def test_the_queue_the_advisory_run_and_the_rollback_keep_asan(self):
        p = plan(["tools/ci/report.py"], unit="all")
        # the merge queue: unchanged, every unit test under ASan unless it is advisory
        r = decide("merge_group", p)
        self.assertTrue(r["lanes"]["asan"]["run"])
        self.assertEqual(r["asan_run"], {"core": True, "editor": True})
        self.assertIn("asan", decide("merge_group", p, advisory=ADVISORY)["advisory_off"])
        # the advisory run covers what a PR left out
        r = decide("advisory", p, advisory=ADVISORY)
        self.assertTrue(r["lanes"]["asan"]["run"])
        self.assertEqual(pl.asan_job_names(pl.planned_asan_run(r["lanes"], r["asan_run"])), ["core", "editor"])
        # the rollback switch and no plan: every lane
        self.assertTrue(decide("pull_request", p, full_selection=True)["lanes"]["asan"]["run"])
        self.assertTrue(decide("pull_request", None, "exit 2")["lanes"]["asan"]["run"])
        self.assertTrue(decide("pull_request", plan([".github/workflows/ci.yml"], builds=()), full_selection=True)["lanes"]["asan"]["run"])

    def test_unit_jobs(self):
        # the editor tests' build is most of their lane (CI runs 43737549764: 110 of 123 min), so two
        # jobs each compile half of it; core stays one job
        self.assertEqual(pl.unit_job_names({"core": True, "editor": True}), ["core", "editor-1", "editor-2"])
        self.assertEqual(pl.unit_job_names({"core": False, "editor": True}), ["editor-1", "editor-2"])
        self.assertEqual(pl.unit_job_names({"core": False, "editor": False}), [])

    def test_asan_jobs(self):
        # one job per test binary: in one job the cold core build, its tests and the editor's build left
        # the editor's tests no time (merge-queue runs 44582666848, 44487933891)
        self.assertEqual(pl.asan_job_names({"core": True, "editor": True}), ["core", "editor"])
        self.assertEqual(pl.asan_job_names({"core": False, "editor": True}), ["editor"])
        self.assertEqual(pl.asan_job_names({"core": False, "editor": False}), [])


TEST_FILE = """CI = True
EDITOR = {{"create": True}}
{extra}
async def test_one():
    pass

async def test_two():
    pass

for _i in range(2):
    async def _made():
        pass
    globals()[f"test_made_{{_i}}"] = _made
    _made.__name__ = f"test_made_{{_i}}"
"""


def sh(cwd: Path, *args: str) -> str:
    return subprocess.run(list(args), cwd=str(cwd), check=True, capture_output=True, text=True).stdout


class Selection(unittest.TestCase):
    """lanes.json's selection: what each test lane runs, and why (the job summary prints it)."""

    def test_reasons_of_a_planned_change(self):
        p = plan(["engine/core/Foo.cpp", "tools/mcp/tests/test_b.py"], builds=("linux", "unit-tests"),
                 unit={"core": ["Foo", "FooGpu"], "editor": [], "htmlui": []}, mcp=["test_a.py", "test_b.py", "test_long.py"])
        p["unit_reasons"] = {"core": {"Foo": ["dependency", "engine/tests/core/FooTest.cpp includes engine/core/Foo.h"],
                                      "FooGpu": ["policy", "a shader changed: the suites that draw (*Gpu)"]},
                             "editor": {}, "htmlui": {}}
        p["mcp"] = [{"file": "test_a.py", "reason": "runs engine/core/Foo.cpp (the changed functions)", "kind": "dependency"},
                    {"file": "test_b.py", "reason": "the test file changed", "kind": "owner"},
                    {"file": "test_long.py", "reason": "runs engine/core/Foo.cpp", "kind": "dependency"}]
        r = decide("pull_request", p)
        sel = pl.selection("pull_request", p, r, CI)
        self.assertEqual(sel["unit"]["core"]["mode"], "suites")
        self.assertEqual(sel["unit"]["core"]["suites"], [
            {"suite": "Foo", "kind": "dependency", "why": "engine/tests/core/FooTest.cpp includes engine/core/Foo.h"},
            {"suite": "FooGpu", "kind": "policy", "why": "a shader changed: the suites that draw (*Gpu)"}])
        self.assertEqual(sel["unit"]["editor"], {"mode": "none", "kind": "", "why": "no unit test sees the change", "suites": []})
        # MCP: the CI suite's planned files only, each with its reason
        self.assertEqual(sel["mcp"]["mode"], "planned")
        self.assertEqual(sel["mcp"]["files"], [
            {"file": "test_a.py", "kind": "dependency", "why": "runs engine/core/Foo.cpp (the changed functions)"},
            {"file": "test_b.py", "kind": "owner", "why": "the test file changed"}])
        self.assertEqual(sel["mcp"]["of"], 3)
        md = pl.selection_markdown(sel, r["fallbacks"], {})
        self.assertIn("| unit (core) | 2 suites |", md)
        self.assertIn("1 dependency, 1 policy", md)
        self.assertIn("`test_b.py` (owner): the test file changed", md)

    def test_a_widening_says_it_is_a_fallback_and_why(self):
        p = plan(["engine/CMakeLists.txt"], unit="all", mcp="ci", fallbacks=["no coverage map: the CI suite for the C++ change"])
        r = decide("pull_request", p)
        sel = pl.selection("pull_request", p, r, CI)
        self.assertEqual((sel["unit"]["core"]["mode"], sel["unit"]["core"]["kind"]), ("all", "fallback"))
        self.assertEqual((sel["mcp"]["mode"], sel["mcp"]["kind"]), ("ci", "fallback"))
        self.assertIn("no coverage map", sel["mcp"]["why"])
        md = pl.selection_markdown(sel, r["fallbacks"], {"coverage-map": {"commit": "abcdef1234567", "run": "9", "created": 0}})
        self.assertIn("| MCP | all 3 files |", md)
        self.assertIn("fallback: no coverage map", md)
        self.assertIn("coverage map of abcdef123456 (run 9", md)
        # the merge queue and a full run: the whole suites by rule, not by a fallback
        r = decide("merge_group", plan(["engine/core/Foo.cpp"], unit={"core": ["Foo"], "editor": [], "htmlui": []}))
        sel = pl.selection("merge_group", plan(["engine/core/Foo.cpp"]), r, CI)
        self.assertEqual((sel["unit"]["editor"]["mode"], sel["unit"]["editor"]["kind"]), ("all", "full"))
        sel = pl.selection("full", None, decide("full", None), CI)
        self.assertEqual((sel["mcp"]["mode"], sel["mcp"]["kind"]), ("ci", "full"))


class Shadow(unittest.TestCase):
    """lanes.json's shadow: what a pull request's selection would run for a fuller run's diff."""

    def test_the_pr_selection_of_a_plan(self):
        p = plan(["engine/core/Foo.cpp"], builds=("linux", "unit-tests"), unit={"core": ["Foo"], "editor": [], "htmlui": ["Css"]},
                 mcp=["test_a.py", "test_long.py"])
        p["base"] = "b" * 40
        self.assertEqual(pl.shadow_selection(p, CI, LEFT_OUT, NIGHTLY_OUT), {
            "base": "b" * 40, "files": ["engine/core/Foo.cpp"], "fallbacks": [],
            "unit": {"core": ["Foo"], "editor": [], "htmlui": ["Css"]}, "asan": {"core": ["Foo"], "editor": []},
            "mcp": ["test_a.py"]})
        # what a PR widens to, it widens to here too; the rollback switch does not apply (it measures the shortcut)
        sh = pl.shadow_selection(plan(["engine/x.cpp"], unit="all", mcp="ci", fallbacks=["no coverage map"]), CI, LEFT_OUT, NIGHTLY_OUT)
        self.assertEqual((sh["unit"], sh["mcp"]), ({"core": "all", "editor": "all", "htmlui": "all"}, "all"))
        self.assertIn("no coverage map", sh["fallbacks"])
        sh = pl.shadow_selection(plan([".github/workflows/ci.yml"], builds=()), CI, LEFT_OUT, NIGHTLY_OUT)
        self.assertEqual((sh["mcp"], sh["asan"]), ("all", {"core": [], "editor": []}))
        sh = pl.shadow_selection(plan([".github/workflows/ci.yml"], builds=()), CI, LEFT_OUT, NIGHTLY_OUT,
                                 ".github/workflows/ci.yml (the asan job or the workflow's env)")
        self.assertEqual(sh["asan"], {"core": "all", "editor": "all"})
        sh = pl.shadow_selection(plan(["docs/specs/S1.md"], builds=()), CI, LEFT_OUT, NIGHTLY_OUT)
        self.assertEqual((sh["unit"], sh["mcp"]), ({"core": [], "editor": [], "htmlui": []}, []))


class Shards(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="plan-lanes-shards-"))
        self.tests = self.tmp / "tests"
        self.tests.mkdir()
        self.shard = pl.load_module(HERE.parent / "mcp" / "shard.py", "test_plan_lanes_shard")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_more_shards_while_each_saves_more_than_its_setup(self):
        tests = [f"test_f{i}.py::test_x" for i in range(100)]
        # 100 files of 10 min, 2 workers a shard: 10 shards of 5 files a worker; 11 or 12 take as long,
        # and 13 save 10 min for 3 more setups (12 min)
        times = {t: 600.0 for t in tests}
        self.assertEqual(self.shard_count(tests, times, 256), 10)
        self.assertEqual(self.shard_count(tests, times, 8), 8)
        self.assertEqual(self.shard_count(tests[:1], {}, 8), 1)
        # 8 files of 5 min: a second shard halves 20 min, more than its 4 min setup
        self.assertEqual(self.shard_count(tests[:8], {t: 300.0 for t in tests}, 8), 2)
        # 4 files of 100 s: a second shard would save less than its setup
        self.assertEqual(self.shard_count(tests[:4], {t: 100.0 for t in tests}, 8), 1)
        # one file longer than all the rest: it alone sets the length, no more shards for it
        self.assertEqual(self.shard_count(tests[:3], {tests[0]: 7200.0}, 256), 1)

    def shard_count(self, tests, times, cap):
        return pl.shard_count(self.shard, tests, times, self.tests, cap)


class Runs(unittest.TestCase):
    """Whole runs on a scratch repository: its own run_tests.py and shard.py, two CI test files."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="plan-lanes-run-"))
        self.root = root = self.tmp / "repo"
        (root / "tools/mcp/tests").mkdir(parents=True)
        for name in ("run_tests.py", "shard.py"):
            shutil.copy(HERE.parent / "mcp" / name, root / "tools/mcp" / name)
        (root / "tools/mcp/requirements.txt").write_text("")
        (root / "tools/mcp/tests/test_a.py").write_text(TEST_FILE.format(extra=""))
        (root / "tools/mcp/tests/test_b.py").write_text(TEST_FILE.format(extra=""))
        (root / "tools/mcp/tests/test_slow.py").write_text(TEST_FILE.format(extra="").replace("CI = True", "CI_EXCLUDED = 'long-running'"))
        (root / "engine/core").mkdir(parents=True)
        (root / "engine/core/Foo.cpp").write_text("int foo;\n")
        (root / "docs/specs").mkdir(parents=True)
        (root / "docs/specs/S1.md").write_text("a\n")
        (root / ".gitignore").write_text("build/\n__pycache__/\n")
        sh(root, "git", "init", "-q", "-b", "main")
        sh(root, "git", "config", "user.email", "t@example.com")
        sh(root, "git", "config", "user.name", "t")
        sh(root, "git", "add", "-A")
        sh(root, "git", "commit", "-qm", "base")
        self.base = sh(root, "git", "rev-parse", "HEAD").strip()
        self.real_root = pl.cc.ROOT
        pl.cc.ROOT = root
        pl.LIST_WITH_UV = False
        self.shard = pl.load_module(HERE.parent / "mcp" / "shard.py", "test_plan_lanes_runs_shard")

    def tearDown(self):
        pl.cc.ROOT = self.real_root
        pl.LIST_WITH_UV = True
        shutil.rmtree(self.tmp, ignore_errors=True)

    def commit(self, rel: str, text: str) -> str:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-qm", rel)
        return sh(self.root, "git", "rev-parse", "HEAD").strip()

    def run_main(self, *argv) -> tuple[int, dict, str]:
        out = io.StringIO()
        with redirect_stdout(out):
            code = pl.main(list(argv))
        lanes = self.tmp / "out" / "lanes.json"
        return code, (json.loads(lanes.read_text()) if lanes.exists() else {}), out.getvalue()

    def test_docs_only_runs_nothing(self):
        head = self.commit("docs/specs/S1.md", "b\n")
        gh = self.tmp / "gh.txt"
        code, doc, out = self.run_main("--event", "pull_request", "--base", self.base, "--head", head,
                                       "--out-dir", str(self.tmp / "out"), "--github-output", str(gh))
        self.assertEqual(code, 0, out)
        self.assertEqual([n for n, e in doc["lanes"].items() if e["run"]], ["changes"])
        self.assertEqual((doc["head"], doc["base"], doc["files"]), (head, self.base, ["docs/specs/S1.md"]))
        self.assertIn("run_lint=false", gh.read_text().splitlines())
        self.assertIn("mcp_shards=[]", gh.read_text().splitlines())

    def test_a_test_change_plans_its_shard_from_the_listed_tests(self):
        head = self.commit("tools/mcp/tests/test_a.py", TEST_FILE.format(extra="# changed"))
        code, doc, out = self.run_main("--event", "pull_request", "--base", self.base, "--head", head,
                                       "--out-dir", str(self.tmp / "out"))
        self.assertEqual(code, 0, out)
        self.assertEqual((doc["mcp"]["mode"], doc["mcp"]["files"], doc["mcp"]["shards"]),
                         ("planned", ["tools/mcp/tests/test_a.py"], ["1"]), out)
        shard = json.loads((self.tmp / "out/mcp-plan/plan-1.json").read_text())
        # (the tests the file makes as it loads too: run_tests.py --list imports it)
        self.assertEqual(sorted(t for c in shard["chunks"] for t in c["tests"]),
                         ["test_made_0", "test_made_1", "test_one", "test_two"])
        self.assertTrue(doc["lanes"]["linux"]["run"] and doc["lanes"]["windows"]["run"])
        # the reasons, in lanes.json and as the job summary's Markdown
        self.assertEqual(doc["selection"]["mcp"]["files"], [{"file": "test_a.py", "kind": "owner", "why": "the test file changed"}])
        self.assertEqual(doc["selection"]["mcp"]["tests"], 4)
        self.assertIn("`test_a.py` (owner): the test file changed", (self.tmp / "out/selection.md").read_text())

    def plan_with_timings(self, event, committed: str, cache: dict | None = None):
        head = self.commit("tools/mcp/ci-timings.json", committed)
        args = ["--event", event, "--base", self.base, "--head", head, "--out-dir", str(self.tmp / "out")]
        if cache is not None:
            (self.tmp / "cache.json").write_text(json.dumps(cache))
            args += ["--test-seconds", str(self.tmp / "cache.json")]
        code, doc, out = self.run_main(*args)
        self.assertEqual(code, 0, out)
        plan = json.loads((self.tmp / "out/mcp-plan/plan.json").read_text())
        return doc, {c["file"]: c["seconds"] for s in plan["shards"].values() for c in s["chunks"]}

    COMMITTED = json.dumps({"format": 1, "default_test_seconds": 20.0, "runs": [], "tests": {},
                            "files": {"test_a.py": {"seconds": 1200.0, "tests": 4}}})

    def test_the_committed_timings_first_the_cache_for_the_rest(self):
        cache = {"test_a.py::test_one": 5.0, "test_b.py::test_one": 100.0}
        doc, seconds = self.plan_with_timings("full", self.COMMITTED, cache)
        overhead = self.shard.CHUNK_OVERHEAD
        self.assertEqual(seconds["test_a.py"], 1200.0 + overhead)                       # committed, not the cache's 5 s
        self.assertEqual(seconds["test_b.py"], 100.0 + 3 * 20.0 + overhead)              # the cache, else its default
        self.assertTrue(any("tools/mcp/ci-timings.json" in n for n in doc["notes"]), doc["notes"])

    def test_the_nightly_plans_by_its_own_seconds(self):
        # its shards build the games a PR's skip: the committed PR timings would misjudge them
        doc, seconds = self.plan_with_timings("nightly", self.COMMITTED, {"test_a.py::test_one": 500.0})
        self.assertEqual(seconds["test_a.py"], 500.0 + 3 * self.shard.DEFAULT_SECONDS + self.shard.CHUNK_OVERHEAD)

    def test_an_unreadable_committed_file_is_said_and_left_out(self):
        doc, seconds = self.plan_with_timings("full", "{", {"test_a.py::test_one": 5.0})
        self.assertEqual(seconds["test_a.py"], 5.0 + 3 * self.shard.DEFAULT_SECONDS + self.shard.CHUNK_OVERHEAD)
        self.assertTrue(any("tools/mcp/ci-timings.json" in f and "left out" in f for f in doc["fallbacks"]),
                        doc["fallbacks"])

    def test_an_older_head_is_planned_in_a_worktree_of_its_own(self):
        old = self.commit("engine/core/Foo.cpp", "int foo = 1;\n")
        self.commit("docs/specs/S1.md", "c\n")                      # HEAD is past it
        code, doc, out = self.run_main("--event", "pull_request", "--base", self.base, "--head", old,
                                       "--out-dir", str(self.tmp / "out"))
        self.assertEqual((code, doc["head"], doc["files"]), (0, old, ["engine/core/Foo.cpp"]), out)
        self.assertNotIn(str(self.tmp), sh(self.root, "git", "worktree", "list").split("\n", 1)[1:] or [""])
        self.assertEqual(len(sh(self.root, "git", "worktree", "list").splitlines()), 1)

    def test_nightly_runs_the_excluded_files_ci_can_run(self):
        code, doc, out = self.run_main("--event", "nightly", "--base", self.base, "--head", self.base,
                                       "--out-dir", str(self.tmp / "out"))
        self.assertEqual(code, 0, out)
        self.assertEqual(doc["mcp"]["files"], [f"tools/mcp/tests/test_{n}.py" for n in ("a", "b", "slow")])
        self.assertTrue(doc["full"])
        self.assertEqual(doc["mcp"]["shards"], ["1"])
        # what check_gate and the report expect a report from
        self.assertEqual(doc["unit_jobs"], ["core", "editor-1", "editor-2"])
        self.assertEqual(doc["asan_jobs"], ["core", "editor"])

    def test_fuller_runs_carry_the_pr_selection_as_their_shadow(self):
        head = self.commit("tools/mcp/tests/test_a.py", TEST_FILE.format(extra="# changed"))
        code, doc, out = self.run_main("--event", "merge_group", "--base", self.base, "--head", head,
                                       "--out-dir", str(self.tmp / "out"))
        self.assertEqual(code, 0, out)
        self.assertEqual((doc["shadow"]["files"], doc["shadow"]["mcp"]), (["tools/mcp/tests/test_a.py"], ["test_a.py"]))
        # the nightly has no diff of its own: the PR selection of the one since --shadow-base
        code, doc, out = self.run_main("--event", "nightly", "--base", head, "--head", head, "--shadow-base", self.base,
                                       "--out-dir", str(self.tmp / "out"))
        self.assertEqual(code, 0, out)
        self.assertEqual((doc["mcp"]["mode"], doc["shadow"]["mcp"], doc["shadow"]["base"]), ("nightly", ["test_a.py"], self.base))
        code, doc, out = self.run_main("--event", "nightly", "--base", head, "--head", head, "--out-dir", str(self.tmp / "out"))
        self.assertEqual(doc["shadow"], None)
        self.assertIn("--shadow-base", doc["shadow_why"])
        # a pull request runs its selection: nothing to shadow
        code, doc, out = self.run_main("--event", "pull_request", "--base", self.base, "--head", head, "--out-dir", str(self.tmp / "out"))
        self.assertEqual(doc["shadow"], None)

    def test_no_plan_fails_open_and_an_unlistable_suite_fails_the_lane(self):
        head = self.commit("engine/core/Foo.cpp", "int foo = 2;\n")
        (self.root / "tools/mcp/tests/test_b.py").write_text("CI = True\nraise ImportError('broken')\n")
        code, doc, out = self.run_main("--event", "pull_request", "--base", "no-such-base", "--head", head,
                                       "--out-dir", str(self.tmp / "out"))
        self.assertEqual(code, 0, out)
        self.assertTrue(all(e["run"] for e in doc["lanes"].values()))
        self.assertTrue(any("check_changed.py --plan-json exited 2" in f for f in doc["fallbacks"]), doc["fallbacks"])
        # the suite cannot be listed: one shard with no plan file, which tools/ci/mcp_shard.sh fails
        self.assertEqual(doc["mcp"]["shards"], ["1"])
        self.assertFalse((self.tmp / "out/mcp-plan/plan-1.json").exists())
        self.assertTrue(any("could not be planned" in f for f in doc["fallbacks"]), doc["fallbacks"])

    def test_a_workflow_change_runs_asan_only_in_its_own_job_or_env(self):
        workflow = ("name: CI\nenv:\n  JASPERENGINE_PCH: \"OFF\"\n\njobs:\n  unit:\n    runs-on: ubuntu-latest\n"
                    "    steps:\n      - run: echo unit\n\n  # one job per test binary\n  asan:\n    runs-on: ubuntu-latest\n"
                    "    steps:\n      - uses: ./.github/actions/ccache-restore\n        with:\n          max-size: 1.7G\n\n"
                    "  # the HTML UI lane\n  htmlui:\n    runs-on: ubuntu-latest\n")
        base = self.commit(pl.WORKFLOW, workflow)
        self.assertEqual(pl.asan_blocks(workflow), [(12, 17), (2, 3)])
        for old, new, runs_asan in (("echo unit", "echo unit2", False), ("# the HTML UI lane", "# HTML UI", False),
                                    ("# one job per test binary", "# a job per test binary", False),
                                    ("max-size: 1.7G", "max-size: 2.2G", True), ('"OFF"', '"ON"', True)):
            head = self.commit(pl.WORKFLOW, workflow.replace(old, new))
            gh = self.tmp / "gh.txt"
            gh.write_text("")
            code, doc, out = self.run_main("--event", "pull_request", "--base", base, "--head", head,
                                           "--out-dir", str(self.tmp / "out"), "--github-output", str(gh))
            self.assertEqual(code, 0, out)
            self.assertEqual(doc["lanes"]["asan"]["run"], runs_asan, (old, doc["lanes"]["asan"]["why"]))
            self.assertTrue(doc["lanes"]["unit"]["run"], old)
            lines = gh.read_text().splitlines()
            self.assertIn('asan_jobs=[{"job":"core","suite":"core"},{"job":"editor","suite":"editor"}]' if runs_asan
                          else "asan_jobs=[]", lines)
            if runs_asan:
                self.assertIn("asan_filter_editor=", lines)
                self.assertIn("the asan job or the workflow's env", doc["lanes"]["asan"]["why"])
            base = head
        # a workflow without the job cannot be read for it: the lane runs (fail open)
        head = self.commit(pl.WORKFLOW, "name: CI\njobs:\n  unit:\n    runs-on: ubuntu-latest\n")
        self.assertIn("no asan job", pl.asan_definition_change(self.root, base, [pl.WORKFLOW]))
        self.assertEqual(pl.asan_definition_change(self.root, base, ["engine/core/Foo.cpp"]), "")

    def test_the_queue_reads_the_advisory_lanes(self):
        head = self.commit("tools/mcp/tests/test_a.py", TEST_FILE.format(extra="# changed"))
        gh = self.tmp / "gh.txt"
        code, doc, out = self.run_main("--event", "merge_group", "--base", self.base, "--head", head,
                                       "--out-dir", str(self.tmp / "out"), "--github-output", str(gh),
                                       "--advisory", " ".join(ADVISORY))
        self.assertEqual(code, 0, out)
        self.assertEqual(doc["advisory_off"], ["unit", "asan", "htmlui", "mcp", "smoke", "windows-editor-tests"])
        self.assertEqual((doc["mcp"]["mode"], doc["mcp"]["shards"], doc["asan_jobs"]), ("none", [], []))
        lines = gh.read_text().splitlines()
        for line in ("run_unit=false", "run_asan=false", "run_mcp=false", "run_linux=true", "asan_jobs=[]", "mcp_shards=[]",
                     'build_only=["windows-editor-tests"]'):
            self.assertIn(line, lines)

    def test_a_push_steps_aside_for_a_newer_push_that_builds(self):
        head = self.commit("engine/core/Foo.cpp", "int foo = 3;\n")
        docs = self.commit("docs/specs/S1.md", "newer\n")
        out_dir = str(self.tmp / "out")
        # the newer push changed only what nothing reads: its run plans nothing, so this one builds
        code, doc, out = self.run_main("--event", "push", "--base", self.base, "--head", head, "--newest", docs,
                                       "--out-dir", out_dir)
        self.assertEqual((code, doc["superseded_by"]), (0, None), out)
        self.assertTrue(doc["lanes"]["linux"]["run"], out)
        # main is still at head, or the newer commit is unknown here: it builds
        for newest in (head, "f" * 40):
            code, doc, out = self.run_main("--event", "push", "--base", self.base, "--head", head, "--newest", newest,
                                           "--out-dir", out_dir)
            self.assertTrue(doc["lanes"]["linux"]["run"], (newest, out))
        newer = self.commit("engine/core/Bar.cpp", "int bar;\n")
        gh = self.tmp / "gh.txt"
        code, doc, out = self.run_main("--event", "push", "--base", self.base, "--head", head, "--newest", newer,
                                       "--out-dir", out_dir, "--github-output", str(gh))
        self.assertEqual((code, doc["superseded_by"]), (0, newer), out)
        self.assertEqual([n for n, e in doc["lanes"].items() if e["run"]], ["changes"])
        self.assertIn("engine/core/Bar.cpp", doc["lanes"]["linux"]["why"])
        self.assertIn("run_linux=false", gh.read_text().splitlines())

    def test_a_push_is_planned_against_the_last_push_run_that_finished(self):
        # a push run waiting for the one before it is replaced by a newer push's and never runs: the newer
        # one's diff (from github.event.before) would leave its change unbuilt
        built = self.commit("engine/core/Foo.cpp", "int foo = 4;\n")
        skipped = self.commit("engine/core/Bar.cpp", "int bar;\n")
        head = self.commit("docs/specs/S1.md", "head\n")
        runs_json = self.tmp / "runs.json"
        runs_json.write_text(json.dumps([
            {"databaseId": 30, "headSha": head, "status": "in_progress", "conclusion": "", "displayTitle": "docs",
             "createdAt": "2026-11-20T18:00:00Z"},
            {"databaseId": 29, "headSha": skipped, "status": "completed", "conclusion": "cancelled", "displayTitle": "bar",
             "createdAt": "2026-11-20T17:00:00Z"},
            {"databaseId": 28, "headSha": built, "status": "completed", "conclusion": "failure", "displayTitle": "foo",
             "createdAt": "2026-11-20T16:00:00Z"}]))
        code, doc, out = self.run_main("--event", "push", "--base", skipped, "--head", head, "--out-dir", str(self.tmp / "out"),
                                       "--previous-runs", str(runs_json), "--run-id", "30")
        self.assertEqual((code, doc["base"]), (0, built), out)
        self.assertEqual(doc["files"], ["docs/specs/S1.md", "engine/core/Bar.cpp"])
        self.assertTrue(doc["lanes"]["linux"]["run"], out)
        self.assertTrue(any("run 28" in n for n in doc["notes"]), doc["notes"])
        # no earlier run finished, or the list is unreadable: the push's own diff
        for text in ("[]", "not json"):
            runs_json.write_text(text)
            code, doc, out = self.run_main("--event", "push", "--base", skipped, "--head", head,
                                           "--out-dir", str(self.tmp / "out"), "--previous-runs", str(runs_json), "--run-id", "30")
            self.assertEqual((code, doc["base"], doc["files"]), (0, skipped, ["docs/specs/S1.md"]), (text, out))

    def test_the_advisory_run_is_planned_against_the_last_advisory_run_that_finished(self):
        tested = self.commit("engine/core/Foo.cpp", "int foo = 5;\n")
        head = self.commit("tools/mcp/tests/test_a.py", TEST_FILE.format(extra="# changed"))
        title = pl.ADVISORY_TITLE + "main at "
        runs_json = self.tmp / "runs.json"

        def write(*entries):
            runs_json.write_text(json.dumps([{"databaseId": i, "headSha": sha, "status": status, "conclusion": conclusion,
                                              "displayTitle": t, "createdAt": f"2026-11-20T{i:02d}:00:00Z"}
                                             for i, sha, status, conclusion, t in entries]))

        def run_advisory():
            return self.run_main("--event", "advisory", "--base", head, "--head", head, "--out-dir", str(self.tmp / "out"),
                                 "--advisory", " ".join(ADVISORY), "--previous-runs", str(runs_json), "--run-id", "20")
        # (a full dispatch is no advisory run; a cancelled one never ran or stopped)
        write((20, head, "in_progress", "", title + head), (19, self.base, "completed", "success", "CI"),
              (18, self.base, "completed", "cancelled", title + self.base), (17, tested, "completed", "failure", title + tested))
        code, doc, out = run_advisory()
        self.assertEqual((code, doc["event"], doc["base"], doc["head"]), (0, "advisory", tested, head), out)
        self.assertEqual(doc["files"], ["tools/mcp/tests/test_a.py"])
        self.assertEqual((doc["mcp"]["files"], doc["mcp"]["shards"]), (["tools/mcp/tests/test_a.py"], ["1"]), out)
        self.assertTrue(any("run 17" in n for n in doc["notes"]), doc["notes"])
        # it runs the advisory tests the queue left out, unfiltered: judged against a PR's selection of its diff
        self.assertEqual((doc["shadow"]["files"], doc["shadow"]["mcp"]), (["tools/mcp/tests/test_a.py"], ["test_a.py"]))
        # main is still at the last tested head: nothing runs
        write((20, head, "in_progress", "", title + head), (17, head, "completed", "success", title + head))
        code, doc, out = run_advisory()
        self.assertEqual([n for n, e in doc["lanes"].items() if e["run"]], ["changes"], out)
        # no advisory run finished yet: every advisory lane (fail open)
        write((20, head, "in_progress", "", title + head))
        code, doc, out = run_advisory()
        self.assertTrue(all(doc["lanes"][n]["run"] for n in ("linux", "unit", "asan", "htmlui", "mcp", "smoke", "windows")), out)
        self.assertEqual(doc["mcp"]["mode"], "ci")
        self.assertTrue(any("no earlier advisory run" in f for f in doc["fallbacks"]), doc["fallbacks"])
        # (no base: --base is a placeholder, and the report must not say "the changes since" it)
        self.assertIsNone(doc["base"])

    def test_unusable_arguments_exit_2(self):
        with self.assertRaises(SystemExit) as e, redirect_stdout(io.StringIO()):
            sys.stderr, saved = io.StringIO(), sys.stderr
            try:
                pl.main(["--event", "tag", "--base", "a", "--head", "b", "--out-dir", str(self.tmp)])
            finally:
                sys.stderr = saved
        self.assertEqual(e.exception.code, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
