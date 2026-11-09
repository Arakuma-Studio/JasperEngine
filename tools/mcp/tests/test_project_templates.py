"""S42 AC10 (docs/specs/S42-pixel-art-mode.md R21): the built-in 2D Pixel Art template. create_project
{template: "2d-pixel-art"} makes a project that check_pixel_art finds clean (0 FAIL, 0 WARN); in Play at
Pixel Scale 1x the Game view shows the 320x180 base image pixel for pixel as a CPU reference composes
it (S41 AC4's method): the scene's background with the 16x16 player blitted at its position.
"""

from __future__ import annotations

from pathlib import Path

import pngdiff
from harness import Editor

CI = True
TIMEOUT = 900
EDITOR = {"create": True, "name": "Templates", "xvfb": "harness"}


def rgba_at(rgba: bytes, width: int, x: int, y: int) -> tuple:
    i = (y * width + x) * 4
    return tuple(rgba[i:i + 4])


async def test_pixel_art_template_is_clean_and_pixel_exact(ed: Editor):
    target = ed.scratch.root / "PixelGame"
    await ed.call("create_project", path=str(target), name="Pixel Game", template="2d-pixel-art", unsaved="discard")
    for _ in range(300):
        status = await ed.call("editor_status")
        if Path(str(status.get("project", {}).get("path", ""))).resolve() == target.resolve():
            break
        await ed.call("wait", frames=10)
    templates = await ed.call("list_project_templates")
    entry = [t for t in templates["templates"] if t["id"] == "2d-pixel-art"]
    ed.check(entry and entry[0]["category"] == "Core", f"listed in Core: {entry}")

    result = await ed.call("check_pixel_art")
    ed.check_eq(result["verdict"], "clean", f"the template is clean: {[f['key'] for f in result['findings']]}")
    ed.check_eq(result["counts"]["warn"], 0, "0 WARN")
    ed.check(result["project"]["mode"] is True and result["project"]["base_resolution"] == [320, 180], f"{result['project']}")

    await ed.call("configure_game_view", resolution="1280x720", pixel_scale=1)
    await ed.call("play", action="start", fixed_fps=60, timeout_seconds=300)
    try:
        await ed.call("wait", frames=20)
        shot = await ed.screenshot(view="game", max_width=0)
    finally:
        await ed.call("play", action="stop")
        await ed.call("configure_game_view", pixel_scale=0)
    width, height, rgba = pngdiff.decode(shot)
    # the base image at 1:1 in the middle of the view, bars around
    x0, y0 = (width - 320) // 2, (height - 180) // 2
    background = rgba_at(rgba, width, x0 + 2, y0 + 2)
    player_w, player_h, player_rgba = pngdiff.decode((target / "sprites" / "player.png").read_bytes())
    # the reference: background, and the player's texels at (152, 82) from the bottom-left (y up)
    px, py = 152, 180 - 82 - player_h
    diffs = 0
    for y in range(180):
        for x in range(320):
            expected = background
            if px <= x < px + player_w and py <= y < py + player_h:
                texel = rgba_at(player_rgba, player_w, x - px, y - py)
                # the import's Alpha Clip at 0.5: opaque from 128 up, transparent below
                if texel[3] >= 128:
                    expected = texel
            got = rgba_at(rgba, width, x0 + x, y0 + y)
            if got[:3] != expected[:3]:
                diffs += 1
    ed.check_eq(diffs, 0, "the 1x frame equals the CPU reference, pixel for pixel")
