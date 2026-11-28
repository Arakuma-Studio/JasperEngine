#!/usr/bin/env python3
"""Tests of tools/ci/ccache_windows.sh (python3 -I tools/ci/test_ccache_windows.py).

The script runs from a copy whose pins name a fixture zip: its ccache.exe is a shell script that
prints a version. A fake curl on PATH serves that zip, fails as an HTTP 503 does (curl -f: exit 22)
a set number of times first, or exits 0 having written nothing or an error page: what Chocolatey's
outage looked like (run 46050080028). A fake sleep keeps retry.sh's pauses out of the run.
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "ccache_windows.sh"

FAKE_CURL = r'''#!/usr/bin/env bash
runs=$(( $(cat "$CURL_COUNT" 2>/dev/null || echo 0) + 1 ))
echo "$runs" > "$CURL_COUNT"
out=""; url=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        --connect-timeout|--max-time) shift 2 ;;
        -*) shift ;;
        *) url="$1"; shift ;;
    esac
done
echo "$url" >> "$CURL_URLS"
if [[ $runs -le ${CURL_FAILURES:-0} ]]; then echo "curl: (22) The requested URL returned error: 503" >&2; exit 22; fi
case "${CURL_MODE:-serve}" in
    serve) cp "$FIXTURE_ZIP" "$out" ;;
    nothing) ;;
    page) echo "<html>503 Service Unavailable</html>" > "$out" ;;
esac
'''

FAKE_SLEEP = "#!/bin/sh\n:\n"


def pins() -> tuple[str, str]:
    text = SCRIPT.read_text()
    return (re.search(r"^CCACHE_VERSION=(\S+)$", text, re.M).group(1),
            re.search(r"^CCACHE_SHA256=([0-9a-f]{64})$", text, re.M).group(1))


class CcacheWindows(unittest.TestCase):
    VERSION = "9.8.7"

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.zip = root / "fixture.zip"
        self.make_zip(self.VERSION)
        # a checkout of its own: the script finds retry.sh beside it
        ci = root / "repo" / "tools" / "ci"
        ci.mkdir(parents=True)
        shutil.copy(HERE / "retry.sh", ci / "retry.sh")
        version, sha = pins()
        digest = hashlib.sha256(self.zip.read_bytes()).hexdigest()
        text = SCRIPT.read_text().replace(f"CCACHE_VERSION={version}\n", f"CCACHE_VERSION={self.VERSION}\n") \
            .replace(f"CCACHE_SHA256={sha}\n", f"CCACHE_SHA256={digest}\n")
        self.script = ci / "ccache_windows.sh"
        self.script.write_text(text)
        bin_dir = root / "bin"
        bin_dir.mkdir()
        for name, body in (("curl", FAKE_CURL), ("sleep", FAKE_SLEEP)):
            (bin_dir / name).write_text(body)
            (bin_dir / name).chmod(0o755)
        self.dir = root / "tool-cache" / "ccache"
        self.count = root / "curl-count"
        self.urls = root / "curl-urls"
        self.github_path = root / "github-path"
        self.github_path.write_text("")
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(("RETRY_", "GITHUB_", "CCACHE_"))}
        self.env.update(PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}", CURL_COUNT=str(self.count),
                        CURL_URLS=str(self.urls), FIXTURE_ZIP=str(self.zip), GITHUB_PATH=str(self.github_path))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def make_zip(self, prints: str) -> None:
        name = f"ccache-{self.VERSION}-windows-x86_64"
        with zipfile.ZipFile(self.zip, "w") as archive:
            exe = zipfile.ZipInfo(f"{name}/ccache.exe")
            exe.external_attr = 0o755 << 16
            archive.writestr(exe, f'#!/bin/sh\necho "ccache version {prints}"\necho "Features: test"\n')
            archive.writestr(f"{name}/LICENSE.md", "fixture\n")

    def run_script(self, **env: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["bash", str(self.script), str(self.dir)], env={**self.env, **env},
                              capture_output=True, text=True, timeout=60)

    def curls(self) -> int:
        return int(self.count.read_text()) if self.count.exists() else 0

    def added_paths(self) -> list[str]:
        return self.github_path.read_text().splitlines()

    def test_installs_the_pinned_zip_and_puts_it_on_the_path(self) -> None:
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.curls(), 1)
        self.assertEqual(self.urls.read_text().split(), [
            f"https://github.com/ccache/ccache/releases/download/v{self.VERSION}/ccache-{self.VERSION}-windows-x86_64.zip"])
        bin_dir = self.dir / f"ccache-{self.VERSION}-windows-x86_64"
        self.assertEqual(self.added_paths(), [str(bin_dir)])
        self.assertTrue((bin_dir / "ccache.exe").is_file())
        self.assertIn(f"ccache version {self.VERSION}", result.stdout)

    def test_a_cached_zip_is_not_downloaded_again(self) -> None:
        self.assertEqual(self.run_script().returncode, 0)
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.curls(), 1)
        self.assertIn("from the cache", result.stdout)

    def test_a_cached_zip_that_does_not_match_is_downloaded_again(self) -> None:
        self.dir.mkdir(parents=True)
        (self.dir / f"ccache-{self.VERSION}-windows-x86_64.zip").write_text("truncated")
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.curls(), 1)

    def test_http_errors_are_retried_until_the_download_lands(self) -> None:
        result = self.run_script(CURL_FAILURES="2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.curls(), 3)
        self.assertEqual(len([line for line in result.stderr.splitlines() if "retrying in" in line]), 2, result.stderr)

    def test_an_outage_that_outlasts_the_retries_fails_with_nothing_on_the_path(self) -> None:
        result = self.run_script(CURL_FAILURES="99")
        self.assertEqual(result.returncode, 22, result.stderr)
        self.assertEqual(self.curls(), 5)
        self.assertEqual(self.added_paths(), [])

    def test_a_download_that_exits_0_without_the_zip_fails(self) -> None:
        # the Chocolatey outage's shape: the fetch "succeeds" and nothing usable arrives
        for mode in ("nothing", "page"):
            with self.subTest(mode=mode):
                self.count.unlink(missing_ok=True)
                result = self.run_script(CURL_MODE=mode)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("is not the pinned", result.stderr)
                self.assertEqual(self.added_paths(), [])
                self.assertFalse((self.dir / f"ccache-{self.VERSION}-windows-x86_64.zip").exists())

    def test_a_binary_of_another_version_fails(self) -> None:
        self.make_zip("1.2.3")
        digest = hashlib.sha256(self.zip.read_bytes()).hexdigest()
        self.script.write_text(re.sub(r"^CCACHE_SHA256=\S+$", f"CCACHE_SHA256={digest}", self.script.read_text(), flags=re.M))
        result = self.run_script()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("'ccache version 1.2.3'", result.stderr)
        self.assertEqual(self.added_paths(), [])

    def test_usage(self) -> None:
        result = subprocess.run(["bash", str(self.script)], env=self.env, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.curls(), 0)

    def test_the_pins_are_a_version_and_a_sha256(self) -> None:
        version, sha = pins()
        self.assertRegex(version, r"^\d+\.\d+(\.\d+)?$")
        self.assertRegex(sha, r"^[0-9a-f]{64}$")


if __name__ == "__main__":
    unittest.main()
