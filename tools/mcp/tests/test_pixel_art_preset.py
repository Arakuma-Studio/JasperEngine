"""S42 AC9 (docs/specs/S42-pixel-art-mode.md R2, R23): the Pixel Art preset through check_pixel_art on a copy
of the built-in 2d template. fix: ["pixel_art_mode"] then fix: ["all"], with output, give: canvas 320x180,
scaling integer, PPU 1, Default Filter Mode point, window 1280x720, a Pixel Perfect Camera on Main's
camera, player.png.meta with pixelArt: true, the sprite's reference nearest; the check is then clean,
and undoing the two steps restores every file byte for byte, the metas the cook queue wrote included:
once Pixel Art Mode is on, the queue's first imports of images the project already had are S14's
(no pixelArt), so the preset's undo leaves nothing of the mode behind (D2).
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
    values = got["settings"]
    return {k: values.get(k) for k in keys}


async def cooks_idle(ed: Editor) -> None:
    """every texture's cook planned and written: the metas hold the import settings the queue inferred"""
    for _ in range(300):
        report = await ed.call("get_texture_cook_report", platforms=["desktop"])
        busy = [row["path"] for row in report["textures"] if row["platforms"]["desktop"]["status"] in ("queued", "cooking")]
        if not busy:
            return
        await ed.call("wait", frames=10)
    ed.check(False, f"the texture cooks did not finish: {busy}")


async def test_preset_through_fixes(ed: Editor):
    await ed.call("tool_groups", enable=["pixel_art"])
    # the 2d template's images get their import settings from their first cook
    await cooks_idle(ed)
    before = hashes(ed.project)
    metas = {k: (ed.project / k).read_text() for k in before if k.endswith(".png.meta")}
    ed.check(metas and all("textureImporter" in text for text in metas.values()), f"every image has its import settings: {metas}")

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

    # Ctrl+Z twice: each fix is one step of the timeline (its histories linked)
    await ed.call("undo", scope="timeline")
    await ed.call("undo", scope="timeline")
    # the recooks after the undo write no meta: every one is as it was, no pixelArt left behind
    await cooks_idle(ed)
    for path, text in metas.items():
        ed.check_eq((ed.project / path).read_text(), text, f"{path} after undo")
    ed.check_eq(hashes(ed.project), before, "undo restores the project byte-identically")


async def test_preset_converts_particle_textures(ed: Editor):
    """R2: the preset marks the textures a particle renderer draws as pixel art, as it does a Sprite's"""
    async with Editor(create=True, name="Pixel Art Particles") as p:
        await p.call("tool_groups", enable=["pixel_art", "particles"])
        await p.call("generate_placeholder_art", path="fx/spark.png", width=8, height=8, color="#ffcc00")
        made = await p.call("create_scene", name="Fx", type="2D", path="scenes/Fx")
        scene = made["scene"]["id"]
        await p.call("create_particles", scene=scene, name="Sparks", preset="sparks", position=[0, 0, 0], texture="fx/spark.png")
        await p.call("save_scene", scene=scene)
        await cooks_idle(p)
        # as Pixel Art Mode sees the project (the preset's view): its 2D scenes are pixel art
        await p.call("set_project_settings", values={"pixel_art.enabled": True})
        preview = await p.call("check_pixel_art", fix=["all"])
        targets = {(c["target"], c["field"]) for fix in preview.get("fix_plan", []) for c in fix["changes"]}
        p.check(any(target.endswith("fx/spark.png") and field == "import.textureImporter" for target, field in targets),
                f"the particle texture is converted: {sorted(targets)} {str(preview)[:1500]}")
