"""Self-test of tools/mcp/shard.py (stdlib only: python3 -I tools/mcp/test_shard.py, no editor).

On scratch test files and synthetic results: chunks (a whole file, an independent file split, some
tests of a file independent), longest processing time first across shards, the pin of the files that
build games, the GPU lane, the union check (a lost test, a duplicate, an unknown one each fail it)
and the merged report and its exit code (a failure, a crash, a missing result, an unexpected skip).
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shard  # noqa: E402

FILES = {
    "test_whole.py": "EDITOR = {}\n",
    "test_free.py": 'TESTS_INDEPENDENT = "each owns its scene"\n',
    "test_some.py": 'TESTS_INDEPENDENT = ("the models own their scenes", ["test_model_*"])\n',
    "test_game.py": 'PREBUILT_PLAYERS = ["glcore-release"]\n',
    "test_build.py": "\n",
}
TESTS = (["test_whole.py::test_a", "test_whole.py::test_b"] + [f"test_free.py::test_{i}" for i in range(6)] +
         ["test_some.py::test_setup", "test_some.py::test_model_x", "test_some.py::test_model_y", "test_some.py::test_end"] +
         ["test_game.py::test_run", "test_build.py::test_compile"])
TIMES = {t: 100.0 for t in TESTS}


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="shard-test-"))
        for name, text in FILES.items():
            (self.tmp / name).write_text(text)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


class Chunks(Base):
    def test_whole_split_and_partial(self):
        chunks = shard.make_chunks(TESTS, TIMES, 250.0, self.tmp)
        by_file = {}
        for c in chunks:
            by_file.setdefault(c.file, []).append(c.tests)
        self.assertEqual(by_file["test_whole.py"], [["test_a", "test_b"]])
        self.assertEqual(by_file["test_free.py"], [["test_0", "test_1"], ["test_2", "test_3"], ["test_4", "test_5"]])
        # the tests that depend on each other stay together, in order; the independent ones apart
        self.assertEqual(by_file["test_some.py"], [["test_setup", "test_end"], ["test_model_x", "test_model_y"]])
        self.assertEqual(sorted(n for c in chunks for n in c.names()), sorted(TESTS))

    def test_small_independent_file_stays_whole(self):
        chunks = [c for c in shard.make_chunks(TESTS, TIMES, 10000.0, self.tmp) if c.file == "test_free.py"]
        self.assertEqual(len(chunks), 1)

    def test_a_test_with_no_time_counts_the_default(self):
        chunks = shard.make_chunks(["test_whole.py::test_new"], {}, 600.0, self.tmp)
        self.assertEqual(chunks[0].seconds, shard.DEFAULT_SECONDS + shard.CHUNK_OVERHEAD)


class Plans(Base):
    def shards(self, *specs):
        return [shard.parse_shard(s) for s in specs]

    def test_lpt_balances_and_the_union_holds(self):
        tests = [f"test_free.py::test_{i}" for i in range(6)]
        data = shard.plan(tests, {t: 100.0 * (i + 1) for i, t in enumerate(tests)}, self.shards("A:1", "B:1"), 1.0,
                          tests_dir=self.tmp)
        self.assertEqual(shard.union_problems(tests, shard.planned_names(data)), [])
        a, b = (data["shards"][s]["estimate_seconds"] for s in "AB")
        self.assertLess(abs(a - b), 150.0)   # 2100 s + overhead, split about evenly
        self.assertEqual(data["estimate_seconds"], max(a, b))

    def test_the_files_that_build_games_are_pinned(self):
        data = shard.plan(TESTS, TIMES, self.shards("A:4", "B:4"), 250.0, "B", self.tmp)
        where = {c["file"]: name for name, s in data["shards"].items() for c in s["chunks"]}
        self.assertEqual((where["test_game.py"], where["test_build.py"]), ("B", "B"))
        with self.assertRaises(SystemExit):
            shard.plan(TESTS, TIMES, self.shards("A:4"), 250.0, "H", self.tmp)

    def test_the_gpu_lane(self):
        classes = {"test_whole.py": "gpu", "test_some.py::test_setup": "gpu"}
        data = shard.plan(TESTS, TIMES, self.shards("A:8", "H:1:gpu:gpu_speed=0.01"), 250.0, "", self.tmp, classes)
        lanes = {(c["file"], tuple(c["tests"])): c["lane"] for s in data["shards"].values() for c in s["chunks"]}
        self.assertEqual(lanes[("test_whole.py", ("test_a", "test_b"))], "gpu")
        # a chunk with one test that is not GPU-OK stays off the GPU
        self.assertEqual(lanes[("test_some.py", ("test_setup", "test_end"))], "")
        self.assertTrue(all(lane == "" for (f, _), lane in lanes.items() if f != "test_whole.py"))

    def test_shard_specs(self):
        s = shard.parse_shard("H:8:gpu:speed=0.5:gpu_speed=0.1")
        self.assertEqual((s.name, s.workers, s.gpu, s.speed, s.gpu_speed), ("H", 8, True, 0.5, 0.1))
        for bad in ("A", "A:x", "A:0", "A:2:what"):
            with self.assertRaises(SystemExit):
                shard.parse_shard(bad)


class Union(unittest.TestCase):
    def test_lost_duplicate_unknown(self):
        self.assertEqual(shard.union_problems(["a", "b"], ["b", "a"]), [])
        self.assertIn("MISSING b (not planned)", shard.union_problems(["a", "b"], ["a"]))
        self.assertTrue(any(p.startswith("DUPLICATE a") for p in shard.union_problems(["a"], ["a", "a"])))
        self.assertTrue(any(p.startswith("UNKNOWN c") for p in shard.union_problems(["a"], ["a", "c"])))
        self.assertTrue(any("twice" in p for p in shard.union_problems(["a", "a"], ["a"])))


class Merge(unittest.TestCase):
    PLAN = {"shards": {"A": {"chunks": [{"file": "t.py", "tests": ["a", "b"]}]},
                       "B": {"chunks": [{"file": "u.py", "tests": ["c"]}]}}}

    def r(self, name, status="pass", seconds=10.0):
        return {"name": name, "status": status, "seconds": seconds, "detail": ""}

    def merged(self, a, b, **kw):
        return shard.merge(self.PLAN, [{"results": a, "crashed": kw.get("crashed", []), "seconds": 100.0},
                                       {"results": b, "crashed": [], "seconds": 300.0}], kw.get("allow", []))

    def test_all_green(self):
        code, text = self.merged([self.r("t.py::a"), self.r("t.py::b")], [self.r("u.py::c")])
        self.assertEqual(code, 0, text)
        self.assertIn("3 passed, 0 failed, 0 skipped in 300.0s", text)

    def test_every_way_to_fail(self):
        fine = [self.r("t.py::a"), self.r("t.py::b")]
        self.assertEqual(self.merged(fine[:1] + [self.r("t.py::b", "fail")], [self.r("u.py::c")])[0], 1)   # a failure
        self.assertEqual(self.merged(fine, [])[0], 1)                                                       # a lost test
        self.assertEqual(self.merged(fine, [self.r("u.py::c"), self.r("u.py::c")])[0], 1)                   # run twice
        self.assertEqual(self.merged(fine, [self.r("u.py::c"), self.r("u.py::d")])[0], 1)                   # unknown
        self.assertEqual(self.merged(fine, [self.r("u.py::c")], crashed=["t.py (worker exit 9)"])[0], 1)    # a crash
        skipped = [self.r("u.py::c", "skip")]
        self.assertEqual(self.merged(fine, skipped)[0], 1)                                                  # a skip
        self.assertEqual(self.merged(fine, skipped, allow=["u.py::c"])[0], 0)                               # expected

    def test_the_command_line(self):
        tmp = Path(tempfile.mkdtemp(prefix="shard-cli-"))
        try:
            (tmp / "list.txt").write_text("run_tests: x\nt.py::a\nt.py::b\nu.py::c\n")
            (tmp / "plan.json").write_text(json.dumps(self.PLAN))
            self.assertEqual(shard.main(["check", "--tests", str(tmp / "list.txt"), str(tmp / "plan.json")]), 0)
            (tmp / "list.txt").write_text("t.py::a\nt.py::b\nu.py::c\nu.py::lost\n")
            self.assertEqual(shard.main(["check", "--tests", str(tmp / "list.txt"), str(tmp / "plan.json")]), 1)
            # a shard's missing results file: a crash in the report
            (tmp / "a.json").write_text(json.dumps({"results": [self.r("t.py::a"), self.r("t.py::b"), self.r("u.py::c")],
                                                    "crashed": [], "seconds": 1.0}))
            self.assertEqual(shard.main(["merge", "--plan", str(tmp / "plan.json"), str(tmp / "a.json")]), 0)
            self.assertEqual(shard.main(["merge", "--plan", str(tmp / "plan.json"), str(tmp / "a.json"), str(tmp / "gone.json")]), 1)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
