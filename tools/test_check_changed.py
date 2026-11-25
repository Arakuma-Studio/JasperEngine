"""Tests of tools/check-changed (tools/check_changed.py) and its coverage map (tools/mcp/coverage_map.py).

    python3 -I tools/test_check_changed.py        # stdlib only

On scratch git repositories and synthetic inputs: the diff (committed, staged, unstaged, untracked,
deleted; the base and its fallbacks; rb.sh's diff and a stale one), the build plan, the unit test
selection from a synthetic `ninja -t deps` output (a changed .cpp through its header, a changed test
file, parameterised suites), the MCP selection from a synthetic coverage map (and every fallback),
the gcda reader, the pass cache and its invalidation, the area lints by path, and whole runs
against a fake tools/build.sh: exit codes, keep-going and the skip of a step whose build failed,
each step's log, the parsed failures and the summary's fix lines, --json, and the red control
(red when the change is reverted, exit 1 when no test pins it, red (build), a dirty tree or a
leftover refused, the restore after a kill, a byte-identical tree).
"""

from __future__ import annotations

import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "mcp"))
import check_changed as cc  # noqa: E402
import coverage_map  # noqa: E402

REAL_ROOT = cc.ROOT


def sh(cwd: Path, *args: str) -> str:
    return subprocess.run(list(args), cwd=str(cwd), check=True, capture_output=True, text=True).stdout


def make_repo(root: Path) -> Path:
    """A repository with oe-port, and a stream branch off it."""
    root.mkdir(parents=True, exist_ok=True)
    sh(root, "git", "init", "-q", "-b", "oe-port")
    sh(root, "git", "config", "user.email", "t@example.com")
    sh(root, "git", "config", "user.name", "t")
    for rel, text in {"engine/core/Foo.cpp": "int foo() { return 1; }\n", "engine/core/Foo.h": "int foo();\n",
                      "editor/App.cpp": "int app;\n", "docs/a.md": "a\n", "engine/core/Gone.cpp": "int gone;\n",
                      ".gitignore": "build/\n"}.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    sh(root, "git", "add", "-A")
    sh(root, "git", "commit", "-qm", "base")
    sh(root, "git", "checkout", "-qb", "stream")
    return root


class Diff(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cc-test-"))
        self.root = make_repo(self.tmp / "repo")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)
        cc.use_root(REAL_ROOT)

    def test_committed_staged_unstaged_untracked_deleted(self):
        r = self.root
        (r / "engine/core/Foo.cpp").write_text("int foo() { return 2; }\n")
        sh(r, "git", "commit", "-qam", "committed")
        (r / "editor/App.cpp").write_text("int app = 1;\n")
        sh(r, "git", "add", "editor/App.cpp")                       # staged
        (r / "engine/core/Foo.h").write_text("int foo(); // x\n")    # unstaged
        (r / "editor/New.cpp").write_text("int n;\n")                # untracked
        sh(r, "git", "rm", "-q", "engine/core/Gone.cpp")             # deleted
        (r / "build").mkdir()
        (r / "build" / "ignored.txt").write_text("x")                # ignored: not a change
        ch = cc.git_changes(None, r)
        self.assertEqual(ch.files, ["editor/App.cpp", "editor/New.cpp", "engine/core/Foo.cpp", "engine/core/Foo.h",
                                    "engine/core/Gone.cpp"])
        self.assertEqual(ch.deleted, ["engine/core/Gone.cpp"])
        self.assertEqual(ch.untracked, ["editor/New.cpp"])
        self.assertIn("oe-port", ch.base_ref)

    def test_base_fallbacks(self):
        r = self.root
        sh(r, "git", "branch", "-qm", "oe-port", "main")             # no oe-port: main
        self.assertIn("and main", cc.find_base(None, r)[1])
        sh(r, "git", "branch", "-qm", "main", "other")               # neither: an error
        with self.assertRaises(LookupError):
            cc.find_base(None, r)
        self.assertIn("other", cc.find_base("other", r)[1])          # --base

    def test_rb_changes_used_when_current_and_refused_when_stale(self):
        r = self.root
        (r / "engine/core/Foo.cpp").write_text("changed\n")
        import hashlib
        rb = r / "build" / ".rb-changes.json"
        rb.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256(b"changed\n").hexdigest()
        rb.write_text(json.dumps({"base": "abc", "base_ref": "merge base of HEAD and oe-port (sent by rb.sh)",
                                  "files": ["engine/core/Foo.cpp"], "deleted": [], "untracked": [],
                                  "sha256": {"engine/core/Foo.cpp": digest}}))
        ch = cc.read_changes(None, r, rb)
        self.assertEqual((ch.source, ch.base, ch.files), ("rb.sh", "abc", ["engine/core/Foo.cpp"]))
        (r / "engine/core/Foo.cpp").write_text("edited after the sync\n")
        ch = cc.read_changes(None, r, rb)                            # stale: git's own diff here
        self.assertEqual(ch.source, "git")
        shutil.rmtree(r / ".git")                                    # stale and no history: refused
        with self.assertRaises(LookupError):
            cc.read_changes(None, r, rb)


class Builds(unittest.TestCase):
    def plan(self, *files):
        p = cc.Plan()
        cc.plan_builds(list(files), p)
        return p

    def test_rules(self):
        self.assertEqual(self.plan("docs/testing.md", "README.md").builds, [])
        self.assertEqual(self.plan("editor/App.cpp").builds, ["linux", "unit-tests"])
        self.assertEqual(self.plan("engine/core/Foo.cpp").builds, ["linux", "unit-tests", "web"])
        self.assertIn("android", self.plan("engine/platform/android/Main.cpp").builds)
        self.assertEqual(self.plan("tools/web/serve.py").builds, ["web"])
        self.assertEqual(self.plan("libs/imgui/imgui.cpp").builds, ["linux", "unit-tests"])
        self.assertEqual(self.plan("docs/mcp/guide/shaders.md").builds, ["linux"])
        self.assertEqual(self.plan("engine/shaders/mesh.frag").builds, ["linux", "unit-tests"])
        self.assertIn("htmlui-tests", self.plan("engine/core/htmlui/Doc.cpp").builds)
        self.assertEqual(self.plan("engine/tests/core/FooTest.cpp").builds, ["linux", "unit-tests"])
        # the tool's own Python: its tests, nothing built
        p = self.plan("tools/check_changed.py")
        self.assertEqual((p.builds, p.self_tests, p.mcp_all), ([], ["tools/test_check_changed.py"], False))
        self.assertEqual(self.plan("tools/mcp/shard.py").self_tests, ["tools/mcp/test_shard.py"])
        # navigation's runtime and the engine's CMake: the engine without Recast/Detour too (S74 R28)
        for f in ("engine/core/navigation/NavMesh.cpp", "engine/libs/recastnavigation/Detour/Source/DetourNavMesh.cpp",
                  "engine/core/subsystem/NavigationSystem.h", "engine/core/object/navigation/NavMeshAgent.cpp",
                  "engine/core/component/NavMeshSurfaceComponent.h", "engine/CMakeLists.txt",
                  "engine/tests/core/navigation/NavStubTest.cpp"):
            self.assertIn("navigation-off", self.plan(f).builds, f)
        self.assertEqual(self.plan("tools/check-navigation-off.sh").builds, ["navigation-off"])
        for f in ("engine/core/Foo.cpp", "editor/navigation/NavigationEditor.cpp", "engine/core/component/Transform.h",
                  "docs/navigation.md"):
            self.assertNotIn("navigation-off", self.plan(f).builds, f)
        # an engine shader: the shader lint and its tests, and the GPU vendor lint (render code names no vendor)
        self.assertEqual(self.plan("engine/shaderlib/includes/lights.glsl").self_tests,
                         ["tools/check_gpu_vendor_branches.py", "tools/ci/test_check_shader_lod.py"])
        self.assertIn("tools/check_gpu_vendor_branches.py", self.plan("engine/renders/sokol/SokolSystem.cpp").self_tests)
        self.assertNotIn("tools/check_gpu_vendor_branches.py", self.plan("engine/platform/linux/Foo.cpp").self_tests)
        self.assertEqual(self.plan("tools/check_shader_lod.py").self_tests, ["tools/ci/test_check_shader_lod.py"])
        # an MCP test file (a new one too): classified for the CI suite, before anything is built
        self.assertEqual(self.plan("tools/mcp/tests/test_new.py").self_tests, ["tools/ci/check-mcp-ci-suite.py"])
        self.assertEqual(self.plan("tools/mcp/run_tests.py").self_tests, ["tools/ci/check-mcp-ci-suite.py"])
        self.assertEqual(self.plan("tools/mcp/tests/helper.py").self_tests, [])
        # an engine source (any can hold an event or walk one): the event clears lint; engine code, the
        # Apple privacy manifest's check
        for f in ("engine/core/component/ButtonComponent.h", "engine/core/Engine.cpp", "engine/core/input/InputActions.cpp",
                  "engine/core/object/sound/Audio.h", "engine/platform/mac/WindowMac.mm"):
            self.assertIn("tools/check_event_clears.py", self.plan(f).lints, f)
            self.assertEqual(self.plan(f).self_tests, ["tools/check_apple_privacy.py"], f)
        for f in ("engine/libs/jolt/Jolt/Core/Core.h", "engine/tests/core/registry/ComponentEventsTest.cpp",
                  "engine/shaderlib/includes/lights.glsl", "editor/App.cpp"):
            self.assertNotIn("tools/check_event_clears.py", self.plan(f).lints, f)
        for trigger in ("CMakeLists.txt", "docker/linux.Dockerfile", "tools/build.sh", "engine/cmake/JasperPch.cmake",
                        "tools/mcp/requirements.txt"):
            p = self.plan(trigger)
            self.assertEqual(p.builds, ["linux", "unit-tests", "web", "android"], trigger)
            self.assertTrue(p.unit_all and p.mcp_all, trigger)

    def test_area_lints_by_path(self):
        raw, keys, events, text, keys_lint = ("tools/check_raw_writes.py", "tools/check_stream_keys.py", "tools/check_event_clears.py",
                                              "tools/check_text_encoding.py", "tools/ci/check-hardcoded-shortcuts.py")
        cases = {
            "editor/window/Foo.cpp": [raw, text, keys_lint],
            "editor/StreamComponents.cpp": [raw, keys, text, keys_lint],
            "editor/reflect/components/Light.yaml": [keys],
            "engine/core/io/File.cpp": [raw, events, text],
            "engine/renders/Render.cpp": [events, text],
            "engine/platform/mac/Window.mm": [events, text],
            "tools/raw_writes_allowlist.txt": [raw],
            "tools/check_raw_writes.py": [raw, f"{raw} --self-test"],
            "tools/check_stream_keys.py": [keys],                       # (no --self-test)
            "tools/check_event_clears.py": [events, f"{events} --self-test"],
            "tools/ci/check-hardcoded-shortcuts.py": [keys_lint, f"{keys_lint} --self-test"],
            "tools/ci/fixtures/hardcoded-shortcuts/bad.cpp": [f"{keys_lint} --self-test"],
            # what no lint reads: the tests, the vendored libraries, docs, Python
            "editor/tests/FooTest.cpp": [], "engine/libs/imgui/imgui.cpp": [],
            "engine/core/tests/X.cpp": [events],                         # (the event lint skips engine/tests/ only)
            "docs/testing.md": [], "tools/mcp/harness.py": [],
        }
        for f, lints in cases.items():
            self.assertEqual(self.plan(f).lints, lints, f)

    def test_a_tool_runs_the_test_beside_it(self):
        # tools/<dir>/x.py with tools/<dir>/test_x.py beside it: its owner test (before: none, unless listed)
        self.assertEqual(self.plan("tools/procmat/import_mm_nodes.py").self_tests, ["tools/procmat/test_import_mm_nodes.py"])
        self.assertEqual(self.plan("tools/procmat/test_import_mm_nodes.py").self_tests, ["tools/procmat/test_import_mm_nodes.py"])
        self.assertEqual(self.plan("tools/ci/unit_split.py").self_tests, ["tools/ci/test_unit_split.py"])
        # a test that needs arguments (a built binary) is left to its lane; no test beside it: nothing
        self.assertEqual(self.plan("tools/crash/symbolicate.py").self_tests, [])
        self.assertEqual(self.plan("tools/ci/test_player_cli.py").self_tests, [])
        self.assertEqual(self.plan("tools/gen_blue_noise.py").self_tests, [])

    def test_ci_nothing_reads(self):
        for f in ("docs/specs/S01-cxx20.md", "docs/superpowers/specs/x.md", "docs/engine-gaps/a.md",
                  "docs/openengine-analysis/b.md", "docs/test-cleanup/c.md", "PIXEL-ART-LEDGER.md", "README.md",
                  "tools/mcp/ci-timings.json"):
            self.assertTrue(cc.ci_reads_nothing(f), f)
        # the editor embeds the guide; tests read other docs; .claude/rules is test_agent_docs.py's
        for f in ("docs/mcp/guide/code.md", "docs/unity-glossary.md", "docs/editor/keyboard-shortcuts.md",
                  "docs/html-ui/goldens-S16.md", "docs/specs-index.md", "tools/README.md", "docs/FOO-LEDGER.md",
                  ".claude/rules/mcp.md", "AGENTS.md", "engine/core/Foo.cpp", ".github/workflows/ci.yml",
                  "tools/mcp/gpu_lane.json", "tools/mcp/tests/ci-timings.json"):
            self.assertFalse(cc.ci_reads_nothing(f), f)

    def test_the_engine_with_every_module_off(self):
        for asks in ("engine/core/subsystem/PhysicsSystem.h", "engine/core/Foo.hpp", "engine/CMakeLists.txt",
                     "tools/check-physics-off.sh", "engine/cmake/JasperPlayerVariants.cmake", "tools/build.sh"):
            self.assertEqual(self.plan(asks, "docs/testing.md").modules_off, asks, asks)
        for not_asks in ("engine/core/Foo.cpp", "editor/App.h", "engine/tests/core/Foo.h", "engine/renders/Foo.h", "docs/testing.md"):
            self.assertEqual(self.plan(not_asks).modules_off, "", not_asks)


DEPS = """\
tests/core/CMakeFiles/jasperengine-core-tests.dir/FooTest.cpp.o: #deps 3, deps mtime 1 (VALID)
    /src/engine/tests/core/FooTest.cpp
    /src/engine/core/Foo.h
    /usr/include/c++/11/vector

tests/core/CMakeFiles/jasperengine-core-tests.dir/BarTest.cpp.o: #deps 2, deps mtime 1 (VALID)
    ../../../engine/tests/core/BarTest.cpp
    ../../../engine/core/Bar.h

CMakeFiles/jasperengine.dir/core/Foo.cpp.o: #deps 2, deps mtime 1 (VALID)
    /src/engine/core/Foo.cpp
    /src/engine/core/Foo.h
"""
SOURCES = {
    "engine/tests/core/FooTest.cpp": "TEST(Foo, Adds) {}\nTEST_F(FooFixture, Works) {}\nTEST_P(FooParam, X) {}\n",
    "engine/tests/core/BarTest.cpp": "TEST(Bar, A) {}\n",
}


class Units(unittest.TestCase):
    def setUp(self):
        self.deps = cc.parse_ninja_deps(DEPS, Path("/src/build/linux/core-tests"), Path("/nonexistent"))

    def test_parse(self):
        self.assertEqual(self.deps["tests/core/CMakeFiles/jasperengine-core-tests.dir/BarTest.cpp.o"],
                         ["engine/tests/core/BarTest.cpp", "engine/core/Bar.h"])
        self.assertEqual(self.deps["CMakeFiles/jasperengine.dir/core/Foo.cpp.o"][0], "engine/core/Foo.cpp")

    def test_a_cpp_counts_through_its_header(self):
        units, suites = cc.select_units(["engine/core/Foo.cpp"], self.deps, SOURCES.__getitem__)
        self.assertEqual(units, ["engine/tests/core/FooTest.cpp"])
        self.assertEqual(suites, ["Foo", "FooFixture", "FooParam"])

    def test_a_header_and_a_test_file(self):
        self.assertEqual(cc.select_units(["engine/core/Bar.h"], self.deps, SOURCES.__getitem__)[1], ["Bar"])
        self.assertEqual(cc.select_units(["engine/tests/core/BarTest.cpp"], self.deps, SOURCES.__getitem__)[1], ["Bar"])
        self.assertEqual(cc.select_units(["engine/core/Other.cpp"], self.deps, SOURCES.__getitem__), ([], []))

    def test_a_new_test_file_and_a_shader(self):
        tmp = Path(tempfile.mkdtemp(prefix="cc-units-"))
        try:
            cc.use_root(tmp)
            new = tmp / "engine/tests/core/NewTest.cpp"
            new.parent.mkdir(parents=True)
            new.write_text("TEST(Brand, New) {}\n")
            (tmp / "engine/tests/core/DrawTest.cpp").write_text("TEST(DrawGpu, A) {}\nTEST(Draw, B) {}\n")
            p = cc.Plan()
            files = ["engine/tests/core/NewTest.cpp"]
            cc.plan_builds(files, p)
            cc.plan_units(cc.Changes("b", "x", files), p, DEPS, DEPS)
            self.assertEqual(p.unit_core, ["Brand"])
            p = cc.Plan()
            files = ["engine/shaders/mesh.frag"]
            cc.plan_builds(files, p)
            cc.plan_units(cc.Changes("b", "x", files), p, DEPS, DEPS)
            self.assertEqual(p.unit_core, ["DrawGpu"])
        finally:
            cc.use_root(REAL_ROOT)
            shutil.rmtree(tmp, ignore_errors=True)

    def test_filter(self):
        self.assertEqual(cc.suite_filter(["Foo"]), "Foo.*:*/Foo.*:Foo/*.*")

    def test_each_suite_says_why_it_runs(self):
        # (the plan's reasons: the test unit that changed, or that includes what changed)
        why = {}
        cc.select_units(["engine/core/Foo.cpp", "engine/tests/core/BarTest.cpp"], self.deps, SOURCES.__getitem__, why)
        self.assertEqual(why, {"engine/tests/core/FooTest.cpp": "engine/core/Foo.h",
                               "engine/tests/core/BarTest.cpp": "engine/tests/core/BarTest.cpp"})
        p = cc.Plan()
        files = ["engine/core/Foo.cpp", "engine/tests/core/BarTest.cpp"]
        cc.plan_builds(files, p)
        tmp = Path(tempfile.mkdtemp(prefix="cc-why-"))
        try:
            cc.use_root(tmp)
            for unit, text in SOURCES.items():
                (tmp / unit).parent.mkdir(parents=True, exist_ok=True)
                (tmp / unit).write_text(text)
            cc.plan_units(cc.Changes("b", "x", files), p, DEPS, DEPS)
        finally:
            cc.use_root(REAL_ROOT)
            shutil.rmtree(tmp, ignore_errors=True)
        self.assertEqual(p.unit_reasons["core"]["Foo"], ("dependency", "engine/tests/core/FooTest.cpp includes engine/core/Foo.h"))
        self.assertEqual(p.unit_reasons["core"]["Bar"], ("owner", "engine/tests/core/BarTest.cpp changed"))

    def test_a_source_no_test_includes_selects_its_nearest_consumers(self):
        # Qux.cpp is linked into the test binary, but no test unit includes Qux.h: Foo.cpp does, so the
        # tests of Foo (which include Foo.h) run Qux's code. Before: no suite at all
        deps = DEPS + """
CMakeFiles/jasperengine.dir/core/Qux.cpp.o: #deps 2, deps mtime 1 (VALID)
    /src/engine/core/Qux.cpp
    /src/engine/core/Qux.h

CMakeFiles/jasperengine-editor.dir/core/Foo.cpp.o: #deps 3, deps mtime 1 (VALID)
    /src/engine/core/Foo.cpp
    /src/engine/core/Foo.h
    /src/engine/core/Qux.h

CMakeFiles/jasperengine.dir/core/Lone.cpp.o: #deps 1, deps mtime 1 (VALID)
    /src/engine/core/Lone.cpp
"""
        parsed = cc.parse_ninja_deps(deps, Path("/src/build/linux/core-tests"), Path("/nonexistent"))
        self.assertEqual(cc.select_units(["engine/core/Qux.cpp"], parsed, SOURCES.__getitem__), ([], []))
        units, via, steps = cc.nearest_consumers("engine/core/Qux.cpp", parsed)
        self.assertEqual((units, via, steps), (["engine/tests/core/FooTest.cpp"], {"engine/tests/core/FooTest.cpp": "engine/core/Foo.h"}, 1))
        # nothing includes it, or it is not in this tree: nothing
        self.assertEqual(cc.nearest_consumers("engine/core/Lone.cpp", parsed)[0], [])
        self.assertEqual(cc.nearest_consumers("editor/Other.cpp", parsed)[0], [])
        tmp = Path(tempfile.mkdtemp(prefix="cc-consumers-"))
        try:
            cc.use_root(tmp)
            for unit, text in SOURCES.items():
                (tmp / unit).parent.mkdir(parents=True, exist_ok=True)
                (tmp / unit).write_text(text)
            p = cc.Plan()
            files = ["engine/core/Qux.cpp"]
            cc.plan_builds(files, p)
            cc.plan_units(cc.Changes("b", "x", files), p, deps, deps)
        finally:
            cc.use_root(REAL_ROOT)
            shutil.rmtree(tmp, ignore_errors=True)
        self.assertEqual(p.unit_core, ["Foo", "FooFixture", "FooParam"])
        self.assertEqual(p.unit_reasons["core"]["Foo"][0], "dependency")
        self.assertIn("nearest consumer", p.unit_reasons["core"]["Foo"][1])

    def test_cmake_or_no_database_runs_every_unit_test(self):
        for files, deps in ((["engine/CMakeLists.txt"], (DEPS, DEPS)), (["engine/core/Foo.cpp"], (None, DEPS))):
            p = cc.Plan()
            cc.plan_builds(files, p)
            cc.plan_units(cc.Changes("b", "x", files), p, *deps)
            self.assertTrue(p.unit_all, files)


SRC = {
    "test_agent_docs.py": "x\n",
    "test_render.py": "await ed.call('set_properties')\n",
    "test_tool.py": "await ed.call(\"resolve_asset\", asset='x')\n",
    "test_fixture.py": "EDITOR = {'project': REPO / 'tools' / 'fixtures' / 'water'}\n",
    "test_build.py": "x\n",
    "test_call_cli.py": "x\n",
    "test_saved_output_determinism.py": "x\n",
    "test_other.py": "y\n",
}
def mapfile(sources, tests=("test_render.py", "test_other.py"), editor_tests=None, seconds=None):
    """A coverage map: sources: {file: (unused, [(first, last, [test indexes])])}."""
    return {"version": 2, "commit": "abc", "created": 0.0, "tests": list(tests), "editor_tests": editor_tests or len(tests),
            "seconds": seconds or {}, "sources": {f: {"functions": [
                [a, b, format(sum(1 << i for i in who), "x")] for a, b, who in functions]}
                for f, (blob, functions) in sources.items()}}


MAP = mapfile({"engine/core/Render.cpp": ("b1", [(1, 9, [0])]), "editor/App.cpp": ("b2", [(1, 9, [0])]),
               "editor/Other.cpp": ("b3", [(1, 9, [1])])})


class Mcp(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cc-mcp-"))
        (self.tmp / "editor/mcp/tools").mkdir(parents=True)
        (self.tmp / "editor/mcp/tools/AssetTools.cpp").write_text('tool.name = "resolve_asset";\n')
        cc.use_root(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)
        cc.use_root(REAL_ROOT)

    def plan(self, files, cmap=MAP, staleness=None, deleted=(), hunks=None, map_diff=None, map_commit="abc"):
        p = cc.Plan()
        cc.plan_mcp(cc.Changes("b", "x", list(files), list(deleted), hunks=hunks or {}, map_commit=map_commit,
                               map_diff=map_diff or {}), p, cmap, SRC, staleness)
        return p

    def test_map_intersection(self):
        p = self.plan(["editor/App.cpp"])
        self.assertEqual((p.mcp_files, p.mcp_ci, p.mcp_all), (["test_render.py"], False, False))
        # an engine change: also the files that run a prebuilt game; what the compile path reads: the compiled games
        p = self.plan(["engine/core/Render.cpp"])
        self.assertEqual(p.mcp_files, ["test_render.py", "test_call_cli.py"])
        p = self.plan(["engine/project/main.cpp"])
        self.assertIn("test_build.py", p.mcp_files)
        self.assertIn("test_build.py", self.plan(["editor/Exporter.cpp"]).mcp_files)
        # a header through its source; or itself, when an inline function of it ran
        self.assertEqual(self.plan(["editor/Other.h"]).mcp_files, ["test_other.py"])
        cmap = mapfile({"engine/core/Inline.h": ("b", [(1, 2, [0])])})
        self.assertEqual(self.plan(["engine/core/Inline.h"], cmap=cmap).mcp_files[0], "test_render.py")

    def test_a_change_of_what_a_save_writes_runs_its_guard(self):
        guard = "test_saved_output_determinism.py"
        for f in ("editor/Factory.cpp", "editor/StreamComponents.cpp", "editor/stream/UpstreamParticles.cpp", "editor/reflect/Properties.cpp",
                  "editor/Generator.cpp", "engine/core/serialization/RuntimeFields.gen.inc"):
            self.assertIn(guard, self.plan([f]).mcp_files, f)
        self.assertNotIn(guard, self.plan(["editor/App.cpp"]).mcp_files)

    def test_functions_from_hunks(self):
        cmap = mapfile({"editor/A.cpp": ("", [(10, 20, [0]), (30, 40, [1]), (50, 60, [])])})
        def selected(hunks, map_commit="abc", map_diff=None):
            return self.plan(["editor/A.cpp"], cmap=cmap, hunks={"editor/A.cpp": hunks}, map_commit=map_commit,
                             map_diff={"editor/A.cpp": map_diff or []})
        self.assertEqual(selected([[12, 2, 12, 2]]).mcp_files, ["test_render.py"])     # inside the first function
        self.assertEqual(selected([[35, 1, 35, 1]]).mcp_files, ["test_other.py"])      # inside the second
        self.assertEqual(selected([[15, 0, 16, 3]]).mcp_files, ["test_render.py"])     # new lines inside it
        self.assertEqual(selected([[12, 1, 12, 0]]).mcp_files, ["test_render.py"])     # a deletion inside it
        self.assertEqual(sorted(selected([[12, 1, 12, 1], [35, 1, 35, 1]]).mcp_files), ["test_other.py", "test_render.py"])
        # a changed line outside every function (a declaration): the whole file
        self.assertEqual(sorted(selected([[25, 1, 25, 1]]).mcp_files), ["test_other.py", "test_render.py"])
        # only new lines outside them (a new function): nothing old is touched, its callers' changes select
        p = selected([[25, 0, 26, 4]])
        self.assertEqual((p.mcp_files, p.mcp_ci), ([], False))
        # not this tree's map: the whole file
        self.assertEqual(sorted(selected([[12, 1, 12, 1]], map_commit="other").mcp_files), ["test_other.py", "test_render.py"])
        # a function no test executes selects nothing, and says so; the CI suite is for what the map does not know
        p = selected([[55, 1, 55, 1]])
        self.assertEqual((p.mcp_files, p.mcp_ci), ([], False))
        self.assertTrue(any("no MCP test executes" in n for n in p.mcp_notes))
        # the tree's lines moved since the map's commit (5 lines added above): still the second function
        self.assertEqual(selected([[40, 1, 40, 1]], map_diff=[[1, 0, 2, 5]]).mcp_files, ["test_other.py"])

    def test_to_map_lines(self):
        # no diff to the map: as they are
        self.assertEqual(cc.to_map_lines([[3, 2, 3, 2]], []), [(3, 4, False)])
        # 5 lines added at the top since the map's commit
        self.assertEqual(cc.to_map_lines([[10, 1, 15, 1]], [[1, 0, 2, 5]]), [(10, 10, False)])
        # 2 lines removed at the top
        self.assertEqual(cc.to_map_lines([[10, 1, 8, 1]], [[1, 2, 0, 0]]), [(10, 10, False)])
        # a line that differs from the map: the map's lines of that hunk
        self.assertEqual(cc.to_map_lines([[10, 1, 10, 1]], [[9, 3, 9, 4]]), [(9, 11, False)])
        # only new lines
        self.assertEqual(cc.to_map_lines([[10, 0, 11, 2]], []), [(11, 12, True)])

    def test_a_function_every_editor_runs_selects_the_quickest(self):
        tests = [f"test_{c}.py" for c in "abcdefghijklmnopqrst"]
        src = {t: "x\n" for t in tests}
        cmap = mapfile({"editor/A.cpp": ("blob", [(1, 9, list(range(20)))])}, tests=tests,
                       seconds={t: 100 - i for i, t in enumerate(tests)})
        p = cc.Plan()
        cc.plan_mcp(cc.Changes("b", "x", ["editor/A.cpp"], hunks={"editor/A.cpp": [[2, 1, 2, 1]]}, map_commit="abc"),
                    p, cmap, src, None)
        self.assertEqual(sorted(p.mcp_files), sorted(tests[-cc.COMMON_SAMPLE:]))

    def test_hunks_of_a_diff(self):
        diff = ("diff --git a/x.cpp b/x.cpp\n--- a/x.cpp\n+++ b/x.cpp\n@@ -3,2 +3 @@ void f()\n-a\n-b\n+c\n@@ -9 +8,2 @@\n-d\n+e\n+f\n@@ -20,0 +21 @@\n+g\n"
                "diff --git a/new.cpp b/new.cpp\n--- /dev/null\n+++ b/new.cpp\n@@ -0,0 +1 @@\n+h\n")
        self.assertEqual(cc.parse_hunks(diff), {"x.cpp": [[3, 2, 3, 1], [9, 1, 8, 2], [20, 0, 21, 1]], "new.cpp": [[0, 0, 1, 1]]})

    def test_fallbacks(self):
        self.assertTrue(self.plan(["editor/App.cpp"], cmap=None).mcp_ci)                    # no map
        self.assertTrue(self.plan(["editor/App.cpp"], staleness="old").mcp_ci)              # a stale map
        p = self.plan(["editor/Brand.cpp"])                                                   # unmapped / new
        self.assertTrue(p.mcp_ci and not p.mcp_files)
        self.assertTrue(self.plan(["tools/mcp/harness.py"]).mcp_all)                         # the harness
        p = self.plan(["editor/tests/FooTest.cpp", "engine/tests/core/BarTest.cpp"])          # unit tests: no MCP
        self.assertTrue(not p.mcp_ci and not p.mcp_files)
        self.assertTrue(self.plan(["tools/mcp/build_cache.py"]).mcp_all)
        self.assertIn("days old", cc.map_staleness({"created": time.time() - 30 * 86400}, time.time()))
        self.assertIn("commits behind", cc.map_staleness({"created": time.time(), "commit": "x"}, time.time(),
                                                         lambda c: cc.MAP_MAX_COMMITS + 1))
        self.assertIsNone(cc.map_staleness({"created": time.time(), "commit": "x"}, time.time(), lambda c: 3))

    def test_each_file_says_what_kind_of_reason_selected_it(self):
        # owner (the test changed), dependency (it runs, calls or loads what changed), policy (a rule
        # watches the path), smoke (a quick sample of code nearly every editor runs)
        p = self.plan(["tools/mcp/tests/test_other.py", "editor/App.cpp", "AGENTS.md", "editor/Exporter.cpp"])
        self.assertEqual(p.mcp_kinds, {"test_other.py": "owner", "test_render.py": "dependency",
                                       "test_agent_docs.py": "policy", "test_build.py": "policy"})
        tests = [f"test_{c}.py" for c in "abcdefghijklmnopqrst"]
        cmap = mapfile({"editor/A.cpp": ("blob", [(1, 9, list(range(20)))])}, tests=tests)
        q = cc.Plan()
        cc.plan_mcp(cc.Changes("b", "x", ["editor/A.cpp"]), q, cmap, {t: "x\n" for t in tests}, None)
        self.assertEqual(set(q.mcp_kinds.values()), {"smoke"})
        doc = cc.plan_document(cc.Changes("b", "x", ["editor/App.cpp"]), p, "map", None)
        self.assertIn({"file": "test_render.py", "reason": p.mcp_reasons["test_render.py"], "kind": "dependency"},
                      doc["mcp_selected"])

    def test_test_files_tools_fixtures(self):
        self.assertEqual(self.plan(["tools/mcp/tests/test_other.py"]).mcp_files, ["test_other.py"])
        self.assertEqual(self.plan(["tools/mcp/tests/test_other.py"], deleted=["tools/mcp/tests/test_other.py"]).mcp_files, [])
        self.assertEqual(self.plan(["editor/mcp/tools/AssetTools.cpp"], cmap=mapfile({})).mcp_files, ["test_tool.py"])
        self.assertEqual(self.plan(["tools/fixtures/water/scenes/Main.scene"]).mcp_files, ["test_fixture.py"])
        # a file no test names: the CI suite
        p = self.plan(["tools/fixtures/nobody/scenes/Main.scene"])
        self.assertTrue(p.mcp_ci and not p.mcp_files)
        # a shader: the suites that draw, by the CI suite; a C++ file is not mapped by path
        p = self.plan(["engine/shaders/mesh.frag"])
        self.assertTrue(p.mcp_ci)
        self.assertFalse(cc.path_mapped("engine/core/Foo.cpp"))
        self.assertTrue(cc.path_mapped("tools/mcp/tests/golden/x/a.png"))
        self.assertFalse(cc.path_mapped("tools/ci/check.py"))
        self.assertEqual(self.plan(["docs/testing.md"]).mcp_files, [])
        self.assertEqual(self.plan(["AGENTS.md"]).mcp_files, ["test_agent_docs.py"])


def gcda(major: int, counters: list[int], zero_compact: bool = False, functions: dict[int, list[int]] | None = None) -> bytes:
    """A minimal .gcda: header, then for each function a function record and an arc counter record."""
    first = chr(ord("A") + major - 10) if major >= 10 else chr(ord("0") + major)
    version = (ord(first) << 24) | (ord("0") << 16) | (ord("4") << 8) | ord("*")
    words = major < 12
    out = struct.pack("<III", coverage_map.GCDA_MAGIC, version, 1) + (b"" if words else struct.pack("<I", 0))
    for ident, values in (functions or {1: counters}).items():
        out += struct.pack("<Ii", coverage_map.TAG_FUNCTION, 3 if words else 12) + struct.pack("<III", ident, 2, 3)
        if zero_compact:
            out += struct.pack("<Ii", coverage_map.TAG_COUNTER_BASE, -len(values))
        else:
            out += struct.pack("<Ii", coverage_map.TAG_COUNTER_BASE, len(values) * (2 if words else 8))
            for c in values:
                out += struct.pack("<II", c & 0xffffffff, c >> 32)
    return out


def gcno(functions: dict[int, tuple[str, int, int]], cwd: str = "/src/build/x") -> bytes:
    """A minimal GCC 11 .gcno: header, current directory, a flag, function records and a block record."""
    def string(text: str) -> bytes:
        raw = text.encode() + b"\0"
        raw += b"\0" * (-len(raw) % 4)
        return struct.pack("<I", len(raw) // 4) + raw
    out = struct.pack("<III", coverage_map.GCNO_MAGIC, 0x4230342a, 1) + string(cwd) + struct.pack("<I", 1)
    for ident, (file, first, last) in functions.items():
        body = struct.pack("<III", ident, 2, 3) + string(f"f{ident}") + struct.pack("<I", 0) + string(file) + struct.pack("<IIII", first, 1, last, 2)
        out += struct.pack("<Ii", coverage_map.TAG_FUNCTION, len(body) // 4) + body
        out += struct.pack("<III", 0x01410000, 1, 7)   # the blocks: a word, their count
    return out


class Coverage(unittest.TestCase):
    def test_gcda(self):
        for major in (11, 12):
            self.assertTrue(coverage_map.gcda_executed(gcda(major, [0, 5, 0])), major)
            self.assertTrue(coverage_map.gcda_executed(gcda(major, [1 << 33])), major)
            self.assertFalse(coverage_map.gcda_executed(gcda(major, [0, 0])), major)
            self.assertFalse(coverage_map.gcda_executed(gcda(major, [0, 0], zero_compact=True)), major)
            self.assertEqual(coverage_map.gcda_functions(gcda(major, [], functions={7: [0, 1], 8: [0], 9: [3]})), {7, 9})
        self.assertFalse(coverage_map.gcda_executed(b"nonsense"))

    def test_gcno(self):
        data = gcno({1: ("/src/engine/core/A.cpp", 3, 5), 2: ("../engine/core/Bar.h", 7, 9)})
        self.assertEqual(coverage_map.gcno_functions(data),
                         {1: ("/src/engine/core/A.cpp", 3, 5), 2: ("/src/build/x/../engine/core/Bar.h", 7, 9)})
        self.assertEqual(coverage_map.gcno_functions(b"nonsense"), {})

    def test_collect(self):
        tmp = Path(tempfile.mkdtemp(prefix="cc-cov-"))
        old = coverage_map.MIN_EDITOR_FUNCTIONS
        coverage_map.MIN_EDITOR_FUNCTIONS = 2
        try:
            tree = Path("/src/build/linux/editor-coverage")
            root = tmp / "root"
            (root / "engine/core").mkdir(parents=True)
            (root / "engine/core/A.cpp").write_bytes(b"int a;\n")
            objs = {"o/A.cpp": {1: ("/src/engine/core/A.cpp", 1, 2), 5: ("/src/engine/core/A.cpp", 3, 4),
                                3: ("/src/engine/core/Inl.h", 5, 6), 4: ("/usr/include/x.h", 7, 8)},
                    "o/B.cpp": {1: ("/src/engine/core/B.cpp", 1, 2), 2: ("/src/engine/core/B.cpp", 3, 4), 9: ("/src/engine/core/B.cpp", 5, 6)}}
            runs = {"test_x-0": {"o/A.cpp": {1: [1], 5: [3], 3: [1], 4: [1]}, "o/B.cpp": {1: [1]}},
                    "test_x-1": {"o/A.cpp": {1: [1]}, "o/B.cpp": {1: [1], 2: [4]}},
                    "test_y-2": {"o/A.cpp": {1: [1]}, "o/B.cpp": {1: [1]}},
                    "test_nothing-3": {}}
            runs_dir = tmp / "runs"
            for run, gcdas in runs.items():
                (runs_dir / run).mkdir(parents=True)
                for name, functions in gcdas.items():
                    p = runs_dir / run / str(tree).lstrip("/") / f"{name}.gcda"
                    p.parent.mkdir(parents=True, exist_ok=True)
                    p.write_bytes(gcda(11, [], functions=functions))
            data = coverage_map.collect(runs_dir, tree, lambda rel: gcno(objs[rel[:-len(".gcno")]]))
            self.assertEqual(data["tests"], ["test_nothing.py", "test_x.py", "test_y.py"])
            self.assertEqual(data["editor_tests"], 2)
            self.assertEqual(data["editor_test_names"], ["test_x.py", "test_y.py"])
            self.assertEqual(data["sources"]["engine/core/A.cpp"]["functions"], [[1, 2, "6"], [3, 4, "2"]])
            self.assertEqual(data["sources"]["engine/core/B.cpp"]["functions"], [[1, 2, "6"], [3, 4, "2"], [5, 6, "0"]])
            self.assertEqual(data["sources"]["engine/core/Inl.h"]["functions"], [[5, 6, "2"]])
            self.assertNotIn("/usr/include/x.h", data["sources"])
        finally:
            coverage_map.MIN_EDITOR_FUNCTIONS = old
            shutil.rmtree(tmp, ignore_errors=True)

    def test_merge(self):
        # the maps of a run split across machines (the nightly's coverage shards): one test's
        # chunks on two shards count once; each mask is renumbered onto the joined test list
        a = {"version": 2, "commit": "c1", "created": 5.0, "tests": ["test_a.py", "test_x.py"], "editor_tests": 2,
             "editor_test_names": ["test_a.py", "test_x.py"], "seconds": {"test_a.py": 1.0, "test_x.py": 2.0},
             "sources": {"engine/core/A.cpp": {"functions": [[1, 2, "1"], [3, 4, "2"], [5, 6, "0"]]}}}
        b = {"version": 2, "commit": "c1", "created": 7.0, "tests": ["test_b.py", "test_x.py"], "editor_tests": 1,
             "editor_test_names": ["test_x.py"], "seconds": {"test_x.py": 3.0},
             "sources": {"engine/core/A.cpp": {"functions": [[1, 2, "2"], [5, 6, "1"]]},
                         "engine/core/B.cpp": {"functions": [[9, 9, "3"]]}}}
        data = coverage_map.merge([a, b])
        self.assertEqual(data["tests"], ["test_a.py", "test_b.py", "test_x.py"])
        self.assertEqual((data["commit"], data["created"], data["version"]), ("c1", 7.0, 2))
        self.assertEqual(data["editor_tests"], 2)
        self.assertEqual(data["seconds"], {"test_a.py": 1.0, "test_x.py": 5.0})
        self.assertEqual(data["sources"]["engine/core/A.cpp"]["functions"], [[1, 2, "5"], [3, 4, "4"], [5, 6, "2"]])
        self.assertEqual(data["sources"]["engine/core/B.cpp"]["functions"], [[9, 9, "6"]])
        with self.assertRaises(ValueError):
            coverage_map.merge([a, dict(b, commit="c2")])


FAKE_BUILD ="""#!/usr/bin/env bash
echo "$* RUN=${RUN:-} UNIT_SUITE=${UNIT_SUITE:-} UNIT_TEST_ARGS=${UNIT_TEST_ARGS:-}" >> "$CALLS"
[[ "$1" == "${FAIL_TARGET:-none}" ]] && exit 1
if [[ "$1" == mcp-tests ]]; then
    mkdir -p build/test-results
    echo "{\\"passed\\": 1, \\"failed\\": 0, \\"skipped\\": ${MCP_SKIPS:-0}, \\"results\\": [{\\"name\\": \\"test_a.py::test_x\\", \\"status\\": \\"$([[ ${MCP_SKIPS:-0} == 0 ]] && echo pass || echo skip)\\"}]}" > build/test-results/mcp.json
fi
exit 0
"""


class Runs(unittest.TestCase):
    """Whole runs on a scratch repository with a fake tools/build.sh: plan, exit codes, the cache."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cc-run-"))
        self.root = make_repo(self.tmp / "repo")
        (self.root / "tools").mkdir(exist_ok=True)
        (self.root / "tools/build.sh").write_text(FAKE_BUILD)
        (self.root / "tools/build.sh").chmod(0o755)
        (self.root / "tools/mcp/tests").mkdir(parents=True)
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-qm", "tools")
        sh(self.root, "git", "branch", "-qf", "oe-port", "HEAD")
        os.environ["JASPER_MCP_COVERAGE_MAP"] = str(self.tmp / "no-map.json")
        os.environ["CALLS"] = str(self.tmp / "calls.txt")
        os.environ.pop("FAIL_TARGET", None)
        os.environ.pop("MCP_SKIPS", None)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)
        cc.use_root(REAL_ROOT)
        for k in ("JASPER_MCP_COVERAGE_MAP", "FAIL_TARGET", "MCP_SKIPS", "CALLS"):
            os.environ.pop(k, None)

    def run_main(self, *argv) -> tuple[int, str]:
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = cc.main(list(argv), root=self.root)
        return code, out.getvalue()

    def calls(self) -> list[str]:
        p = self.tmp / "calls.txt"
        return p.read_text().splitlines() if p.exists() else []

    def test_nothing_buildable(self):
        (self.root / "docs/a.md").write_text("b\n")
        code, out = self.run_main()
        self.assertEqual(code, 0, out)
        self.assertIn("nothing to build or test", out)
        self.assertIn("the full gate is CI's", out)
        self.assertEqual(self.calls(), [])

    def test_dry_run_runs_nothing(self):
        (self.root / "editor/App.cpp").write_text("int app = 2;\n")
        code, out = self.run_main("--dry-run")
        self.assertEqual(code, 0, out)
        self.assertIn("builds: linux", out)
        self.assertEqual(self.calls(), [])

    def test_the_header_gates_run_after_the_editor_build(self):
        (self.root / "tools/build.sh").write_text(FAKE_BUILD.replace(
            "exit 0\n", '[[ "$1" != linux ]] || touch editor-ready\nexit 0\n'))
        (self.root / "tools/check_fanout.py").write_text("import sys; sys.exit(1)\n")
        (self.root / "tools/check_headers.py").write_text(
            "import sys; from pathlib import Path; assert Path('editor-ready').is_file(); "
            "open('headers.txt', 'w').write(' '.join(sys.argv[1:]))\n")
        (self.root / "editor/App.h").write_text("int app();\n")
        real = cc.in_image
        cc.in_image = lambda: True
        try:
            code, out = self.run_main("--no-cache")
        finally:
            cc.in_image = real
        self.assertEqual(code, 1, out)
        self.assertIn("headers compile alone (1 edited)", out)
        self.assertRegex(out, r"\nFAIL +header fan-out limits +\d+s +\(exit 1\)\n +fix: python3 -I tools/check_fanout.py\n")
        self.assertTrue((self.root / "headers.txt").is_file(), out)
        self.assertEqual((self.root / "headers.txt").read_text(), "editor/App.h")

    def test_a_core_header_builds_the_engine_with_every_module_off(self):
        (self.root / "tools/check-physics-off.sh").write_text('echo "$@ WERROR=$WERROR" > args.txt\nexit "${MODULES_OFF_RC:-0}"\n')
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-qm", "the check")
        sh(self.root, "git", "branch", "-qf", "oe-port", "HEAD")
        (self.root / "engine/core").mkdir(parents=True, exist_ok=True)
        (self.root / "engine/core/Module.h").write_text("int module();\n")
        real = cc.in_image
        cc.in_image = lambda: True
        try:
            code, out = self.run_main("--no-cache", "--no-mcp")
            self.assertIn("the engine with every module off (engine/core/Module.h changed)", out)
            self.assertEqual(code, 0, out)
            self.assertRegex((self.root / "args.txt").read_text(), r"^-j \d+ all-off WERROR=1$")
            os.environ["MODULES_OFF_RC"] = "1"
            code, out = self.run_main("--no-cache", "--no-mcp")
            self.assertEqual(code, 1, out)
            self.assertRegex(out, r"\nFAIL +the engine with every module off +\d+s +\(exit 1\)")
        finally:
            cc.in_image = real
            os.environ.pop("MODULES_OFF_RC", None)
        # a source alone keeps it out
        (self.root / "args.txt").unlink()
        (self.root / "engine/core/Module.h").unlink()
        (self.root / "engine/core/Module.cpp").write_text("int module = 2;\n")
        code, out = self.run_main("--no-cache", "--no-mcp")
        self.assertEqual(code, 0, out)
        self.assertNotIn("every module off", out)
        self.assertFalse((self.root / "args.txt").exists())

    def test_no_mcp_leaves_the_mcp_tests_out_and_is_not_cached(self):
        (self.root / "editor/App.cpp").write_text("int app = 8;\n")
        code, out = self.run_main("--no-mcp")
        self.assertEqual(code, 0, out)
        self.assertFalse([c for c in self.calls() if c.startswith("mcp-tests")])
        self.assertRegex(out, r"\nskip +MCP tests +0s +\(--no-mcp\)\n")
        self.assertIn("the pass is not cached", out)
        self.assertFalse((self.root / "build/.cache/check-changed.json").exists())
        previous = len(self.calls())
        code, out = self.run_main()
        self.assertEqual(code, 0, out)
        self.assertTrue([c for c in self.calls()[previous:] if c.startswith("mcp-tests")])

    def test_in_the_image_web_is_left_out_and_said_to_be(self):
        (self.root / "engine/core/Foo.cpp").write_text("int foo() { return 9; }\n")
        real = cc.in_image
        cc.in_image = lambda: True
        try:
            code, out = self.run_main()
        finally:
            cc.in_image = real
        self.assertEqual(code, 0, out)
        self.assertRegex(out, r"\nskip +build web +0s +\(not in the Linux image")
        self.assertIn("the pass is not cached", out)
        self.assertFalse((self.root / "build/.cache/check-changed.json").exists())
        self.assertFalse([c for c in self.calls() if c.startswith("web")])
        self.assertTrue([c for c in self.calls() if c.startswith("linux")])

    def test_navigation_off_runs_its_script(self):
        nav = self.root / "engine/core/navigation/NavMesh.cpp"
        nav.parent.mkdir(parents=True, exist_ok=True)
        nav.write_text("int mesh;\n")
        code, out = self.run_main()
        self.assertEqual(code, 0, out)
        self.assertIn("navigation-off (engine/core/navigation/NavMesh.cpp changed", out)
        self.assertIn("exec linux tools/check-navigation-off.sh RUN= UNIT_SUITE= UNIT_TEST_ARGS=", self.calls())
        os.environ["FAIL_TARGET"] = "exec"
        code, out = self.run_main("--no-cache")
        self.assertEqual(code, 1, out)
        self.assertRegex(out, r"\nFAIL +navigation OFF \(NavStubTest, NavGridTest\) +\d+s +\(exit 1\)")

    def test_failure_exits_1_and_is_not_cached(self):
        (self.root / "editor/App.cpp").write_text("int app = 3;\n")
        os.environ["FAIL_TARGET"] = "linux"
        code, out = self.run_main()
        self.assertEqual(code, 1, out)
        self.assertIn("FAILED", out)
        self.assertFalse((self.root / "build/.cache/check-changed.json").exists())

    def test_keep_going_and_skip_on_a_failed_build(self):
        # the editor's build fails: the unit tests (their own build) still run, the MCP tests (its editor) skip
        (self.root / "editor/App.cpp").write_text("int app = 7;\n")
        os.environ["FAIL_TARGET"] = "linux"
        code, out = self.run_main()
        self.assertEqual(code, 1, out)
        self.assertFalse([c for c in self.calls() if c.startswith("mcp-tests")])
        self.assertTrue([c for c in self.calls() if c.startswith("unit-tests RUN= ")], self.calls())
        self.assertRegex(out, r"\nFAIL +build linux +\d+s +\(exit 1\)\n +fix: WERROR=1 SKIP_ENGINE=1 tools/build.sh linux\n"
                              r" +log: build/check-changed/build-linux.log\n")
        self.assertRegex(out, r"\nok +unit tests \(all\) +\d+s\n")
        self.assertRegex(out, r"\nskip +MCP tests \(the CI suite\) +0s +\(build linux: failed\)\n")
        self.assertTrue(out.rstrip().endswith("FAILED in 0s") or "\nFAILED in" in out, out)
        # the unit tests' build fails: their run skips, the MCP tests run
        os.environ["FAIL_TARGET"] = "unit-tests"
        n = len(self.calls())
        code, out = self.run_main("--no-cache")
        self.assertEqual(code, 1, out)
        self.assertRegex(out, r"\nFAIL +build the unit tests +\d+s +\(exit 1\)\n +fix: WERROR=1 RUN=0 tools/build.sh unit-tests\n")
        self.assertRegex(out, r"\nskip +unit tests \(all\) +0s +\(build the unit tests: failed\)\n")
        self.assertTrue([c for c in self.calls()[n:] if c.startswith("mcp-tests")])

    def test_pass_is_cached_and_invalidated(self):
        (self.root / "editor/App.cpp").write_text("int app = 4;\n")
        code, out = self.run_main()
        self.assertEqual(code, 0, out)
        first = len(self.calls())
        self.assertGreater(first, 0)
        self.assertTrue((self.root / "build/.cache/check-changed.json").exists())
        code, out = self.run_main()                     # the same inputs: the summary again, no build
        self.assertEqual((code, len(self.calls())), (0, first), out)
        self.assertIn("passed before", out)
        self.assertRegex(out, r"\ncached +build linux +\d+s\n")
        self.assertRegex(out, r"\ncached +unit tests \(all\) +\d+s\n")
        code, out = self.run_main("--no-cache")         # --no-cache: a real run
        self.assertEqual(code, 0, out)
        self.assertGreater(len(self.calls()), first)
        n = len(self.calls())
        (self.root / "editor/App.cpp").write_text("int app = 5;\n")   # a new diff: a real run
        code, out = self.run_main()
        self.assertEqual(code, 0, out)
        self.assertGreater(len(self.calls()), n)
        n = len(self.calls())
        (self.root / "editor/Extra.cpp").write_text("int extra;\n")   # an untracked file: a real run
        code, out = self.run_main()
        self.assertGreater(len(self.calls()), n)

    def test_a_selected_test_that_cannot_run_fails(self):
        (self.root / "editor/App.cpp").write_text("int app = 6;\n")
        os.environ["MCP_SKIPS"] = "1"          # the selected MCP test skipped: it could not run
        code, out = self.run_main()
        self.assertEqual(code, 1, out)
        self.assertIn("could not run", out)
        # a map that names a test file the checkout no longer has: not selected
        sha = sh(self.root, "git", "rev-parse", "HEAD").strip()
        (self.tmp / "map.json").write_text(json.dumps(dict(mapfile({"editor/App.cpp": ("b", [(1, 9, [0])])}, tests=["test_old.py"]),
                                                           commit=sha, created=time.time())))
        os.environ["JASPER_MCP_COVERAGE_MAP"] = str(self.tmp / "map.json")
        os.environ["MCP_SKIPS"] = "0"
        code, out = self.run_main("--dry-run")
        self.assertNotIn("test_old.py:", out)

    def test_plan_json_writes_the_plan_and_runs_nothing(self):
        (self.root / "engine/core/Foo.cpp").write_text("int foo() { return 3; }\n")
        out_file = self.tmp / "plan.json"
        code, out = self.run_main("--plan-json", str(out_file))
        self.assertEqual(code, 0, out)
        self.assertEqual(self.calls(), [])                                   # implies --dry-run
        self.assertFalse((self.root / "build/.cache/check-changed.json").exists())
        plan = json.loads(out_file.read_text())
        for key in ("base", "head", "files", "deleted", "builds", "build_reasons", "unit", "mcp", "mcp_selected",
                    "self_tests", "modules_off", "navigation_off", "fallbacks", "notes"):
            self.assertIn(key, plan)
        self.assertEqual(plan["files"], ["engine/core/Foo.cpp"])
        self.assertEqual(plan["head"], sh(self.root, "git", "rev-parse", "HEAD").strip())
        self.assertEqual(plan["builds"], ["linux", "unit-tests", "web"])
        self.assertIn("engine/core/Foo.cpp", plan["build_reasons"]["linux"])
        # no dependency database (no tree, no --deps-from): every unit test, and why
        self.assertEqual(plan["unit"], {"core": "all", "editor": "all", "htmlui": []})
        self.assertTrue(any("no dependency database" in f for f in plan["fallbacks"]), plan["fallbacks"])
        # no map: the CI suite, and why
        self.assertEqual(plan["mcp"], "ci")
        self.assertTrue(any("no coverage map" in f for f in plan["fallbacks"]), plan["fallbacks"])
        self.assertFalse(plan["navigation_off"])
        # a docs-only change: nothing at all
        (self.root / "engine/core/Foo.cpp").write_text("int foo() { return 1; }\n")
        (self.root / "docs/a.md").write_text("c\n")
        code, out = self.run_main("--plan-json", str(out_file))
        plan = json.loads(out_file.read_text())
        self.assertEqual((code, plan["builds"], plan["mcp"], plan["fallbacks"]), (0, [], [], []), out)
        self.assertEqual(plan["unit"], {"core": [], "editor": [], "htmlui": []})

    def test_plan_json_exits_2_when_it_cannot_plan(self):
        code, out = self.run_main("--plan-json", str(self.tmp / "plan.json"), "--base", "no-such-ref")
        self.assertEqual(code, 2, out)
        self.assertFalse((self.tmp / "plan.json").exists())
        code, out = self.run_main("--base", "no-such-ref", "--dry-run")    # (without it: 1, as before)
        self.assertEqual(code, 1, out)

    def test_deps_from_reads_the_dumps_instead_of_ninja(self):
        test = self.root / "engine/tests/core/FooTest.cpp"
        test.parent.mkdir(parents=True)
        test.write_text("TEST(Foo, Adds) {}\nTEST_F(FooFixture, Works) {}\n")
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-qm", "a test")
        sh(self.root, "git", "branch", "-qf", "oe-port", "HEAD")
        # a tree whose ninja the run must not ask: its build.ninja is there, and the fake build.sh logs any call
        (self.root / "build/linux/core-tests").mkdir(parents=True)
        (self.root / "build/linux/core-tests/build.ninja").write_text("")
        dumps = self.tmp / "deps"
        dumps.mkdir()
        (dumps / "core-tests.deps").write_text(DEPS)
        (dumps / "editor-tests.deps").write_text("x.o: #deps 1, deps mtime 1 (VALID)\n    /src/editor/tests/OtherTest.cpp\n")
        (self.root / "engine/core/Foo.h").write_text("int foo(); // changed\n")
        out_file = self.tmp / "plan.json"
        code, out = self.run_main("--plan-json", str(out_file), "--deps-from", str(dumps))
        self.assertEqual(code, 0, out)
        self.assertEqual(self.calls(), [])
        plan = json.loads(out_file.read_text())
        self.assertEqual(plan["unit"], {"core": ["Foo", "FooFixture"], "editor": [], "htmlui": []})
        self.assertEqual(plan["deps_from"], str(dumps))
        self.assertFalse([f for f in plan["fallbacks"] if "unit" in f or "dependency" in f], plan["fallbacks"])
        # a missing dump: every unit test, naming the file
        (dumps / "editor-tests.deps").unlink()
        code, out = self.run_main("--plan-json", str(out_file), "--deps-from", str(dumps))
        plan = json.loads(out_file.read_text())
        self.assertEqual((code, plan["unit"]["core"], plan["unit"]["editor"]), (0, "all", "all"), out)
        self.assertTrue(any("editor-tests.deps" in f for f in plan["fallbacks"]), plan["fallbacks"])
        # --deps-from without --plan-json plans the same (a dry run here)
        code, out = self.run_main("--dry-run", "--deps-from", str(dumps))
        self.assertEqual(self.calls(), [])
        self.assertIn("editor-tests.deps", out)

    def run_split(self, *argv) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cc.main(list(argv), root=self.root)
        return code, out.getvalue(), err.getvalue()

    def with_a_failing_test(self, failing: int = 1) -> Path:
        """A committed core test that sees engine/core/Foo.h, its tree's dependency dump (the suites Foo
        and FooFixture), and a build.sh whose unit test run fails `failing` tests after 60 lines of noise."""
        test = self.root / "engine/tests/core/FooTest.cpp"
        test.parent.mkdir(parents=True)
        test.write_text("TEST(Foo, Adds) {}\nTEST_F(FooFixture, Works) {}\n")
        (self.root / "tools/build.sh").write_text(FAKE_BUILD.replace("exit 0\n", f"""\
if [[ "$1" == unit-tests && "${{RUN:-1}}" != 0 ]]; then
    for i in $(seq 1 60); do echo "noise line $i"; done
    for i in $(seq 1 {failing}); do
        n=$([[ $i == 1 ]] && echo "" || echo $i)
        echo "[ RUN      ] Foo.Adds$n"
        echo "/src/engine/tests/core/FooTest.cpp:$i: Failure"
        echo "Expected equality of these values:"
        echo "  foo()"
        echo "    Which is: 1"
        echo "  2"
        echo "[  FAILED  ] Foo.Adds$n (0 ms)"
    done
    echo "[ RUN      ] FooFixture.Works"
    echo "[       OK ] FooFixture.Works (0 ms)"
    exit 1
fi
exit 0
"""))
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-qm", "a test")
        sh(self.root, "git", "branch", "-qf", "oe-port", "HEAD")
        dumps = self.tmp / "deps"
        dumps.mkdir()
        (dumps / "core-tests.deps").write_text(DEPS)
        (dumps / "editor-tests.deps").write_text("x.o: #deps 1, deps mtime 1 (VALID)\n    /src/editor/tests/OtherTest.cpp\n")
        (self.root / "engine/core/Foo.h").write_text("int foo(); // changed\n")
        return dumps

    def test_a_failed_step_shows_its_parsed_failures_log_and_narrowed_fix(self):
        dumps = self.with_a_failing_test()
        code, out = self.run_main("--deps-from", str(dumps), "--no-mcp")
        self.assertEqual(code, 1, out)
        label = "unit tests (core: 2 suite(s))"
        # the terminal: the failure parsed (paths repository-relative), not the noise, then the log
        block = out[out.index(f"[check-changed] {label} FAILED (exit 1"):]
        block = block[:block.index("\n  log: ")]
        self.assertIn("\n  FAILED Foo.Adds\n      engine/tests/core/FooTest.cpp:1: Failure\n      Expected equality of these values:\n"
                      "      foo()\n      Which is: 1\n      2", block)
        self.assertNotIn("noise line", out)
        self.assertNotIn("FooFixture.Works", block)
        # the summary: one line per step, the FAIL with its rerun command narrowed to the failed test, and its log
        log = "build/check-changed/unit-tests-core-2-suite-s.log"
        self.assertRegex(out, r"\nok +build the unit tests +\d+s\n")
        self.assertIn(f"\nFAIL    {label}  0s  (exit 1)\n        fix: WERROR=1 UNIT_SUITE=core UNIT_TEST_ARGS=--gtest_filter=Foo.Adds "
                      f"tools/build.sh unit-tests\n        log: {log}\n", out)
        self.assertRegex(out, r"\nskip +MCP tests +0s +\(--no-mcp\)\n")
        # the log: the whole output, after the command as it ran
        text = (self.root / log).read_text()
        self.assertTrue(text.startswith("$ WERROR=1 UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=Foo.*:*/Foo.*:Foo/*.*:"
                                        "FooFixture.*:*/FooFixture.*:FooFixture/*.*' tools/build.sh unit-tests\n"), text[:300])
        self.assertIn("noise line 60\n", text)
        self.assertIn("[       OK ] FooFixture.Works", text)
        # every step has its log
        self.assertTrue((self.root / "build/check-changed/build-the-unit-tests.log").is_file())

    def test_a_failed_step_shows_at_most_40_lines(self):
        dumps = self.with_a_failing_test(failing=30)
        code, out = self.run_main("--deps-from", str(dumps), "--no-mcp")
        self.assertEqual(code, 1, out)
        lines = out.splitlines()
        start = next(i for i, l in enumerate(lines) if "unit tests (core: 2 suite(s)) FAILED" in l)
        end = next(i for i in range(start, len(lines)) if lines[i].startswith("  log: "))
        self.assertEqual(end - start - 1, cc.FAILURE_LINES, "\n".join(lines[start:end + 1]))
        self.assertIn("more line(s): see the log", lines[end - 1])
        # the fix names every failed test
        self.assertIn("UNIT_TEST_ARGS=--gtest_filter=Foo.Adds:Foo.Adds2:", out)
        self.assertIn(":Foo.Adds30 tools/build.sh unit-tests", out)

    def test_json_prints_the_plan_steps_logs_and_fixes(self):
        dumps = self.with_a_failing_test()
        code, out, err = self.run_split("--deps-from", str(dumps), "--no-mcp", "--json")
        self.assertEqual(code, 1, err)
        doc = json.loads(out)                                 # stdout: the one document; the progress is on stderr
        self.assertIn("unit tests (core: 2 suite(s))", err)
        self.assertEqual(set(doc), {"plan", "steps", "could_not_run", "partial", "cached", "passed", "exit", "seconds",
                                    "red_control"})
        self.assertEqual((doc["exit"], doc["passed"], doc["cached"], doc["partial"], doc["red_control"]), (1, False, False, True, None))
        self.assertEqual(doc["plan"]["files"], ["engine/core/Foo.h"])
        self.assertEqual(doc["plan"]["unit"]["core"], ["Foo", "FooFixture"])
        self.assertIn("lints", doc["plan"])
        for step in doc["steps"]:
            self.assertEqual(set(step), {"name", "status", "kind", "seconds", "log", "fix", "reason", "failures", "tests"})
            self.assertIn(step["status"], ("ok", "FAIL", "skip"))
            if step["status"] != "skip":
                self.assertTrue((self.root / step["log"]).is_file(), step)
        failed = next(s for s in doc["steps"] if s["status"] == "FAIL")
        self.assertEqual((failed["name"], failed["kind"], failed["tests"], failed["reason"]),
                         ("unit tests (core: 2 suite(s))", "unit", ["Foo.Adds"], "exit 1"))
        self.assertEqual(failed["fix"], "WERROR=1 UNIT_SUITE=core UNIT_TEST_ARGS=--gtest_filter=Foo.Adds tools/build.sh unit-tests")
        self.assertIn("FAILED Foo.Adds", failed["failures"])
        self.assertEqual([s["status"] for s in doc["steps"] if s["name"] == "MCP tests"], ["skip"])
        # a dry run: the plan, no steps; a cached pass: its steps, as cached
        code, out, err = self.run_split("--deps-from", str(dumps), "--dry-run", "--json")
        doc = json.loads(out)
        self.assertEqual((code, doc["steps"], doc["plan"]["files"]), (0, [], ["engine/core/Foo.h"]))
        (self.root / "tools/build.sh").write_text(FAKE_BUILD)
        sh(self.root, "git", "commit", "-qm", "a passing build", "tools/build.sh")
        sh(self.root, "git", "branch", "-qf", "oe-port", "HEAD")
        self.assertEqual(self.run_split("--deps-from", str(dumps), "--json")[0], 0)
        code, out, err = self.run_split("--deps-from", str(dumps), "--json")
        doc = json.loads(out)
        self.assertEqual((code, doc["cached"], doc["passed"]), (0, True, True), err)
        self.assertTrue(doc["steps"] and all(s["status"] == "cached" for s in doc["steps"]), doc["steps"])
        self.assertEqual(doc["plan"]["files"], ["engine/core/Foo.h"])

    def test_the_area_lints_run_before_the_builds(self):
        lint = 'import os, sys\nopen(os.environ["CALLS"], "a").write("lint " + " ".join(sys.argv[1:]) + "\\n")\n'
        (self.root / "tools/check_raw_writes.py").write_text(
            lint + 'print("editor/App.cpp:1: std::ofstream: write through AtomicFile"); sys.exit(1)\n')
        (self.root / "tools/check_text_encoding.py").write_text(lint)
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-qm", "the lints")
        sh(self.root, "git", "branch", "-qf", "oe-port", "HEAD")
        (self.root / "editor/App.cpp").write_text("int app = 9;\n")
        code, out = self.run_main("--no-mcp")
        self.assertEqual(code, 1, out)
        self.assertIn("lints: tools/check_raw_writes.py, tools/check_text_encoding.py, tools/ci/check-hardcoded-shortcuts.py", out)
        calls = self.calls()
        self.assertEqual(calls[:2], ["lint ", "lint "], calls)            # first, and only those the checkout has
        self.assertTrue(calls[2].startswith("unit-tests RUN=0"), calls)
        self.assertIn("[check-changed] lint tools/check_raw_writes.py FAILED (exit 1, 0s):\n"
                      "  editor/App.cpp:1: std::ofstream: write through AtomicFile\n"
                      "  log: build/check-changed/lint-tools-check-raw-writes-py.log\n", out)
        self.assertIn("\nFAIL    lint tools/check_raw_writes.py  0s  (exit 1)\n        fix: python3 -I tools/check_raw_writes.py\n", out)
        self.assertRegex(out, r"\nok +lint tools/check_text_encoding.py +\d+s\n")
        self.assertRegex(out, r"\nok +build linux +\d+s\n")                      # (the rest goes on)
        self.assertRegex(out, r"\nok +unit tests \(all\) +\d+s\n")
        # a change of the lint: its --self-test as well
        (self.root / "tools/check_text_encoding.py").write_text(lint + "# changed\n")
        (self.root / "editor/App.cpp").write_text("int app;\n")
        n = len(self.calls())
        code, out = self.run_main("--no-mcp")
        self.assertEqual(self.calls()[n:], ["lint ", "lint --self-test"], out)

    def test_inputs_hash_follows_content_base_untracked_and_map(self):
        cc.use_root(self.root)
        (self.root / "editor/App.cpp").write_text("one\n")
        ch = cc.git_changes(None, self.root)
        a = cc.inputs_hash(ch, "map1", "", self.root)
        self.assertEqual(a, cc.inputs_hash(ch, "map1", "", self.root))
        self.assertNotEqual(a, cc.inputs_hash(ch, "map2", "", self.root))
        (self.root / "editor/App.cpp").write_text("two\n")
        self.assertNotEqual(a, cc.inputs_hash(ch, "map1", "", self.root))
        cache = self.root / "build/.cache/check-changed.json"
        cc.store("k", "summary", cache)
        self.assertEqual(cc.cached("k", cache)["summary"], "summary")
        self.assertIsNone(cc.cached("other", cache))


RED_BUILD = """#!/usr/bin/env bash
seen() { [[ -f "$1" ]] && echo yes || echo no; }
echo "$* RUN=${RUN:-} New=$(seen engine/core/New.cpp) Gone=$(seen engine/core/Gone.cpp) Test=$(seen engine/tests/core/FooTest.cpp)" >> "$CALLS"
fixed() { grep -q "return 2" engine/core/Foo.cpp; }
if [[ "$1" == unit-tests ]]; then
    if [[ "${RUN:-1}" == 0 ]]; then
        if [[ -n "${BUILD_NEEDS_FIX:-}" ]] && ! fixed; then echo "/src/engine/core/Foo.cpp:1:5: error: 'two' was not declared"; exit 1; fi
        exit 0
    fi
    if [[ -n "${RED_SIGNAL:-}" ]] && ! fixed; then kill -"$RED_SIGNAL" "$PPID"; exit 1; fi
    echo "[ RUN      ] Foo.ReturnsTwo"
    if [[ -z "${NOT_PINNED:-}" ]] && ! fixed; then
        echo "/src/engine/tests/core/FooTest.cpp:3: Failure"
        echo "[  FAILED  ] Foo.ReturnsTwo (0 ms)"
        exit 1
    fi
    echo "[       OK ] Foo.ReturnsTwo (0 ms)"
fi
if [[ "$1" == mcp-tests ]]; then
    mkdir -p build/test-results
    echo '{"results": [{"name": "test_a.py::test_x", "status": "pass"}]}' > build/test-results/mcp.json
fi
exit 0
"""


class RedControl(unittest.TestCase):
    """--red-control on a scratch repository: a branch that fixes Foo (return 2), adds New.cpp, deletes
    Gone.cpp and adds the test that pins the fix, against a build.sh whose unit test run checks Foo."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cc-red-"))
        self.root = make_repo(self.tmp / "repo")
        (self.root / "tools").mkdir(exist_ok=True)
        (self.root / "tools/build.sh").write_text(RED_BUILD)
        (self.root / "tools/build.sh").chmod(0o755)
        (self.root / "tools/mcp/tests").mkdir(parents=True)
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-qm", "tools")
        sh(self.root, "git", "branch", "-qf", "oe-port", "HEAD")
        (self.root / "engine/core/Foo.cpp").write_text("int foo() { return 2; }\n")
        (self.root / "engine/core/New.cpp").write_text("int fresh;\n")
        (self.root / "engine/core/Gone.cpp").unlink()
        (self.root / "engine/tests/core").mkdir(parents=True)
        (self.root / "engine/tests/core/FooTest.cpp").write_text("TEST(Foo, ReturnsTwo) { EXPECT_EQ(foo(), 2); }\n")
        (self.root / "docs/a.md").write_text("the fix\n")
        sh(self.root, "git", "add", "-A")
        sh(self.root, "git", "commit", "-qm", "the fix and its test")
        self.state = cc.tree_state(self.root)
        os.environ["JASPER_MCP_COVERAGE_MAP"] = str(self.tmp / "no-map.json")
        os.environ["CALLS"] = str(self.tmp / "calls.txt")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)
        cc.use_root(REAL_ROOT)
        for k in ("JASPER_MCP_COVERAGE_MAP", "CALLS", "NOT_PINNED", "BUILD_NEEDS_FIX"):
            os.environ.pop(k, None)

    def run_main(self, *argv) -> tuple[int, str]:
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = cc.main(list(argv), root=self.root)
        return code, out.getvalue()

    def calls(self) -> list[str]:
        p = self.tmp / "calls.txt"
        return p.read_text().splitlines() if p.exists() else []

    def assert_restored(self):
        self.assertEqual(sh(self.root, "git", "status", "--porcelain", "--untracked-files=all"), "")
        self.assertEqual(cc.tree_state(self.root), self.state)
        self.assertEqual((self.root / "engine/core/Foo.cpp").read_text(), "int foo() { return 2; }\n")
        self.assertTrue((self.root / "engine/core/New.cpp").is_file())
        self.assertFalse((self.root / "engine/core/Gone.cpp").exists())
        self.assertFalse((self.root / "build/red-control/in-progress.json").exists())

    def test_red_when_the_fix_is_reverted_and_never_cached(self):
        code, out = self.run_main("--red-control")
        self.assertEqual(code, 0, out)
        # the pass first (with the change), then the same tests without it: the change reverted, the test kept
        self.assertIn("unit-tests RUN= New=yes Gone=no Test=yes", self.calls())
        self.assertIn("unit-tests RUN= New=no Gone=yes Test=yes", self.calls())
        self.assertIn("\nred          unit tests (all): Foo.ReturnsTwo\n", out)
        self.assertIn("\ngreen        MCP tests (the CI suite): passes without the change, so it does not pin it\n", out)
        self.assertIn("red control: 1 of 2 selected test step(s) went red without the change", out)
        self.assertIn("byte-identical to before", out)
        self.assert_restored()
        patch = (self.root / "build/red-control/patch.diff").read_text()
        for reverted in ("engine/core/Foo.cpp", "engine/core/New.cpp", "engine/core/Gone.cpp"):
            self.assertIn(f"a/{reverted}", patch)
        for kept in ("FooTest.cpp", "docs/a.md", "tools/build.sh"):
            self.assertNotIn(kept, patch)
        # the red run's logs are its own; the pass's are untouched
        self.assertIn("Foo.ReturnsTwo", (self.root / "build/red-control/logs/unit-tests-all.log").read_text())
        self.assertNotIn("FAILED", (self.root / "build/check-changed/unit-tests-all.log").read_text())
        # again: the pass is cached, the red control is not
        n = len(self.calls())
        code, out = self.run_main("--red-control")
        self.assertEqual(code, 0, out)
        self.assertIn("passed before", out)
        self.assertEqual([c for c in self.calls()[n:] if c.startswith("unit-tests RUN= ")], ["unit-tests RUN= New=no Gone=yes Test=yes"])
        self.assertNotIn("red", json.loads((self.root / "build/.cache/check-changed.json").read_text())["summary"].split("== check-changed summary")[0])
        self.assert_restored()

    def test_exit_1_when_the_test_does_not_pin_the_fix(self):
        os.environ["NOT_PINNED"] = "1"
        code, out = self.run_main("--red-control", "--no-mcp")
        self.assertEqual(code, 1, out)
        self.assertIn("\ngreen        unit tests (all): passes without the change, so it does not pin it\n", out)
        self.assertIn("red control FAILED: no selected test failed without the change", out)
        self.assert_restored()

    def test_a_test_that_fails_to_build_without_the_change_is_red_build(self):
        os.environ["BUILD_NEEDS_FIX"] = "1"
        code, out = self.run_main("--red-control", "--no-mcp")
        self.assertEqual(code, 0, out)
        self.assertIn("\nred (build)  unit tests (all)\n", out)
        self.assertIn("engine/core/Foo.cpp:1:5", out)             # (the red build's parsed error)
        self.assert_restored()

    def test_json_carries_the_verdicts(self):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cc.main(["--red-control", "--no-mcp", "--json"], root=self.root)
        self.assertEqual(code, 0, err.getvalue())
        red = json.loads(out.getvalue())["red_control"]
        self.assertEqual((red["red"], red["restored"], red["patch"]), (1, True, "build/red-control/patch.diff"))
        self.assertEqual(sorted(red["reverted"]), ["engine/core/Foo.cpp", "engine/core/Gone.cpp", "engine/core/New.cpp"])
        self.assertEqual([(r["step"], r["verdict"], r["tests"], r["reason"]) for r in red["steps"]],
                         [("unit tests (all)", "red", ["Foo.ReturnsTwo"], "exit 1"), ("MCP tests", "not run", [], "--no-mcp")])

    def test_refuses_a_dirty_tree_and_a_leftover(self):
        (self.root / "editor/Wip.cpp").write_text("int wip;\n")
        code, out = self.run_main("--red-control")
        self.assertEqual(code, 1, out)
        self.assertIn("needs it clean: 1 uncommitted path(s) (e.g. editor/Wip.cpp): commit them first", out)
        self.assertEqual(self.calls(), [])
        (self.root / "editor/Wip.cpp").unlink()
        (self.root / "build/red-control").mkdir(parents=True)
        (self.root / "build/red-control/in-progress.json").write_text("{}")
        for argv in (["--red-control"], [], ["--dry-run"]):
            code, out = self.run_main(*argv)
            self.assertEqual(code, 1, out)
            self.assertIn("restore the tree first: tools/check-changed --red-control --restore", out)
        self.assertEqual(self.calls(), [])

    def red_process(self, signal_name: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-I", str(HERE / "check_changed.py"), "--root", str(self.root), "--red-control",
                               "--no-mcp"], env=dict(os.environ, RED_SIGNAL=signal_name), capture_output=True, text=True,
                              timeout=300)

    def test_restore_after_a_kill(self):
        r = self.red_process("KILL")                          # killed mid-revert: no finally
        self.assertEqual(r.returncode, -9, r.stdout + r.stderr)
        self.assertTrue((self.root / "build/red-control/in-progress.json").is_file())
        self.assertEqual((self.root / "engine/core/Foo.cpp").read_text(), "int foo() { return 1; }\n")
        self.assertNotEqual(cc.tree_state(self.root), self.state)
        code, out = self.run_main()
        self.assertEqual(code, 1, out)
        self.assertIn("tools/check-changed --red-control --restore", out)
        code, out = self.run_main("--red-control", "--restore")
        self.assertEqual(code, 0, out)
        self.assertIn("byte-identical to before the red control (3 path(s))", out)
        self.assert_restored()
        code, out = self.run_main("--red-control", "--restore")
        self.assertEqual(code, 0, out)
        self.assertIn("nothing to restore", out)

    def test_a_terminated_run_restores_the_tree(self):
        r = self.red_process("TERM")
        self.assertEqual(r.returncode, 130, r.stdout + r.stderr)
        self.assertIn("red control: interrupted; the tree is restored", r.stdout)
        self.assert_restored()


if __name__ == "__main__":
    unittest.main(verbosity=2)
