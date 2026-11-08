"""S42 AC9 (docs/specs/S42-pixel-art-mode.md R2, R23): the Pixel Art preset through check_pixel_art on a copy
of the built-in 2d template. fix: ["pixel_art_mode"] then fix: ["all"], with output, give: canvas 320x180,
scaling integer, PPU 1, Default Filter Mode point, window 1280x720, a Pixel Perfect Camera on Main's
camera, player.png.meta with pixelArt: true, the sprite's reference nearest; the check is then clean,
and undoing the two steps restores every file byte for byte.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

from harness import REPO, Editor

CI = True
TIMEOUT = 900
EDITOR = {"project": REPO / "editor" / "resources" / "templates" / "2d"}
M = "submeshes[0].material."


def hashes(root: Path) -> dict[str, str]:
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob("*"))
            if p.is_file() and p.suffix in (".yaml", ".meta", ".scene", ".png") and ".jasperengine" not in p.parts}


async def settings(ed: Editor) -> dict:
    keys = ["graphics.canvas_width", "graphics.canvas_height", "graphics.scaling_mode", "graphics.pixels_per_unit",
            "graphics.default_filter_mode", "player.window_width", "player.window_height", "pixel_art.enabled"]
    got = await ed.call("get_project_settings", keys=keys)
    values = got.get("values", got)
    return {k: (values[k]["value"] if isinstance(values.get(k), dict) and "value" in values[k] else values.get(k)) for k in keys}


async def test_preset_through_fixes(ed: Editor):
    await ed.call("tool_groups", enable=["pixel_art"])
    # the 2d template's art gets its import settings from the first cook
    for _ in range(100):
        meta = ed.project / "sprites" / "player.png.meta"
        if meta.exists() and "textureImporter" in meta.read_text():
            break
        await ed.call("wait", frames=10)
    before = hashes(ed.project)

    first = await ed.call("check_pixel_art", fix=["pixel_art_mode"], output=True)
    ed.check(first.get("applied", {}).get("undo_step"), f"the project part applied: {first.get('applied')}")
    second = await ed.call("check_pixel_art", fix=["all"], output=True)
    ed.check(second.get("applied", {}).get("undo_step"), f"the rest applied: {second.get('applied')}")

    got = await settings(ed)
    ed.check_eq(got, {"graphics.canvas_width": 320, "graphics.canvas_height": 180, "graphics.scaling_mode": "integer",
                      "graphics.pixels_per_unit": 1, "graphics.default_filter_mode": "point", "player.window_width": 1280,
                      "player.window_height": 720, "pixel_art.enabled": True}, "the preset's project settings")
    tree = await ed.call("get_scene_tree", scene="Main", format="json")
    text = str(tree)
    ed.check("Main Camera" in text, f"a Main Camera: {text[:600]}")
    camera = await ed.call("get_entity", scene="Main", entity="Main Camera")
    ed.check("pixelperfectcamera" in camera["components"], f"with a Pixel Perfect Camera: {list(camera['components'])}")
    ed.check("pixelArt: true" in (ed.project / "sprites" / "player.png.meta").read_text(), "player.png.meta has pixelArt: true")
    player = await ed.call("get_entity", scene="Main", entity="Player", components=["mesh"])
    texture = player["components"]["mesh"].get(M + "baseColorTexture") or player["components"]["mesh"]["submeshes"][0]["material"]["baseColorTexture"]
    ed.check(isinstance(texture, dict) and texture.get("min_filter") == "nearest" and texture.get("mag_filter") == "nearest",
             f"the sprite's reference is nearest: {texture}")
    clean = await ed.call("check_pixel_art")
    ed.check_eq(clean["verdict"], "clean", f"the check is clean: {[f['key'] for f in clean['findings']]}")

    await ed.call("undo")
    await ed.call("undo")
    ed.check_eq(hashes(ed.project), before, "undo restores the project byte-identically")
