#!/usr/bin/env python3
"""Unit tests of tools/android-screen-check.py (python3 tools/ci/test_android_screen_check.py).

The emulator gate's screen check on synthetic screenshots: a black screen and a clear-colour-only
frame fail as BLANK whatever the golden says, a drawn frame passes its own golden and fails one of
another scene, --update writes a golden only from a frame that passed the content check.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "mcp"))
import pngdiff  # noqa: E402

spec = importlib.util.spec_from_file_location("android_screen_check", HERE.parent / "android-screen-check.py")
screen_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(screen_check)

WIDTH, HEIGHT = 108, 240


def flat(rgb) -> tuple:
    return WIDTH, HEIGHT, bytearray(bytes((*rgb, 255)) * (WIDTH * HEIGHT))


def drawn(rgb, box_rgb, box=(20, 40, 60, 80)) -> tuple:
    width, height, rgba = flat(rgb)
    x0, y0, w, h = box
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            i = (y * width + x) * 4
            rgba[i:i + 3] = bytes(box_rgb)
    return width, height, rgba


class ScreenCheck(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())

    def write(self, name: str, img) -> Path:
        path = self.dir / name
        path.write_bytes(pngdiff.encode(img))
        return path

    def run_check(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = screen_check.main(list(args))
        return status, out.getvalue()

    def test_black_screen_fails(self):
        status, out = self.run_check(str(self.write("black.png", flat((0, 0, 0)))))
        self.assertEqual(status, 1)
        self.assertIn("BLANK", out)

    def test_clear_colour_only_fails_even_against_its_own_golden(self):
        clear = flat((51, 64, 89))
        screen = self.write("clear.png", clear)
        golden = self.dir / "golden.png"
        golden.write_bytes(pngdiff.encode(pngdiff.downscale(clear, 27, 60)))
        status, out = self.run_check(str(screen), "--golden", str(golden), "--width", "27")
        self.assertEqual(status, 1)
        self.assertIn("BLANK", out)

    def test_drawn_frame_matches_its_golden(self):
        frame = drawn((51, 64, 89), (220, 60, 40))
        screen = self.write("frame.png", frame)
        golden = self.dir / "golden" / "frame.png"
        status, out = self.run_check(str(screen), "--golden", str(golden), "--width", "27", "--update")
        self.assertEqual(status, 0, out)
        self.assertTrue(golden.is_file())
        status, out = self.run_check(str(screen), "--golden", str(golden), "--width", "27")
        self.assertEqual(status, 0, out)
        self.assertIn("GOLDEN   ok", out)

    def test_another_scene_differs_from_the_golden(self):
        golden = self.dir / "golden.png"
        self.run_check(str(self.write("a.png", drawn((51, 64, 89), (220, 60, 40)))), "--golden", str(golden),
                       "--width", "27", "--update")
        status, out = self.run_check(str(self.write("b.png", drawn((51, 64, 89), (40, 200, 60), box=(10, 120, 80, 100)))),
                                     "--golden", str(golden), "--width", "27")
        self.assertEqual(status, 1)
        self.assertIn("DIFFERS", out)
        self.assertTrue((self.dir / "b.diff.png").is_file())

    def test_a_blank_frame_never_becomes_a_golden(self):
        golden = self.dir / "golden.png"
        status, out = self.run_check(str(self.write("black.png", flat((0, 0, 0)))), "--golden", str(golden), "--update")
        self.assertEqual(status, 1)
        self.assertFalse(golden.exists())

    def test_missing_golden_fails(self):
        status, out = self.run_check(str(self.write("frame.png", drawn((0, 0, 0), (255, 255, 255)))),
                                     "--golden", str(self.dir / "none.png"))
        self.assertEqual(status, 1)
        self.assertIn("MISSING", out)


if __name__ == "__main__":
    unittest.main()
