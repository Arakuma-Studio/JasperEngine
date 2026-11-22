"""Tests of tools/ci/mcp_timings.py (python3 -I tools/ci/test_mcp_timings.py; stdlib only, no gh, no editor).

On synthetic CI results and scratch test files: the robust median (an outlier dropped, one sample not
enough), the band (a small move kept, a large one taken, a test count change taken), a file gone from
the suite dropped, a TESTS_INDEPENDENT file timed per test, a crashed chunk's partial file left out,
which runs fetch takes (no fork, no nightly), the file's layout and tools/mcp/shard.py reading it back,
and the check of the committed file.
"""

from __future__ import annotations

import io
import json
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import mcp_timings as mt  # noqa: E402

FILES = {"test_whole.py": "CI = True\n", "test_free.py": 'CI = True\nTESTS_INDEPENDENT = "each owns its scene"\n',
         "test_other.py": "CI = True\n"}


def row(name, seconds, status="pass"):
    return {"name": name, "status": status, "seconds": seconds, "detail": ""}


def run(whole=(100.0, 100.0), free=(50.0, 60.0), other=10.0):
    return [row("test_whole.py::test_a", whole[0]), row("test_whole.py::test_b", whole[1]),
            row("test_free.py::test_0", free[0]), row("test_free.py::test_1", free[1]), row("test_other.py::test_x", other)]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="mcp-timings-test-"))
        self.tests = self.tmp / "tests"
        self.tests.mkdir()
        for name, text in FILES.items():
            (self.tests / name).write_text(text)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def refit(self, runs, old=None):
        return mt.refit(old or mt.empty(), {str(1000 + i): r for i, r in enumerate(runs)}, self.tests)


class Robust(unittest.TestCase):
    def test_the_median_without_its_outliers(self):
        self.assertEqual(mt.robust([100.0, 110.0, 90.0, 1000.0]), 100.0)    # (1000: a timeout)
        self.assertEqual(mt.robust([5.0, 30.0, 6.0]), 6.0)                  # (24 s off: within the floor, kept)
        self.assertIsNone(mt.robust([100.0]))                               # one run says nothing
        self.assertIsNone(mt.robust([100.0, 1000.0, 1.0]))                  # a timeout and a crash: one left

    def test_the_band(self):
        self.assertFalse(mt.outside_band(110.0, 100.0))    # within 25 %
        self.assertTrue(mt.outside_band(130.0, 100.0))
        self.assertFalse(mt.outside_band(20.0, 10.0))      # 10 s: within BAND_SECONDS
        self.assertTrue(mt.outside_band(40.0, 10.0))


class Refit(Base):
    def test_a_first_fit(self):
        data, changes = self.refit([run(), run(whole=(110.0, 110.0)), run(whole=(2000.0, 2000.0))])
        self.assertEqual(data["files"]["test_whole.py"], {"seconds": 210.0, "tests": 2})   # 4000: an outlier
        # an independent file per test as well
        self.assertEqual(data["tests"], {"test_free.py::test_0": 50.0, "test_free.py::test_1": 60.0})
        self.assertEqual(data["files"]["test_free.py"], {"seconds": 110.0, "tests": 2})
        self.assertEqual(data["default_test_seconds"], round((210.0 + 110.0 + 10.0) / 5, 1))
        self.assertEqual(data["runs"], ["1000", "1001", "1002"])
        self.assertTrue(changes)

    def test_one_run_changes_nothing(self):
        data, changes = self.refit([run()])
        self.assertEqual((data["files"], data["tests"], changes), ({}, {}, []))

    def test_the_band_keeps_a_small_move_and_takes_a_large_one(self):
        old, _ = self.refit([run(), run()])
        same, changes = self.refit([run(whole=(110.0, 110.0)), run(whole=(110.0, 110.0))], old)
        self.assertEqual((same, changes), (old, []))
        moved, changes = self.refit([run(whole=(300.0, 300.0)), run(whole=(300.0, 300.0))], old)
        self.assertEqual(moved["files"]["test_whole.py"]["seconds"], 600.0)
        self.assertEqual([c for c in changes if "test_whole.py" in c], ["~ test_whole.py 200s -> 600s (2 -> 2 tests)"])
        # the others stayed in the band: as they were
        self.assertEqual(moved["files"]["test_other.py"], old["files"]["test_other.py"])

    def test_a_test_count_change_and_a_file_gone(self):
        old, _ = self.refit([run(), run()])
        (self.tests / "test_other.py").unlink()
        three = run() + [row("test_whole.py::test_c", 1.0)]
        data, changes = self.refit([three, three], old)
        self.assertEqual(data["files"]["test_whole.py"], {"seconds": 201.0, "tests": 3})
        self.assertNotIn("test_other.py", data["files"])
        self.assertIn("- test_other.py (no longer in the suite)", changes)

    def test_a_test_no_run_reported_goes(self):
        old, _ = self.refit([run(), run()])
        renamed = [dict(r, name=r["name"].replace("test_free.py::test_1", "test_free.py::test_2")) for r in run()]
        data, changes = self.refit([renamed, renamed], old)
        self.assertEqual(sorted(data["tests"]), ["test_free.py::test_0", "test_free.py::test_2"])
        self.assertIn("- test_free.py::test_1 (no run reported it)", changes)

    def test_a_crashed_chunk_and_a_failure(self):
        partial = [r for r in run(whole=(150.0, 150.0)) if r["name"] != "test_whole.py::test_b"]
        failed = run(whole=(130.0, 70.0))
        failed[1]["status"] = "fail"
        data, _ = self.refit([run(), partial, partial, failed])
        # the partial runs of test_whole.py left out (with them: 175 s); the failure counted (its shard paid for it)
        self.assertEqual(data["files"]["test_whole.py"], {"seconds": 200.0, "tests": 2})

    def test_dump_is_one_entry_a_line_and_shard_reads_it(self):
        data, _ = self.refit([run(), run()])
        text = mt.dump(data)
        self.assertEqual(json.loads(text), data)
        self.assertIn('\n  "test_free.py": {"seconds": 110.0, "tests": 2},\n', text)
        path = self.tmp / "ci-timings.json"
        path.write_text(text)
        times = mt.shard.load_timings([path])
        chunks = mt.shard.make_chunks(["test_whole.py::test_a", "test_whole.py::test_b", "test_free.py::test_0",
                                       "test_new.py::test_y"], times, 10000.0, self.tests)
        self.assertEqual({c.file: c.seconds - mt.shard.CHUNK_OVERHEAD for c in chunks},
                         {"test_whole.py": 200.0, "test_free.py": 50.0, "test_new.py": data["default_test_seconds"]})
        self.assertEqual(mt.problems(data, self.tests), [])
        (self.tests / "test_other.py").unlink()
        self.assertEqual(mt.problems(data, self.tests),
                         ["test_other.py: no such test file (tools/ci/mcp_timings.py refit drops it)"])


class CommandLine(Base):
    def test_refit_writes_only_a_change(self):
        results = self.tmp / "results"
        for i in range(2):
            (results / str(1000 + i)).mkdir(parents=True)
            (results / str(1000 + i) / "test-results-mcp-1.json").write_text(json.dumps({"results": run()}))
        (results / "not-a-run").mkdir()
        path = self.tmp / "ci-timings.json"
        real = mt.TESTS
        mt.TESTS = self.tests
        try:
            with redirect_stdout(io.StringIO()):
                self.assertEqual(mt.main(["refit", "--results", str(results), "--timings", str(path)]), 0)
                self.assertFalse(path.exists())                      # without --write: only said
                self.assertEqual(mt.main(["refit", "--results", str(results), "--timings", str(path), "--write"]), 0)
                first = path.read_text()
                self.assertEqual(mt.main(["refit", "--results", str(results), "--timings", str(path), "--write"]), 0)
                self.assertEqual(path.read_text(), first)            # nothing moved: the same bytes
                self.assertEqual(mt.main(["check", "--timings", str(path)]), 0)
                path.write_text("{")
                self.assertEqual(mt.main(["check", "--timings", str(path)]), 1)
                self.assertEqual(mt.main(["refit", "--results", str(self.tmp / "none"), "--timings", str(path)]), 2)
        finally:
            mt.TESTS = real

    def test_fetch_takes_this_repositorys_pr_queue_and_dispatch_runs(self):
        repo = "owner/name"
        ok = {"event": "pull_request", "head_repository": {"full_name": repo}}
        self.assertTrue(mt.wanted(ok, repo))
        self.assertTrue(mt.wanted(dict(ok, event="merge_group"), repo))
        self.assertTrue(mt.wanted(dict(ok, event="workflow_dispatch"), repo))
        self.assertFalse(mt.wanted(dict(ok, head_repository={"full_name": "fork/name"}), repo))
        self.assertFalse(mt.wanted(dict(ok, event="schedule"), repo))
        self.assertFalse(mt.wanted(dict(ok, event="push"), repo))

    def test_the_committed_file_is_fine(self):
        with redirect_stdout(io.StringIO()) as out:
            code = mt.main(["check"])
        self.assertEqual(code, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
