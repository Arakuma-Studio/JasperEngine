"""Tests of the overflow runners' hooks, tools/ci/self-hosted/job-started.sh and job-completed.sh
(python3 -I tools/ci/test_self_hosted_hooks.py; bash, no runner).

job-started.sh refuses every job but this repository's push, merge queue, dispatch and pull request
from its own branches; job-completed.sh empties only the job's own workspace and removes only the
Docker images CI noted, never the newest of each, on a fake docker and df.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

HOOKS = Path(__file__).resolve().parent / "self-hosted"
REPO = "Arakuma-Studio/JasperEngine"


class Hooks(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.runner = self.tmp / "je-runner"
        (self.runner / "hooks").mkdir(parents=True)
        for name in ("job-started.sh", "job-completed.sh"):
            shutil.copy(HOOKS / name, self.runner / "hooks" / name)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        self.log = self.tmp / "docker.log"
        # docker: logs its arguments; `image rm` of an image named *busy* fails (a container uses it)
        self.fake("docker", '#!/bin/sh\necho "docker $*" >> "$FAKE_LOG"\ncase "$*" in *busy*) exit 1;; esac\n'
                            '[ "$1" = buildx ] && exit 1\nexit 0\n')
        self.fake("df", '#!/bin/sh\necho "Filesystem 1024-blocks Used Available Capacity Mounted"\n'
                        'echo "/dev/x 1000 1 $((FAKE_FREE_GB * 1048576)) 1% /"\n')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fake(self, name, text):
        p = self.bin / name
        p.write_text(text)
        p.chmod(0o755)

    def hook(self, name, free_gb=500, **env):
        full = {"PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}", "FAKE_LOG": str(self.log),
                "FAKE_FREE_GB": str(free_gb), **env}
        return subprocess.run(["bash", str(self.runner / "hooks" / name)], env=full, capture_output=True, text=True,
                              timeout=60)

    def started(self, event, head=REPO, repo=REPO):
        path = self.tmp / "event.json"
        payload = {"pull_request": {"head": {"repo": {"full_name": head} if head else None}}} if event == "pull_request" else {}
        path.write_text(json.dumps(payload))
        return self.hook("job-started.sh", GITHUB_REPOSITORY=repo, GITHUB_EVENT_NAME=event, GITHUB_EVENT_PATH=str(path))

    def test_only_this_repositorys_trusted_events_run(self):
        for event in ("push", "merge_group", "workflow_dispatch", "pull_request"):
            r = self.started(event)
            self.assertEqual(r.returncode, 0, (event, r.stdout, r.stderr))
            self.assertIn(f"{event} of {REPO}", r.stdout)
        for event, head, repo in (("pull_request", "someone/JasperEngine", REPO), ("pull_request", None, REPO),
                                  ("pull_request_target", REPO, REPO), ("schedule", REPO, REPO), ("issue_comment", REPO, REPO),
                                  ("", REPO, REPO), ("push", REPO, "someone/JasperEngine")):
            r = self.started(event, head, repo)
            self.assertEqual(r.returncode, 1, (event, head, repo, r.stdout))
            self.assertIn("::error::overflow runner", r.stdout)

    def test_the_jobs_workspace_is_emptied_and_nothing_else(self):
        ws = self.runner / "_work" / "JasperEngine" / "JasperEngine"
        (ws / "build" / "deep").mkdir(parents=True)
        (ws / "build" / "deep" / "o.o").write_text("x")
        (ws / ".git").mkdir()
        mine = self.tmp / "user-files"
        mine.mkdir()
        (mine / "keep.txt").write_text("x")
        r = self.hook("job-completed.sh", GITHUB_WORKSPACE=str(ws))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(ws.is_dir())
        self.assertEqual(list(ws.iterdir()), [])
        # a workspace outside the runner's _work is never touched
        for outside in (str(mine), "", "/", str(self.runner)):
            r = self.hook("job-completed.sh", GITHUB_WORKSPACE=outside)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("left alone", r.stdout)
        self.assertTrue((mine / "keep.txt").exists())

    def test_only_the_images_ci_noted_go_and_never_the_newest_of_each(self):
        if shutil.which("uname") and os.uname().sysname != "Linux":
            self.skipTest("the image prune runs on Linux only")
        ledger = self.runner / "_work" / "_tool" / "je-ci-images.txt"
        ledger.parent.mkdir(parents=True)
        now = int(time.time())
        old, recent = now - 5 * 86400, now - 3600
        ledger.write_text(f"r/linux-build:a {old}\nr/linux-build:b {recent}\nr/web-build:c {old}\n"
                          f"r/linux-build:old {old}\nr/linux-build:busy {old}\nr/linux-build:a {old + 10}\n")
        r = self.hook("job-completed.sh", GITHUB_WORKSPACE="")
        self.assertEqual(r.returncode, 0, r.stderr)
        removed = [line.split()[-1] for line in self.log.read_text().splitlines() if line.startswith("docker image rm")]
        # linux-build:b is the newest linux image, web-build:c the only web one; a used one stays noted
        self.assertEqual(sorted(removed), ["r/linux-build:a", "r/linux-build:busy", "r/linux-build:old"])
        self.assertEqual(sorted(line.split()[0] for line in ledger.read_text().split("\n") if line),
                         ["r/linux-build:b", "r/linux-build:busy", "r/web-build:c"])
        # short of disk: every one but the newest of each, however recent
        ledger.write_text(f"r/linux-build:b {recent}\nr/linux-build:d {recent - 60}\n")
        self.log.write_text("")
        self.hook("job-completed.sh", free_gb=40, GITHUB_WORKSPACE="")
        self.assertIn("docker image rm r/linux-build:d", self.log.read_text())
        self.assertNotIn("r/linux-build:b", self.log.read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
