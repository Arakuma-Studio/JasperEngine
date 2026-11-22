#!/usr/bin/env python3
"""Tests of tools/ci/editor_install.sh (python3 -I tools/ci/test_editor_install.py).

Each test builds a small checkout (SOURCE_ROOT) and an editor install beside it, packs the install
and unpacks it elsewhere: the folders the install copies unchanged from the checkout leave the
archive and come back from the checkout, everything else travels in it.
"""

from __future__ import annotations

import filecmp
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "editor_install.sh"


def write(path: Path, text: str, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(mode)


def same_tree(a: Path, b: Path) -> list[str]:
    """The relative paths that differ between two trees (content, presence, executable bit)."""
    diffs = []

    def walk(cmp: filecmp.dircmp, rel: Path) -> None:
        diffs.extend(str(rel / n) for n in cmp.left_only + cmp.right_only + cmp.funny_files)
        for name in cmp.common_files:
            left, right = Path(cmp.left) / name, Path(cmp.right) / name
            if left.read_bytes() != right.read_bytes() or os.access(left, os.X_OK) != os.access(right, os.X_OK):
                diffs.append(str(rel / name))
        for name, sub in cmp.subdirs.items():
            walk(sub, rel / name)

    walk(filecmp.dircmp(a, b), Path("."))
    return diffs


class EditorInstall(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.source = root / "checkout"
        self.install = root / "build" / "linux" / "editor-install"
        self.archive = root / "build" / "editor-install.tar.zst"
        self.unpacked = root / "elsewhere" / "linux"
        res = self.source / "editor" / "resources"
        write(res / "titleart" / "hdri" / "sky.hdr", "hdr bytes")
        write(res / "titleart" / "fonts" / "a" / "A.ttf", "font bytes")
        write(res / "titleart" / "thumbnails" / "looks" / "x.png", "png bytes")
        write(res / "titleart" / "manifest.json", "{}")  # not installed
        write(res / "sound-library" / "library" / "chime" / "c.ogg", "ogg bytes")
        write(res / "sound-library" / "library" / ".gitattributes", "*.ogg binary")
        bin_dir = self.install / "bin"
        write(bin_dir / "jasperengine-editor", "editor binary", 0o755)
        write(bin_dir / "engine" / "jasperengine.py", "print()", 0o755)
        write(bin_dir / "titleart" / "hdri" / "sky.hdr", "hdr bytes")
        write(bin_dir / "titleart" / "fonts" / "a" / "A.ttf", "font bytes")
        write(bin_dir / "titleart" / "thumbnails" / "looks" / "x.png", "png bytes")
        # the install drops the library's .gitattributes
        write(bin_dir / "sound-library" / "chime" / "c.ogg", "ogg bytes")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_script(self, *args: str) -> subprocess.CompletedProcess:
        env = dict(os.environ, SOURCE_ROOT=str(self.source))
        return subprocess.run(["bash", str(SCRIPT), *map(str, args)], env=env, capture_output=True, text=True,
                              timeout=120)

    def archived(self) -> list[str]:
        out = subprocess.run(["tar", "--zstd", "-tf", str(self.archive)], capture_output=True, text=True, check=True)
        return out.stdout.split()

    def pack_and_unpack(self) -> None:
        packed = self.run_script("pack", self.install, self.archive)
        self.assertEqual(packed.returncode, 0, packed.stdout + packed.stderr)
        unpacked = self.run_script("unpack", self.archive, self.unpacked)
        self.assertEqual(unpacked.returncode, 0, unpacked.stdout + unpacked.stderr)

    def test_unchanged_copies_of_the_checkout_leave_the_archive(self) -> None:
        self.pack_and_unpack()
        names = self.archived()
        self.assertIn("editor-install/bin/jasperengine-editor", names)
        self.assertIn("editor-install/bin/engine/jasperengine.py", names)
        for gone in ("titleart/hdri/sky.hdr", "titleart/fonts/a/A.ttf", "titleart/thumbnails/looks/x.png",
                     "sound-library/chime/c.ogg"):
            self.assertNotIn(f"editor-install/bin/{gone}", names)

    def test_unpacking_restores_the_install_exactly(self) -> None:
        self.pack_and_unpack()
        self.assertEqual(same_tree(self.install, self.unpacked / "editor-install"), [])

    def test_a_folder_the_build_changed_travels_in_the_archive(self) -> None:
        write(self.install / "bin" / "titleart" / "fonts" / "a" / "A.ttf", "a different font")
        self.pack_and_unpack()
        self.assertIn("editor-install/bin/titleart/fonts/a/A.ttf", self.archived())
        self.assertEqual(same_tree(self.install, self.unpacked / "editor-install"), [])

    def test_a_folder_the_install_lacks_stays_absent(self) -> None:
        subprocess.run(["rm", "-rf", str(self.install / "bin" / "sound-library")], check=True)
        self.pack_and_unpack()
        self.assertFalse((self.unpacked / "editor-install" / "bin" / "sound-library").exists())
        self.assertEqual(same_tree(self.install, self.unpacked / "editor-install"), [])

    def test_unpacking_without_the_checkout_folder_fails_and_names_it(self) -> None:
        packed = self.run_script("pack", self.install, self.archive)
        self.assertEqual(packed.returncode, 0, packed.stderr)
        subprocess.run(["rm", "-rf", str(self.source / "editor" / "resources" / "titleart" / "hdri")], check=True)
        unpacked = self.run_script("unpack", self.archive, self.unpacked)
        self.assertNotEqual(unpacked.returncode, 0)
        self.assertIn("editor/resources/titleart/hdri", unpacked.stderr)


if __name__ == "__main__":
    unittest.main()
