#!/usr/bin/env python3
"""Tests of .github/actions/ccache-save/trim.sh (python3 -I tools/ci/test_ccache_trim.py).

trim.sh runs on a macOS-like runner (the runner's ccache, not an image's) against a fake ccache on PATH
that reports a cache size, logs every call with the CCACHE_MAXSIZE it saw, and writes nothing.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github" / "actions" / "ccache-save" / "trim.sh"

FAKE_CCACHE = r'''#!/usr/bin/env bash
echo "$* maxsize=${CCACHE_MAXSIZE:-}" >> "$FAKE_CCACHE_LOG"
case "$1" in
    --print-stats) printf 'cache_size_kibibyte\t%s\nstats_updated_timestamp\t1\n' "$FAKE_CCACHE_KIB" ;;
    --show-stats) echo "Summary: (fake)" ;;
esac
exit 0
'''


class CcacheTrim(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        (bin_dir / "ccache").write_text(FAKE_CCACHE)
        (bin_dir / "ccache").chmod(0o755)
        self.cache = self.root / "build" / ".cache" / "ccache"
        self.cache.mkdir(parents=True)
        self.marker = self.root / "marker"
        self.marker.write_text("")
        os.utime(self.marker, (1, 1))
        (self.cache / "0").mkdir()
        (self.cache / "0" / "abcR").write_text("an object written by this run")
        self.log = self.root / "ccache.log"
        self.output = self.root / "github_output"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def trim(self, kib: int, cap: str = "0.95G", event: str = "push") -> list[str]:
        env = {k: v for k, v in os.environ.items() if not k.startswith(("CCACHE_", "GITHUB_", "JASPER_"))}
        env.update(PATH=f"{self.root / 'bin'}{os.pathsep}{os.environ['PATH']}", RUNNER_OS="macOS", LANE="linux",
                   EVENT=event, REF="refs/heads/main", JASPER_CCACHE_LANE="linux", JASPER_CCACHE_CAP=cap,
                   JASPER_CCACHE_MARKER=str(self.marker), JASPER_CCACHE_START="1", JASPER_CCACHE_RESTORED_SIZE="1M",
                   GITHUB_OUTPUT=str(self.output), FAKE_CCACHE_LOG=str(self.log), FAKE_CCACHE_KIB=str(kib))
        done = subprocess.run(["bash", str(SCRIPT)], cwd=self.root, env=env, capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.stdout = done.stdout
        return self.log.read_text().splitlines()

    def test_a_working_set_under_the_cap_is_saved_whole(self):
        # 828 MiB under 0.95 GB: ccache's cleanup would still cut its fullest subdirectories
        calls = self.trim(828 * 1024)
        self.assertFalse([c for c in calls if c.startswith("--cleanup")], calls)
        self.assertIn("--zero-stats maxsize=0.95G", calls)
        self.assertIn("within the cap; nothing cut", self.stdout)
        self.assertEqual(self.output.read_text(), "save=true\n")

    def test_a_working_set_over_the_cap_is_cut_to_it(self):
        calls = self.trim(1000 * 1024)  # 1,000 MiB, over 0.95 GB
        self.assertIn("--cleanup maxsize=0.95G", calls)
        self.assertIn("--zero-stats maxsize=0.95G", calls)

    def test_a_saved_working_set_is_recompressed_after_the_cut(self):
        # after the cut, which evicts by mtime: ccache 4.5's recompression gives each file a new one
        calls = [c.split(" maxsize=")[0] for c in self.trim(1000 * 1024)]
        self.assertIn("--recompress 9", calls)
        self.assertLess(calls.index("--cleanup"), calls.index("--recompress 9"))
        self.assertLess(calls.index("--recompress 9"), calls.index("--zero-stats"))
        self.log.unlink()
        self.assertIn("--recompress 9", [c.split(" maxsize=")[0] for c in self.trim(828 * 1024)])

    def test_binary_and_bare_caps(self):
        self.assertFalse([c for c in self.trim(1000 * 1024, cap="1Gi") if c.startswith("--cleanup")])
        self.log.unlink()
        # a bare number is in G, as ccache reads it
        self.assertIn("--cleanup maxsize=0.5", self.trim(600 * 1024, cap="0.5"))

    def test_a_pull_request_saves_nothing_and_cuts_nothing(self):
        calls = self.trim(5 * 1024 * 1024, event="pull_request")
        self.assertFalse([c for c in calls if c.startswith(("--cleanup", "--recompress", "--zero-stats"))], calls)
        self.assertEqual(self.output.read_text(), "save=false\n")


if __name__ == "__main__":
    unittest.main()
