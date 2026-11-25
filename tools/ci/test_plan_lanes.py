"""Tests of tools/ci/plan_lanes.py (python3 -I tools/ci/test_plan_lanes.py; stdlib only, no build).

The lane rules on synthetic check-changed plans (docs only, a building change, unit and HTML UI
filters, smoke paths, MCP restricted to the CI suite, merge_group, push, full, nightly, .github and
no plan), the job outputs, the shard count, and whole runs on a scratch repository with its own
run_tests.py and shard.py.
"""

from __future__ import annotations

import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
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
        # (macos and ios: an Apple-only input, below)
        self.assertEqual(runs(r), {"changes", "lint", "mcp", *pl.PLATFORM_LANES} - {"macos", "ios"})
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
        # (ios: only for an Apple-only input, test_the_queue_builds_ios_only_for_an_apple_only_input)
        self.assertEqual(runs(r), set(pl.LANES) - {"ios"})
        self.assertEqual((r["unit_filter"], r["htmlui_filter"]), ({"core": "", "editor": ""}, ""))
        self.assertEqual(r["unit_run"], {"core": True, "editor": True})
        self.assertEqual(r["mcp"]["files"], ["tools/mcp/tests/test_b.py"])
        # no MCP file planned: the lane stays off, every other runs
        r = decide("merge_group", plan(["editor/App.cpp"]))
        self.assertEqual(runs(r), set(pl.LANES) - {"mcp", "ios"})

    def test_the_queue_builds_no_editor_tests_binary_on_windows(self):
        # their run is advisory in the queue, and the binary was a quarter of the editor leg's 110 min (run
        # 45291112468); a pull request, a push to main, the advisory run and full runs build it
        p = plan(["editor/App.cpp"], unit="all", mcp=["test_b.py"])
        r = decide("merge_group", p, advisory=ADVISORY)
        self.assertEqual(r["unbuilt"], ["windows-editor-tests"])
        self.assertTrue(r["lanes"]["windows"]["run"])
        for event, kw in (("pull_request", {}), ("pull_request", {"pr_advisory": False}), ("push", {}), ("advisory", {}),
                          ("full", {}), ("workflow_dispatch", {})):
            self.assertEqual(decide(event, p, advisory=ADVISORY, **kw)["unbuilt"], [], (event, kw))
        # a test run taken off ADVISORY_LANES blocks the queue: it is built and run there again
        r = decide("merge_group", p, advisory=("unit", "mcp"))
        self.assertEqual((r["unbuilt"], r["build_only"]), ([], []))
        # ci.yml's editor step reads it; windows.sh builds the same tree without the binary
        text = (HERE.parent.parent / pl.WORKFLOW).read_text()
        self.assertIn("EDITOR_TESTS_BUILD: ${{ contains(fromJSON(needs.changes.outputs.unbuilt || '[]'), 'windows-editor-tests') && '0' || '1' }}", text)
        self.assertIn("unbuilt: ${{ steps.plan.outputs.unbuilt }}", text)
        script = (HERE / "windows.sh").read_text()
        self.assertIn('elif [[ "${EDITOR_TESTS_BUILD:-1}" != 0 ]]; then\n        targets+=(jasperengine-editor-tests)', script)

    def test_merge_group_leaves_the_advisory_lanes_out(self):
        # an advisory lane cannot block a merge, so the queue does not wait hours for it (queue runs
        # of #124-#126: 217-276 min, ASan the longest, the slowest blocking job 96-137)
        p = plan(["editor/App.cpp"], unit={"core": ["Foo"], "editor": [], "htmlui": []}, mcp=["test_b.py"])
        r = decide("merge_group", p, advisory=ADVISORY)
        self.assertEqual(runs(r), {"changes", "lint", *pl.PLATFORM_LANES} - {"ios"})
        self.assertEqual(r["advisory_off"], ["unit", "asan", "htmlui", "mcp", "smoke", "windows-editor-tests"])
        # the windows editor leg still builds, without its advisory test run
        self.assertEqual(r["build_only"], ["windows-editor-tests"])
        for name in ("unit", "asan", "htmlui", "mcp", "smoke"):
            self.assertIn("ADVISORY_LANES", r["lanes"][name]["why"], name)
        self.assertEqual(r["mcp"], {"mode": "none", "files": [], "not_in_ci": []})
        # it follows the list: a lane taken off it runs in the queue again
        r = decide("merge_group", p, advisory=("unit", "htmlui", "mcp", "smoke"))
        self.assertEqual(runs(r), set(pl.LANES) - {"unit", "htmlui", "mcp", "smoke", "ios"})
        self.assertEqual((r["advisory_off"], r["build_only"]), (["unit", "htmlui", "mcp", "smoke"], []))
        self.assertEqual(pl.asan_job_names(pl.planned_asan_run(r["lanes"], r["asan_run"])), ["core", "editor"])
        self.assertEqual(decide("merge_group", p, advisory=()), decide("merge_group", p))
        self.assertEqual(runs(decide("merge_group", p)), set(pl.LANES) - {"ios"})
        # only the lanes the push to main runs instead: lint and the platform builds stay in the queue
        r = decide("merge_group", p, advisory=("lint", "linux", "windows", "mcp"))
        self.assertEqual(runs(r), set(pl.LANES) - {"mcp", "ios"})
        self.assertEqual(r["advisory_off"], ["mcp"])
        # however the queue widened: a .github change (no Apple-only input: no ios), no plan (fail open: ios too)
        for q, ios in ((plan([".github/workflows/ci.yml"], builds=()), False), (None, True)):
            r = decide("merge_group", q, "exit 2", advisory=ADVISORY)
            self.assertEqual(runs(r), {"changes", "lint", *pl.PLATFORM_LANES} - ({"ios"} if not ios else set()), q)
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
        # CI itself: every lane but ASan, which a workflow change outside its own job does not reach, and the
        # Apple lanes, which a ci.yml change alone does not (the merge queue builds them)
        r = decide("pull_request", plan([".github/workflows/ci.yml"], builds=()))
        self.assertEqual(runs(r), set(pl.LANES) - {"asan", "macos", "ios"})
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
                                "asan_filter_editor", "htmlui_filter", "mcp_shards", "windows_parts",
                                "build_only", "unbuilt", "macos_full", "dispatch_advisory", "runs_on"])
        self.assertIn("dispatch_advisory=false", lines)
        self.assertIn('unbuilt=["windows-editor-tests"]',
                      pl.github_output(dict(doc, unbuilt=["windows-editor-tests"]), r["unit_run"]).splitlines())
        self.assertIn('runs_on={"macos":"je-mac"}', pl.github_output(dict(doc, runs_on={"macos": "je-mac"}), r["unit_run"]).splitlines())
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
        # the windows matrix (fromJSON in ci.yml: a list even when the lane does not run)
        self.assertIn("windows_parts=[]", pl.github_output(doc, r["unit_run"]).splitlines())
        self.assertIn('windows_parts=["editor","engine-glcore"]',
                      pl.github_output(dict(doc, windows_parts=["editor", "engine-glcore"]), r["unit_run"]).splitlines())
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

    def test_a_pr_builds_macos_and_ios_only_for_an_apple_only_input(self):
        # macOS runners are the scarce ones, and the merge queue builds both before anything reaches main
        for f in ("engine/core/Foo.cpp", "editor/App.cpp", "engine/platform/linux/Window.cpp", "engine/shaderlib/lit.glsl",
                  "engine/CMakeLists.txt", "tools/ci/windows.sh", "docs/testing.md", ".github/actions/image/image.sh",
                  ".github/workflows/ci.yml"):
            for event in ("pull_request", "workflow_dispatch"):
                r = decide(event, plan([f], unit="all"))
                for name in pl.APPLE_LANES:
                    self.assertFalse(r["lanes"][name]["run"], (f, event, name))
                    self.assertIn("merge queue", r["lanes"][name]["why"])
                self.assertTrue(r["lanes"]["linux"]["run"] and r["lanes"]["windows"]["run"], f)
        for f in ("engine/platform/apple/Renderer.mm", "engine/platform/apple/ios/Foo.mm", "engine/platform/mac/MacViewMetal.mm",
                  "engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj", "engine/workspaces/xcode/ios/Info.plist",
                  "tools/xcode_sync_sources.py", "tools/test_xcode_sync_sources.py", "engine/shaders/msl21ios.h",
                  "tools/ci/macos.sh", "tools/ci/platform-common.sh", ".github/actions/report/action.yml"):
            r = decide("pull_request", plan([f, "editor/App.cpp"]))
            for name in pl.APPLE_LANES:
                self.assertTrue(r["lanes"][name]["run"], (f, name))
                self.assertIn(f, r["lanes"][name]["why"])
        # the merge queue builds macos for any change that builds (ios: below); a full run, the nightly and
        # the rollback both; a push macos (its cache)
        self.assertTrue(decide("merge_group", plan(["editor/App.cpp"]))["lanes"]["macos"]["run"])
        for name in pl.APPLE_LANES:
            self.assertTrue(decide("full", None)["lanes"][name]["run"], name)
            self.assertTrue(decide("nightly", None)["lanes"][name]["run"], name)
            for files in (["editor/App.cpp"], [".github/workflows/ci.yml"]):
                self.assertTrue(decide("pull_request", plan(files), full_selection=True)["lanes"][name]["run"], (files, name))
        self.assertTrue(decide("push", plan(["editor/App.cpp"]))["lanes"]["macos"]["run"])

    def test_the_queue_builds_macos_only_for_a_macos_only_input(self):
        # macOS caps the queue (5 hosted runners and je-mac, a macos job a group)
        for mac in ("", None):
            r = decide("merge_group", plan(["editor/App.cpp"], unit="all"), macos_inputs=mac)
            self.assertEqual(r["lanes"]["macos"]["run"], mac is None, mac)           # (None: not read: it runs)
            self.assertEqual("macos" in r["queue_off"], mac == "")
        for f in ("editor/util/ClipboardMac.mm", "engine/platform/mac/MacViewMetal.mm", "editor/platform/MacBundle.cpp",
                  "editor/util/ApplePrivacy.cpp", "engine/libs/sokol/sokol_gfx.h", "editor/resources/macos/Info.plist",
                  "engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj", "tools/ci/macos.sh",
                  ".github/actions/ccache-save/trim.sh"):
            r = decide("merge_group", plan([f, "editor/App.cpp"]), macos_inputs="")
            self.assertTrue(r["lanes"]["macos"]["run"], f)
            self.assertNotIn("macos", r["queue_off"])
        for f in ("editor/StateMachine.cpp", "engine/core/Foo.cpp", "editor/App.cpp"):
            self.assertFalse(decide("merge_group", plan([f]), macos_inputs="")["lanes"]["macos"]["run"], f)
        r = decide("merge_group", plan(["engine/core/Engine.cpp"]), macos_inputs="engine/core/Engine.cpp (it names __APPLE__)")
        self.assertIn("__APPLE__", r["lanes"]["macos"]["why"])
        # a push: the whole lane when the queue left it out; its build only when the queue ran it
        self.assertTrue(decide("push", plan(["editor/App.cpp"]), macos_inputs="")["macos_full"])
        self.assertFalse(decide("push", plan(["editor/Mac.mm"]), macos_inputs="")["macos_full"])
        self.assertFalse(decide("push", plan(["editor/App.cpp"]), macos_inputs="editor/App.cpp (it names __APPLE__)")["macos_full"])
        # a pull request keeps its own rule, full runs and the nightly build it
        self.assertFalse(decide("pull_request", plan(["engine/core/Engine.cpp"]), macos_inputs="x")["lanes"]["macos"]["run"])
        for event in ("full", "nightly"):
            self.assertTrue(decide(event, None)["lanes"]["macos"]["run"])
        # ci.yml runs the push's tests and the call CLI then (its Compile build on every push: the macos
        # ccache entry holds its objects)
        text = (HERE.parent.parent / pl.WORKFLOW).read_text()
        self.assertEqual(text.count("needs.changes.outputs.macos_full == 'true'"), 2)

    def test_the_macos_inputs_are_read_from_the_change(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            subprocess.run(["git", "init", "-q", "-b", "main", str(root)], check=True)
            def commit(path, text):
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).write_text(text)
                subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
                subprocess.run(["git", "-C", str(root), "-c", "user.email=a@b", "-c", "user.name=a", "commit", "-qm", path], check=True)
                return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
            cm = "cmake_minimum_required(VERSION 3.20)\n" + "".join(f"set(A{i} {i})\n" for i in range(40)) + \
                 "if(APPLE)\n  set(MAC 1)\nendif()\n" + "".join(f"set(B{i} {i})\n" for i in range(40))
            base = commit("CMakeLists.txt", cm)
            commit("engine/core/Plain.cpp", "int a;\n")
            commit("engine/core/Thread.cpp", "#if defined(__APPLE__)\nint b;\n#endif\n")
            self.assertEqual(pl.macos_inputs(root, base, ["engine/core/Plain.cpp"]), "")
            self.assertIn("names __APPLE__", pl.macos_inputs(root, base, ["engine/core/Thread.cpp"]))
            # a CMake change near an Apple setting counts, one far from it does not
            far = commit("CMakeLists.txt", cm.replace("set(A2 2)", "set(A2 3)"))
            self.assertEqual(pl.macos_inputs(root, base, ["CMakeLists.txt"]), "")
            near = commit("CMakeLists.txt", cm.replace("set(MAC 1)", "set(MAC 2)"))
            self.assertIn("a change near APPLE", pl.macos_inputs(root, far, ["CMakeLists.txt"]))

    def test_the_queue_builds_ios_only_for_an_apple_only_input(self):
        # a compile check with the Apple compiler the queue's macos lane runs on every change; macOS runners
        # are the bottleneck, and the nightly builds the Xcode project for iOS every night
        for files in (["editor/App.cpp"], ["engine/core/Foo.cpp", "engine/shaderlib/lit.glsl"], [".github/workflows/ci.yml"]):
            r = decide("merge_group", plan(files, unit="all"))
            self.assertFalse(r["lanes"]["ios"]["run"], files)
            self.assertIn("nightly", r["lanes"]["ios"]["why"])
            self.assertEqual(r["queue_off"], ["ios"])
            self.assertTrue(r["lanes"]["macos"]["run"], files)
        for f in ("engine/platform/apple/Renderer.mm", "engine/workspaces/xcode/ios/Info.plist", "tools/ci/macos.sh"):
            r = decide("merge_group", plan([f, "editor/App.cpp"]))
            self.assertTrue(r["lanes"]["ios"]["run"] and r["lanes"]["macos"]["run"], f)
            self.assertEqual(r["queue_off"], [])
        # push keeps no ios (no cache); full and nightly build it; the rollback runs everything
        self.assertFalse(decide("push", plan(["editor/App.cpp"]))["lanes"]["ios"]["run"])
        for event in ("full", "nightly"):
            self.assertTrue(decide(event, None)["lanes"]["ios"]["run"], event)
        r = decide("merge_group", plan(["editor/App.cpp"]), full_selection=True)
        self.assertTrue(r["lanes"]["ios"]["run"])
        self.assertEqual(r["queue_off"], [])
        # nightly.yml builds the Xcode project for iOS every night, whatever changed
        nightly = (HERE.parent.parent / ".github/workflows/nightly.yml").read_text()
        start = nightly.index("\n  ios:\n")
        end = re.search(r"\n  [A-Za-z0-9_-]+:\n", nightly[start + 1:])
        job = nightly[start:start + 1 + end.start()] if end else nightly[start:]
        self.assertIn("run: tools/ci/macos.sh xcode ios", job)
        self.assertNotIn("if:", job.split("steps:")[0])

    def test_a_pr_runs_its_blocking_lanes_only_while_the_variable_is_off(self):
        # PR_ADVISORY_LANES=off while GitHub's 60 concurrent jobs are the bottleneck: as the merge queue,
        # the advisory test lanes are left to main's advisory run
        p = plan(["engine/core/Foo.cpp", "engine/platform/apple/Renderer.mm"], unit="all", mcp=["test_a.py"])
        on = decide("pull_request", p, advisory=ADVISORY)
        self.assertTrue(all(on["lanes"][n]["run"] for n in ("unit", "asan", "htmlui", "mcp", "smoke")))
        off = decide("pull_request", p, advisory=ADVISORY, pr_advisory=False)
        self.assertEqual(runs(off), {"changes", "lint", *pl.PLATFORM_LANES})
        self.assertEqual(off["advisory_off"], ["unit", "asan", "htmlui", "mcp", "smoke", "windows-editor-tests"])
        self.assertEqual(off["build_only"], ["windows-editor-tests"])
        self.assertIn(pl.PR_ADVISORY_VARIABLE, off["lanes"]["unit"]["why"])
        self.assertEqual(off["mcp"]["mode"], "none")
        # only a pull request: the queue, a push, the advisory run and full runs as before
        for event in ("merge_group", "push", "advisory", "full"):
            self.assertEqual(decide(event, p, advisory=ADVISORY, pr_advisory=False), decide(event, p, advisory=ADVISORY), event)
        # a lane taken off ADVISORY_LANES blocks, so it runs
        self.assertTrue(decide("pull_request", p, advisory=("mcp",), pr_advisory=False)["lanes"]["unit"]["run"])

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

    def test_the_windows_backend_legs_build_only_for_their_inputs(self):
        # engine-d3d11 and engine-vulkan (60-97 min each) set the merge queue's floor, and only catch code that
        # compiles for their backend alone: a PR, a dispatch, the queue and the advisory run build them for
        # their own inputs; the nightly builds both every night
        both, glcore = list(pl.WINDOWS_PARTS), ["editor", "engine-glcore"]
        tmp = Path(tempfile.mkdtemp(prefix="plan-lanes-windows-"))
        self.addCleanup(shutil.rmtree, tmp, True)
        texts = {"engine/core/Guarded.cpp": "#if defined(SOKOL_GLCORE)\nint a;\n#else\nint b;\n#endif\n",
                 "engine/core/render/Readback.cpp": "ID3D11Texture2D* staging;\n",
                 "engine/core/texture/Formats.h": "VkFormat format = VK_FORMAT_R8_UNORM;\n",
                 "engine/tests/support/Context.cpp": "// the lavapipe Vulkan device\n",
                 "editor/procmat/Device.cpp": "#ifdef SOKOL_D3D11\n#endif\n",
                 "engine/core/Generic.cpp": "int generic; // sg_make_buffer\n",
                 "engine/platform/mac/View.mm": "#if defined(SOKOL_METAL)\n#endif\n",
                 "engine/platform/linux/Window.cpp": "#if defined(SOKOL_VULKAN)\n#endif\n",
                 "editor/backend/BackendWin.cpp": "ID3D11Device* device;\n",
                 "engine/core/Notes.md": "D3D11 and Vulkan\n",
                 "engine/CMakeLists.txt": "if(WIN32)\n  add_definitions(-DSOKOL_D3D11)\nendif()\n",
                 "engine/tests/core/CMakeLists.txt": "add_executable(core-tests a.cpp)\n"}
        for rel, text in texts.items():
            (tmp / rel).parent.mkdir(parents=True, exist_ok=True)
            (tmp / rel).write_text(text)

        def legs(event, files, definition="", **extra):
            p = plan(files, unit="all") if files is not None else None
            # (the advisory run: for windows-editor-tests)
            r = decide(event, p, "exit 2", advisory=ADVISORY)
            return pl.windows_legs(event, p, r, pl.windows_inputs(tmp, files or [], definition), **extra), r

        for event in pl.WINDOWS_SCOPED:
            for files in (["editor/App.cpp"], ["engine/core/Generic.cpp", "engine/core/Gone.cpp"], ["engine/platform/mac/View.mm"],
                          ["engine/platform/linux/Window.cpp"], ["editor/backend/BackendWin.cpp"], ["engine/core/Notes.md"],
                          ["engine/core/render/LightingTiers.cpp", "engine/core/ecs/World.cpp", "tools/ci/macos.sh"],
                          [".github/workflows/nightly.yml"], ["engine/tests/core/CMakeLists.txt"]):
                parts, r = legs(event, files)
                self.assertEqual(parts, glcore, (event, files))
                self.assertIn("not engine-d3d11 or engine-vulkan", r["lanes"]["windows"]["why"])
                self.assertIn("nightly", r["lanes"]["windows"]["why"])
        # their inputs: every backend's, then each one's own
        for files, expect in ((["engine/renders/sokol/SokolSystem.cpp"], both), (["engine/libs/sokol/sokol_gfx.h"], both),
                              (["engine/libs/shadercompiler/libs/spirv-cross/spirv_hlsl.cpp"], both),
                              (["engine/shaderlib/lit.frag"], both), (["engine/shaders/hlsl5.h"], both),
                              (["engine/core/shader/ShaderVariantCompiler.cpp"], both), (["engine/core/pool/ShaderPool.cpp"], both),
                              (["engine/platform/win/D3D11Context.cpp"], both), (["engine/CMakePresets.json"], both),
                              # (a CMake file without a diff: every line of it)
                              (["engine/CMakeLists.txt"], both),
                              (["engine/cmake/JasperWarnings.cmake"], both), (["tools/ci/windows.sh"], both),
                              (["tools/ci/platform-common.sh"], both), ([".github/actions/report/action.yml"], both),
                              (["engine/platform/common/VulkanContext.cpp"], [*glcore, "engine-vulkan"]),
                              # what a changed file's code says
                              (["engine/core/Guarded.cpp"], both), (["editor/procmat/Device.cpp"], both),
                              (["engine/core/render/Readback.cpp"], [*glcore, "engine-d3d11"]),
                              (["engine/core/texture/Formats.h", "editor/App.cpp"], [*glcore, "engine-vulkan"]),
                              (["engine/tests/support/Context.cpp"], [*glcore, "engine-vulkan"])):
            for event in ("pull_request", "merge_group"):
                parts, r = legs(event, files)
                self.assertEqual(parts, expect, (event, files))
                self.assertIn(files[0], r["lanes"]["windows"]["why"])
        # the windows job or the workflow's env (definition_change)
        parts, r = legs("merge_group", [pl.WORKFLOW], f"{pl.WORKFLOW} (the windows job or the workflow's env)")
        self.assertEqual(parts, both)
        # every leg: a push (the Vulkan SDK's cache), a full run, the nightly, no plan, the rollback
        for event, files in (("push", ["editor/App.cpp"]), ("full", None), ("nightly", None), ("pull_request", None),
                             ("merge_group", None)):
            self.assertEqual(legs(event, files)[0], both, (event, files))
        self.assertEqual(legs("merge_group", ["editor/App.cpp"], full_selection=True)[0], both)
        # a dispatch that forces a leg onto a runner builds it
        self.assertEqual(legs("workflow_dispatch", ["editor/App.cpp"], forced={"windows-engine-d3d11": "je-win"})[0],
                         [*glcore, "engine-d3d11"])
        # no lane, no legs
        self.assertEqual(legs("pull_request", ["docs/specs/S1.md"])[0], [])

    def test_the_backend_markers_find_the_real_backend_code(self):
        # this tree's backend-specific files are found by their text, and a generic one is not
        root = HERE.parent.parent
        for f, legs in (("engine/core/Engine.cpp", ("engine-d3d11", "engine-vulkan")),
                        ("engine/core/render/FrameReadback.cpp", ("engine-d3d11", "engine-vulkan")),
                        ("engine/core/render/VulkanRobustness.h", ("engine-vulkan",)),
                        ("engine/core/render/FramebufferRender.cpp", ("engine-d3d11",)),
                        ("engine/tests/support/HeadlessContext.cpp", ("engine-d3d11", "engine-vulkan")),
                        ("editor/procmat/ProceduralGpuEvaluator.cpp", ("engine-vulkan",)),
                        ("engine/core/component/Transform.h", ())):
            self.assertTrue((root / f).is_file(), f)
            found = pl.windows_inputs(root, [f])
            self.assertEqual(tuple(leg for leg in pl.WINDOWS_BACKEND_LEGS if found[leg]), legs, (f, found))

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


NOW = 1_800_000_000.0
REPO = "Arakuma-Studio/JasperEngine"


def state(age=30, saturated=("macos", "linux", "windows"), **runners):
    """A runner state (state.json of plan_lanes.STATE_BRANCH): every overflow runner online and idle unless given."""
    r = {label: {"online": True, "busy": False, "fit": True} for label in pl.OVERFLOW}
    for label, entry in runners.items():
        r[label.replace("_", "-")].update(entry)
    return {"t": NOW - age, "os": {o: {"saturated": o in saturated, "running": 5, "queued": 3} for o in ("macos", "linux", "windows")},
            "runners": r}


JOBS = {"lint": "linux", "linux": "linux", "unit-core": "linux", "unit-editor-1": "linux", "asan-editor": "linux",
        "mcp-1": "linux", "windows-editor": "windows", "windows-engine-glcore": "windows", "windows-engine-d3d11": "windows",
        "windows-engine-vulkan": "windows", "windows-compile": "windows", "macos": "macos", "ios": "macos"}


def route(st, trigger="pull_request", head=REPO, attempt=1, disabled="", force=None, jobs=JOBS, run_id="42"):
    return pl.route(jobs, st, NOW, trigger, head, REPO, attempt, disabled, force, run_id=run_id)


def overflowed(runs_on):
    return {job: label for job, label in runs_on.items() if label not in pl.HOSTED.values()}


class Routing(unittest.TestCase):
    def test_hosted_first_an_idle_runner_only_when_hosted_is_saturated(self):
        runs_on, notes = route(state(saturated=()))
        self.assertEqual(runs_on, {job: pl.HOSTED[os_] for job, os_ in JOBS.items()})
        self.assertTrue(any("je-mac: hosted macos runners not saturated" in n for n in notes), notes)
        # saturated: each runner its best job (the Mac both macOS jobs: TAKES)
        runs_on, notes = route(state())
        self.assertEqual(overflowed(runs_on), {"macos": "je-mac", "ios": "je-mac", "linux": "je-linux",
                                               "windows-engine-d3d11": "je-win"})
        self.assertEqual((runs_on["windows-editor"], runs_on["mcp-1"]), ("windows-latest", "ubuntu-latest"))
        # only the saturated OS
        self.assertEqual(overflowed(route(state(saturated=("macos",)))[0]), {"macos": "je-mac", "ios": "je-mac"})
        # the next job of its list when the best one is not planned
        self.assertEqual(overflowed(route(state(saturated=("macos",)), jobs={"ios": "macos", "linux": "linux"})[0]), {"ios": "je-mac"})
        self.assertEqual(overflowed(route(state(saturated=("linux",)), jobs={"lint": "linux", "web": "linux"})[0]), {"web": "je-linux"})

    def test_the_merge_queue_takes_the_mac_as_soon_as_a_hosted_macos_job_waits(self):
        # the queue is the critical path: no need for all five hosted macOS runners busy
        st = state(saturated=())
        st["os"]["macos"].update(running=2, queued=1)
        self.assertEqual(overflowed(route(st, trigger="merge_group")[0]), {"macos": "je-mac", "ios": "je-mac"})
        # a pull request keeps the stricter rule; other OSes too
        self.assertEqual(overflowed(route(st, trigger="pull_request")[0]), {})
        st["os"]["linux"].update(running=2, queued=4)
        self.assertNotIn("je-linux", overflowed(route(st, trigger="merge_group")[0]).values())
        # nothing waits: hosted
        st["os"]["macos"].update(queued=0)
        self.assertEqual(overflowed(route(st, trigger="merge_group")[0]), {})
        # a runner never takes a job it cannot do: the Windows editor leg, an MCP shard
        self.assertEqual(overflowed(route(state(), jobs={"windows-editor": "windows", "mcp-1": "linux"})[0]), {})

    def test_an_offline_busy_unfit_or_disabled_runner_takes_nothing(self):
        for st, disabled, why in ((state(je_mac={"online": False}), "", "je-mac: offline"),
                                  (state(je_mac={"busy": True}), "", "je-mac: busy"),
                                  (state(je_mac={"fit": False, "why": "12 GB free"}), "", "je-mac: unfit (12 GB free)"),
                                  (state(), "je-mac", "je-mac: off (SELF_HOSTED_DISABLED)"),
                                  (state(), "je-win,je-mac", "je-mac: off")):
            runs_on, notes = route(st, disabled=disabled)
            self.assertEqual(runs_on["macos"], "macos-latest", why)
            self.assertTrue(any(why in n for n in notes), (why, notes))
        self.assertEqual(overflowed(route(state(), disabled="all")[0]), {})
        self.assertTrue(route(state(je_mac={"busy": True}))[0]["linux"] == "je-linux")

    def test_nothing_paid_github_first_then_the_users_machines(self):
        # (the Blacksmith tier was removed: a deep backlog with every machine busy still waits for GitHub)
        st = state(je_mac={"busy": True}, je_linux={"busy": True}, je_win={"busy": True})
        for o in st["os"].values():
            o.update(queued=500, running=60)
        for trigger in ("merge_group", "push", "pull_request", "workflow_dispatch"):
            runs_on, notes = route(st, trigger=trigger)
            self.assertEqual(set(runs_on.values()) - set(pl.HOSTED.values()), set(), trigger)
        allowed = set(pl.HOSTED.values()) | set(pl.OVERFLOW)
        self.assertTrue(set(route(state(), trigger="merge_group")[0].values()) <= allowed)
        force, bad = pl.parse_force("macos=blacksmith-6vcpu-macos-26 linux=blacksmith-4vcpu-ubuntu-2404")
        self.assertEqual((force, len(bad)), ({}, 2))

    def test_a_stale_or_missing_state_means_no_overflow_runner(self):
        for st, why in ((None, "no runner state"), ({}, "no runner state"), ("garbage", "no runner state"),
                        (state(age=301), "301 s old"), (state(age=-3600), "-3600 s old")):
            runs_on, notes = route(st)
            self.assertEqual(overflowed(runs_on), {}, why)
            self.assertTrue(any(why in n for n in notes), (why, notes))
        self.assertEqual(overflowed(route(state(age=299))[0])["macos"], "je-mac")

    def test_never_a_forks_pull_request(self):
        for trigger, head in (("pull_request", "someone/JasperEngine"), ("pull_request", ""), ("pull_request_target", REPO),
                              ("schedule", REPO), ("workflow_run", REPO), ("", REPO)):
            runs_on, notes = route(state(), trigger=trigger, head=head)
            self.assertEqual(overflowed(runs_on), {}, (trigger, head))
            self.assertTrue(notes[0].startswith("hosted only:"), notes)
        for trigger in ("pull_request", "push", "merge_group", "workflow_dispatch"):
            self.assertEqual(overflowed(route(state(), trigger=trigger)[0])["macos"], "je-mac", trigger)
        # a fork cannot force one either
        self.assertEqual(overflowed(route(state(), head="someone/JasperEngine", force={"macos": "je-mac"})[0]), {})

    def test_a_rerun_after_a_runner_went_away_stays_hosted(self):
        # the watchdog reruns a run whose runner went away and lists it in the state's hosted_only: the
        # rerun must not wait for it again; a forced dispatch's rerun, or a rerun of an unknown run, neither
        held = dict(state(), hosted_only=["42", 7])
        for st, force, run_id in ((held, None, "42"), (held, None, "7"), (state(), {"macos": "je-mac"}, "42"), (state(), None, "")):
            runs_on, notes = route(st, trigger="workflow_dispatch", attempt=2, force=force, run_id=run_id)
            self.assertEqual(overflowed(runs_on), {}, (force, run_id))
            self.assertIn("attempt 2", notes[0])
        # its rerun to reach an idle runner (the watchdog's other reroute) routes as a first attempt does
        runs_on, notes = route(held, trigger="workflow_dispatch", attempt=2, run_id="43")
        self.assertEqual(overflowed(runs_on)["macos"], "je-mac")

    def test_a_dispatch_forces_a_runner_whatever_the_state(self):
        force, bad = pl.parse_force("macos=je-mac, lint=je-linux windows-engine-glcore=je-win ios=je-pi linux")
        self.assertEqual(force, {"macos": "je-mac", "lint": "je-linux", "windows-engine-glcore": "je-win"})
        self.assertEqual(len(bad), 2)
        for st in (None, state(saturated=(), je_mac={"online": False})):
            runs_on, notes = route(st, trigger="workflow_dispatch", force=force)
            self.assertEqual(overflowed(runs_on), force)
        # only a dispatch, only a job of the plan on a runner of its OS
        self.assertEqual(overflowed(route(None, trigger="push", force=force)[0]), {})
        runs_on, notes = route(None, trigger="workflow_dispatch", force={"macos": "je-win", "web": "je-linux"})
        self.assertEqual(overflowed(runs_on), {})
        self.assertEqual(len(notes), 2)

    def test_a_forced_dispatch_runs_only_the_forced_lanes(self):
        r = decide("workflow_dispatch", plan(["engine/core/Foo.cpp"], unit="all", mcp=["test_a.py"]))
        pl.restrict_to_forced(r, {"macos": "je-mac", "windows-engine-d3d11": "je-win"})
        # (macos too, though no Apple input changed: the dispatch tests the runner)
        self.assertEqual(runs(r), {"changes", "macos", "windows"})
        self.assertEqual((r["mcp"]["mode"], r["unit_run"], r["asan_run"]["core"]), ("none", {"core": False, "editor": False}, False))

    def test_the_planned_jobs_are_ci_yml_s_jobs(self):
        r = decide("merge_group", plan(["engine/core/Foo.cpp", "engine/platform/apple/Renderer.mm"], unit="all",
                                       mcp=["test_a.py"]))
        doc = dict(r, unit_jobs=pl.unit_job_names(r["unit_run"]), asan_jobs=pl.asan_job_names(r["asan_run"]),
                   mcp=dict(r["mcp"], shards=["1", "2"]))
        jobs = pl.planned_jobs(doc)
        self.assertEqual(set(jobs), {"lint", "linux", "htmlui", "smoke", "web", "android", "unit-core", "unit-editor-1",
                                     "unit-editor-2", "asan-core", "asan-editor", "mcp-1", "mcp-2", "windows-editor",
                                     "windows-engine-glcore", "windows-engine-d3d11", "windows-engine-vulkan",
                                     "windows-compile", "macos", "ios"})
        # the windows legs the plan builds (windows_parts), and windows-compile
        jobs_without = pl.planned_jobs(dict(doc, windows_parts=["editor", "engine-glcore"]))
        self.assertEqual(set(jobs) - set(jobs_without), {"windows-engine-d3d11", "windows-engine-vulkan"})
        self.assertIn("windows-compile", jobs_without)
        # every job an overflow runner may take is one of them, on its OS
        for label, (os_, order) in pl.OVERFLOW.items():
            for job in order:
                self.assertEqual(jobs.get(job), os_, (label, job))
        # and ci.yml reads runs_on by these names, with the hosted label as its fallback
        text = (HERE.parent.parent / pl.WORKFLOW).read_text()
        # the plan gets the PR switch and its run's id (the state's hosted_only)
        self.assertIn('--pr-advisory "${PR_ADVISORY:-on}"', text)
        self.assertIn("PR_ADVISORY: ${{ vars.PR_ADVISORY_LANES }}", text)
        self.assertIn('--run-attempt "$GITHUB_RUN_ATTEMPT" --run-id "$GITHUB_RUN_ID"', text)
        # the planner: never a push, a fork's pull request or a rerun
        changes = text[text.index("\n  changes:"):text.index("\n  lint:")]
        for guard in ("github.event_name != 'push'", "github.run_attempt == 1",
                      "github.event.pull_request.head.repo.full_name == github.repository", "|| 'ubuntu-latest'"):
            self.assertIn(guard, changes)
        for key in ("'linux'", "'macos'", "'ios'", "format('unit-{0}', matrix.job)", "format('asan-{0}', matrix.job)",
                    "format('mcp-{0}', matrix.shard)", "format('windows-{0}', matrix.part)", "'windows-compile'"):
            self.assertIn(f"fromJSON(needs.changes.outputs.runs_on)[{key}] ||", text, key)
        # the windows matrix is the plan's legs
        windows = text[text.index("\n  windows:"):text.index("\n  windows-compile:")]
        self.assertIn("part: ${{ fromJSON(needs.changes.outputs.windows_parts) }}", windows)
        self.assertIn("windows_parts: ${{ steps.plan.outputs.windows_parts }}", changes)
        # and the nightly builds both backend legs every night, as the lane's command
        nightly = (HERE.parent.parent / ".github/workflows/nightly.yml").read_text()
        job = nightly[nightly.index("\n  windows-engine:\n"):]
        job = job[:job.index("\n  # ---")]
        self.assertIn("backend: [d3d11, vulkan]", job)
        self.assertIn('run: tools/ci/windows.sh engine "$BACKEND"', job)
        self.assertNotIn("\n    if:", job)
        self.assertIn("windows-engine,", nightly[nightly.index("\n  report:"):])


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

    def test_the_runner_state_routes_the_jobs(self):
        head = self.commit("tools/mcp/tests/test_a.py", TEST_FILE.format(extra="# changed"))
        st = self.tmp / "state.json"
        gh = self.tmp / "gh.txt"
        common = ("--base", self.base, "--head", head, "--out-dir", str(self.tmp / "out"), "--github-output", str(gh),
                  "--runner-state", str(st), "--repo", REPO, "--head-repo", REPO)
        # the runner state as ci.yml fetches it, fresh: macOS saturated, je-mac idle
        st.write_text(json.dumps(dict(state(saturated=("macos",)), t=time.time() - 30)))
        # (the merge queue without a macOS-only input runs neither macOS lane)
        code, doc, out = self.run_main("--event", "merge_group", "--trigger", "merge_group", *common)
        self.assertEqual(code, 0, out)
        self.assertEqual(overflowed(doc["runs_on"]), {}, doc["routing"])
        self.assertEqual(doc["queue_off"], ["ios", "macos"])
        # with one, macos runs there, on the Mac
        head = self.commit("engine/platform/mac/Window.mm", "// mac\n")
        common = ("--base", self.base, "--head", head, *common[4:])
        code, doc, out = self.run_main("--event", "merge_group", "--trigger", "merge_group", *common)
        self.assertEqual(overflowed(doc["runs_on"]).get("macos"), "je-mac", (doc["lanes"]["macos"], doc["routing"]))
        self.assertEqual(doc["runs_on"]["mcp-1"], "ubuntu-latest")
        self.assertIn("runner: macos on je-mac", out)
        self.assertIn('"macos":"je-mac"', [l for l in gh.read_text().splitlines() if l.startswith("runs_on=")][-1])
        # an empty variable (ci.yml writes it as it is): hosted
        st.write_text("")
        code, doc, out = self.run_main("--event", "pull_request", "--trigger", "pull_request", *common)
        self.assertEqual(overflowed(doc["runs_on"]), {})
        # a dispatch that forces a runner runs only that job's lane, on it
        code, doc, out = self.run_main("--event", "workflow_dispatch", "--trigger", "workflow_dispatch",
                                       "--self-hosted", "lint=je-linux", *common)
        self.assertEqual([n for n, e in doc["lanes"].items() if e["run"]], ["changes", "lint"], out)
        self.assertEqual(doc["runs_on"], {"lint": "je-linux"})
        # its rerun (the watchdog's, when the runner went away) runs the same lane, hosted
        code, doc, out = self.run_main("--event", "workflow_dispatch", "--trigger", "workflow_dispatch",
                                       "--self-hosted", "lint=je-linux", "--run-attempt", "2", *common)
        self.assertEqual(doc["runs_on"], {"lint": "ubuntu-latest"})

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
        self.assertEqual(pl.job_blocks(workflow), [(12, 17), (2, 3)])
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
        self.assertIn("no asan job", pl.definition_change(self.root, base, [pl.WORKFLOW]))
        self.assertEqual(pl.definition_change(self.root, base, ["engine/core/Foo.cpp"]), "")

    def test_a_workflow_change_alone_builds_no_apple_lane_on_a_pr(self):
        # not even a change to their own jobs: the merge queue builds them before anything reaches main
        workflow = ("name: CI\nenv:\n  KEEP_GOING: 1\n\njobs:\n  macos:\n    runs-on: macos-latest\n"
                    "    steps:\n      - run: tools/ci/macos.sh editor\n\n  ios:\n    runs-on: macos-latest\n"
                    "    steps:\n      - run: tools/ci/macos.sh xcode ios\n\n  check:\n    runs-on: ubuntu-slim\n")
        base = self.commit(pl.WORKFLOW, workflow)
        for old, new in (("macos.sh editor", "macos.sh engine"), ("xcode ios", "xcode ios --x"), ("KEEP_GOING: 1", "KEEP_GOING: 0")):
            head = self.commit(pl.WORKFLOW, workflow.replace(old, new))
            code, doc, out = self.run_main("--event", "pull_request", "--base", base, "--head", head,
                                           "--out-dir", str(self.tmp / "out"))
            self.assertEqual(code, 0, out)
            for name in pl.APPLE_LANES:
                self.assertFalse(doc["lanes"][name]["run"], (old, name, doc["lanes"][name]["why"]))
            self.assertTrue(doc["lanes"]["linux"]["run"], old)
            base = head
        # the lanes' own script does
        head = self.commit("tools/ci/macos.sh", "#!/bin/sh\n")
        code, doc, out = self.run_main("--event", "pull_request", "--base", base, "--head", head, "--out-dir", str(self.tmp / "out"))
        self.assertTrue(doc["lanes"]["macos"]["run"] and doc["lanes"]["ios"]["run"], out)

    def test_the_windows_backend_legs_in_a_whole_run(self):
        workflow = ("name: CI\nenv:\n  KEEP_GOING: 1\n\njobs:\n  linux:\n    runs-on: ubuntu-latest\n"
                    "    steps:\n      - run: tools/build.sh linux\n\n  windows:\n    runs-on: windows-latest\n"
                    "    steps:\n      - run: tools/ci/windows.sh engine\n\n  windows-compile:\n    runs-on: windows-latest\n"
                    "    steps:\n      - run: tools/ci/windows.sh compile-smoke\n")
        base = self.commit(pl.WORKFLOW, workflow)
        gh = self.tmp / "gh.txt"
        for rel, text, legs in (("engine/core/Foo.cpp", "int foo2;\n", ["editor", "engine-glcore"]),
                                ("engine/core/Foo.cpp", "#ifdef SOKOL_VULKAN\nint foo3;\n#endif\n", list(pl.WINDOWS_PARTS)),
                                (pl.WORKFLOW, workflow.replace("compile-smoke", "compile-smoke x"), ["editor", "engine-glcore"]),
                                (pl.WORKFLOW, workflow.replace("windows.sh engine", "windows.sh engine d3d11"), list(pl.WINDOWS_PARTS)),
                                (pl.WORKFLOW, workflow.replace("KEEP_GOING: 1", "KEEP_GOING: 0"), list(pl.WINDOWS_PARTS))):
            head = self.commit(rel, text)
            for event in ("pull_request", "merge_group"):
                gh.write_text("")
                code, doc, out = self.run_main("--event", event, "--base", base, "--head", head,
                                               "--out-dir", str(self.tmp / "out"), "--github-output", str(gh))
                self.assertEqual(code, 0, out)
                self.assertEqual(doc["windows_parts"], legs, (rel, text, event, doc["lanes"]["windows"]["why"]))
                self.assertIn(f"windows_parts={json.dumps(legs, separators=(',', ':'))}", gh.read_text().splitlines())
            base = head

    def test_a_cmake_change_reaches_the_backend_legs_only_in_backend_or_windows_code(self):
        # engine/CMakeLists.txt's source lists change with most features, the same for every backend
        cmake = ("set(CORE_SOURCES\n    core/A.cpp\n    core/B.cpp\n)\n"
                 "if(GRAPHIC_BACKEND STREQUAL \"vulkan\")\n    list(APPEND PLATFORM_SOURCES\n        platform/common/Ctx.cpp\n"
                 "    )\nelseif(GRAPHIC_BACKEND STREQUAL \"glcore\")\n    set(GL ON)\nelse()\n    set(OTHER ON)\nendif()\n"
                 "if(WIN32)\n    set(WIN_LIBS ws2_32)\nendif()\n"
                 "macro(find_sdk option)\n    find_package(SDK QUIET)\nendmacro()\n"
                 "if(APPLE)\n    set(APPLE_LIBS x)\nendif()\n")
        base = self.commit("engine/CMakeLists.txt", cmake)
        both, glcore = list(pl.WINDOWS_PARTS), ["editor", "engine-glcore"]
        for old, new, legs in (("    core/B.cpp\n", "    core/B.cpp\n    core/C.cpp\n", glcore),
                               ("    core/A.cpp\n", "", glcore),
                               ("set(APPLE_LIBS x)", "set(APPLE_LIBS y)", glcore),
                               ("        platform/common/Ctx.cpp\n", "        platform/common/Ctx.cpp\n        platform/common/Ctx2.cpp\n", both),
                               ("    set(OTHER ON)\n", "", both),
                               ("ws2_32", "ws2_32 dbghelp", both),
                               ("find_package(SDK QUIET)", "find_package(SDK REQUIRED)", both)):
            head = self.commit("engine/CMakeLists.txt", cmake.replace(old, new))
            code, doc, out = self.run_main("--event", "merge_group", "--base", base, "--head", head, "--out-dir", str(self.tmp / "out"))
            self.assertEqual(code, 0, out)
            self.assertEqual(doc["windows_parts"], legs, (old, new, doc["lanes"]["windows"]["why"]))
            base = head
        self.assertEqual(pl.cmake_scopes("if(A)\nx\nelse()\ny\nendif()\nz\n"), ["A) if(A)", "A) x", "A) ) else()", "A) ) y",
                                                                                   "A) ) endif()", "z"])

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
