"""S51 AC22 (docs/specs/S51-pixel-fonts.md R34): the exported game draws its bitmap text as the
editor's Game view does, from the cooked .jfont files, without shipping the font sources. It reads
what `SMOKE_METHOD=compile tools/smoke-test.sh tools/fixtures/pixel-fonts` left in build/smoke/ (GL
core): game.png, the screen with the game's 1280x720 window, and export/, the exported project.
Not in the CI suite: it needs that smoke run first (SMOKE_GAME_PNG names another game.png).
"""

from __future__ import annotations

import os
import struct
import zlib
from pathlib import Path

from harness import REPO, Editor
from htmlui_pixels import decode_png

EDITOR = {"project": REPO / "tools" / "fixtures" / "pixel-fonts"}
CI = False
TIMEOUT = 600
OUT = REPO / "build" / "test-results" / "pixel-fonts-export"


def palette_rows(png: bytes):
    """an 8-bit palette PNG (ImageMagick's screenshot of the screen) as RGB rows"""
    pos, idat, palette = 8, b"", []
    while pos < len(png):
        length, kind = struct.unpack(">I4s", png[pos:pos + 8])
        data = png[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, _ = struct.unpack(">IIBB", data[:10])
            assert depth == 8, f"palette depth {depth}"
        elif kind == b"PLTE":
            palette = [tuple(data[i:i + 3]) for i in range(0, len(data), 3)]
        elif kind == b"IDAT":
            idat += data
        pos += 12 + length
    raw, rows, previous, i = zlib.decompress(idat), [], bytearray(w), 0
    for _ in range(h):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + w])
        i += 1 + w
        for x in range(w):
            a = line[x - 1] if x else 0
            b, c = previous[x], previous[x - 1] if x else 0
            if kind == 1: line[x] = (line[x] + a) & 255
            elif kind == 2: line[x] = (line[x] + b) & 255
            elif kind == 3: line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([palette[v] for v in line])
        previous = line
    return w, h, rows


def rgb_rows(png: bytes):
    if png[25] == 3:
        return palette_rows(png)
    w, h, channels, rows = decode_png(png)
    return w, h, [[tuple(row[x * channels:x * channels + 3]) for x in range(w)] for row in rows]


async def test_export_draws_text_as_the_editor(ed: Editor):
    game_png = Path(os.environ.get("SMOKE_GAME_PNG", REPO / "build" / "smoke" / "game.png"))
    ed.check(game_png.exists(), f"{game_png}: run SMOKE_METHOD=compile tools/smoke-test.sh tools/fixtures/pixel-fonts first")
    export = game_png.parent / "export"
    shipped = sorted(p.relative_to(export).as_posix() for p in export.rglob("*") if p.is_file())
    ed.check(any(p.endswith("fonts/hud.bitmapfont.jfont") for p in shipped), f"hud.bitmapfont ships cooked: {shipped[:40]}")
    ed.check(not any(p.endswith("fonts/hud@6x9.png") for p in shipped), "the font's sheet is not shipped")

    # the editor's Game view of the same scene, playing, at the game window's size
    await ed.call("configure_game_view", resolution="1280x720")
    await ed.call("play", action="start")
    try:
        await ed.call("wait", frames=60)
        editor_png = await ed.screenshot(view="game", max_width=0)
    finally:
        await ed.call("play", action="stop")
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "editor.png").write_bytes(editor_png)
    ew, eh, editor = rgb_rows(editor_png)
    gw, gh, screen = rgb_rows(game_png.read_bytes())

    # the window in the screen: where it is not the black root window
    xs = [x for y in range(gh) for x in range(gw) if screen[y][x] != (0, 0, 0)]
    ys = [y for y in range(gh) if any(p != (0, 0, 0) for p in screen[y])]
    ed.check(xs and ys, "the game drew its frame")
    ox, oy = min(xs), min(ys)
    ed.check((max(xs) + 1 - ox, max(ys) + 1 - oy) == (ew, eh), f"the window is {ew}x{eh} at ({ox}, {oy})")
    game = [row[ox:ox + ew] for row in screen[oy:oy + eh]]

    # the text: every pixel either image draws other than the background, tolerance 0
    background = editor[0][0]
    text = [(x, y) for y in range(eh) for x in range(ew) if editor[y][x] != background or game[y][x] != background]
    ed.check(len(text) > 1000, f"text drawn: {len(text)} pixels")
    differing = [(x, y) for x, y in text if editor[y][x] != game[y][x]]
    ed.check(not differing, f"{len(differing)} of {len(text)} text pixels differ between the editor and the export, first {differing[:5]}")
