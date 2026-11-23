#!/usr/bin/env python3
"""Tests of .github/actions/free-disk/free-disk.sh (python3 -I tools/ci/test_free_disk.py).

The script runs against a fake df, sudo and docker on PATH: df reports FAKE_AVAIL_GB free (and
FAKE_AVAIL_AFTER_GB once something was removed), sudo and docker only log their arguments, so
nothing is deleted.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / ".github" / "actions" / "free-disk" / "free-disk.sh"

# df -BG --output=avail / prints a header and the free space in GB
FAKE_DF = r'''#!/usr/bin/env bash
echo "df $*" >> "$FAKE_LOG"
avail="$FAKE_AVAIL_GB"
if [[ -n "${FAKE_AVAIL_AFTER_GB:-}" ]] && grep -q '^sudo ' "$FAKE_LOG"; then avail="$FAKE_AVAIL_AFTER_GB"; fi
if [[ " $* " == *" --output=avail "* ]]; then
    echo " Avail"
    echo "  ${avail}G"
else
    echo "Filesystem      Size  Used Avail Use% Mounted on"
    echo "/dev/root       145G   58G  ${avail}G  40% /"
fi
'''

FAKE_LOGGER = r'''#!/usr/bin/env bash
echo "$(basename "$0") $*" >> "$FAKE_LOG"
'''


class FreeDisk(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.bin = root / "bin"
        self.bin.mkdir()
        for name, body in (("df", FAKE_DF), ("sudo", FAKE_LOGGER), ("docker", FAKE_LOGGER)):
            tool = self.bin / name
            tool.write_text(body)
            tool.chmod(0o755)
        self.log = root / "log"
        self.log.touch()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_script(self, avail: int, need: str | None, after: int | None = None,
                   environment: str = "github-hosted") -> tuple[subprocess.CompletedProcess, list[str]]:
        env = {
            "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}",
            "FAKE_LOG": str(self.log),
            "FAKE_AVAIL_GB": str(avail),
            "RUNNER_OS": "Linux",
            "RUNNER_ENVIRONMENT": environment,
        }
        if need is not None:
            env["NEED"] = need
        if after is not None:
            env["FAKE_AVAIL_AFTER_GB"] = str(after)
        result = subprocess.run(["bash", str(SCRIPT)], env=env, capture_output=True, text=True, timeout=60)
        return result, self.log.read_text().splitlines()

    def removals(self, calls: list[str]) -> list[str]:
        return [c for c in calls if c.startswith(("sudo ", "docker "))]

    def test_enough_space_removes_nothing(self) -> None:
        result, calls = self.run_script(avail=87, need="60")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.removals(calls), [])
        self.assertIn("87 GB free", result.stdout)
        self.assertIn("needs 60", result.stdout)

    def test_exactly_the_need_is_enough(self) -> None:
        result, calls = self.run_script(avail=60, need="60")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.removals(calls), [])

    def test_short_space_removes_the_preinstalled_tools_and_images(self) -> None:
        result, calls = self.run_script(avail=20, need="60", after=80)
        self.assertEqual(result.returncode, 0, result.stderr)
        removed = self.removals(calls)
        self.assertIn("sudo rm -rf /usr/local/lib/android", removed)
        self.assertIn("sudo rm -rf /usr/share/dotnet", removed)
        self.assertIn("docker image prune --all --force", removed)
        self.assertNotIn("::warning::", result.stdout)

    def test_a_self_hosted_runner_is_never_touched(self) -> None:
        # an overflow runner is a person's machine (docs/testing.md "Overflow runners")
        for need in ("60", None):
            result, calls = self.run_script(avail=5, need=need, environment="self-hosted")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self.removals(calls), [])
            self.assertIn("self-hosted runner: nothing removed", result.stdout)
        action = (SCRIPT.parent / "action.yml").read_text()
        self.assertIn("runner.environment != 'self-hosted'", action)

    def test_still_short_after_removal_warns(self) -> None:
        result, _ = self.run_script(avail=20, need="100", after=80)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("::warning::", result.stdout)
        self.assertIn("80 GB free", result.stdout)

    def test_no_need_given_always_removes(self) -> None:
        result, calls = self.run_script(avail=120, need=None)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("sudo rm -rf /usr/share/dotnet", self.removals(calls))

    def test_need_must_be_a_whole_number(self) -> None:
        result, calls = self.run_script(avail=87, need="lots")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("NEED", result.stdout + result.stderr)
        self.assertEqual(self.removals(calls), [])


if __name__ == "__main__":
    unittest.main()
