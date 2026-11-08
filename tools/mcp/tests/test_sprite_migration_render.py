"""The S44 format bump renders what it loads unchanged (docs/specs/S44-aseprite-import.md AC23,
R44): editor/tests/fixtures/migrations/scene-v7-sprites.scene (a scene written before the bump,
with an inline-frame Sprite and a Sprite Animation) is drawn the same in the Scene view as it was
loaded and after a save at the new version and a reopen; the saved file is at the current version
and keeps the sprite's frames. (SpriteSheetMigrationTest checks the YAML side; this is the render.)
"""

from __future__ import annotations

import struct
import zlib

import yaml

from harness import REPO, Editor, TestFailure
from htmlui_pixels import decode_png

CI = True
EDITOR = {"create": True, "name": "SpriteMigrationRender"}
TIMEOUT = 300

FIXTURE = REPO / "editor" / "tests" / "fixtures" / "migrations" / "scene-v7-sprites.scene"


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def strip_png() -> bytes:
    """the fixture's sprites/hero.png: three 16x16 frames side by side, red, green and blue"""
    rows = []
    for _ in range(16):
        row = bytearray()
        for frame in range(3):
            colour = [(220, 40, 40), (40, 200, 60), (40, 80, 220)][frame]
            row += bytes((*colour, 255)) * 16
        rows.append(bytes(row))
    raw = b"".join(b"\x00" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 48, 16, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


async def test_v7_scene_renders_the_same_after_the_bump(ed: Editor):
    (ed.project / "assets" / "sprites").mkdir(parents=True, exist_ok=True)
    (ed.project / "assets" / "sprites" / "hero.png").write_bytes(strip_png())
    (ed.project / "scenes").mkdir(exist_ok=True)
    scene_file = ed.project / "scenes" / "SpritesV7.scene"
    scene_file.write_text(FIXTURE.read_text())
    await ed.call("sync_files", paths=["assets/sprites/hero.png", "scenes/SpritesV7.scene"])
    await ed.call("open_scene", scene="scenes/SpritesV7.scene")
    await ed.call("wait", frames=10)
    before = decode_png(await ed.screenshot("scene"))
    pixels = b"".join(before[3])
    check(len({pixels[i:i + 3] for i in range(0, len(pixels), 3 * 97)}) > 1, "the view shows something")

    await ed.call("save_scene", scene="scenes/SpritesV7.scene")
    saved = yaml.safe_load(scene_file.read_text())
    check(saved["formatVersion"] > 7, f"saved at the new version: {saved['formatVersion']}")
    hero = next(e for e in saved["entities"] if e.get("name") == "Hero")
    check(len(hero["components"]["sprite"]["framesRect"]) == 3, "the inline frames are saved as they were")

    await ed.call("close_scene", scene="scenes/SpritesV7.scene")
    await ed.call("open_scene", scene="scenes/SpritesV7.scene")
    await ed.call("wait", frames=10)
    after = decode_png(await ed.screenshot("scene"))
    check(after[:3] == before[:3], f"same view size: {after[:3]} vs {before[:3]}")
    check(after[3] == before[3], "the migrated scene draws exactly what the v7 file drew")
