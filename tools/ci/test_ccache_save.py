"""Tests of .github/actions/ccache-save/trim.sh's save decision (python3 -I tools/ci/test_ccache_save.py).

The script runs on a scratch ccache directory against a fake docker on PATH (ccache runs in the lane's
image on Linux), and says save=true or save=false: a push to main or a schedule saves, and so does a
Blacksmith runner on main whatever the event (its own cache store, which GitHub's runners never fill);
a Blacksmith job of another branch, a pull request or the merge queue only restores.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github" / "actions" / "ccache-save" / "trim.sh"
ACTION = SCRIPT.parent / "action.yml"


class TrimSave(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        (self.bin / "docker").write_text('#!/bin/sh\necho "docker $*" >> "$FAKE_LOG"\n')
        (self.bin / "docker").chmod(0o755)
        self.log = self.tmp / "log"
        self.log.touch()
        self.cache = self.tmp / "build" / ".cache" / "ccache"
        self.cache.mkdir(parents=True)
        self.marker = self.tmp / "marker"
        self.marker.touch()
        past = time.time() - 60
        os.utime(self.marker, (past, past))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def trim(self, event: str, ref: str, runner: str = "GitHub Actions 1000000001", new: bool = True) -> str:
        if new:
            (self.cache / "a" / "b").mkdir(parents=True, exist_ok=True)
            (self.cache / "a" / "b" / "object.o").write_text("x")
        out = self.tmp / "output"
        out.write_text("")
        env = {"PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}", "FAKE_LOG": str(self.log), "LANE": "linux",
               "EVENT": event, "REF": ref, "RUNNER_NAME": runner, "RUNNER_OS": "Linux", "GITHUB_OUTPUT": str(out),
               "JASPER_CCACHE_LANE": "linux", "JASPER_CCACHE_MARKER": str(self.marker), "JASPER_CCACHE_CAP": "0.95G",
               "JASPER_CCACHE_START": str(int(time.time()) - 60), "JASPER_CCACHE_RESTORED_SIZE": "4.0K",
               "JASPER_DOCKER_IMAGE": "ghcr.io/arakuma-studio/jasperengine-linux-build:x"}
        r = subprocess.run(["bash", str(SCRIPT)], cwd=self.tmp, env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.out = r.stdout
        return out.read_text().strip()

    def test_github_runners_save_on_main_only(self):
        self.assertEqual(self.trim("push", "refs/heads/main"), "save=true")
        self.assertEqual(self.trim("schedule", "refs/heads/main"), "save=true")
        for event, ref in (("pull_request", "refs/pull/1/merge"), ("merge_group", "refs/heads/gh-readonly-queue/main/pr-1-a"),
                           ("workflow_dispatch", "refs/heads/main"), ("push", "refs/heads/feature")):
            self.assertEqual(self.trim(event, ref), "save=false", (event, ref))

    def test_a_blacksmith_runner_saves_on_main_whatever_the_event(self):
        # (run 45291101229 restored nothing there: no GitHub runner writes Blacksmith's store)
        bs = "blacksmith-4vcpu-ubuntu-2404-33rhmoq6uvnqrqhm"
        for event in ("push", "workflow_dispatch", "merge_group", "schedule"):
            self.assertEqual(self.trim(event, "refs/heads/main", bs), "save=true", event)
        # its store scopes entries by branch: another branch's entry would reach no other job
        for event, ref in (("merge_group", "refs/heads/gh-readonly-queue/main/pr-1-a"), ("pull_request", "refs/pull/1/merge"),
                           ("workflow_dispatch", "refs/heads/ci/x")):
            self.assertEqual(self.trim(event, ref, bs), "save=false", (event, ref))
            self.assertIn("main's reach every branch", self.out)
        # nothing new: nothing to save
        shutil.rmtree(self.cache / "a", ignore_errors=True)
        self.assertEqual(self.trim("workflow_dispatch", "refs/heads/main", bs, new=False), "save=false")

    def test_the_prune_never_runs_on_blacksmith(self):
        # (it lists GitHub's cache: on main it would delete the hosted lane's live entry)
        text = ACTION.read_text()
        step = text[text.index("- name: Delete the older entries"):]
        self.assertIn("!startsWith(runner.name, 'blacksmith-')", step.split("shell:")[0])
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text()
        self.assertIn("if: github.event_name == 'push' || (startsWith(runner.name, 'blacksmith-') && github.ref == 'refs/heads/main')", ci)


if __name__ == "__main__":
    unittest.main(verbosity=2)
