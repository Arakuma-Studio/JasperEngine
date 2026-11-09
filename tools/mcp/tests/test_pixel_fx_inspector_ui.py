"""The sprite material Inspector's Pixel FX rows (docs/specs/S48-pixel-fx.md R37, R38, AC24), in the harness's
Xvfb with real input aimed where find_ui_item says a control is drawn now:

  test_setup              a 2D scene with a 16x16 sprite drawn with library:sprite, Dissolve and a Pixel4 outline on
  test_thumbnails         each feature row has its 40x40 thumbnail; a value change draws it again (its pixels in an
                          editor screenshot change), an unchanged value does not
  test_scrub              the Preview slider shows the dissolve in the Game view without a command: the scene is not
                          modified, the undo history is unchanged, and another selection puts the material's value back
  test_play               the play button plays the effect once and the value comes back by itself
  test_presets            Pixel FX presets apply as one undo step; Burn Away makes the project's copy of the fire palette
                          and points the ramp slot at its ramp; the presets list names them
Screenshots: <scratch>/pixel-fx-inspector/ (JE_EVIDENCE_DIR to keep them).
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pngdiff
from harness import Editor
import ui_input

EDITOR = {"xvfb": "harness", "create": True, "name": "Pixel FX Inspector"}
CI = True
TIMEOUT = 900

TEXTURE = "assets/textures/hero16.png"
M = "submeshes[0].material."
WINDOW = "Inspector"
S: dict = {}


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root) / "pixel-fx-inspector"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def shot(ed: Editor, name: str) -> bytes:
    return await ed.screenshot("editor", evidence(ed) / f"{name}.png", focus=False, max_width=0)


def ui_of(ed: Editor) -> ui_input.Ui:
    if "ui" not in S:
        S["ui"] = ui_input.for_editor(ed)
    return S["ui"]


def hero_png() -> bytes:
    """16x16: a transparent 3-texel margin around a body, a face and boots"""
    pixels = []
    for y in range(16):
        for x in range(16):
            c = (0, 0, 0, 0)
            if 3 <= x < 13 and 3 <= y < 13:
                c = (58, 68, 102, 255)
                if 4 <= y < 8 and 5 <= x < 11:
                    c = (232, 183, 150, 255)
                if y >= 11:
                    c = (139, 69, 19, 255)
            pixels.extend(c)
    return pngdiff.encode(16, 16, bytes(pixels))


async def material(ed: Editor) -> dict:
    got = await ed.call("get_entity", scene=S["scene"], entity=S["hero"], components=["mesh"])
    return got["components"]["mesh"]["submeshes[0].material"]


def param(mat: dict, name: str):
    """a one-number shader value (the editor gives [x, y, z, w])"""
    value = (mat.get("shaderParams") or {}).get(name)
    return value[0] if isinstance(value, list) else value


async def scene_state(ed: Editor) -> tuple[bool, int]:
    scenes = (await ed.call("list_scenes"))["scenes"]
    modified = next(s for s in scenes if s["id"] == S["scene"]).get("modified")
    history = (await ed.call("get_undo_history", scene=S["scene"], limit=500))["entries"]
    return modified, len(history)


def crop(png: bytes, rect: dict):
    return pngdiff.crop(pngdiff.decode(png), round(rect["x"]), round(rect["y"]), round(rect["width"]), round(rect["height"]))


async def test_setup(ed: Editor):
    (ed.project / TEXTURE).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / TEXTURE).write_bytes(hero_png())
    await ed.call("tool_groups", enable=["editor_ux"])
    scene = (await ed.call("create_scene", name="PixelFx", type="2D"))["scene"]["id"]
    S["scene"] = scene
    made = await ed.call("create_entity", scene=scene, type="sprite", name="Hero", position=[0, 0, 0],
                         values={"mesh": {M + "baseColorTexture": {"path": TEXTURE, "min_filter": "nearest", "mag_filter": "nearest"}},
                                 "sprite": {"pivotPreset": "center", "width": 64, "height": 64}})
    S["hero"] = made["entity_id"]
    other = await ed.call("create_entity", scene=scene, type="sprite", name="Other", position=[120, 0, 0],
                          values={"sprite": {"pivotPreset": "center", "width": 32, "height": 32}})
    S["other"] = other["entity_id"]
    await ed.call("set_properties", scene=scene, entity=S["hero"], values={"mesh": {M + "shader": "library:sprite"}})
    await ed.call("set_properties", scene=scene, entity=S["hero"], values={"mesh": {
        M + "shaderFeatures": ["DISSOLVE", "OUTLINE", "OUTLINE_STYLE_PIXEL4"],
        M + "shaderParams": {"dissolveProgress": 0.0, "outlineColorFrom": "Ramp darkest"}}})
    mat = await material(ed)
    ed.check_eq(param(mat, "outlineColorFrom"), 1, "a choice set by its name stores its index")
    await ed.call("select_scene", scene=scene)
    await ed.call("save_scene", scene=scene, path="assets/PixelFx.scene")
    await ed.call("select_entities", scene=scene, entities=[S["hero"]])
    await ui_input.frames(ed, 30)


async def test_thumbnails(ed: Editor):
    ui = ui_of(ed)
    for define in ("DISSOLVE", "OUTLINE", "DITHER_FADE", "RAMP_RECOLOR", "DROP_SHADOW"):
        item = await ui.find(f"##spritefx_thumb_{define}", WINDOW, frames=240)
        ed.check(item is not None, f"the {define} row has its thumbnail")
        if item:
            ed.check(round(item["rect"]["width"]) >= 40 and round(item["rect"]["height"]) >= 40, f"40x40: {item['rect']}")
    item = await ui.find("##spritefx_thumb_DISSOLVE", WINDOW)
    await ui_input.frames(ed, 20)
    before = crop(await shot(ed, "1-thumbnails"), item["rect"])
    await ui_input.frames(ed, 10)
    again = crop(await shot(ed, "1b-thumbnails-again"), item["rect"])
    ed.check(again == before, "an unchanged value draws the same thumbnail")

    # the dissolve's thumbnail follows its progress: half burnt, it shows less of the sprite
    await ed.call("set_properties", scene=S["scene"], entity=S["hero"], values={"mesh": {M + "shaderParams.dissolveProgress": 0.6}})
    changed = None
    for _ in range(60):
        await ui_input.frames(ed, 6)
        item = await ui.find("##spritefx_thumb_DISSOLVE", WINDOW)
        changed = crop(await shot(ed, "2-thumbnail-dissolve"), item["rect"])
        if changed != before:
            break
    ed.check(changed != before, "the thumbnail is drawn again when its value changes")
    await ed.call("set_properties", scene=S["scene"], entity=S["hero"], values={"mesh": {M + "shaderParams.dissolveProgress": 0.0}})


async def test_scrub(ed: Editor):
    ui = ui_of(ed)
    await ed.call("save_scene", scene=S["scene"])
    modified, history = await scene_state(ed)
    ed.check(not modified, "saved before the preview")
    game_before = await ed.screenshot("game", evidence(ed) / "3-game-before.png", max_width=0)

    slider = await ui.click_revealed("##spritefx_scrub_dissolveProgress", WINDOW)
    rect = slider["rect"]
    await ui.click(ui.at(rect, (rect["width"] * 0.7, rect["height"] / 2)))
    await ui_input.frames(ed, 20)
    mat = await material(ed)
    ed.check(param(mat, "dissolveProgress") and param(mat, "dissolveProgress") > 0.3,
             f"the preview shows in the scene's material: {param(mat, 'dissolveProgress')}")
    game = await ed.screenshot("game", evidence(ed) / "4-game-preview.png", max_width=0)
    ed.check(game != game_before, "the Game view shows the dissolve")
    modified, history_now = await scene_state(ed)
    ed.check(not modified, "the preview does not modify the scene")
    ed.check_eq(history_now, history, "the preview makes no undo step")
    await shot(ed, "5-scrubbed")

    # another selection puts the material's value back
    await ed.call("select_entities", scene=S["scene"], entities=[S["other"]])
    await ui_input.frames(ed, 10)
    ed.check_eq(param(await material(ed), "dissolveProgress"), 0, "the value is back after the selection changed")
    modified, _ = await scene_state(ed)
    ed.check(not modified, "still not modified")
    await ed.call("select_entities", scene=S["scene"], entities=[S["hero"]])
    await ui_input.frames(ed, 10)


async def test_play(ed: Editor):
    ui = ui_of(ed)
    await ui.click_revealed("##spritefx_play_dissolveProgress", WINDOW)
    seen = 0.0
    for _ in range(30):
        await ui_input.frames(ed, 2)
        value = param(await material(ed), "dissolveProgress") or 0.0
        seen = max(seen, value)
        if seen > 0.0 and value == 0:
            break
    ed.check(seen > 0.0, "the play button animates the value")
    back = None
    for _ in range(120):
        await ui_input.frames(ed, 4)
        back = param(await material(ed), "dissolveProgress")
        if back == 0:
            break
    ed.check_eq(back, 0, "the value comes back at the end")
    modified, _ = await scene_state(ed)
    ed.check(not modified, "playing the preview does not modify the scene")


async def test_presets(ed: Editor):
    listed = await ed.call("list_material_presets")
    ids = {p["id"] for p in listed["presets"]}
    for preset in ("hit_flash", "pixel_outline", "selout_outline", "drop_shadow", "teleport_fade", "burn_away",
                   "petrify", "frozen", "poison", "pixel_enemy"):
        ed.check(preset in ids, f"list_material_presets names {preset}")

    _, history = await scene_state(ed)
    applied = await ed.call("apply_material_preset", preset="drop_shadow", scene=S["scene"], entity=S["hero"])
    ed.check(applied.get("undo", {}).get("id") is not None, f"one undo step: {applied}")
    _, after = await scene_state(ed)
    ed.check_eq(after, history + 1, "one scene undo step")
    mat = await material(ed)
    ed.check("DROP_SHADOW" in mat["shaderFeatures"] and "DISSOLVE" in mat["shaderFeatures"],
             f"a preset adds its feature and keeps the others: {mat['shaderFeatures']}")

    await ed.call("apply_material_preset", preset="burn_away", scene=S["scene"], entity=S["hero"])
    copy = ed.project / "assets" / "Palettes" / "fire.palette"
    ed.check(copy.is_file(), "burn_away makes the project's copy of the fire palette")
    mat = await material(ed)
    textures = mat.get("shaderTextures") or {}
    ramp = textures.get("u_fxRampTexture")
    ramp_path = ramp.get("path") if isinstance(ramp, dict) else ramp
    ed.check_eq(ramp_path, "Palettes/fire.palette#ramp:fire", "the ramp slot points at the copy's ramp")
    await ed.call("undo", scope="scene")
    mat = await material(ed)
    textures = mat.get("shaderTextures") or {}
    ed.check("u_fxRampTexture" not in textures, f"one undo takes the preset back: {textures}")
    await shot(ed, "6-presets")
