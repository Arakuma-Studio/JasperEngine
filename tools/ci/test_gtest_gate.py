#!/usr/bin/env python3
"""Unit tests of tools/ci/gtest-gate.py (python3 tools/ci/test_gtest_gate.py; CI's unit job).

The XML fixtures are what googletest writes: a run with a failure, a skip and a disabled test, a
--gtest_filter that matched nothing, and a crash that left no file.
"""

from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("gtest_gate", HERE / "gtest-gate.py")
gtest_gate = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = gtest_gate  # (dataclasses look their module up)
spec.loader.exec_module(gtest_gate)


def case(name: str, body: str = "", status: str = "run", result: str = "completed") -> str:
    return f'<testcase name="{name}" status="{status}" result="{result}" time="0" classname="Suite">{body}</testcase>'


def document(*cases: str, tests: int | None = None) -> str:
    count = len(cases) if tests is None else tests
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n<testsuites tests="{count}" failures="0" disabled="0" errors="0" '
            f'time="0" name="AllTests"><testsuite name="Suite" tests="{count}">{"".join(cases)}</testsuite></testsuites>')


PASSED = case("Passes")
FAILED = case("Fails", '<failure message="x" type=""><![CDATA[Test.cpp:3\nExpected equality]]></failure>')
SKIPPED = case("Skips", '<skipped message="no GPU"/>', result="skipped")
DISABLED = case("DISABLED_Quarantined", status="notrun", result="suppressed")


class GtestGateTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="gtest-gate-test-")
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def gate(self, xml: str | None, min_tests: int = 1, max_skipped: int = 0,
             allow_reasons: list[str] | None = None) -> tuple[int, str]:
        path = self.root / "results.xml"
        if xml is not None:
            path.write_text(xml)
        out = io.StringIO()
        return gtest_gate.gate([path], min_tests, max_skipped, out, allow_reasons or []), out.getvalue()

    def test_passing_run_passes(self):
        status, out = self.gate(document(PASSED, PASSED))
        self.assertEqual(status, 0, out)
        self.assertIn("2 ran, 0 failed", out)

    def test_zero_tests_fails(self):
        status, out = self.gate(document())
        self.assertEqual(status, 1)
        self.assertIn("no test ran", out)

    def test_filter_matching_nothing_fails(self):
        # what --gtest_filter=NoSuchSuite.* leaves: an empty <testsuites>
        xml = '<?xml version="1.0" encoding="UTF-8"?>\n<testsuites tests="0" failures="0" disabled="0" errors="0" name="AllTests"></testsuites>'
        status, out = self.gate(xml)
        self.assertEqual(status, 1)
        self.assertIn("no test ran", out)

    def test_one_failure_fails(self):
        status, out = self.gate(document(PASSED, FAILED))
        self.assertEqual(status, 1)
        self.assertIn("FAILED:   Suite.Fails", out)
        self.assertIn("1 tests failed", out)

    def test_error_element_fails(self):
        status, _ = self.gate(document(case("Errors", '<error message="crash"/>')))
        self.assertEqual(status, 1)

    def test_more_skips_than_allowed_fail(self):
        status, out = self.gate(document(PASSED, SKIPPED), max_skipped=0)
        self.assertEqual(status, 1)
        self.assertIn("1 tests skipped, at most 0 allowed", out)

    def test_allowed_skips_pass(self):
        status, out = self.gate(document(PASSED, SKIPPED), max_skipped=1)
        self.assertEqual(status, 0, out)

    def test_skip_with_an_allowed_reason_is_not_counted(self):
        alloc = case("CountsAllocations", '<skipped message="this binary cannot count allocations"/>', result="skipped")
        status, out = self.gate(document(PASSED, alloc), max_skipped=0, allow_reasons=["count allocations"])
        self.assertEqual(status, 0, out)
        self.assertIn("skipped (allowed: count allocations)", out)

    def test_skip_with_another_reason_still_counts(self):
        status, out = self.gate(document(PASSED, SKIPPED), max_skipped=0, allow_reasons=["count allocations"])
        self.assertEqual(status, 1, out)
        self.assertIn("1 tests skipped, at most 0 allowed", out)

    def test_skipped_tests_do_not_count_as_run(self):
        status, out = self.gate(document(SKIPPED), max_skipped=5)
        self.assertEqual(status, 1)
        self.assertIn("no test ran", out)

    def test_too_few_tests_fail(self):
        status, out = self.gate(document(PASSED, PASSED), min_tests=3)
        self.assertEqual(status, 1)
        self.assertIn("only 2 tests ran, at least 3 expected", out)

    def test_disabled_tests_are_listed_not_failed(self):
        status, out = self.gate(document(PASSED, DISABLED))
        self.assertEqual(status, 0, out)
        self.assertIn("disabled: Suite.DISABLED_Quarantined", out)
        self.assertIn("1 disabled", out)

    def test_missing_file_fails(self):
        status, out = self.gate(None)
        self.assertEqual(status, 1)
        self.assertIn("missing", out)

    def test_invalid_xml_fails(self):
        status, out = self.gate("<testsuites><testsuite>")
        self.assertEqual(status, 1)
        self.assertIn("not valid XML", out)

    def test_other_xml_fails(self):
        status, out = self.gate("<junit/>")
        self.assertEqual(status, 1)
        self.assertIn("not googletest XML", out)

    def test_several_files_add_up(self):
        first, second = self.root / "a.xml", self.root / "b.xml"
        first.write_text(document(PASSED))
        second.write_text(document(PASSED))
        out = io.StringIO()
        self.assertEqual(gtest_gate.gate([first, second], 2, 0, out), 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
