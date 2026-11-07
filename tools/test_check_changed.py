"""Tests of tools/check-changed (tools/check_changed.py) and its coverage map (tools/mcp/coverage_map.py).

    python3 -I tools/test_check_changed.py        # stdlib only

On scratch git repositories and synthetic inputs: the diff (committed, staged, unstaged, untracked,
deleted; the base and its fallbacks; rb.sh's diff and a stale one), the build plan, the unit test
selection from a synthetic `ninja -t deps` output (a changed .cpp through its header, a changed test
file, parameterised suites), the MCP selection from a synthetic coverage map (and every fallback),
the gcda reader, the pass cache and its invalidation, and the exit codes of whole runs against a
fake tools/build.sh.
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
        # an engine shader: the shader lint and its tests
        self.assertEqual(self.plan("engine/shaderlib/includes/lights.glsl").self_tests, ["tools/ci/test_check_shader_lod.py"])
        self.assertEqual(self.plan("tools/check_shader_lod.py").self_tests, ["tools/ci/test_check_shader_lod.py"])
        # an MCP test file (a new one too): classified for the CI suite, before anything is built
        self.assertEqual(self.plan("tools/mcp/tests/test_new.py").self_tests, ["tools/ci/check-mcp-ci-suite.py"])
        self.assertEqual(self.plan("tools/mcp/run_tests.py").self_tests, ["tools/ci/check-mcp-ci-suite.py"])
        self.assertEqual(self.plan("tools/mcp/tests/helper.py").self_tests, [])
        # an engine event's holder, or a clear walking them: the event clears lint
        self.assertEqual(self.plan("engine/core/component/ButtonComponent.h").self_tests,
                         ["tools/check_event_clears.py", "tools/check_apple_privacy.py"])
        self.assertEqual(self.plan("engine/core/Engine.cpp").self_tests, ["tools/check_event_clears.py"])
        self.assertEqual(self.plan("engine/core/component/ButtonComponent.cpp").self_tests, [])
        for trigger in ("CMakeLists.txt", "docker/linux.Dockerfile", "tools/build.sh", "engine/cmake/JasperPch.cmake",
                        "tools/mcp/requirements.txt"):
            p = self.plan(trigger)
            self.assertEqual(p.builds, ["linux", "unit-tests", "web", "android"], trigger)
            self.assertTrue(p.unit_all and p.mcp_all, trigger)

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
            self.assertEqual(data["sources"]["engine/core/A.cpp"]["functions"], [[1, 2, "6"], [3, 4, "2"]])
            self.assertEqual(data["sources"]["engine/core/B.cpp"]["functions"], [[1, 2, "6"], [3, 4, "2"], [5, 6, "0"]])
            self.assertEqual(data["sources"]["engine/core/Inl.h"]["functions"], [[5, 6, "2"]])
            self.assertNotIn("/usr/include/x.h", data["sources"])
        finally:
            coverage_map.MIN_EDITOR_FUNCTIONS = old
            shutil.rmtree(tmp, ignore_errors=True)


FAKE_BUILD = """#!/usr/bin/env bash
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
        self.assertIn("the full gate is still the full suite", out)
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
        self.assertIn("FAIL (exit 1) header fan-out limits", out)
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
            code, out = self.run_main("--no-cache", "--no-mcp", "--keep-going")
            self.assertIn("the engine with every module off (engine/core/Module.h changed)", out)
            self.assertEqual(code, 0, out)
            self.assertRegex((self.root / "args.txt").read_text(), r"^-j \d+ all-off WERROR=1$")
            os.environ["MODULES_OFF_RC"] = "1"
            code, out = self.run_main("--no-cache", "--no-mcp", "--keep-going")
            self.assertEqual(code, 1, out)
            self.assertIn("FAIL (exit 1) the engine with every module off", out)
        finally:
            cc.in_image = real
            os.environ.pop("MODULES_OFF_RC", None)
        # a source alone keeps it out
        (self.root / "args.txt").unlink()
        (self.root / "engine/core/Module.h").unlink()
        (self.root / "engine/core/Module.cpp").write_text("int module = 2;\n")
        code, out = self.run_main("--no-cache", "--no-mcp", "--keep-going")
        self.assertEqual(code, 0, out)
        self.assertNotIn("every module off", out)
        self.assertFalse((self.root / "args.txt").exists())

    def test_no_mcp_leaves_the_mcp_tests_out_and_is_not_cached(self):
        (self.root / "editor/App.cpp").write_text("int app = 8;\n")
        code, out = self.run_main("--no-mcp")
        self.assertEqual(code, 0, out)
        self.assertFalse([c for c in self.calls() if c.startswith("mcp-tests")])
        self.assertIn("SKIP (--no-mcp)", out)
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
        self.assertIn("SKIP (not in the Linux image", out)
        self.assertIn("the pass is not cached", out)
        self.assertFalse((self.root / "build/.cache/check-changed.json").exists())
        self.assertFalse([c for c in self.calls() if c.startswith("web")])
        self.assertTrue([c for c in self.calls() if c.startswith("linux")])

    def test_failure_exits_1_and_is_not_cached(self):
        (self.root / "editor/App.cpp").write_text("int app = 3;\n")
        os.environ["FAIL_TARGET"] = "linux"
        code, out = self.run_main()
        self.assertEqual(code, 1, out)
        self.assertIn("FAILED", out)
        self.assertFalse((self.root / "build/.cache/check-changed.json").exists())

    def test_keep_going_runs_the_tests_after_a_failure(self):
        (self.root / "editor/App.cpp").write_text("int app = 7;\n")
        os.environ["FAIL_TARGET"] = "linux"
        code, out = self.run_main()
        self.assertEqual(code, 1, out)
        self.assertFalse([c for c in self.calls() if c.startswith("mcp-tests")])
        code, out = self.run_main("--keep-going", "--no-cache")
        self.assertEqual(code, 1, out)
        self.assertTrue([c for c in self.calls() if c.startswith("mcp-tests")])

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
