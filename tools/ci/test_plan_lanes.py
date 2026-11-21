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


def decide(event, p, error="", advisory=()):
    return pl.decide(event, p, error, CI, LEFT_OUT, NIGHTLY_OUT, **({"advisory": advisory} if advisory else {}))


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
        self.assertEqual(pl.asan_job_names(pl.planned_asan_run(r["lanes"], r["unit_run"])), ["core", "editor"])
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
        self.assertEqual(runs(r), {"changes", "unit", "htmlui", "smoke", *pl.PLATFORM_LANES})
        self.assertIn("build only", r["lanes"]["unit"]["why"])
        self.assertEqual(r["mcp"]["mode"], "none")
        # ASan keeps no ccache entry (the 10 GB budget): a build on push would warm nothing
        self.assertIn("no ccache entry", r["lanes"]["asan"]["why"])
        self.assertEqual(r["build_only"], ["unit", "htmlui", "windows-editor-tests"])

    def test_push_runs_what_the_queue_left_out(self):
        # the merge queue does not run the advisory lanes, so the push to main does: every merged change
        # still gets their results, as the queue would have run them
        p = plan(["editor/window/InspectorX.cpp"], unit={"core": ["Foo"], "editor": [], "htmlui": ["Css"]},
                 mcp=["test_a.py", "test_fixture.py"])
        r = decide("push", p, advisory=ADVISORY)
        self.assertEqual(runs(r), set(pl.LANES) - {"lint"})
        self.assertEqual((r["build_only"], r["advisory_off"]), ([], []))
        self.assertEqual((r["unit_run"], r["unit_filter"], r["htmlui_filter"]),
                         ({"core": True, "editor": True}, {"core": "", "editor": ""}, ""))
        self.assertEqual(r["mcp"], decide("merge_group", p)["mcp"])
        self.assertEqual(r["mcp"]["files"], ["tools/mcp/tests/test_a.py"])
        for name in ("unit", "asan", "htmlui", "mcp", "smoke"):
            self.assertIn("ADVISORY_LANES", r["lanes"][name]["why"], name)
        # (smoke: every game export, as in the queue, not only the cache warming)
        self.assertIn("every game export", r["lanes"]["smoke"]["why"])
        # it follows the list: a lane taken off it is the queue's again, and the push builds only
        r = decide("push", p, advisory=("mcp",))
        self.assertEqual(runs(r), {"changes", "unit", "htmlui", "mcp", *pl.PLATFORM_LANES})
        self.assertEqual(r["build_only"], ["unit", "htmlui", "windows-editor-tests"])
        self.assertIn("build only", r["lanes"]["unit"]["why"])
        # a push nothing reads runs nothing
        r = decide("push", plan(["README.md"], builds=()), advisory=ADVISORY)
        self.assertEqual((runs(r), r["build_only"]), ({"changes"}, []))

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
        r = decide("pull_request", plan([".github/workflows/ci.yml"], builds=()))
        self.assertEqual(runs(r), set(pl.LANES))
        self.assertEqual(r["mcp"]["mode"], "ci")
        self.assertTrue(any(".github/workflows/ci.yml" in f for f in r["fallbacks"]))

    def test_github_output(self):
        r = decide("pull_request", plan(["engine/core/Foo.cpp"], unit={"core": ["Foo"], "editor": [], "htmlui": []}))
        doc = {"full": False, "lanes": r["lanes"], "unit_filter": r["unit_filter"], "htmlui_filter": r["htmlui_filter"],
               "mcp": dict(r["mcp"], shards=["1", "2"])}
        lines = pl.github_output(doc, r["unit_run"]).splitlines()
        keys = [line.split("=", 1)[0] for line in lines]
        self.assertEqual(keys, ["full", *[f"run_{n}" for n in pl.LANES], "unit_filter_core", "unit_filter_editor",
                                "unit_run_core", "unit_run_editor", "unit_jobs", "asan_jobs", "htmlui_filter", "mcp_shards",
                                "build_only"])
        self.assertIn("unit_filter_core=Foo.*:*/Foo.*:Foo/*.*", lines)
        self.assertIn('mcp_shards=["1","2"]', lines)
        self.assertIn("unit_run_editor=false", lines)
        # the unit lane's matrix: one job per planned test binary, the editor's in two
        self.assertIn('unit_jobs=[{"job":"core","suite":"core","part":"","other":""}]', lines)
        # the asan lane's matrix: one job per planned test binary
        self.assertIn('asan_jobs=[{"job":"core","suite":"core"}]', lines)
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
        both = [line for line in pl.github_output(doc, {"core": True, "editor": True}).splitlines() if line.startswith("asan_jobs=")]
        self.assertEqual(json.loads(both[0].split("=", 1)[1]), [{"job": "core", "suite": "core"}, {"job": "editor", "suite": "editor"}])
        # a lane that does not run has no jobs (push builds the unit trees, not ASan's)
        doc["lanes"] = dict(doc["lanes"], asan={"run": False, "why": "push"})
        self.assertIn("asan_jobs=[]", pl.github_output(doc, {"core": True, "editor": True}).splitlines())

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
        (root / ".gitignore").write_text("build/\n")
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
