"""Sprite normals phase 1 in the real editor (docs/superpowers/specs/2026-10-27-sprite-normals-phase1-engine-design.md
section 5.3): Light 2Ds the editor makes (create_entity light_2d, add_component) start at height 64 and undo
removes them; a sprite with an OpenGL normal map (a hemisphere, green up) is brighter on the side of a Light 2D at
its left, and on its top under one above, with the built-in shader and with library:sprite; library:sprite keeps
the material's Normal Texture property; a Lua script in play mode assigns the normal map
(Sprite:setNormalTexture, getNormalTexture) and the sprite lights the same.

S78 (docs/specs/S78-sprite-normals-improvements.md R14, R15, R23): create_entity light_2d and set_properties light2d
say `next` (set_scene_settings with a dark ambient) while the scene's 2D ambient is bright, by its brightness (intensity
times its brightest channel), not after it is darkened; generate_sprite_normals warns light_low for a Light 2D under
half a 512 px sprite's side, not for a 16 px one, nor once the light is raised."""

from __future__ import annotations

import math
import sys
from pathlib import Path

from harness import Editor

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import pngdiff  # noqa: E402
from htmlui_pixels import decode_png  # noqa: E402

EDITOR = {"create": True, "name": "Light2D Normals"}
CI = True
TIMEOUT = 900

SIZE = 64
WHITE = "assets/sprites/white.png"
DOME = "assets/sprites/dome.png"
M = "submeshes[0].material."
# the scene background: pure blue, so the sprite's dark side still stands out from it
BACKGROUND = (0, 0, 255)
S: dict = {}


def dome_rgba() -> bytes:
    """A hemisphere normal map, OpenGL convention: n = (nx, ny up, sqrt(1 - r2)) inside the disc, +Z outside."""
    out = bytearray()
    half = SIZE / 2
    for y in range(SIZE):
        for x in range(SIZE):
            nx = (x + 0.5 - half) / half
            ny = (half - (y + 0.5)) / half
            r2 = nx * nx + ny * ny
            n = (nx, ny, math.sqrt(1 - r2)) if r2 < 1 else (0.0, 0.0, 1.0)
            out += bytes(round((c * 0.5 + 0.5) * 255) for c in n) + b"\xff"
    return bytes(out)


def write_textures(ed: Editor) -> None:
    folder = ed.project / "assets" / "sprites"
    folder.mkdir(parents=True, exist_ok=True)
    (ed.project / WHITE).write_bytes(pngdiff.encode(SIZE, SIZE, b"\xff" * (SIZE * SIZE * 4)))
    (ed.project / DOME).write_bytes(pngdiff.encode(SIZE, SIZE, dome_rgba()))


def component_values(got: dict, key: str) -> dict:
    """The values of the component holding `key` in a get_entity result."""
    for values in got.get("components", {}).values():
        if isinstance(values, dict) and key in values:
            return values
    return {}


async def light_height(ed: Editor, sid, entity) -> float:
    got = await ed.call("get_entity", scene=sid, entity=entity, properties=["height"], detail="values")
    values = component_values(got, "height")
    ed.check(values, f"the Light 2D's height: {got!r}")
    return float(values["height"])


async def shot(ed: Editor, sid, label: str):
    args = {"view": "game", "max_width": 0}
    if sid is not None:
        args["scene"] = sid
    raw = await ed.call_raw("screenshot", args)
    ed.check(not raw.is_error and raw.images, f"{label}: no screenshot: {raw.text[:300]}")
    png = raw.images[0][0]
    (ed.scratch.root / f"light2d-normals-{label}.png").write_bytes(png)
    S["canvas"] = raw.value.get("canvas") if isinstance(raw.value, dict) else None
    return decode_png(png)


def quarters(image, label: str, ed: Editor) -> dict:
    """Mean luminance of the left, right, top and bottom quarters of the sprite (what is not background)."""
    width, height, ch, rows = image
    xs, ys = [], []
    for y in range(height):
        row = rows[y]
        for x in range(width):
            p = row[x * ch:x * ch + 3]
            if abs(p[0] - BACKGROUND[0]) + abs(p[1] - BACKGROUND[1]) + abs(p[2] - BACKGROUND[2]) > 60:
                xs.append(x)
                ys.append(y)
    ed.check(len(xs) > 100, f"{label}: the sprite is on screen ({len(xs)} pixels)")
    x0, x1, y0, y1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    qw, qh = max(1, (x1 - x0) // 4), max(1, (y1 - y0) // 4)

    def mean(ax0, ay0, ax1, ay1):
        total = count = 0
        for y in range(ay0, ay1):
            row = rows[y]
            for x in range(ax0, ax1):
                p = row[x * ch:x * ch + 3]
                total += (p[0] + p[1] + p[2]) / 3
                count += 1
        return total / max(count, 1)

    return {"left": mean(x0, y0, x0 + qw, y1), "right": mean(x1 - qw, y0, x1, y1),
            "top": mean(x0, y0, x1, y0 + qh), "bottom": mean(x0, y1 - qh, x1, y1), "box": (x0, y0, x1, y1)}


def brighter(ed: Editor, q: dict, lit: str, dark: str, label: str) -> None:
    ed.check(q[lit] > 1.3 * q[dark] and q[lit] > 40, f"{label}: the {lit} quarter is the lit one: {q}")


async def lit_from_light_side(ed: Editor, label: str) -> None:
    """The light at the sprite's left, then above it: the facing quarter is the bright one."""
    sx, sy = S["sprite_position"]
    await ed.call("set_properties", scene=S["scene"], entity=S["light"], values={"transform": {"position": [sx - 400, sy, 0]}})
    brighter(ed, quarters(await shot(ed, S["scene"], f"{label}-left"), label, ed), "left", "right", f"{label}, light on the left")
    await ed.call("set_properties", scene=S["scene"], entity=S["light"], values={"transform": {"position": [sx, sy + 400, 0]}})
    brighter(ed, quarters(await shot(ed, S["scene"], f"{label}-top"), label, ed), "top", "bottom", f"{label}, light above")


async def test_created_light_2d_is_at_64(ed: Editor):
    made = await ed.call("create_scene", name="Light Heights", type="2D")
    sid = made["scene"]["id"]
    light = (await ed.call("create_entity", scene=sid, type="light_2d", name="Lamp"))["entity_id"]
    ed.check_eq(await light_height(ed, sid, light), 64.0, "create_entity light_2d: height 64")
    await ed.call("undo", scene=sid)
    found = await ed.call("find_entities", scene=sid, name="Lamp")
    ed.check_eq(found["count"], 0, "undo removes the light")
    await ed.call("redo", scene=sid)
    light = (await ed.call("find_entities", scene=sid, name="Lamp"))["entities"][0]["id"]
    ed.check_eq(await light_height(ed, sid, light), 64.0, "redo: height 64")

    holder = (await ed.call("create_entity", scene=sid, type="empty", name="Holder"))["entity_id"]
    await ed.call("add_component", scene=sid, entity=holder, component="light2d")
    ed.check_eq(await light_height(ed, sid, holder), 64.0, "add_component Light 2D: height 64")
    await ed.call("undo", scene=sid)
    got = await ed.call("get_entity", scene=sid, entity=holder, properties=["height"], detail="values")
    ed.check(not component_values(got, "height"), f"undo removes the component: {got!r}")
    await ed.call("redo", scene=sid)
    ed.check_eq(await light_height(ed, sid, holder), 64.0, "redo: height 64")
    await ed.call("close_scene", scene=sid, unsaved="discard")


async def test_sprite_lit_from_the_light_side(ed: Editor):
    write_textures(ed)
    made = await ed.call("create_scene", name="Normals", type="2D")
    sid = S["scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=sid, settings={
        "background_color": [c / 255 for c in BACKGROUND] + [1.0], "ambient_light_2d_intensity": 0.0})
    # a 2D scene's world is its canvas in pixels, y up from the bottom left: the sprite at its centre
    await ed.call("select_scene", scene=sid)
    await shot(ed, sid, "empty")
    canvas = S["canvas"]
    ed.check(canvas and canvas.get("width") and canvas.get("height"), f"the screenshot names the canvas: {canvas!r}")
    position = [canvas["width"] / 2, canvas["height"] / 2]
    S["sprite_position"] = position
    sprite = await ed.call("create_entity", scene=sid, type="sprite", name="Dome", position=[position[0], position[1], 0],
                           values={"mesh": {M + "baseColorTexture": WHITE}, "sprite": {"pivotPreset": "center"},
                                   "transform": {"scale": [4, 4, 1]}})
    S["sprite"] = sprite["entity_id"]
    await ed.call("set_properties", scene=sid, entity=S["sprite"], values={"mesh": {M + "normalTexture": DOME}})
    light = await ed.call("create_entity", scene=sid, type="light_2d", name="Lamp", position=[position[0] - 400, position[1], 0],
                          values={"light2d": {"intensity": 1.0, "range": 4000.0, "falloff": 0.0}})
    S["light"] = light["entity_id"]
    ed.check_eq(await light_height(ed, sid, S["light"]), 64.0, "the lamp's height")
    await lit_from_light_side(ed, "built-in")


async def test_library_sprite_reads_the_normal_texture(ed: Editor):
    sid = S["scene"]
    await ed.call("set_properties", scene=sid, entity=S["sprite"], values={"mesh": {M + "shader": "library:sprite"}})
    got = await ed.call("get_entity", scene=sid, entity=S["sprite"], properties=[M + "normalTexture", M + "shader"], detail="values")
    values = component_values(got, M + "normalTexture")
    ed.check(DOME.split("/")[-1] in str(values.get(M + "normalTexture")), f"library:sprite keeps the Normal Texture: {values!r}")
    await lit_from_light_side(ed, "library-sprite")
    await ed.call("set_properties", scene=sid, entity=S["sprite"], values={"mesh": {M + "shader": ""}})


LUA = """---@class NormalProbe : ScriptInstance
local NormalProbe = {}

function NormalProbe:init()
    local sprite = Sprite(self.scene, self.entity)
    sprite:setNormalTexture("")
    normalProbeCleared = sprite:getNormalTexture()
    sprite:setNormalTexture("%s")
    normalProbePath = sprite:getNormalTexture()
    normalProbeProperty = sprite.normalTexture
    normalProbeDone = true
end

return NormalProbe
"""


async def test_lua_assigns_the_normal_map(ed: Editor):
    sid = S["scene"]
    # without a normal map in the scene: the script gives it one at play
    await ed.call("set_properties", scene=sid, entity=S["sprite"], values={"mesh": {M + "normalTexture": ""}})
    sx, sy = S["sprite_position"]
    await ed.call("set_properties", scene=sid, entity=S["light"], values={"transform": {"position": [sx - 400, sy, 0]}})
    await ed.call("create_script", kind="lua", name="NormalProbe", content=LUA % DOME, entity=S["sprite"])
    saved = await ed.call("save_scene", scene=sid, path="scenes/Normals")
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scene"]["path"])
    await ed.call("save_project")
    await ed.console_since(None, limit=1)
    try:
        started = await ed.call("play", action="start", scene=sid)
        ed.check_eq(started["state"], "playing", "play starts")
        waited = await ed.call_raw("wait", {"until": "normalProbeDone == true", "timeout_seconds": 60})
        ed.check(not waited.is_error, f"the probe ran: {waited.text[:800]}")
        values = (await ed.call("eval_lua", code="return normalProbeCleared, normalProbePath, normalProbeProperty"))["values"]
        ed.check_eq(values[0], "", "setNormalTexture('') removes it")
        ed.check_eq(values[1], DOME, "getNormalTexture returns the path")
        ed.check_eq(values[2], DOME, "the normalTexture property")
        q = quarters(await shot(ed, None, "lua-left"), "lua", ed)
        brighter(ed, q, "left", "right", "play mode, the script's normal map, light on the left")
        errors = [e.get("message", "") for e in await ed.console_since(None, limit=500) if e.get("type") == "error"]
        ed.check(not errors, f"no errors: {errors}")
    finally:
        await ed.call("play", action="stop")
    await ed.check_alive()


AMBIENT_HINT = ("set_scene_settings", "ambient_light_2d_color", "ambient_light_2d_intensity")


def hints_dark_ambient(result: dict) -> bool:
    return all(word in result.get("next", "") for word in AMBIENT_HINT)


async def test_light_2d_next_under_a_bright_ambient(ed: Editor):
    made = await ed.call("create_scene", name="Ambient Hint", type="2D")
    sid = made["scene"]["id"]
    # a new 2D scene's ambient is white at intensity 1
    raw = await ed.call_raw("create_entity", {"scene": sid, "type": "light_2d", "name": "Lamp"})
    ed.check(not raw.is_error and hints_dark_ambient(raw.value), f"create_entity light_2d: next names the fix: {raw.value!r}")
    ed.check("set_scene_settings" in raw.text, f"and its text: {raw.text}")
    lamp = raw.value["entity_id"]
    edited = await ed.call("set_properties", scene=sid, entity=lamp, values={"light2d": {"range": 300}})
    ed.check(hints_dark_ambient(edited), f"set_properties light2d: next: {edited!r}")
    moved = await ed.call("set_properties", scene=sid, entity=lamp, values={"transform": {"position": [10, 20, 0]}})
    ed.check("next" not in moved, f"not for another component: {moved!r}")
    sprite = await ed.call("create_entity", scene=sid, type="sprite", name="Crate")
    ed.check("next" not in sprite, f"not for another type: {sprite!r}")

    # darkened as the hint says: no next
    await ed.call("set_scene_settings", scene=sid, settings={"ambient_light_2d_color": [0.24, 0.25, 0.33], "ambient_light_2d_intensity": 1})
    again = await ed.call("create_entity", scene=sid, type="light_2d", name="Lamp 2")
    ed.check("next" not in again, f"a dark-blue ambient at intensity 1 is not bright: {again!r}")
    edited = await ed.call("set_properties", scene=sid, entity=lamp, values={"light2d": {"range": 400}})
    ed.check("next" not in edited, f"set_properties under a dark ambient: {edited!r}")
    # white at 0.8 is bright, at 0.7 not
    await ed.call("set_scene_settings", scene=sid, settings={"ambient_light_2d_color": [1, 1, 1], "ambient_light_2d_intensity": 0.8})
    ed.check(hints_dark_ambient(await ed.call("set_properties", scene=sid, entity=lamp, values={"light2d": {"range": 500}})), "white at 0.8")
    await ed.call("set_scene_settings", scene=sid, settings={"ambient_light_2d_intensity": 0.7})
    ed.check("next" not in await ed.call("set_properties", scene=sid, entity=lamp, values={"light2d": {"range": 600}}), "white at 0.7")
    await ed.call("close_scene", scene=sid, unsaved="discard")


BIG_WHITE = "assets/sprites/white512.png"
SMALL_WHITE = "assets/sprites/white16.png"


async def test_light_low_over_a_large_sprite(ed: Editor):
    folder = ed.project / "assets" / "sprites"
    folder.mkdir(parents=True, exist_ok=True)
    (ed.project / BIG_WHITE).write_bytes(pngdiff.encode(512, 512, b"\xff" * (512 * 512 * 4)))
    (ed.project / SMALL_WHITE).write_bytes(pngdiff.encode(16, 16, b"\xff" * (16 * 16 * 4)))
    made = await ed.call("create_scene", name="Light Low", type="2D")
    sid = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=sid, settings={"ambient_light_2d_intensity": 0.2})
    await ed.call("select_scene", scene=sid)
    # drawn at their textures' sizes (a Sprite's Width and Height are what light_low measures)
    await ed.call("create_entity", scene=sid, type="sprite", name="Boss",
                  values={"mesh": {M + "baseColorTexture": BIG_WHITE}, "sprite": {"width": 512, "height": 512}})
    await ed.call("create_entity", scene=sid, type="sprite", name="Imp",
                  values={"mesh": {M + "baseColorTexture": SMALL_WHITE}, "sprite": {"width": 16, "height": 16}})
    lamp = (await ed.call("create_entity", scene=sid, type="light_2d", name="Lamp"))["entity_id"]
    ed.check_eq(await light_height(ed, sid, lamp), 64.0, "the editor's height")

    def low(result: dict):
        return next((w for w in result["warnings"] if w["kind"] == "light_low"), None)

    boss = await ed.call("generate_sprite_normals", scene=sid, source="Boss")
    w = low(boss)
    ed.check(w and "\"Lamp\"" in w["message"] and "512" in w["fix"], f"light_low over the 512 px Boss (a new project: 1 px per unit): {boss['warnings']}")
    imp = await ed.call("generate_sprite_normals", scene=sid, source="Imp")
    ed.check(low(imp) is None, f"not over the 16 px Imp: {imp['warnings']}")
    await ed.call("set_properties", scene=sid, entity=lamp, values={"light2d": {"height": 512}})
    boss = await ed.call("generate_sprite_normals", scene=sid, source="Boss")
    ed.check(low(boss) is None, f"raised to the sprite's side: {boss['warnings']}")
    await ed.call("close_scene", scene=sid, unsaved="discard")
