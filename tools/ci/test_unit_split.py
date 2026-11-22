#!/usr/bin/env python3
"""Tests of tools/ci/unit_split.py (python3 -I tools/ci/test_unit_split.py; stdlib only, no build).

The compile parts (disjoint, covering, the same in every job), the test shards (checked with a
googletest filter matcher: the shards of a filter partition what the filter selects), the ccache
pack, the wait for the other part (a fake GitHub API and clock), and the command line with a fake
Ninja and a fake test binary.
"""

from __future__ import annotations

import fnmatch
import io
import json
import os
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import unit_split as us  # noqa: E402

COMMANDS = """\
cd /src/build/linux/editor-tests && /usr/bin/cmake -E echo gen
/usr/bin/python3 /src/tools/gen.py -c config.json -o generated/Out.h
/usr/bin/ccache /usr/bin/cc -DX=1 -I/src -O2 -g -o libs/a/CMakeFiles/a.dir/a.c.o -c /src/libs/a/a.c
/usr/bin/ccache /usr/bin/c++ -DX=1 -I/src -O2 -g -o CMakeFiles/e.dir/editor/B.cpp.o -c /src/editor/B.cpp
/usr/bin/ccache /usr/bin/c++ -DX=1 -I/src -O2 -g -o CMakeFiles/e.dir/editor/A.cpp.o -c /src/editor/A.cpp
/usr/bin/ccache /usr/bin/c++ -x c++-header -o CMakeFiles/e.dir/cmake_pch.hxx.gch -c /src/build/cmake_pch.hxx.cxx
/usr/bin/ccache /usr/bin/c++ -DX=1 -I/src -O2 -g -o CMakeFiles/t.dir/editor/tests/CTest.cpp.o -c /src/editor/tests/CTest.cpp
/usr/bin/ccache /usr/bin/c++ -DX=1 -I/src -O2 -g -o CMakeFiles/e.dir/editor/A.cpp.o -c /src/editor/A.cpp
: && /usr/bin/c++ -O2 -g CMakeFiles/t.dir/editor/tests/CTest.cpp.o -o jasperengine-editor-tests && :
"""

LISTING = """\
Running main() from TestMain.cpp
Vector3.
  Adds
  Scales
Quaternion.
  SlerpReturnsTheEndPoints
  DISABLED_Flaky
Inst/Param.
  Holds/0  # GetParam() = 1
  Holds/1  # GetParam() = 2
Typed/0.  # TypeParam = int
  Works
DISABLED_Off.
  Never
RenderGpu.
  Draws
  Clears
  Reads
TitleGenGpuSlow.
  Deterministic
"""


def matches(name: str, flt: str) -> bool:
    """googletest's filter: positive patterns, then after the first '-' the negative ones."""
    pos, _, neg = flt.partition("-")
    def any_of(patterns: str) -> bool:
        return any(fnmatch.fnmatchcase(name, p) for p in patterns.split(":") if p)
    return any_of(pos or "*") and not any_of(neg)


def names(suites: dict[str, list[str]]) -> list[str]:
    return [f"{s}.{t}" for s, tests in suites.items() for t in tests]


def runnable(name: str) -> bool:
    suite, test = name.split(".", 1)
    return not suite.startswith("DISABLED_") and not test.startswith("DISABLED_")


class Parts(unittest.TestCase):
    def test_compile_objects_are_the_compile_steps(self):
        self.assertEqual(us.compile_objects(COMMANDS), [
            "CMakeFiles/e.dir/editor/A.cpp.o", "CMakeFiles/e.dir/editor/B.cpp.o",
            "CMakeFiles/t.dir/editor/tests/CTest.cpp.o", "libs/a/CMakeFiles/a.dir/a.c.o"])

    def test_parts_are_disjoint_and_cover_every_object(self):
        objects = [f"o/{i:04d}.cpp.o" for i in range(1001)]
        for n in (1, 2, 3):
            parts = [us.take_part(objects, k, n) for k in range(1, n + 1)]
            self.assertEqual(sorted(sum(parts, [])), objects)
            self.assertLessEqual(max(map(len, parts)) - min(map(len, parts)), 1)
        # every job lists the commands in its own order: the parts do not depend on it
        shuffled = list(reversed(objects))
        self.assertEqual(us.take_part(sorted(shuffled), 1, 2), us.take_part(objects, 1, 2))

    def test_a_part_does_not_wait_for_the_libraries(self):
        # CMake makes a target's custom commands (generated headers) wait for its link libraries, and its
        # objects for the executables it depends on (core-tests: the crash test), all order-only: every
        # part would link every library first (the core tree: 1,955 of 2,577 steps for 1/40 of its
        # objects). The part's own build file drops what a linker makes from the order-only inputs
        ninja = (
            "include CMakeFiles/rules.ninja\n"
            "build libs/box2d/libbox2d.a: C_STATIC_LIBRARY_LINKER__box2d_RelWithDebInfo libs/box2d/a.c.o\n"
            "build libs/lua/liblua.a: C_STATIC_LIBRARY_LINKER__lua_RelWithDebInfo libs/lua/b.c.o\n"
            "build tests/crash: CXX_EXECUTABLE_LINKER__crash_RelWithDebInfo c.o | libs/lua/liblua.a || libs/lua/liblua.a\n"
            "build generated/shaderlib.h | ${cmake_ninja_workdir}generated/shaderlib.h: CUSTOM_COMMAND /src/a.frag "
            "/src/embed.cmake || libs/box2d/libbox2d.a libs/lua/liblua.a\n"
            "  COMMAND = cd /src && cmake -P embed.cmake\n"
            "build gen/tool.h: CUSTOM_COMMAND /src/x.in | tests/crash || libs/lua/liblua.a gen/other.h\n"
            "build CMakeFiles/e.dir/A.cpp.o: CXX_COMPILER__e_RelWithDebInfo /src/A.cpp || cmake_object_order_depends_target_e\n"
            "build cmake_object_order_depends_target_e: phony || cmake_object_order_depends_target_box2d "
            "generated/shaderlib.h tests/crash\n")
        self.assertEqual(us.part_ninja(ninja).splitlines(), [
            "include CMakeFiles/rules.ninja",
            "build libs/box2d/libbox2d.a: C_STATIC_LIBRARY_LINKER__box2d_RelWithDebInfo libs/box2d/a.c.o",
            "build libs/lua/liblua.a: C_STATIC_LIBRARY_LINKER__lua_RelWithDebInfo libs/lua/b.c.o",
            "build tests/crash: CXX_EXECUTABLE_LINKER__crash_RelWithDebInfo c.o | libs/lua/liblua.a",
            "build generated/shaderlib.h | ${cmake_ninja_workdir}generated/shaderlib.h: CUSTOM_COMMAND /src/a.frag /src/embed.cmake",
            "  COMMAND = cd /src && cmake -P embed.cmake",
            # a tool the command runs is a real input: it stays
            "build gen/tool.h: CUSTOM_COMMAND /src/x.in | tests/crash || gen/other.h",
            "build CMakeFiles/e.dir/A.cpp.o: CXX_COMPILER__e_RelWithDebInfo /src/A.cpp || cmake_object_order_depends_target_e",
            "build cmake_object_order_depends_target_e: phony || cmake_object_order_depends_target_box2d generated/shaderlib.h"])

    def test_part_syntax(self):
        self.assertEqual(us.parse_part("1/2"), (1, 2))
        for bad in ("0/2", "3/2", "2", "a/b", "1/0", ""):
            with self.assertRaises(ValueError, msg=bad):
                us.parse_part(bad)


class Shards(unittest.TestCase):
    def suites(self):
        return us.parse_listing(LISTING)

    def test_listing(self):
        s = self.suites()
        self.assertEqual(list(s), ["Vector3", "Quaternion", "Inst/Param", "Typed/0", "DISABLED_Off", "RenderGpu",
                                   "TitleGenGpuSlow"])
        self.assertEqual(s["Inst/Param"], ["Holds/0", "Holds/1"])
        self.assertEqual(s["Typed/0"], ["Works"])

    def check_partition(self, base: str | None, n: int, listed: dict[str, list[str]] | None = None):
        listed = listed if listed is not None else self.suites()
        everything = names(listed)
        wanted = [t for t in everything if matches(t, base or "*") and runnable(t)]
        # what `--gtest_list_tests --gtest_filter=<base>` lists (disabled tests included)
        selected = {s: [t for t in tests if matches(f"{s}.{t}", base or "*")] for s, tests in listed.items()}
        selected = {s: tests for s, tests in selected.items() if tests}
        shards = [us.shard_filter(selected, base, k, n) for k in range(1, n + 1)]
        ran = [[t for t in everything if matches(t, f) and runnable(t)] for f in shards]
        return wanted, ran

    def test_shards_partition_everything(self):
        wanted, ran = self.check_partition(None, 2)
        self.assertEqual(sorted(sum(ran, [])), sorted(wanted))
        self.assertTrue(all(ran), ran)
        # balanced by tests: 9 runnable tests over 2 shards
        self.assertLessEqual(abs(len(ran[0]) - len(ran[1])), 1)

    def test_shards_keep_the_planned_filter(self):
        for base in ("Vector3.*:RenderGpu.*:Inst/*.*", "*-*Gpu*", "*Gpu*-RenderGpu.Clears", "Vector3.*:Quaternion.*-Vector3.Adds"):
            wanted, ran = self.check_partition(base, 2)
            self.assertEqual(sorted(sum(ran, [])), sorted(wanted), base)
            self.assertTrue(all(ran), (base, ran))

    def test_fewer_suites_than_shards_splits_the_tests(self):
        wanted, ran = self.check_partition("RenderGpu.*", 2)
        self.assertEqual(sorted(sum(ran, [])), sorted(wanted))
        self.assertTrue(all(ran), ran)

    def test_fewer_tests_than_shards_runs_them_in_every_shard(self):
        # a shard that runs nothing fails its lane; one planned test runs in both
        wanted, ran = self.check_partition("TitleGenGpuSlow.*", 2)
        self.assertEqual(ran, [wanted, wanted])

    def test_nothing_selected_is_an_error(self):
        with self.assertRaises(ValueError):
            us.shard_filter({}, "Nothing.*", 1, 2)

    def test_one_shard_is_the_filter(self):
        self.assertEqual(us.shard_filter(self.suites(), "Vector3.*", 1, 1), "Vector3.*")

    def test_args_replace_the_filter_and_keep_the_rest(self):
        rest, flt = us.split_filter(["--gtest_repeat=2", "--gtest_filter=A.*", "--gtest_filter=B.*", "--x"])
        self.assertEqual((rest, flt), (["--gtest_repeat=2", "--x"], "B.*"))
        self.assertEqual(us.split_filter([]), ([], None))


class Pack(unittest.TestCase):
    def test_pack_takes_what_the_part_wrote(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cache = root / "ccache"
            for rel in ("a/b/oldR", "a/c/oldM", "a/c/grownM"):
                (cache / rel).parent.mkdir(parents=True, exist_ok=True)
                (cache / rel).write_text("old")
            before = us.snapshot(cache)
            # a hit refreshes its result's mtime (ccache's LRU): not this part's object
            os.utime(cache / "a/b/oldR")
            # a miss under a known manifest adds its entry to it
            (cache / "a/c/grownM").write_text("old+entry")
            for rel in ("a/b/newR", "a/b/newM", "c/d/new2R", "a/stats", "a/b/x.lock", "ccache.conf", "tmp/t.tmp"):
                (cache / rel).parent.mkdir(parents=True, exist_ok=True)
                (cache / rel).write_text("new")
            out = root / "part.tar"
            got = us.pack(cache, before, out)
            self.assertEqual(got, {"files": 4, "results": 2})
            with tarfile.open(out) as t:
                self.assertEqual(sorted(t.getnames()), ["a/b/newM", "a/b/newR", "a/c/grownM", "c/d/new2R"])


class Wait(unittest.TestCase):
    """us.wait_for over a scripted API: each poll reads the next state."""

    def run_wait(self, states, budget=600.0, poll=20.0, results=500, objects=1000):
        clock = [1000.0]
        calls = {"n": 0}

        def api(path):
            i = min(calls["n"] // 2, len(states) - 1)
            calls["n"] += 1
            artifact, job = states[i]
            if "/artifacts" in path:
                return {"total_count": 1 if artifact else 0, "artifacts": [{"name": "unit-editor-ccache-2"}] if artifact else []}
            return {"jobs": [{"name": "unit (core)", "status": "completed"}]
                    + ([{"name": "unit (editor-2)", "status": job}] if job else [])}

        def sleep(s):
            clock[0] += s

        found, why = us.wait_for(api, "o/r", "77", "unit-editor-ccache-2", "unit (editor-2)", budget, poll,
                                 results, objects, now=lambda: clock[0], sleep=sleep)
        return found, why, clock[0] - 1000.0

    def test_found_at_once(self):
        found, why, waited = self.run_wait([(True, "in_progress")])
        self.assertEqual((found, waited), (True, 0.0))

    def test_waits_while_the_other_part_compiles(self):
        found, why, waited = self.run_wait([(False, "in_progress")] * 3 + [(True, "in_progress")])
        self.assertTrue(found)
        self.assertEqual(waited, 60.0)

    def test_does_not_wait_for_a_part_that_has_not_started(self):
        for job in ("queued", "waiting", None):
            found, why, waited = self.run_wait([(False, job)])
            self.assertEqual((found, waited), (False, 0.0), job)
            self.assertIn("not started", why)

    def test_stops_when_the_other_part_ended_without_its_objects(self):
        found, why, waited = self.run_wait([(False, "in_progress"), (False, "completed")])
        self.assertFalse(found)
        self.assertIn("ended", why)

    def test_waits_no_longer_than_the_part_took(self):
        found, why, waited = self.run_wait([(False, "in_progress")], budget=100.0)
        self.assertFalse(found)
        self.assertLessEqual(waited, 100.0)
        self.assertIn("compile the rest", why)

    def test_a_warm_part_does_not_wait(self):
        # this part compiled under a tenth of its objects: the other part's hit the restored cache too
        found, why, waited = self.run_wait([(False, "in_progress")], results=50, objects=1000)
        self.assertEqual((found, waited), (False, 0.0))
        self.assertIn("warm", why)

    def test_an_api_error_does_not_wait(self):
        def api(path):
            raise us.ApiError("HTTP 403")
        found, why = us.wait_for(api, "o/r", "77", "a", "j", 600.0, 20.0, 500, 1000, now=time.time, sleep=lambda s: None)
        self.assertFalse(found)
        self.assertIn("403", why)


def fake(path: Path, body: str) -> Path:
    path.write_text("#!/usr/bin/env bash\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class Cli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="unit-split-")
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *args):
        return subprocess.run([sys.executable, "-I", str(HERE / "unit_split.py"), *args], capture_output=True, text=True)

    def test_objects(self):
        commands = self.dir / "commands.txt"
        commands.write_text(COMMANDS)
        build = self.dir / "build"
        build.mkdir()
        (build / "build.ninja").write_text("build libs/a.a: C_STATIC_LIBRARY_LINKER__a x.o\nbuild g.h: CUSTOM_COMMAND x || libs/a.a\n")
        ninja = fake(self.dir / "ninja", f'[[ "$*" == "-C {build} -t commands jasperengine-editor-tests" ]] || exit 9\ncat "{commands}"\n')
        r = self.run_cli("objects", "--ninja", str(ninja), "--build-dir", str(build), "--target", "jasperengine-editor-tests",
                         "--part", "2/2")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.split(), ["CMakeFiles/e.dir/editor/B.cpp.o", "libs/a/CMakeFiles/a.dir/a.c.o"])
        self.assertEqual((build / "unit-part.ninja").read_text(), "build libs/a.a: C_STATIC_LIBRARY_LINKER__a x.o\nbuild g.h: CUSTOM_COMMAND x\n")

    def test_objects_of_a_tree_without_compile_steps_fail(self):
        ninja = fake(self.dir / "ninja", "echo ': && /usr/bin/c++ -o x'\n")
        r = self.run_cli("objects", "--ninja", str(ninja), "--build-dir", "b", "--target", "t", "--part", "1/2")
        self.assertEqual(r.returncode, 1)
        self.assertIn("no compile step", r.stderr)

    def test_args(self):
        listing = self.dir / "listing.txt"
        listing.write_text(LISTING)
        binary = fake(self.dir / "tests", f'[[ "$1" == --gtest_list_tests ]] || exit 9\n'
                                          f'[[ "$2" == --gtest_filter=*-*Gpu* ]] || exit 8\ncat "{listing}"\n')
        outs = []
        for k in (1, 2):
            r = self.run_cli("args", "--binary", str(binary), "--shard", f"{k}/2", "--", "--gtest_repeat=1",
                             "--gtest_filter=*-*Gpu*")
            self.assertEqual(r.returncode, 0, r.stderr)
            args = r.stdout.split()
            self.assertEqual(args[0], "--gtest_repeat=1")
            self.assertTrue(args[1].startswith("--gtest_filter=*-*Gpu*:"), args)
            outs.append(args[1].split("=", 1)[1])
        everything = names(us.parse_listing(LISTING))
        wanted = [t for t in everything if matches(t, "*-*Gpu*") and runnable(t)]
        ran = [[t for t in everything if matches(t, f) and runnable(t)] for f in outs]
        self.assertEqual(sorted(sum(ran, [])), sorted(wanted))
        self.assertTrue(all(ran))

    def test_args_of_a_binary_that_lists_nothing_fail(self):
        binary = fake(self.dir / "tests", "echo 'Running main()'\n")
        r = self.run_cli("args", "--binary", str(binary), "--shard", "1/2", "--", "--gtest_filter=Nope.*")
        self.assertEqual(r.returncode, 1)
        self.assertIn("no test", r.stderr)

    def test_snapshot_then_pack_writes_outputs(self):
        cache = self.dir / "ccache"
        (cache / "a").mkdir(parents=True)
        (cache / "a" / "oldR").write_text("x")
        before = self.dir / "before.json"
        r = self.run_cli("snapshot", "--ccache", str(cache), "--out", str(before))
        self.assertEqual(r.returncode, 0, r.stderr)
        (cache / "a" / "xR").write_text("x")
        objects = self.dir / "objects.txt"
        objects.write_text("a.o\nb.o\n")
        out_file = self.dir / "out.txt"
        r = self.run_cli("pack", "--ccache", str(cache), "--before", str(before), "--out", str(self.dir / "p.tar"),
                         "--objects", str(objects), "--github-output", str(out_file))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(out_file.read_text().split(), ["files=1", "results=1", "objects=2"])
        # an empty cache (nothing restored) snapshots as nothing
        r = self.run_cli("snapshot", "--ccache", str(self.dir / "none"), "--out", str(before))
        self.assertEqual((r.returncode, json.loads(before.read_text())), (0, {}))


if __name__ == "__main__":
    unittest.main(verbosity=2)
