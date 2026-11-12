#!/usr/bin/env python3
"""Unit tests of tools/ci/report.py (python3 tools/ci/test_report.py).

Fixtures in tools/ci/fixtures/report/, and where each comes from:
  buildlogs/linux-build.log     a real Ninja build in the linux image (jasperengine/linux-build), sources
                            mounted at /src as tools/build.sh mounts the repository: two GCC errors and
                            an ld undefined reference
  buildlogs/cmake-configure.log a real CMake configure failure in the same image
  buildlogs/web-build.log       real em++ / wasm-ld errors in the web image (jasperengine/web-build)
  gtest/core.xml, unit.log  a real googletest run (libs/googletest) with two failures, a skip and a
                            disabled test, plus tools/ci/gtest-gate.py's verdict
  gtest/crash.log           the same binary segfaulting inside a test: no XML, the gate's message
  mcp/mcp.json, mcp-run.log a real tools/mcp/run_tests.py --suite ci --json run of scratch test files
                            (one failure, one skip, one worker crash, one left-out file); the host's
                            checkout prefix was rewritten to /src/
  web/web-gate.log, web/web/sample-project.log
                            a real tools/web/browser_gate.py run in the web image on a page logging
                            a console error
  smoke/smoke-no-editor.log a real tools/smoke-test.sh run without a built editor
  synthetic (no MSVC, Xcode or Android SDK on this host), written from the tools' documented formats:
  buildlogs/windows-editor.log, buildlogs/windows-msbuild.log (cl.exe and link.exe through Ninja and MSBuild),
  buildlogs/macos-xcode.log (xcodebuild, Apple ld), buildlogs/android-build.log (Gradle with the NDK's clang),
  smoke/smoke-*.log (tools/smoke-test.sh's own "== step" and "FAIL:" lines).
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures" / "report"
spec = importlib.util.spec_from_file_location("ci_report", HERE / "report.py")
report = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = report
spec.loader.exec_module(report)

UNIT_REPRO = "UNIT_TEST_ARGS='--gtest_filter=*' WERROR=1 tools/build.sh unit-tests"
MCP_REPRO = "tools/ci/mcp_shard.sh 3 build/ci-plan"


def titles(failures):
    return [f["title"] for f in failures]


def locations(failures):
    return [f["location"] for f in failures]


class PathTest(unittest.TestCase):
    def test_docker_and_runner_prefixes_are_stripped(self):
        cases = {
            "/src/engine/core/Scene.cpp": "engine/core/Scene.cpp",
            r"D:\a\JasperEngine\JasperEngine\editor\window\Inspector.cpp": "editor/window/Inspector.cpp",
            "/home/runner/work/JasperEngine/JasperEngine/editor/Project.h": "editor/Project.h",
            "/Users/runner/work/JasperEngine/JasperEngine/engine/platform/apple/A.mm": "engine/platform/apple/A.mm",
            "../../engine/core/Scene.cpp": "engine/core/Scene.cpp",
            "engine/core/Scene.cpp": "engine/core/Scene.cpp",
            "/usr/include/c++/11/vector": "/usr/include/c++/11/vector",
        }
        for raw, want in cases.items():
            self.assertEqual(report.relative_path(raw), want, raw)

    def test_a_root_of_slash_strips_nothing(self):
        # report.py copied to /tmp/ci/ makes its "repository root" /: no root may eat every slash
        self.assertEqual(report.relative_text("run tools/build.sh linux", roots=["/", ""]), "run tools/build.sh linux")
        self.assertEqual(report.relative_path("/usr/include/x.h", roots=["/", ""]), "/usr/include/x.h")

    def test_workspace_root_is_stripped(self):
        self.assertEqual(report.relative_path("/opt/ws/repo/tools/x.py", roots=["/opt/ws/repo"]), "tools/x.py")


class GtestTest(unittest.TestCase):
    def test_real_xml_failures_counts_and_repro(self):
        tests, failures = report.parse_gtest_xml(FIXTURES / "gtest" / "core.xml", UNIT_REPRO)
        self.assertEqual(tests, {"ran": 4, "failed": 2, "skipped": 1, "disabled": 1})
        self.assertEqual(titles(failures), ["MeshBvh.Refit", "Vector3.Normalise"])
        self.assertEqual(locations(failures), ["engine/tests/core/ReportFixtureTest.cpp:3",
                                               "engine/tests/core/ReportFixtureTest.cpp:7"])
        self.assertIn("Which is: 4", failures[0]["excerpt"])
        self.assertIn("expected 3 tiles after undo", failures[0]["excerpt"])
        self.assertEqual(failures[0]["kind"], "test")
        self.assertEqual(failures[0]["repro"],
                         "UNIT_TEST_ARGS=--gtest_filter=MeshBvh.Refit WERROR=1 UNIT_SUITE=core tools/build.sh unit-tests")

    def test_crash_is_named_from_the_console(self):
        failures = report.parse_log(FIXTURES / "gtest" / "crash.log", "SANITIZE=address,undefined tools/build.sh unit-tests")
        crashed = [f for f in failures if f["title"] == "Crash.Segfaults crashed"]
        self.assertEqual(len(crashed), 1, titles(failures))
        self.assertIn("crash exit 139", crashed[0]["excerpt"])
        self.assertEqual(crashed[0]["repro"], "SANITIZE=address,undefined UNIT_SUITE=editor "
                                              "UNIT_TEST_ARGS=--gtest_filter=Crash.Segfaults tools/build.sh unit-tests")
        # the gate's "missing" line says the same thing: it is not repeated
        self.assertFalse([f for f in failures if "missing" in f["title"]], titles(failures))

    def test_gate_line_without_xml_failures(self):
        failures = report.parse_log(FIXTURES / "gtest" / "unit.log", UNIT_REPRO)
        # "2 tests failed" repeats the XML; "1 tests skipped" is the gate's own finding
        self.assertEqual(titles(failures), ["gtest-gate: 1 tests skipped, at most 0 allowed"])
        self.assertEqual(failures[0]["kind"], "gate")
        self.assertIn("skipped:  MeshBvh.NeedsGpu", failures[0]["excerpt"])

    def test_htmlui_and_other_commands_narrow(self):
        self.assertEqual(report.narrow_gtest("JASPER_HTMLUI_PERF_SLACK=3 tools/build.sh htmlui-tests", "htmlui-glcore", "Golden.Text"),
                         "JASPER_HTMLUI_PERF_SLACK=3 HTMLUI_TEST_ARGS=--gtest_filter=Golden.Text tools/build.sh htmlui-tests")
        self.assertEqual(report.narrow_gtest("tools/ci/windows.sh tests", "core", "A.B"), "GTEST_FILTER=A.B tools/ci/windows.sh tests")


class BuildLogTest(unittest.TestCase):
    def test_real_gcc_and_ld(self):
        failures = report.parse_log(FIXTURES / "buildlogs" / "linux-build.log", "WERROR=1 tools/build.sh linux")
        self.assertEqual(locations(failures), ["engine/core/Link.cpp:2", "engine/core/Broken.cpp:6", "engine/core/Broken.cpp:6"])
        self.assertEqual(titles(failures)[1], "error: invalid use of incomplete type 'struct jasperengine::Transform'")
        self.assertIn("undefined reference to `missing_function(int)'", titles(failures)[0])
        self.assertIn("|             ^~", failures[1]["excerpt"])
        self.assertIn("note: forward declaration", failures[1]["excerpt"])
        self.assertNotIn("/src/", failures[1]["excerpt"])
        self.assertTrue(all(f["kind"] == "build" and f["repro"] == "WERROR=1 tools/build.sh linux" for f in failures))

    def test_real_cmake(self):
        failures = report.parse_log(FIXTURES / "buildlogs" / "cmake-configure.log", "x")
        self.assertEqual(locations(failures), ["CMakeLists.txt:4"])
        self.assertIn("find_package", failures[0]["title"])
        self.assertIn("NoSuchPackageAnywhere", failures[0]["excerpt"])

    def test_real_emcc(self):
        failures = report.parse_log(FIXTURES / "buildlogs" / "web-build.log", "tools/build.sh web")
        self.assertEqual(locations(failures), ["engine/platform/web/WebOnly.cpp:2", "engine/platform/web/WebOnly.cpp:3", ""])
        self.assertIn("undefined symbol: undefined_web_symbol()", failures[2]["title"])
        self.assertFalse([t for t in titles(failures) if "failed (returned 1)" in t])

    def test_msvc_ninja_and_link(self):
        failures = report.parse_log(FIXTURES / "buildlogs" / "windows-editor.log", "tools/ci/windows.sh editor")
        self.assertEqual(locations(failures), ["editor/window/InspectorTerrain.cpp:412", "editor/window/InspectorTerrain.cpp:413",
                                               "editor/window/InspectorLight.cpp:7", "InspectorTerrain.cpp.obj",
                                               "bin/jasperengine-editor.exe", "LINK"])
        self.assertEqual(titles(failures)[0], "error C2027: use of undefined type 'jasperengine::Transform'")
        self.assertTrue(titles(failures)[2].startswith("fatal error C1083"))
        self.assertTrue(titles(failures)[3].startswith("error LNK2019: unresolved external symbol"))
        self.assertIn("see declaration", failures[0]["excerpt"])

    def test_msbuild_suffix_and_column(self):
        failures = report.parse_log(FIXTURES / "buildlogs" / "windows-msbuild.log", "x")
        self.assertEqual(locations(failures), ["editor/window/InspectorTerrain.cpp:412"])
        self.assertNotIn("vcxproj", failures[0]["title"])

    def test_xcode(self):
        failures = report.parse_log(FIXTURES / "buildlogs" / "macos-xcode.log", "tools/ci/macos.sh xcode macos")
        self.assertEqual(locations(failures)[0], "engine/platform/apple/AppleWindow.mm:88")
        self.assertIn('Undefined symbols for architecture arm64: "jasperengine::AppleWindow::missing()"', titles(failures)[1])
        self.assertTrue(titles(failures)[2].startswith("error: Build input file cannot be found"))
        self.assertEqual(len(failures), 3, titles(failures))  # no "linker command failed", no BUILD FAILED

    def test_gradle_with_ndk(self):
        failures = report.parse_log(FIXTURES / "buildlogs" / "android-build.log", "tools/build.sh android")
        self.assertEqual(locations(failures), ["engine/platform/android/AndroidMain.cpp:42"])
        self.assertIn("42 |     ANativeActivity_noSuch", failures[0]["excerpt"])

    def test_gradle_only_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "android-build.log"
            log.write_text("> Task :app:mergeDebugResources FAILED\n\nFAILURE: Build failed with an exception.\n\n"
                           "* What went wrong:\nExecution failed for task ':app:mergeDebugResources'.\n"
                           "> Resource compilation failed. Check logs for details.\n\n* Try:\n> Run with --stacktrace\n")
            failures = report.parse_log(log, "tools/build.sh android")
        self.assertEqual(titles(failures), ["Gradle: Execution failed for task ':app:mergeDebugResources'."])
        self.assertIn("Resource compilation failed", failures[0]["excerpt"])

    def test_first_twenty_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "linux-build.log"
            log.write_text("".join(f"/src/engine/a{i}.cpp:{i + 1}:1: error: bad {i}\n" for i in range(30)))
            failures = report.parse_log(log, "x")
        self.assertEqual(len(failures), 20)
        self.assertEqual(failures[-1]["location"], "engine/a19.cpp:20")

    def test_ansi_and_crlf(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "b.log"
            log.write_bytes(b"\x1b[1m/src/engine/a.cpp:3:1: \x1b[31merror: \x1b[0mbad thing\r\n")
            failures = report.parse_log(log, "x")
        self.assertEqual(titles(failures), ["error: bad thing"])


class McpTest(unittest.TestCase):
    def test_real_mcp_json(self):
        tests, failures, left_out = report.parse_mcp_json(FIXTURES / "mcp" / "mcp.json", MCP_REPRO,
                                                           [FIXTURES / "mcp" / "mcp-run.log"])
        self.assertEqual(tests, {"ran": 2, "failed": 2, "skipped": 1, "disabled": 0})
        self.assertEqual(titles(failures), ["test_tileset.py::test_undo_paint", "test_crashes.py (worker exit 3) crashed"])
        self.assertEqual(locations(failures), ["tools/mcp/tests/test_tileset.py", "tools/mcp/tests/test_crashes.py"])
        self.assertIn("AssertionError: expected 3 tiles after undo, got 4", failures[0]["excerpt"])
        self.assertEqual(failures[0]["repro"],
                         "MCP_TEST_ARGS='-k test_undo_paint' tools/build.sh mcp-tests tools/mcp/tests/test_tileset.py")
        self.assertEqual(failures[1]["repro"], "tools/build.sh mcp-tests tools/mcp/tests/test_crashes.py")
        self.assertEqual(left_out, [{"file": "test_ecs.py", "reason": "long-running: the nightly full suite runs it"}])

    def test_mcp_run_log_is_not_parsed_twice(self):
        self.assertEqual(report.parse_log(FIXTURES / "mcp" / "mcp-run.log", MCP_REPRO), [])


class SmokeAndWebTest(unittest.TestCase):
    def test_compile_failure(self):
        failures = report.parse_log(FIXTURES / "smoke" / "smoke-post-processing.log", "ignored when the log names it")
        self.assertEqual(len(failures), 1, titles(failures))
        self.assertEqual(failures[0]["kind"], "smoke")
        self.assertEqual(failures[0]["title"], "build exported project: FAIL: build of the exported project")
        self.assertEqual(failures[0]["location"], "build/smoke/export/generated/SceneFactories.cpp:120")
        self.assertEqual(failures[0]["repro"], "SMOKE_METHOD=compile tools/smoke-test.sh tools/fixtures/post-processing")
        self.assertIn("'PostProcessVolume' is not a member", failures[0]["excerpt"])

    def test_game_errors(self):
        failures = report.parse_log(FIXTURES / "smoke" / "smoke-unified-lighting.log", "x")
        self.assertEqual(titles(failures), ["run game: FAIL: game logged errors:"])
        self.assertIn("Lua Error: scripts/Sun.lua:14", failures[0]["excerpt"])

    def test_real_no_editor_and_pass(self):
        failures = report.parse_log(FIXTURES / "smoke" / "smoke-no-editor.log", "SMOKE_METHOD=compile tools/smoke-test.sh x")
        self.assertEqual(len(failures), 0)  # no diagnostic in it: collect's fallback reports the tail
        self.assertEqual(report.parse_log(FIXTURES / "smoke" / "smoke-pass.log", "x"), [])

    def test_real_web_gate(self):
        failures = report.parse_log(FIXTURES / "web" / "web-gate.log", "tools/build.sh web && tools/build.sh web-gate",
                                    transcripts=[FIXTURES / "web" / "web" / "sample-project.log"])
        self.assertEqual(titles(failures), ["browser_gate: 1 console errors"])
        self.assertIn("error: (ERROR): Missing precompiled shader 'lit_skinned'", failures[0]["excerpt"])
        self.assertEqual(failures[0]["repro"], "tools/build.sh web && tools/build.sh web-gate")


class CollectTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="report-test-")
        self.root = Path(self.tmp.name)
        self.results = self.root / "test-results"
        self.results.mkdir()
        self.summary = self.root / "summary.md"

    def tearDown(self):
        self.tmp.cleanup()

    def env(self, status, repro):
        return {"JASPER_CI_STATUS": status, "JASPER_CI_REPRO": repro, "GITHUB_SHA": "3f2c1ab" + "0" * 33,
                "GITHUB_STEP_SUMMARY": str(self.summary)}

    def test_unit_lane(self):
        shutil.copy(FIXTURES / "gtest" / "core.xml", self.results / "core.xml")
        shutil.copy(FIXTURES / "gtest" / "unit.log", self.results / "unit.log")
        out = self.results / "ci-report" / "unit.json"
        data = report.collect("unit", self.results, out, self.env("failure", UNIT_REPRO))
        self.assertEqual(json.loads(out.read_text()), data)
        self.assertEqual(set(data), {"lane", "status", "sha", "commands", "tests", "failures", "artifacts", "timings", "disk"})
        self.assertEqual(data["status"], "failure")
        self.assertEqual(data["commands"], [UNIT_REPRO])
        self.assertEqual(data["tests"]["ran"], 4)
        self.assertEqual(titles(data["failures"])[:2], ["MeshBvh.Refit", "Vector3.Normalise"])
        self.assertEqual(data["artifacts"], ["core.xml", "unit.log"])
        self.assertEqual(set(data["failures"][0]), {"kind", "title", "location", "excerpt", "repro"})
        summary = self.summary.read_text()
        self.assertIn("MeshBvh.Refit", summary)
        self.assertIn("Reproduce:", summary)

    def test_mcp_lane_keeps_left_out(self):
        shutil.copy(FIXTURES / "mcp" / "mcp.json", self.results / "mcp.json")
        data = report.collect("mcp-3", self.results, self.results / "ci-report" / "mcp-3.json", self.env("failure", MCP_REPRO))
        self.assertEqual(data["tests"], {"ran": 2, "failed": 2, "skipped": 1, "disabled": 0})
        self.assertEqual(data["left_out"], [{"file": "test_ecs.py", "reason": "long-running: the nightly full suite runs it"}])

    def test_failure_without_diagnostics_reports_the_tail(self):
        shutil.copy(FIXTURES / "smoke" / "smoke-no-editor.log", self.results / "smoke.log")
        data = report.collect("smoke", self.results, self.results / "ci-report" / "smoke.json",
                              self.env("failure", "SMOKE_METHOD=compile tools/smoke-test.sh tools/fixtures/post-processing"))
        self.assertEqual(len(data["failures"]), 1)
        self.assertEqual(data["failures"][0]["kind"], "smoke")
        self.assertIn("No editor at build/linux/editor-install", data["failures"][0]["excerpt"])

    def test_failure_with_nothing_is_infra(self):
        data = report.collect("windows", self.results, self.results / "ci-report" / "windows.json",
                              self.env("failure", "tools/ci/windows.sh editor"))
        self.assertEqual([f["kind"] for f in data["failures"]], ["infra"])
        self.assertEqual(data["failures"][0]["repro"], "tools/ci/windows.sh editor")

    def test_success_has_no_failures_and_cancelled_is_kept(self):
        shutil.copy(FIXTURES / "buildlogs" / "linux-build.log", self.results / "linux-build.log")
        ok = report.collect("linux", self.results, self.results / "ci-report" / "linux.json", self.env("success", "x"))
        self.assertEqual(ok["failures"], [])  # a green job's old log is not a failure
        gone = report.collect("linux", self.results, self.results / "ci-report" / "linux.json", self.env("cancelled", "x"))
        self.assertEqual(gone["status"], "cancelled")

    def test_bad_lane_name_is_refused(self):
        with self.assertRaises(SystemExit):
            report.main(["collect", "--lane", "../x", "--results", str(self.results)])


def lanes_json(**run):
    lanes = {name: {"run": True, "why": "changed"} for name in
             ("changes", "lint", "linux", "unit", "asan", "htmlui", "mcp", "smoke", "web", "android", "windows", "macos", "ios")}
    for name, value in run.items():
        lanes[name] = {"run": value[0], "why": value[1]}
    return {"event": "pull_request", "full": False, "base": "a" * 40, "head": "b" * 40, "lanes": lanes,
            "unit_filter": {"core": "", "editor": ""}, "htmlui_filter": "",
            "mcp": {"mode": "planned", "files": ["tools/mcp/tests/test_tileset.py"],
                    "not_in_ci": [{"file": "tools/mcp/tests/test_ecs.py", "reason": "needs the fixture project"}],
                    "shards": ["1", "2"]},
            "fallbacks": []}


def lane_report(lane, status="success", ran=0, failures=(), seconds=600):
    return {"lane": lane, "status": status, "sha": "3f2c1ab" + "0" * 33, "commands": ["x"],
            "tests": {"ran": ran, "failed": len(failures), "skipped": 0, "disabled": 0},
            "failures": list(failures), "artifacts": [f"{lane}.log"], "timings": {"seconds": seconds}, "disk": "/dev/root 72G 50G 22G 70% /"}


def open_fence_at_end(lines):
    """The line that opens a code fence never closed (CommonMark: a closing fence is at least as long)."""
    fence = None
    for i, line in enumerate(lines):
        run = len(line) - len(line.lstrip("`"))
        if fence is None and run >= 3:
            fence = (i, run)
        elif fence is not None and run >= fence[1] and not line.strip("`").strip():
            fence = None
    return fence


def failure(title, kind="test", location="", excerpt="", repro="x"):
    return {"kind": kind, "title": title, "location": location, "excerpt": excerpt, "repro": repro}


class RenderTest(unittest.TestCase):
    RUN = "https://github.com/o/r/actions/runs/812"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="render-test-")
        self.dir = Path(self.tmp.name)
        self.reports = self.dir / "reports"
        self.reports.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, *reports, lanes=None, nested=False):
        for r in reports:
            target = self.reports / (f"ci-report-{r['lane']}" if nested else "")
            target.mkdir(exist_ok=True)
            (target / f"{r['lane']}.json").write_text(json.dumps(r))
        path = self.dir / "lanes.json"
        path.write_text(json.dumps(lanes or lanes_json()))
        return path

    def render(self, lanes_path, advisory="", **extra):
        return report.render(report.load_reports(self.reports), report.load_lanes(lanes_path), self.RUN, "3f2c1ab" + "0" * 33,
                             advisory.split(), **extra)

    def green_reports(self):
        names = ["changes", "lint", "linux", "unit", "asan", "smoke", "web", "android", "windows", "macos", "ios", "mcp-1", "mcp-2"]
        return [lane_report(n, ran={"unit": 412, "asan": 400, "mcp-1": 120, "mcp-2": 94}.get(n, 0)) for n in names]

    def test_red_layout(self):
        windows = lane_report("windows", "failure", failures=[failure(
            "error C2027: use of undefined type 'jasperengine::Transform'", "build", "editor/window/InspectorTerrain.cpp:412",
            "D:/a/x.cpp(412): error C2027\n410 |   auto& t = scene->getComponent<Transform>(e);", "tools/ci/windows.sh editor")])
        windows["artifacts"] = ["windows-build.log"]
        mcp3 = lane_report("mcp-2", "failure", ran=215, failures=[failure(
            "test_tileset.py::test_undo_paint", location="tools/mcp/tests/test_tileset.py",
            excerpt="AssertionError: expected 3 tiles after undo, got 4",
            repro="MCP_TEST_ARGS='-k test_undo_paint' tools/build.sh mcp-tests tools/mcp/tests/test_tileset.py")])
        others = [r for r in self.green_reports() if r["lane"] not in ("windows", "mcp-2")]
        path = self.write(windows, mcp3, *others, lanes=lanes_json(htmlui=(False, "no HTML UI change")))
        body = self.render(path)
        lines = body.splitlines()
        self.assertEqual(lines[0], "<!-- jasper-ci-report -->")
        self.assertIn("CI failed on `3f2c1ab`", lines[1])
        self.assertIn("2 of 13 jobs", lines[1])
        self.assertIn("[run 812](https://github.com/o/r/actions/runs/812)", lines[1])
        self.assertIn("windows", body)
        self.assertIn("Windows MSVC", body)
        self.assertIn("`editor/window/InspectorTerrain.cpp:412`", body)
        self.assertIn("Reproduce (Windows, x64 Native Tools shell): `tools/ci/windows.sh editor`", body)
        self.assertIn("mcp (2/2)", body)
        self.assertIn("1 failed, 214 passed, 0 skipped", body)
        self.assertIn("Evidence: artifact `test-results-windows`", body)
        self.assertIn("Passed: changes · lint · linux · unit (412 tests)", body)
        self.assertIn("htmlui (skipped: no HTML UI change)", body)
        self.assertIn("Not run in CI: `tools/mcp/tests/test_ecs.py` (needs the fixture project)", body)
        self.assertIn("Machine-readable: `gh run download 812 -p 'ci-report-*'`; per failed lane: `-n ci-report-mcp-2`", body)
        self.assertIn("mcp (1/2, 120 tests)", body)
        self.assertIn("`-n ci-report-windows`", body)
        self.assertLess(body.index("windows"), body.index("Passed:"))

    def test_advisory_is_on_top_and_not_counted(self):
        unit = lane_report("unit", "failure", ran=412, failures=[failure("MeshBvh.Refit", location="engine/tests/core/A.cpp:2")])
        others = [r for r in self.green_reports() if r["lane"] != "unit"]
        body = self.render(self.write(unit, *others, lanes=lanes_json(htmlui=(False, "no HTML UI change"))), advisory="unit asan mcp")
        lines = body.splitlines()
        self.assertIn("CI passed on `3f2c1ab`", lines[1])
        self.assertIn("advisory (rollout): failing", body)
        self.assertLess(body.index("advisory (rollout): failing"), body.index("<details>"))
        self.assertIn("MeshBvh.Refit", body)

    def test_green_is_one_line_and_details(self):
        body = self.render(self.write(*self.green_reports(), lanes=lanes_json(htmlui=(False, "no HTML UI change"))))
        lines = body.splitlines()
        self.assertEqual(lines[0], "<!-- jasper-ci-report -->")
        self.assertRegex(lines[1], r"^✅ CI passed on `3f2c1ab`: 13 jobs, 10 min · \[run 812\]\(https://github.com/o/r/actions/runs/812\)$")
        self.assertEqual(lines[2], "")
        self.assertTrue(lines[3].startswith("<details>"))
        self.assertIn("unit: passed, 412 tests", body)
        self.assertIn("htmlui: skipped (no HTML UI change)", body)
        self.assertTrue(body.rstrip().endswith("</details>"))

    def test_nested_artifact_dirs_and_missing_report(self):
        reports = [r for r in self.green_reports() if r["lane"] != "macos"]
        path = self.write(*reports, nested=True, lanes=lanes_json(htmlui=(False, "no HTML UI change")))
        body = self.render(path)
        self.assertIn("No report from: macos", body)
        # nothing failed, but a lane left no report: not a pass the comment can claim
        self.assertIn("CI finished on `3f2c1ab`: 13 jobs, 1 without a report (the `Check` status decides)", body.splitlines()[1])
        self.assertNotIn("CI passed", body)
        needs = {"macos": {"result": "failure"}}
        body = self.render(path, needs=needs)
        self.assertIn("CI failed on", body)
        self.assertIn("macos", body.split("Passed:")[0])

    def test_size_limit_and_forty_lines(self):
        # what collect can write at most: 200 failures of 40 clipped lines each (under MAX_JSON_BYTES)
        huge = "\n".join(f"line {i} " + "x" * 230 for i in range(40))
        many = [failure(f"Suite.Test{i}", location=f"engine/a{i}.cpp:{i}", excerpt=huge) for i in range(200)]
        reports = [lane_report(n, "failure", ran=1000, failures=many) for n in ("unit", "asan", "htmlui", "mcp-1", "mcp-2")]
        body = self.render(self.write(*reports))
        self.assertLessEqual(len(body), report.COMMENT_LIMIT)
        self.assertIn("more failures", body)
        lines = body.splitlines()
        self.assertEqual(open_fence_at_end(lines), None)
        # no failure takes more than 40 lines, from its title to its Reproduce line
        starts = [i for i, line in enumerate(lines) if line.startswith("**`")]
        self.assertGreater(len(starts), 5)
        # every failing lane shows up, however long another lane's failures are
        self.assertEqual([l.split(" — ")[0] for l in lines if l.startswith("### ")],
                         ["### unit", "### asan", "### htmlui", "### mcp (1/2)", "### mcp (2/2)"])
        for start in starts:
            end = next(i for i in range(start, len(lines)) if lines[i].startswith("Reproduce:"))
            self.assertLessEqual(end - start + 1, 40)

    def test_untrusted_text_cannot_escape(self):
        evil = failure("<img src=x onerror=alert(1)> @octocat `code`", location="a.cpp:1 @team",
                       excerpt="ok a@b.example ?draw@Foo@@QEAA\n````\n# Injected heading\n<script>alert(1)</script>\n@everyone ping\n```",
                       repro="echo `id` @octocat")
        lanes = lanes_json(htmlui=(False, "[click](https://evil.example) @octocat <b>bold</b>"))
        body = self.render(self.write(lane_report("unit", "failure", ran=1, failures=[evil]), lanes=lanes))
        self.assertNotIn("@octocat", body)
        self.assertNotIn("@everyone", body)
        self.assertIn("a@b.example ?draw@Foo@@QEAA", body)  # not mentions: left as they are
        self.assertNotIn("@team", body)
        lines = body.splitlines()
        self.assertIn("**`` <img src=x onerror=alert(1)> @\u2060octocat `code` ``** · `a.cpp:1 @\u2060team`", lines)
        self.assertNotIn("](https://evil.example)", body)
        self.assertNotIn("<b>", body)
        self.assertIn("Reproduce: ``echo `id` @\u2060octocat``", lines)
        # the excerpt's own fences are shorter than the one around it, so it cannot close it
        heading = lines.index("# Injected heading")
        opening = lines.index("ok a@b.example ?draw@Foo@@QEAA") - 1
        self.assertEqual(lines[opening], "`````text")
        self.assertEqual(lines[opening + 1], "ok a@b.example ?draw@Foo@@QEAA")
        closing = next(i for i in range(heading, len(lines)) if lines[i] == "`````")
        self.assertIn("<script>alert(1)</script>", lines[opening:closing])
        self.assertEqual(open_fence_at_end(lines), None)

    def test_bad_input_is_refused(self):
        (self.reports / "big.json").write_text(json.dumps(lane_report("unit") | {"excerpt": "x" * (report.MAX_JSON_BYTES + 1)}))
        (self.reports / "bad-lane.json").write_text(json.dumps(lane_report("../../etc")))
        (self.reports / "list.json").write_text("[1, 2]")
        (self.reports / "deep.json").write_text("[" * 100000 + "]" * 100000)
        (self.reports / "unit.json").write_text(json.dumps(lane_report("unit") | {"status": "exploded"}))
        (self.reports / "lint.json").write_text(json.dumps(lane_report("lint")))
        loaded = report.load_reports(self.reports)
        self.assertEqual(sorted(loaded), ["lint", "unit"])
        self.assertEqual(loaded["unit"]["status"], "failure")  # an unknown status is not a pass


FAKE_GH = """#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
record = {"args": args}
for flag in ("--input", "--body-file"):
    if flag in args:
        record[flag] = open(args[args.index(flag) + 1]).read()
with open(os.environ["FAKE_GH_LOG"], "a") as log:
    log.write(json.dumps(record) + "\\n")
responses = json.load(open(os.environ["FAKE_GH_RESPONSES"]))
joined = " ".join(args)
for match, out, code in responses:
    if match in joined:
        sys.stdout.write(out)
        sys.exit(code)
"""


class GhTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="fake-gh-")
        self.dir = Path(self.tmp.name)
        gh = self.dir / "bin" / "gh"
        gh.parent.mkdir()
        gh.write_text(FAKE_GH.replace("#!/usr/bin/env python3", "#!" + sys.executable))
        gh.chmod(gh.stat().st_mode | stat.S_IEXEC)
        self.log = self.dir / "calls.jsonl"
        self.responses = self.dir / "responses.json"
        self.body = self.dir / "comment.md"
        self.body.write_text("<!-- jasper-ci-report -->\n❌ CI failed\n")
        self.env = dict(os.environ, PATH=f"{gh.parent}{os.pathsep}{os.environ['PATH']}", FAKE_GH_LOG=str(self.log),
                        FAKE_GH_RESPONSES=str(self.responses), GITHUB_REPOSITORY="o/r", GH_TOKEN="x")

    def tearDown(self):
        self.tmp.cleanup()

    def run_report(self, responses, *args):
        self.responses.write_text(json.dumps(responses))
        p = subprocess.run([sys.executable, str(HERE / "report.py"), *args], env=self.env, capture_output=True, text=True)
        calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return p, calls

    def test_post_creates(self):
        p, calls = self.run_report([["/comments?per_page=100", "", 0]], "post", "--pr", "7", "--body", str(self.body))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(calls[0]["args"][:3], ["api", "--paginate", "repos/o/r/issues/7/comments?per_page=100"])
        self.assertIn('"github-actions[bot]"', " ".join(calls[0]["args"]))
        self.assertEqual(calls[1]["args"], ["api", "--method", "POST", "repos/o/r/issues/7/comments", "--input", calls[1]["args"][-1]])
        self.assertEqual(json.loads(calls[1]["--input"]), {"body": self.body.read_text()})

    def test_post_updates_the_marked_comment(self):
        p, calls = self.run_report([["/comments?per_page=100", "1234\n5678\n", 0]], "post", "--pr", "7", "--body", str(self.body))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[1]["args"][:4], ["api", "--method", "PATCH", "repos/o/r/issues/comments/1234"])

    def test_post_from_queue_ref_and_fork_head(self):
        p, calls = self.run_report([["/comments?per_page=100", "", 0]], "post", "--pr-from-ref",
                                   "gh-readonly-queue/main/pr-42-0123456789abcdef0123456789abcdef01234567",
                                   "--body", str(self.body))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("repos/o/r/issues/42/comments", calls[-1]["args"])
        self.log.unlink()
        sha = "c" * 40
        p, calls = self.run_report([["pulls?state=open", "9\n", 0], ["/comments?per_page=100", "", 0]], "post",
                                   "--head-sha", sha, "--head-label", "forker:feature/x y", "--body", str(self.body))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("repos/o/r/pulls?state=open&head=forker%3Afeature%2Fx%20y&per_page=100", calls[0]["args"])
        self.assertIn(f'"{sha}"', " ".join(calls[0]["args"]))
        self.assertIn("repos/o/r/issues/9/comments", calls[-1]["args"])

    def test_post_no_matching_fork_pr_posts_nothing(self):
        p, calls = self.run_report([["pulls?state=open", "", 0]], "post", "--head-sha", "c" * 40, "--head-label", "f:b",
                                   "--body", str(self.body))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(len(calls), 1)
        self.assertIn("no open pull request", p.stdout)

    def test_issue_creates_with_label(self):
        p, calls = self.run_report([["label create", "", 1], ["issue list", "", 0]], "issue", "--label", "ci-red",
                                   "--title", "main is red", "--body", str(self.body))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(calls[0]["args"][:3], ["label", "create", "ci-red"])
        self.assertEqual(calls[1]["args"], ["issue", "list", "--repo", "o/r", "--label", "ci-red", "--state", "open",
                                            "--limit", "1", "--json", "number", "--jq", ".[0].number // empty"])
        self.assertEqual(calls[2]["args"][:2], ["issue", "create"])
        self.assertIn("main is red", calls[2]["args"])
        self.assertEqual(calls[2]["--body-file"], self.body.read_text())

    def test_issue_comments_and_links_the_pr(self):
        p, calls = self.run_report([["label create", "", 0], ["issue list", "31\n", 0], ["/pulls", "12\n", 0]],
                                   "issue", "--label", "nightly", "--title", "Nightly failures", "--body", str(self.body),
                                   "--commit", "d" * 40)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(calls[2]["args"][:3], ["issue", "comment", "31"])
        self.assertEqual(calls[3]["args"][:2], ["api", f"repos/o/r/commits/{'d' * 40}/pulls"])
        self.assertEqual(calls[4]["args"][:3], ["pr", "comment", "12"])
        self.assertIn("issues/31", " ".join(calls[4]["args"]))

    def test_issue_label_is_checked(self):
        p, calls = self.run_report([], "issue", "--label", "bug", "--title", "t", "--body", str(self.body))
        self.assertEqual(p.returncode, 2)
        self.assertIn("invalid choice: 'bug'", p.stderr)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
