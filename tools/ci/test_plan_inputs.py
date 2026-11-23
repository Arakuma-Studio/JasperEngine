"""Tests of tools/ci/plan_inputs.py (python3 -I tools/ci/test_plan_inputs.py; stdlib and git only).

The compact deps dump (what check-changed reads of it selects the same suites), and publish and fetch
against a scratch bare repository standing in for origin: a first publish makes the branch, a second
keeps the files it does not replace, an append keeps the newest lines, a lost race retries, and a
missing branch fetches nothing.
"""

from __future__ import annotations

import gzip
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
sys.path.insert(0, str(HERE.parent))
import plan_inputs as pi  # noqa: E402
import check_changed as cc  # noqa: E402

DEPS = """libs/x/CMakeFiles/x.dir/a.c.o: #deps 2, deps mtime 1 (VALID)
    /src/engine/libs/x/a.c
    /usr/include/stdio.h

engine/tests/CMakeFiles/core.dir/core/FooTest.cpp.o: #deps 4, deps mtime 1 (VALID)
    /src/engine/tests/core/FooTest.cpp
    /usr/include/c++/14/vector
    /src/engine/core/Foo.h
    /src/engine/core/Bar.h

engine/CMakeFiles/engine.dir/core/Foo.cpp.o: #deps 3, deps mtime 1 (VALID)
    /src/engine/core/Foo.cpp
    /src/engine/core/Foo.h
    /usr/include/string.h

"""


def sh(cwd: Path, *args: str) -> str:
    return subprocess.run(list(args), cwd=str(cwd), check=True, capture_output=True, text=True).stdout


class Compact(unittest.TestCase):
    def test_keeps_what_check_changed_reads(self):
        text = pi.compact_deps(DEPS)
        self.assertNotIn("/usr/", text)
        self.assertLess(len(text), len(DEPS))
        root = Path("/nonexistent-root")
        tree = root / "build/linux/core-tests"
        full = cc.parse_ninja_deps(DEPS, tree, root)
        small = cc.parse_ninja_deps(text, tree, root)
        # the test units, with every dependency in the repository
        self.assertEqual(small["engine/tests/CMakeFiles/core.dir/core/FooTest.cpp.o"],
                         ["engine/tests/core/FooTest.cpp", "engine/core/Foo.h", "engine/core/Bar.h"])
        # another object keeps what it includes of the repository (nearest_consumers walks it); a vendored
        # library's object only its source
        self.assertEqual(small["engine/CMakeFiles/engine.dir/core/Foo.cpp.o"], ["engine/core/Foo.cpp", "engine/core/Foo.h"])
        self.assertEqual(small["libs/x/CMakeFiles/x.dir/a.c.o"], ["engine/libs/x/a.c"])
        self.assertEqual(cc.nearest_consumers("engine/core/Foo.cpp", small), cc.nearest_consumers("engine/core/Foo.cpp", full))
        read = lambda p: "TEST(Foo, A) {}\n"
        for changed in (["engine/core/Foo.cpp"], ["engine/core/Bar.h"], ["engine/core/Baz.h"]):
            self.assertEqual(cc.select_units(changed, small, read), cc.select_units(changed, full, read), changed)
        self.assertIn("engine/core/Foo.cpp", text)


class Branch(unittest.TestCase):
    """publish and fetch against a bare repository (origin) from a clone of it."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="plan-inputs-"))
        self.origin = self.tmp / "origin.git"
        sh(self.tmp, "git", "init", "-q", "--bare", "-b", "main", str(self.origin))
        self.work = self.tmp / "work"
        sh(self.tmp, "git", "clone", "-q", str(self.origin), str(self.work))
        sh(self.work, "git", "config", "user.email", "t@example.com")
        sh(self.work, "git", "config", "user.name", "t")
        (self.work / "a.txt").write_text("a\n")
        sh(self.work, "git", "add", "-A")
        sh(self.work, "git", "commit", "-qm", "main")
        sh(self.work, "git", "push", "-q", "origin", "main")
        self.head = sh(self.work, "git", "rev-parse", "HEAD").strip()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def file(self, name: str, text: str) -> Path:
        p = self.tmp / "in" / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return p

    def run_main(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with redirect_stdout(out):
            code = pi.main(list(argv))
        return code, out.getvalue()

    def branch_file(self, path: str) -> bytes:
        return subprocess.run(["git", "--git-dir", str(self.origin), "show", f"{pi.BRANCH}:{path}"],
                              check=True, capture_output=True).stdout

    def test_fetch_without_the_branch_fetches_nothing(self):
        code, out = self.run_main("fetch", "--repo", str(self.work), "--out", str(self.tmp / "got"))
        self.assertEqual(code, 0, out)
        self.assertIn("no ci-plan-inputs branch", out)
        self.assertFalse((self.tmp / "got" / "coverage-map.json").exists())

    def test_publish_then_fetch(self):
        cmap = self.file("coverage-map.json", json.dumps({"commit": self.head, "sources": {}}))
        core = self.file("deps/core-tests.deps", DEPS)
        code, out = self.run_main("publish", "--repo", str(self.work), "--commit", self.head, "--run", "11",
                                  "--set", f"coverage-map={cmap}", "--set", f"deps/core-tests={core}")
        self.assertEqual(code, 0, out)
        # stored compressed (the deps compacted), with a manifest of where each came from
        self.assertEqual(json.loads(gzip.decompress(self.branch_file("coverage-map.json.gz")))["commit"], self.head)
        self.assertEqual(gzip.decompress(self.branch_file("deps/core-tests.deps.gz")).decode(), pi.compact_deps(DEPS))
        manifest = json.loads(self.branch_file("manifest.json"))
        self.assertEqual((manifest["coverage-map"]["commit"], manifest["coverage-map"]["run"]), (self.head, "11"))
        # a second publish replaces only what it names, and the branch stays one commit
        editor = self.file("deps/editor-tests.deps", DEPS)
        code, out = self.run_main("publish", "--repo", str(self.work), "--commit", self.head, "--run", "12",
                                  "--set", f"deps/editor-tests={editor}", "--set", f"deps/core-tests={core}")
        self.assertEqual(code, 0, out)
        log = sh(self.origin, "git", "log", "--format=%H", pi.BRANCH).split()
        self.assertEqual(len(log), 1)
        manifest = json.loads(self.branch_file("manifest.json"))
        self.assertEqual((manifest["coverage-map"]["run"], manifest["deps/editor-tests"]["run"]), ("11", "12"))
        got = self.tmp / "got"
        code, out = self.run_main("fetch", "--repo", str(self.work), "--out", str(got))
        self.assertEqual(code, 0, out)
        self.assertEqual(json.loads((got / "coverage-map.json").read_text())["commit"], self.head)
        self.assertEqual((got / "deps" / "editor-tests.deps").read_text(), pi.compact_deps(DEPS))
        self.assertTrue((got / "deps" / "core-tests.deps").is_file())
        self.assertFalse((got / "deps" / "htmlui-tests.deps").exists())
        self.assertEqual(json.loads((got / "manifest.json").read_text())["deps/core-tests"]["run"], "12")

    def test_append_keeps_the_newest_lines(self):
        for i in range(5):
            line = self.file(f"l{i}.json", json.dumps({"run": i}))
            code, out = self.run_main("publish", "--repo", str(self.work), "--commit", self.head, "--run", str(i),
                                      "--append", f"shadow={line}", "--keep", "3")
            self.assertEqual(code, 0, out)
        lines = self.branch_file("shadow.jsonl").decode().splitlines()
        self.assertEqual([json.loads(x)["run"] for x in lines], [2, 3, 4])
        got = self.tmp / "got"
        self.run_main("fetch", "--repo", str(self.work), "--out", str(got))
        self.assertEqual(len((got / "shadow.jsonl").read_text().splitlines()), 3)

    def test_a_lost_race_publishes_on_top_of_the_winner(self):
        a = self.file("coverage-map.json", "{}")
        b = self.file("deps/core-tests.deps", DEPS)
        real = pi.push
        calls = []

        def racing_push(repo, commit, lease, remote="origin"):
            if not calls:
                # another job publishes between this one's read and its push
                calls.append("other")
                pi.publish(Path(repo), {"coverage-map": a}, {}, 500, self.head, "other")
            calls.append("mine")
            return real(repo, commit, lease, remote)

        pi.push = racing_push
        try:
            code, out = self.run_main("publish", "--repo", str(self.work), "--commit", self.head, "--run", "13",
                                      "--set", f"deps/core-tests={b}")
        finally:
            pi.push = real
        self.assertEqual(code, 0, out)
        manifest = json.loads(self.branch_file("manifest.json"))
        self.assertEqual((manifest["coverage-map"]["run"], manifest["deps/core-tests"]["run"]), ("other", "13"))

    def test_unknown_names_are_refused(self):
        p = self.file("x.json", "{}")
        with redirect_stdout(io.StringIO()):
            for bad in (["--set", f"../escape={p}"], ["--set", f"secrets={p}"], ["--append", f"coverage-map={p}"]):
                with self.assertRaises(SystemExit):
                    sys.stderr, saved = io.StringIO(), sys.stderr
                    try:
                        pi.main(["publish", "--repo", str(self.work), "--commit", self.head, "--run", "1", *bad])
                    finally:
                        sys.stderr = saved


if __name__ == "__main__":
    unittest.main(verbosity=2)
