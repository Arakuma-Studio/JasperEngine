#!/usr/bin/env python3
"""Unit tests of tools/players/write_hashes.py (python3 -I tools/players/test_write_hashes.py).

A release's archives are the full player and its variants (-ui, -core, -lite3d, -lite2d) of every
platform and backend: each must land under its own id, the one its player-info.json names, and an
archive of another version (or two of one id) must stop the release instead of shipping a table
the editor refuses.
"""

from __future__ import annotations

import importlib.util
import io
import json
import re
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("write_hashes", HERE / "write_hashes.py")
write_hashes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(write_hashes)

ENTRY = re.compile(r'\{"([^"]+)", "([0-9a-f]{64})", (\d+)ull\}')


def archive(directory: Path, runtime_id: str, version: str, name: str | None = None) -> Path:
    path = directory / (name or f"jasperengine-player-{runtime_id}-{version}.zip")
    with zipfile.ZipFile(path, "w") as zipped:
        zipped.writestr("player-info.json", json.dumps({"id": runtime_id, "engineVersion": version}))
        zipped.writestr("files/jasperengine-player", runtime_id)
    return path


def run(*args: str) -> tuple[int, str]:
    out = io.StringIO()
    argv, sys.argv = sys.argv, ["write_hashes.py", *args]
    with redirect_stdout(out), redirect_stderr(out):
        try:
            code = write_hashes.main()
        except SystemExit as e:
            code = e.code if isinstance(e.code, int) else 1
        finally:
            sys.argv = argv
    return code, out.getvalue()


class WriteHashesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.header = self.dir / "PlayerRuntimeHashes.gen.h"

    def tearDown(self):
        self.tmp.cleanup()

    def entries(self) -> dict[str, str]:
        found = ENTRY.findall(self.header.read_text())
        ids = [i for i, _, _ in found]
        self.assertEqual(len(ids), len(set(ids)), f"an id twice: {ids}")
        return {i: digest for i, digest, _ in found}

    def test_every_variant_gets_its_own_id(self):
        for version in ("v1.2.3", "nightly-20261112"):
            with self.subTest(version=version):
                archives = self.dir / version
                archives.mkdir()
                ids = ["linux-x64-glcore-release", "linux-x64-glcore-release-lite3d", "linux-x64-glcore-release-ui",
                       "linux-x64-glcore-release-core", "web-wasm32-gles3-development"]
                for runtime_id in ids:
                    archive(archives, runtime_id, version)
                (archives / f"jasperengine-player-linux-x64-glcore-release-{version}-symbols.zip").write_bytes(b"symbols")
                code, out = run(str(archives), "--out", str(self.header), "--version", version)
                self.assertEqual(code, 0, out)
                entries = self.entries()
                self.assertEqual(sorted(entries), sorted(ids))
                for runtime_id in ids:
                    data = (archives / f"jasperengine-player-{runtime_id}-{version}.zip").read_bytes()
                    self.assertEqual(entries[runtime_id], write_hashes.hashlib.sha256(data).hexdigest(), runtime_id)

    def test_an_archive_of_another_version_stops_the_table(self):
        archive(self.dir, "linux-x64-glcore-release", "nightly-20261112")
        archive(self.dir, "linux-x64-vulkan-release", "v0.7.1-12-gabc1234")
        code, out = run(str(self.dir), "--out", str(self.header), "--version", "nightly-20261112")
        self.assertEqual(code, 1, out)
        self.assertIn("v0.7.1-12-gabc1234", out)
        self.assertFalse(self.header.exists())

    def test_a_name_that_is_not_its_player_info_stops_the_table(self):
        archive(self.dir, "linux-x64-glcore-release-lite3d", "v1.2.3", name="jasperengine-player-linux-x64-glcore-release-v1.2.3.zip")
        code, out = run(str(self.dir), "--out", str(self.header))
        self.assertEqual(code, 1, out)
        self.assertIn("player-info.json", out)

    def test_no_archives_is_the_development_table(self):
        code, out = run(str(self.dir / "none"), "--out", str(self.header))
        self.assertEqual(code, 0, out)
        self.assertEqual(self.entries(), {})


if __name__ == "__main__":
    unittest.main()
