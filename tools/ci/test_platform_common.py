#!/usr/bin/env python3
"""Tests of tools/ci/platform-common.sh (python3 -I tools/ci/test_platform_common.py): cache_has finds a
cache entry in a CMakeCache.txt larger than a pipe's buffer under pipefail (the platform scripts' setting),
with Unix or Windows line ends, and not a line that is only a prefix of one.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMMON = HERE / "platform-common.sh"


class CacheHas(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="platform-common-test-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def cache_has(self, text: str, line: str) -> int:
        cache = self.tmp / "CMakeCache.txt"
        cache.write_bytes(text.encode())
        return subprocess.run(["bash", "-c", 'set -euo pipefail; . "$1"; cache_has "$2" "$3"', "_", str(COMMON), str(cache), line],
                              capture_output=True, text=True, timeout=60).returncode

    def test_an_early_entry_of_a_large_cache(self):
        # (a 2 MB cache with the entry near its top: grep -q stopped there and tr died of SIGPIPE, so the
        # macOS editor's package step said "the editor was built with its tests", release dry run 46114356440)
        filler = "".join(f"SOME_ENTRY_{i}:STRING=value\n" for i in range(80_000))
        for end in ("\n", "\r\n"):
            text = f"JASPERENGINE_TESTS:BOOL=OFF{end}" + filler.replace("\n", end)
            self.assertEqual(self.cache_has(text, "JASPERENGINE_TESTS:BOOL=OFF"), 0, repr(end))
            self.assertNotEqual(self.cache_has(text, "JASPERENGINE_TESTS:BOOL=ON"), 0, repr(end))
            self.assertNotEqual(self.cache_has(text, "JASPERENGINE_TESTS:BOOL=OF"), 0, repr(end))


if __name__ == "__main__":
    unittest.main()
