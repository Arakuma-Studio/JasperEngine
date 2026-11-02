"""Tests of tools/check-changed (tools/check_changed.py) and its coverage map (tools/mcp/coverage_map.py).

    python3 -I tools/test_check_changed.py        # stdlib only: on a build server, or in the image

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
        for trigger in ("CMakeLists.txt", "docker/linux.Dockerfile", "tools/build.sh", "engine/cmake/JasperPch.cmake",
                        "tools/mcp/requirements.txt"):
            p = self.plan(trigger)
            self.assertEqual(p.builds, ["linux", "unit-tests", "web", "android"], trigger)
            self.assertTrue(p.unit_all and p.mcp_all, trigger)


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
    "test_render.py": "await ed.call('set_properties')\n",
    "test_tool.py": "await ed.call(\"resolve_asset\", asset='x')\n",
    "test_fixture.py": "EDITOR = {'project': REPO / 'tools' / 'fixtures' / 'water'}\n",
    "test_build.py": "x\n",
    "test_call_cli.py": "x\n",
    "test_other.py": "y\n",
}
MAP = {"commit": "abc", "created": 0.0, "files": {"test_render.py": ["engine/core/Render.cpp", "editor/App.cpp"],
                                                  "test_other.py": ["editor/Other.cpp"]}}


class Mcp(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cc-mcp-"))
        (self.tmp / "editor/mcp/tools").mkdir(parents=True)
        (self.tmp / "editor/mcp/tools/AssetTools.cpp").write_text('tool.name = "resolve_asset";\n')
        cc.use_root(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)
        cc.use_root(REAL_ROOT)

    def plan(self, files, cmap=MAP, staleness=None, header_units=None, deleted=()):
        p = cc.Plan()
        cc.plan_mcp(cc.Changes("b", "x", list(files), list(deleted)), p, cmap, SRC, header_units, staleness)
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
        # a header through the units that include it
        p = self.plan(["editor/Other.h"], header_units=lambda h: ["editor/Other.cpp"])
        self.assertEqual(p.mcp_files, ["test_other.py"])

    def test_fallbacks(self):
        self.assertTrue(self.plan(["editor/App.cpp"], cmap=None).mcp_ci)                    # no map
        self.assertTrue(self.plan(["editor/App.cpp"], staleness="old").mcp_ci)              # a stale map
        p = self.plan(["editor/Brand.cpp"])                                                   # unmapped / new
        self.assertTrue(p.mcp_ci and not p.mcp_files)
        self.assertTrue(self.plan(["tools/mcp/harness.py"]).mcp_all)                         # the harness
        self.assertTrue(self.plan(["tools/mcp/build_cache.py"]).mcp_all)
        self.assertIn("days old", cc.map_staleness({"created": time.time() - 30 * 86400}, time.time()))
        self.assertIn("commits behind", cc.map_staleness({"created": time.time(), "commit": "x"}, time.time(),
                                                         lambda c: cc.MAP_MAX_COMMITS + 1))
        self.assertIsNone(cc.map_staleness({"created": time.time(), "commit": "x"}, time.time(), lambda c: 3))

    def test_test_files_tools_fixtures(self):
        self.assertEqual(self.plan(["tools/mcp/tests/test_other.py"]).mcp_files, ["test_other.py"])
        self.assertEqual(self.plan(["tools/mcp/tests/test_other.py"], deleted=["tools/mcp/tests/test_other.py"]).mcp_files, [])
        self.assertEqual(self.plan(["editor/mcp/tools/AssetTools.cpp"], cmap={"files": {}}).mcp_files, ["test_tool.py"])
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


def gcda(major: int, counters: list[int], zero_compact: bool = False) -> bytes:
    """A minimal .gcda: header, a function record, an arc counter record."""
    first = chr(ord("A") + major - 10) if major >= 10 else chr(ord("0") + major)
    version = (ord(first) << 24) | (ord("0") << 16) | (ord("4") << 8) | ord("*")
    words = major < 12
    out = struct.pack("<III", coverage_map.GCDA_MAGIC, version, 1) + (b"" if words else struct.pack("<I", 0))
    out += struct.pack("<Ii", coverage_map.TAG_FUNCTION, 3 if words else 12) + struct.pack("<III", 1, 2, 3)
    if zero_compact:
        out += struct.pack("<Ii", coverage_map.TAG_COUNTER_BASE, -len(counters))
    else:
        out += struct.pack("<Ii", coverage_map.TAG_COUNTER_BASE, len(counters) * (2 if words else 8))
        for c in counters:
            out += struct.pack("<II", c & 0xffffffff, c >> 32)
    return out


class Coverage(unittest.TestCase):
    def test_gcda(self):
        for major in (11, 12):
            self.assertTrue(coverage_map.gcda_executed(gcda(major, [0, 5, 0])), major)
            self.assertTrue(coverage_map.gcda_executed(gcda(major, [1 << 33])), major)
            self.assertFalse(coverage_map.gcda_executed(gcda(major, [0, 0])), major)
            self.assertFalse(coverage_map.gcda_executed(gcda(major, [0, 0], zero_compact=True)), major)
        self.assertFalse(coverage_map.gcda_executed(b"nonsense"))

    def test_collect(self):
        tmp = Path(tempfile.mkdtemp(prefix="cc-cov-"))
        try:
            tree = "/src/build/linux/editor-coverage"
            deps = cc.parse_ninja_deps(
                "engine/CMakeFiles/jasperengine.dir/core/A.cpp.o: #deps 1\n    /src/engine/core/A.cpp\n\n"
                "engine/CMakeFiles/jasperengine.dir/core/B.cpp.o: #deps 1\n    /src/engine/core/B.cpp\n",
                Path(tree), Path("/nonexistent"))
            for run, objs in (("test_x-0", {"A": [3], "B": [0]}), ("test_x-1", {"B": [1]}), ("test_y-2", {"A": [0]})):
                for name, counts in objs.items():
                    p = tmp / run / tree.lstrip("/") / "engine/CMakeFiles/jasperengine.dir/core" / f"{name}.cpp.gcda"
                    p.parent.mkdir(parents=True, exist_ok=True)
                    p.write_bytes(gcda(11, counts))
            files = coverage_map.collect(tmp, Path(tree), deps, tree)
            self.assertEqual(files, {"test_x.py": ["engine/core/A.cpp", "engine/core/B.cpp"], "test_y.py": []})
        finally:
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
        (self.tmp / "map.json").write_text(json.dumps({"commit": sha, "created": time.time(),
                                                       "files": {"test_old.py": ["editor/App.cpp"]}}))
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
