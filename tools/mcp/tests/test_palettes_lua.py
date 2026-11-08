"""Palette Swap and Palette Quantize through the existing tools and Lua (docs/specs/S43-palettes.md R41, R42, R45, AC17,
AC19's asset identity), on a new project: a 2D scene with two Sprites ("Hero", "Villain") drawing a 2x2 texture of the
palette's colours 1-4 with library:sprite, PALETTE_SWAP and a shaderPalettes.u_palette binding set by set_properties.
The assets directory is `assets/` (the fixtures' layout), so the paths tools take (project-relative) differ from the
ones materials and Lua use (relative to the assets directory).

- get_entity returns the resolved row names (palette_rows: "", "Team Blue") and the palette_swap_lossy_texture warning
  until the texture is lossless (set_texture_import_settings);
- in edit mode both draw row 0, the source's own colours (sample_view_color);
- in Play a script sets hero.palette = "Team Blue": the hero's pixels are the variant's colours; a wrong name raises
  the Lua error listing the rows (logged, read_console); villain:setPaletteTarget(2, Palette.new(...)) and
  paletteRow = 2 draw the runtime palette's colours, after the script let go of it and collected garbage; Palette.load / getHex / variants / nearest answer;
- scene.postProcessing.paletteEnabled = true (after set_scene_settings named post_processing.palette) puts the
  background, a colour outside the palette, onto PICO-8;
- the scene keeps the binding through save and reopen, and renaming the palette with move_asset keeps the reference.
"""

from __future__ import annotations

import json

import pngdiff
from harness import REPO, Editor

CI = True
TIMEOUT = 900
EDITOR = {"create": True, "name": "PaletteLua"}

M = "submeshes[0].material."
HERO_PNG = "assets/sprites/hero.png"
PALETTE = "assets/Palettes/hero.palette"
SOURCE = ["#1a1c2c", "#b13e53", "#ef7d57", "#ffcd75", "#38b764"]
TEAM_BLUE = [None, "#29366f", "#3b5dc9", "#41a6f6", "#73eff7"]
RUNTIME = ["#000000", "#ff0000", "#00ff00", "#0000ff", "#ffff00"]
PICO8 = json.loads((REPO / "editor" / "resources" / "palettes" / "pico-8.palette").read_text())["colors"]
CANVAS = (320, 180)
HERO_AT = (100, 90)
VILLAIN_AT = (220, 90)
BACKGROUND = [0.2, 0.5, 0.75, 1.0]
S: dict = {}

LUA = """---@class PaletteProbe : ScriptInstance
local PaletteProbe = {}

function PaletteProbe:init()
    local hero = Sprite(self.scene, self.entity)
    hero.palette = "Team Blue"
    paletteProbeName = hero.palette
    paletteProbeRow = hero.paletteRow
    local ok, err = pcall(function() hero.palette = "Purple" end)
    paletteProbeWrongOk = ok
    paletteProbeWrong = tostring(err)
    Log.error(tostring(err))

    local villain = Sprite(self.scene, self.scene:findEntity("Villain"))
    local runtime = Palette.new({ "#000000", "#ff0000", "#00ff00", "#0000ff", "#ffff00" })
    villain:setPaletteTarget(2, runtime)
    villain.paletteRow = 2
    paletteProbeRuntimeCount = runtime.count
    -- the script lets go of it before the first draw: the material's row keeps it
    runtime = nil
    collectgarbage()
    collectgarbage()

    local loaded = Palette.load("Palettes/hero.palette")
    paletteProbeHex = loaded:getHex(1)
    paletteProbeVariants = #loaded.variants
    paletteProbeNearest = loaded:nearest(Vector4(0.0066, 0.0078, 0.021, 1.0))   -- #1a1c2c, linear
    RegisterEngineEvent(self, "onUpdate")
    paletteProbeDone = true
end

function PaletteProbe:onUpdate()
    if paletteProbeQuantize and not paletteProbeQuantized then
        local p = self.scene.postProcessing
        p.paletteEnabled = true
        self.scene.postProcessing = p
        paletteProbeQuantized = true
    end
end

return PaletteProbe
"""


def view_point(size: dict, canvas_x: float, canvas_y: float) -> tuple[int, int]:
    """A canvas point (y up from the bottom left) in Game view pixels (the canvas fitted, centred)."""
    width, height = size["width"], size["height"]
    scale = min(width / CANVAS[0], height / CANVAS[1])
    left = (width - CANVAS[0] * scale) / 2
    top = (height - CANVAS[1] * scale) / 2
    return int(left + canvas_x * scale), int(top + (CANVAS[1] - canvas_y) * scale)


async def probe_sprite(ed: Editor, at: tuple[float, float]) -> set[str]:
    """The colours at the centres of a sprite's four texels (64 canvas px wide, 2x2 texels)."""
    size = (await ed.call("sample_view_color", view="game", x=0, y=0))["size"]
    hexes = set()
    for dx in (-16, 16):
        for dy in (-16, 16):
            x, y = view_point(size, at[0] + dx, at[1] + dy)
            hexes.add((await ed.call("sample_view_color", view="game", x=x, y=y))["hex"].lower())
    return hexes


async def test_setup(ed: Editor):
    await ed.call("get_project_summary")  # the new project is made by then
    (ed.project / "assets").mkdir(exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
    ed.check_eq((await ed.call("get_project_summary"))["assets_dir"], "assets")
    await ed.call("tool_groups", enable=["pixel_art", "editor_tools"])
    made = await ed.call("edit_palette", {"path": PALETTE, "from": {"colors": SOURCE},
                                          "ops": [{"op": "add_variant", "name": "Team Blue", "colors": TEAM_BLUE}], "output": True})
    S["asset_path"] = made["asset_path"]
    ed.check_eq(S["asset_path"], "Palettes/hero.palette", "the path materials store")
    pico = await ed.call("edit_palette", {"from": {"builtin": "pico-8"}, "output": True})
    S["pico"] = pico["path"]

    texture = ed.project / HERO_PNG
    texture.parent.mkdir(parents=True, exist_ok=True)
    rgba = b"".join(bytes(int(c[i:i + 2], 16) for i in (1, 3, 5)) + b"\xff" for c in SOURCE[1:])
    texture.write_bytes(pngdiff.encode(2, 2, rgba))

    await ed.call("set_project_settings", values={"graphics.canvas_width": CANVAS[0], "graphics.canvas_height": CANVAS[1]})
    scene = (await ed.call("create_scene", name="Palettes", type="2D"))["scene"]["id"]
    S["scene"] = scene
    await ed.call("set_scene_settings", scene=scene, settings={"background_color": BACKGROUND,
                                                                "post_processing": {"palette": S["pico"], "palette_enabled": False}})
    texture_value = {"path": HERO_PNG, "min_filter": "nearest", "mag_filter": "nearest"}
    for name, at in (("Hero", HERO_AT), ("Villain", VILLAIN_AT)):
        made = await ed.call("create_entity", scene=scene, type="sprite", name=name, position=[at[0], at[1], 0],
                             values={"mesh": {M + "baseColorTexture": texture_value},
                                     "sprite": {"pivotPreset": "center", "width": 64, "height": 64}})
        entity = made["entity_id"]
        S[name] = entity
        await ed.call("set_properties", scene=scene, entity=entity, values={"mesh": {M + "shader": "library:sprite"}})
        await ed.call("set_properties", scene=scene, entity=entity, values={"mesh": {
            M + "shaderFeatures": ["PALETTE_SWAP"],
            M + "shaderPalettes": {"u_palette": {"source": S["asset_path"]}}}})
    await ed.call("select_scene", scene=scene)


async def test_get_entity_row_names(ed: Editor):
    got = await ed.call("get_entity", scene=S["scene"], entity=S["Hero"], components=["mesh"])
    rows = got.get("palette_rows")
    ed.check(rows, f"get_entity returns the palette rows: {list(got)}")
    ed.check_eq(rows[0]["submesh"], 0)
    ed.check_eq(rows[0]["rows"], ["", "Team Blue"], "row 0 and the variant")
    ed.check_eq(rows[0]["row"], 0, "the active row")
    # R15: a compressed (the import default) texture under Palette Swap is a warning with its fix
    warnings = [w for w in got.get("warnings", []) if w.get("kind") == "palette_swap_lossy_texture"]
    ed.check(warnings and "lossless" in warnings[0]["message"] and warnings[0]["fix"], f"the lossy-texture warning: {got.get('warnings')}")
    await ed.call("set_texture_import_settings", paths=[HERO_PNG], settings={"quality": "none", "mipmaps": False})
    got = await ed.call("get_entity", scene=S["scene"], entity=S["Hero"], components=["mesh"])
    ed.check(not [w for w in got.get("warnings", []) if w.get("kind") == "palette_swap_lossy_texture"],
             f"lossless and Nearest: no warning: {got.get('warnings')}")


async def test_edit_mode_draws_the_source(ed: Editor):
    await ed.screenshot(view="game", max_width=0)
    original = {c for c in SOURCE[1:]}
    ed.check_eq(await probe_sprite(ed, HERO_AT), original, "the hero draws row 0")
    ed.check_eq(await probe_sprite(ed, VILLAIN_AT), original, "the villain draws row 0")


async def test_lua_swaps_rows(ed: Editor):
    scene = S["scene"]
    await ed.call("create_script", kind="lua", name="PaletteProbe", content=LUA, entity=S["Hero"])
    saved = await ed.call("save_scene", scene=scene, path="assets/scenes/Palettes")
    S["scene_path"] = saved["scene"]["path"]
    await ed.call("edit_build_scenes", action="add", scene=S["scene_path"])
    await ed.call("edit_build_scenes", action="set_start", scene=S["scene_path"])
    await ed.call("save_project")
    await ed.console_since(None, limit=1)
    try:
        started = await ed.call("play", action="start", scene=scene)
        ed.check_eq(started["state"], "playing", "play starts")
        waited = await ed.call_raw("wait", {"until": "paletteProbeDone == true", "timeout_seconds": 60})
        ed.check(not waited.is_error, f"the probe ran: {waited.text[:800]}")
        values = (await ed.call("eval_lua", code="return paletteProbeName, paletteProbeRow, paletteProbeWrongOk, paletteProbeWrong, "
                                                 "paletteProbeRuntimeCount, paletteProbeHex, paletteProbeVariants, paletteProbeNearest"))["values"]
        ed.check_eq(values[0], "Team Blue", "hero.palette reads the row's name")
        ed.check_eq(values[1], 1, "hero.paletteRow")
        ed.check(values[2] is False and "Team Blue" in values[3] and "Purple" in values[3], f"a wrong name lists the rows: {values[3]}")
        ed.check_eq(values[4], 5, "Palette.new's count")
        ed.check_eq(values[5], SOURCE[1], "Palette.load + getHex(1) (indices from 0)")
        ed.check_eq(values[6], 1, "p.variants")
        ed.check_eq(values[7], 0, "p:nearest finds the nearest index")
        logged = [e.get("message", "") for e in await ed.console_since(None, limit=500) if e.get("type") == "error"]
        ed.check(any("Purple" in m and "Team Blue" in m for m in logged), f"the Lua error is in the Console: {logged}")

        await ed.call("wait", frames=3)
        ed.check_eq(await probe_sprite(ed, HERO_AT), set(TEAM_BLUE[1:]), "the hero draws Team Blue")
        ed.check_eq(await probe_sprite(ed, VILLAIN_AT), set(RUNTIME[1:]), "the villain draws the runtime palette")

        # Palette Quantize: the background (outside every palette) goes onto PICO-8 once the table is baked
        size = (await ed.call("sample_view_color", view="game", x=0, y=0))["size"]
        x, y = view_point(size, 20, 160)
        before = (await ed.call("sample_view_color", view="game", x=x, y=y))["hex"].lower()
        ed.check(before not in PICO8, f"the background is off the palette first: {before}")
        await ed.call("eval_lua", code="paletteProbeQuantize = true")
        waited = await ed.call_raw("wait", {"until": "paletteProbeQuantized == true", "timeout_seconds": 30})
        ed.check(not waited.is_error, f"the script turned quantize on: {waited.text[:400]}")
        after = before
        for _ in range(120):
            await ed.call("wait", frames=5)
            after = (await ed.call("sample_view_color", view="game", x=x, y=y))["hex"].lower()
            if after in PICO8:
                break
        ed.check(after in PICO8, f"Palette Quantize puts the background on PICO-8: {after}")
    finally:
        await ed.call("play", action="stop")
    await ed.check_alive()


async def test_scene_keeps_the_binding(ed: Editor):
    scene_file = ed.project / S["scene_path"]
    text = scene_file.read_text()
    ed.check("shaderPalettes" in text and "u_palette" in text and "Palettes/hero.palette" in text,
             "the scene file holds the binding")
    ed.check("sourceGuid" in text, "and the palette's GUID")
    await ed.call("close_scene", scene=S["scene"], unsaved="discard")
    opened = await ed.call("open_scene", scene=S["scene_path"])
    S["scene"] = opened["scene"]["id"] if isinstance(opened.get("scene"), dict) else opened.get("scene_id", S["scene"])
    hero = (await ed.call("find_entities", scene=S["scene"], name="Hero"))["entities"][0]["id"]
    got = await ed.call("get_entity", scene=S["scene"], entity=hero, properties=[M + "shaderPalettes"])
    ed.check("Palettes/hero.palette" in json.dumps(got["components"]), f"the binding round-trips: {json.dumps(got['components'])[:400]}")
    ed.check_eq(got["palette_rows"][0]["rows"], ["", "Team Blue"], "the rows after reopening")

    await ed.call("move_asset", path=PALETTE, to="assets/Palettes/heroes.palette")
    got = await ed.call("get_entity", scene=S["scene"], entity=hero, properties=[M + "shaderPalettes"])
    shown = json.dumps(got["components"])
    ed.check("Palettes/heroes.palette" in shown and "Palettes/hero.palette\"" not in shown, f"the rename follows the GUID: {shown[:400]}")
    ed.check_eq(got["palette_rows"][0]["rows"], ["", "Team Blue"], "the rows after the rename")
