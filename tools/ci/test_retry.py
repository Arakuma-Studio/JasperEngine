#!/usr/bin/env python3
"""Tests of tools/ci/retry.sh (python3 -I tools/ci/test_retry.py).

The command under retry is a script that fails a set number of times, counting its runs in a file;
a fake sleep on PATH records each pause instead of waiting.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "retry.sh"

# fails (with FLAKY_STATUS) until it has run FLAKY_FAILURES times, then succeeds
FLAKY = r'''#!/usr/bin/env bash
runs=$(( $(cat "$FLAKY_COUNT" 2>/dev/null || echo 0) + 1 ))
echo "$runs" > "$FLAKY_COUNT"
echo "args: $*"
[[ $runs -gt $FLAKY_FAILURES ]] || exit "$FLAKY_STATUS"
'''

FAKE_SLEEP = '#!/bin/sh\necho "$1" >> "$SLEEP_LOG"\n'


class Retry(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        bin_dir = root / "bin"
        bin_dir.mkdir()
        for name, text in (("flaky", FLAKY), ("sleep", FAKE_SLEEP)):
            (bin_dir / name).write_text(text)
            (bin_dir / name).chmod(0o755)
        self.count = root / "count"
        self.sleeps = root / "sleeps"
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(("RETRY_", "GITHUB_"))}
        self.env.update(PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}", FLAKY_COUNT=str(self.count),
                        SLEEP_LOG=str(self.sleeps), FLAKY_STATUS="7")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_retry(self, failures: int, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["bash", str(SCRIPT), *args], env={**self.env, "FLAKY_FAILURES": str(failures), **env},
                              capture_output=True, text=True, timeout=60)

    def runs(self) -> int:
        return int(self.count.read_text()) if self.count.exists() else 0

    def pauses(self) -> list[str]:
        return self.sleeps.read_text().split() if self.sleeps.exists() else []

    def retry_lines(self, stderr: str) -> list[str]:
        return [line for line in stderr.splitlines() if "retrying in" in line]

    def test_two_failures_then_success_passes_with_two_logged_retries(self) -> None:
        result = self.run_retry(2, "flaky", "a b", "c")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.runs(), 3)
        lines = self.retry_lines(result.stderr)
        self.assertEqual(len(lines), 2, result.stderr)
        self.assertIn("try 1 of 5", lines[0])
        self.assertIn("exit 7", lines[0])
        self.assertIn("flaky a b c failed", lines[0])
        # growing pauses: try k waits k * RETRY_DELAY (default 5 s)
        self.assertEqual(self.pauses(), ["5", "10"])
        # the command's own output and arguments pass through untouched
        self.assertIn("args: a b c", result.stdout)

    def test_failing_every_time_fails_after_n_tries_with_its_exit_code(self) -> None:
        result = self.run_retry(99, "flaky", RETRY_ATTEMPTS="3", RETRY_DELAY="2")
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(self.runs(), 3)
        self.assertEqual(len(self.retry_lines(result.stderr)), 2, result.stderr)
        self.assertIn("gave up after 3 tries", result.stderr)
        self.assertEqual(self.pauses(), ["2", "4"])

    def test_success_never_retries(self) -> None:
        result = self.run_retry(0, "flaky")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.runs(), 1)
        self.assertEqual(result.stderr, "")
        self.assertEqual(self.pauses(), [])

    def test_retries_are_annotations_on_github_actions(self) -> None:
        result = self.run_retry(1, "flaky", GITHUB_ACTIONS="true")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = self.retry_lines(result.stderr)
        self.assertEqual(len(lines), 1, result.stderr)
        self.assertTrue(lines[0].startswith("::warning::"), lines[0])

    def test_usage_errors(self) -> None:
        self.assertEqual(self.run_retry(0).returncode, 2)
        self.assertEqual(self.run_retry(0, "flaky", RETRY_ATTEMPTS="0").returncode, 2)
        self.assertEqual(self.run_retry(0, "flaky", RETRY_DELAY="x").returncode, 2)
        self.assertEqual(self.runs(), 0)


if __name__ == "__main__":
    unittest.main()
