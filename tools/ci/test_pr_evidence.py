#!/usr/bin/env python3
"""Tests of tools/ci/pr-evidence.sh (python3 -I tools/ci/test_pr_evidence.py).

The script runs against a fake gh on PATH that keeps the release in a JSON file, so nothing is
written to GitHub.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "pr-evidence.sh"

FAKE_GH = r'''#!/usr/bin/env python3
import json, os, sys
state_path = os.environ["FAKE_GH_STATE"]
with open(os.environ["FAKE_GH_LOG"], "a") as log:
    log.write(json.dumps(sys.argv[1:]) + "\n")
state = json.load(open(state_path)) if os.path.exists(state_path) else {}
args = sys.argv[1:]
def save():
    json.dump(state, open(state_path, "w"))
if args[:2] == ["release", "view"]:
    if args[2] not in state:
        sys.stderr.write("release not found\n"); sys.exit(1)
    rel = state[args[2]]
    if "assets" in args:
        for name in rel["assets"]:
            print(f"{name}\thttps://example.invalid/releases/download/{args[2]}/{name}")
    else:
        print("true" if rel["prerelease"] else "false")
elif args[:2] == ["release", "create"]:
    state[args[2]] = {"prerelease": "--prerelease" in args, "flags": args[3:], "assets": {}}
    save()
elif args[:2] == ["release", "upload"]:
    rel = state[args[2]]
    clobber = "--clobber" in args
    for f in [a for a in args[3:] if not a.startswith("-")]:
        name = os.path.basename(f)
        if name in rel["assets"] and not clobber:
            sys.stderr.write(f"asset {name} already exists\n"); sys.exit(1)
        rel["assets"][name] = open(f).read()
    save()
else:
    sys.stderr.write(f"fake gh: unexpected {args}\n"); sys.exit(3)
'''


class PrEvidence(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.bin = root / "bin"
        self.bin.mkdir()
        gh = self.bin / "gh"
        gh.write_text(FAKE_GH)
        gh.chmod(0o755)
        self.state = root / "state.json"
        self.log = root / "gh.log"
        self.files = root / "files"
        self.files.mkdir()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def file(self, rel: str, text: str = "data") -> str:
        path = self.files / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return str(path)

    def run_script(self, *args: str) -> subprocess.CompletedProcess:
        env = dict(os.environ, PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}",
                   FAKE_GH_STATE=str(self.state), FAKE_GH_LOG=str(self.log))
        return subprocess.run(["bash", str(SCRIPT), *args], env=env, capture_output=True, text=True)

    def calls(self) -> list[list[str]]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def release(self) -> dict:
        return json.loads(self.state.read_text())["pr-evidence"]

    def test_creates_a_prerelease_never_latest_and_prints_markdown(self) -> None:
        r = self.run_script("12", self.file("shot.png", "png"), self.file("editor.log", "log"))
        self.assertEqual(r.returncode, 0, r.stderr)
        rel = self.release()
        self.assertTrue(rel["prerelease"])
        self.assertIn("--latest=false", rel["flags"])
        self.assertEqual(rel["assets"], {"pr12-shot.png": "png", "pr12-editor.log": "log"})
        self.assertEqual(r.stdout.splitlines(), [
            "![pr12-shot.png](https://example.invalid/releases/download/pr-evidence/pr12-shot.png)",
            "[pr12-editor.log](https://example.invalid/releases/download/pr-evidence/pr12-editor.log)",
        ])

    def test_reuses_the_release_and_replaces_an_asset(self) -> None:
        self.assertEqual(self.run_script("5", self.file("a.png", "old")).returncode, 0)
        r = self.run_script("5", self.file("a.png", "new"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(sum(1 for c in self.calls() if c[:2] == ["release", "create"]), 1)
        self.assertEqual(self.release()["assets"], {"pr5-a.png": "new"})

    def test_refuses_a_release_that_is_not_a_prerelease(self) -> None:
        self.state.write_text(json.dumps({"pr-evidence": {"prerelease": False, "flags": [], "assets": {}}}))
        r = self.run_script("5", self.file("a.png"))
        self.assertEqual(r.returncode, 1)
        self.assertIn("not a prerelease", r.stderr)
        self.assertFalse([c for c in self.calls() if c[:2] == ["release", "upload"]])

    def test_names_github_would_rewrite_become_dashes(self) -> None:
        r = self.run_script("3", self.file("my shot#1.PNG"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(list(self.release()["assets"]), ["pr3-my-shot-1.PNG"])
        self.assertTrue(r.stdout.startswith("![pr3-my-shot-1.PNG]("))

    def test_bad_arguments_call_no_gh(self) -> None:
        cases = [("abc", self.file("a.png")), ("0", self.file("a.png")), ("7", str(self.files / "missing.png")),
                 ("7", self.file("x/same.png"), self.file("y/same.png")), ("7",)]
        for args in cases:
            with self.subTest(args=args):
                self.assertEqual(self.run_script(*args).returncode, 2)
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    unittest.main()
