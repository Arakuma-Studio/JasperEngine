"""Tests of tools/ci/mcp_gate.py (python3 -I tools/ci/test_mcp_gate.py; stdlib only): the allow list's
format, every way a shard fails (a failure, a crash, nothing ran, an unlisted skip, a skip whose own
lane is this one, a planned test not reported once), and the committed allow list parses."""

from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import mcp_gate as gate  # noqa: E402

ALLOWED = {"test_web.py::test_latency": ("needs the web build", "nightly")}


def results(*rows, crashed=()):
    return {"results": [{"name": n, "status": s, "detail": d, "seconds": 1.0} for n, s, d in rows], "crashed": list(crashed)}


class Gate(unittest.TestCase):
    def test_pass_and_allowed_skip(self):
        code, lines = gate.gate(results(("test_a.py::test_x", "pass", ""), ("test_web.py::test_latency", "skip", "no web")),
                                ALLOWED, "mcp")
        self.assertEqual(code, 0, lines)
        self.assertIn("mcp_gate: allowed skip test_web.py::test_latency", "\n".join(lines))
        self.assertIn("2 ran: 1 passed, 0 failed, 1 skipped (1 allowed)", lines[-1])

    def test_a_slow_build_skip_is_allowed_outside_the_nightly_only(self):
        # PR and merge-queue shards run without JASPER_SLOW_BUILD; the nightly builds the games
        slow = ("test_build.py::test_desktop", "skip", "set JASPER_SLOW_BUILD=1 to build and run a real Desktop player")
        code, lines = gate.gate(results(("test_a.py::test_x", "pass", ""), slow), {}, "mcp")
        self.assertEqual(code, 0, lines)
        self.assertIn("allowed skip test_build.py::test_desktop: a slow game build; runs in nightly", "\n".join(lines))
        code, lines = gate.gate(results(("test_a.py::test_x", "pass", ""), slow), {}, "nightly")
        self.assertEqual(code, 1, lines)

    def test_each_failure(self):
        cases = {
            "mcp_gate: FAIL: test_a.py::test_x failed: AssertionError: 3 != 4": results(("test_a.py::test_x", "fail", "Traceback\nAssertionError: 3 != 4")),
            "mcp_gate: FAIL: worker crashed: test_b.py (worker exit 1)": results(("test_a.py::test_x", "pass", ""), crashed=["test_b.py (worker exit 1)"]),
            "mcp_gate: FAIL: no test ran": results(),
            "mcp_gate: FAIL: test_a.py::test_y skipped, and the allow list": results(("test_a.py::test_y", "skip", "needs xdotool")),
        }
        for needle, res in cases.items():
            code, lines = gate.gate(res, ALLOWED, "mcp")
            self.assertEqual(code, 1, needle)
            self.assertIn(needle, "\n".join(lines))
        # an allowed skip in the lane the list says it runs in: it must run there
        code, lines = gate.gate(results(("test_web.py::test_latency", "skip", "no web")), ALLOWED, "nightly")
        self.assertEqual(code, 1)
        self.assertIn("mcp_gate: FAIL: test_web.py::test_latency skipped, but", "\n".join(lines))
        # no results at all
        self.assertEqual(gate.gate(None, ALLOWED, "mcp")[0], 1)
        # every failure is its own line, and none reads as a count
        code, lines = gate.gate(results(("test_a.py::test_x", "fail", "e"), ("test_a.py::test_y", "fail", "e")), ALLOWED, "mcp")
        fails = [line for line in lines if line.startswith("mcp_gate: FAIL: ")]
        self.assertEqual(len(fails), 2)
        self.assertFalse([line for line in fails if line[len("mcp_gate: FAIL: "):][:1].isdigit()])

    def test_the_plan_union(self):
        plan = {"chunks": [{"file": "test_a.py", "tests": ["test_x", "test_y"]}]}
        code, lines = gate.gate(results(("test_a.py::test_x", "pass", "")), ALLOWED, "mcp", plan)
        self.assertEqual(code, 1)
        self.assertIn("mcp_gate: FAIL: test_a.py::test_y was planned and not reported", "\n".join(lines))
        code, lines = gate.gate(results(("test_a.py::test_x", "pass", ""), ("test_a.py::test_y", "pass", ""),
                                        ("test_a.py::test_y", "pass", ""), ("test_b.py::test_z", "pass", "")), ALLOWED, "mcp", plan)
        self.assertIn("mcp_gate: FAIL: test_a.py::test_y was reported 2 times", "\n".join(lines))
        self.assertIn("mcp_gate: FAIL: test_b.py::test_z was reported and not planned", "\n".join(lines))
        code, _ = gate.gate(results(("test_a.py::test_x", "pass", ""), ("test_a.py::test_y", "pass", "")), ALLOWED, "mcp", plan)
        self.assertEqual(code, 0)


class AllowList(unittest.TestCase):
    def test_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "allowed.txt"
            p.write_text("# header\n\ntest_web.py::test_latency  # needs the web build; runs in nightly\n")
            self.assertEqual(gate.load_allowed(p), ({"test_web.py::test_latency": ("needs the web build", "nightly")}, []))
            p.write_text("test_web.py::test_latency  # no lane\ntest_web.py\n")
            self.assertEqual(len(gate.load_allowed(p)[1]), 2)
            r = Path(tmp) / "mcp.json"
            r.write_text(json.dumps(results(("test_a.py::test_x", "pass", ""))))
            with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
                self.assertEqual(gate.main(["--json", str(r), "--allowed", str(p)]), 2)   # a malformed list
                p.write_text("")
                self.assertEqual(gate.main(["--json", str(r), "--allowed", str(p)]), 0)
                self.assertEqual(gate.main(["--json", str(Path(tmp) / "none.json"), "--allowed", str(p)]), 1)

    def test_the_committed_list_parses_and_names_real_tests(self):
        path = HERE / "mcp-allowed-skips.txt"
        allowed, bad = gate.load_allowed(path)
        self.assertEqual(bad, [])
        for name in allowed:
            file, test = name.split("::")
            source = (HERE.parent / "mcp" / "tests" / file).read_text()
            self.assertIn(f"def {test}(", source, name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
