#!/usr/bin/env python3
"""Tests of .github/actions/image/image.sh (python3 -I tools/ci/test_image_action.py).

The script runs from the repository root (it asks tools/build.sh image-tag for the tags) against a
fake docker on PATH: FAKE_REGISTRY lists the references a pull finds, FAKE_LOCAL the ones already
loaded, and every call is logged, so nothing is pulled or built.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github" / "actions" / "image" / "image.sh"
REGISTRY = "ghcr.io/owner"

FAKE_DOCKER = r'''#!/usr/bin/env bash
echo "docker $*" >> "$FAKE_LOG"
last="${@: -1}"
case "$1 $2" in
    "image inspect") [[ " $FAKE_LOCAL " == *" $last "* ]] ;;
    "pull -q"|"pull "*)
        if [[ " $FAKE_REGISTRY " == *" $last "* ]]; then echo "$last"; exit 0; fi
        if [[ -n "${FAKE_PULL_FLAKY:-}" ]] && ! grep -q '^flaked$' "$FAKE_LOG.state" 2>/dev/null; then
            echo flaked >> "$FAKE_LOG.state"
            echo "Error response from daemon: Get \"https://ghcr.io/v2/\": net/http: TLS handshake timeout" >&2
            exit 1
        fi
        echo "Error response from daemon: manifest unknown" >&2
        exit 1 ;;
    "buildx inspect") exit 1 ;;
    *) exit 0 ;;
esac
'''


def tag(target: str) -> str:
    return subprocess.run(["tools/build.sh", "image-tag", target], cwd=ROOT, capture_output=True, text=True,
                          check=True).stdout.strip()


def ref(target: str) -> str:
    return f"{REGISTRY}/jasperengine-{target}-build:{tag(target)}"


class ImageAction(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.bin = root / "bin"
        self.bin.mkdir()
        docker = self.bin / "docker"
        docker.write_text(FAKE_DOCKER)
        docker.chmod(0o755)
        # (sleep: the retry's pause, instant here)
        sleep = self.bin / "sleep"
        sleep.write_text("#!/bin/sh\n")
        sleep.chmod(0o755)
        self.log = root / "log"
        self.log.touch()
        self.env_file = root / "env"
        self.output_file = root / "output"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_script(self, target: str, registry: list[str], local: list[str] | None = None,
                   flaky: bool = False, extra: dict[str, str] | None = None) -> tuple[subprocess.CompletedProcess, list[str]]:
        env = {**(extra or {}),
            "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}",
            "FAKE_LOG": str(self.log),
            "FAKE_REGISTRY": " ".join(registry),
            "FAKE_LOCAL": " ".join(local or []),
            "REGISTRY": REGISTRY,
            "GITHUB_ENV": str(self.env_file),
            "GITHUB_OUTPUT": str(self.output_file),
        }
        if flaky:
            env["FAKE_PULL_FLAKY"] = "1"
        result = subprocess.run(["bash", str(SCRIPT), target], cwd=ROOT, env=env, capture_output=True, text=True,
                                timeout=120)
        return result, self.log.read_text().splitlines()

    @staticmethod
    def builds(calls: list[str]) -> list[str]:
        return [c for c in calls if c.startswith(("docker build ", "docker buildx build "))]

    def test_a_published_tag_is_pulled_not_built(self) -> None:
        result, calls = self.run_script("linux", registry=[ref("linux")])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.builds(calls), [])
        self.assertIn(f"image: pulled {ref('linux')} in ", result.stderr)
        self.assertIn(f"JASPER_DOCKER_IMAGE={ref('linux')}", self.env_file.read_text())
        self.assertIn(f"JASPER_IMAGE_LINUX={ref('linux')}", self.env_file.read_text())
        self.assertIn(f"tag={tag('linux')}", self.output_file.read_text())

    def test_an_overflow_runner_notes_the_image_for_its_prune(self) -> None:
        # (tools/ci/self-hosted/job-completed.sh prunes only what this ledger names)
        ledger = self.log.parent / "tool" / "je-ci-images.txt"
        ledger.parent.mkdir()
        for environment, noted in (("github-hosted", False), ("self-hosted", True)):
            result, _ = self.run_script("linux", registry=[ref("linux")],
                                        extra={"RUNNER_ENVIRONMENT": environment, "RUNNER_TOOL_CACHE": str(ledger.parent)})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(ledger.exists(), noted, environment)
        ref_, when = ledger.read_text().split()
        self.assertEqual(ref_, ref("linux"))
        self.assertTrue(when.isdigit())

    def test_a_loaded_image_is_neither_pulled_nor_built(self) -> None:
        result, calls = self.run_script("web", registry=[], local=[ref("web")])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([c for c in calls if c.startswith("docker pull")], [])
        self.assertEqual(self.builds(calls), [])

    # a PR that changes a Dockerfile: its tag is not published yet
    def test_an_unpublished_tag_is_built_with_the_registry_cache(self) -> None:
        result, calls = self.run_script("web", registry=[])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("is not in the registry; building it", result.stderr)
        builds = self.builds(calls)
        self.assertEqual(len(builds), 1, calls)
        self.assertIn(f"--cache-from type=registry,ref={REGISTRY}/jasperengine-web-build:buildcache", builds[0])
        self.assertIn(f"-t {ref('web')}", builds[0])
        self.assertIn("-f docker/web.Dockerfile", builds[0])
        self.assertIn(f"image: built {ref('web')} in ", result.stderr)

    def test_a_missing_tag_is_asked_for_once(self) -> None:
        _, calls = self.run_script("web", registry=[])
        self.assertEqual(len([c for c in calls if c.startswith("docker pull")]), 1, calls)

    def test_a_network_error_is_tried_again(self) -> None:
        result, calls = self.run_script("linux", registry=[], flaky=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len([c for c in calls if c.startswith("docker pull")]), 2, calls)

    def test_linux_backends_builds_from_the_published_linux_image(self) -> None:
        result, calls = self.run_script("linux-backends", registry=[ref("linux")])
        self.assertEqual(result.returncode, 0, result.stderr)
        builds = self.builds(calls)
        self.assertEqual(len(builds), 1, calls)
        self.assertTrue(builds[0].startswith("docker build "), builds[0])
        self.assertIn(f"--build-arg BASE={ref('linux')}", builds[0])
        self.assertIn(f"JASPER_DOCKER_IMAGE={ref('linux-backends')}", self.env_file.read_text())


if __name__ == "__main__":
    unittest.main()
