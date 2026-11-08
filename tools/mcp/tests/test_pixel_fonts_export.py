"""S51 AC22 (docs/specs/S51-pixel-fonts.md R34): the exported game draws its bitmap text as the
editor's Game view does, from the cooked .jfont files, without shipping the font sources. It reads
what `SMOKE_METHOD=compile tools/smoke-test.sh tools/fixtures/pixel-fonts` left in build/smoke/ (GL
core): game.png, the screen with the game's 1280x720 window, and export/, the exported project.
Not in the CI suite: it needs that smoke run first (SMOKE_GAME_PNG names another game.png).
"""

from __future__ import annotations

import os
from pathlib import Path

from harness import REPO, Editor
from htmlui_pixels import decode_png

EDITOR = {"project": REPO / "tools" / "fixtures" / "pixel-fonts"}
CI = False
TIMEOUT = 600
OUT = REPO / "build" / "test-results" / "pixel-fonts-export"


def rgb_rows(png: bytes):
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
