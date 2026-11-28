"""The Light 2D Inspector's notes (docs/specs/S78-sprite-normals-improvements.md R20), clicked with xdotool in the
harness's Xvfb: at height 0 "Height 0: normal maps are ignored" with Set Height 64 (one undo step); under a bright
2D ambient a note with Darken Ambient, which sets Ambient Light 2D to (0.24, 0.25, 0.33) at intensity 1 in one undo
step; neither note once its cause is gone."""

from __future__ import annotations

from harness import Editor
import ui_input

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Light2D Inspector"}
TIMEOUT = 600

INSPECTOR = "Inspector"
SET_HEIGHT = "Set Height 64##light2d_height"
DARKEN = "Darken Ambient##light2d_ambient"


async def height_of(ed: Editor, sid: int, entity: int) -> float:
    got = await ed.call("get_entity", scene=sid, entity=entity, components=["light2d"], properties=["height"])
    return float(got["components"]["light2d"]["height"])


async def ambient(ed: Editor, sid: int) -> tuple[list, float]:
    got = (await ed.call("get_scene_settings", scene=sid, names=["ambient_light_2d_color", "ambient_light_2d_intensity"]))["settings"]
    return got["ambient_light_2d_color"], float(got["ambient_light_2d_intensity"])


async def test_height_and_ambient_notes(ed: Editor):
    made = await ed.call("create_scene", name="Lamp Notes", type="2D")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    lamp = (await ed.call("create_entity", scene=sid, type="light_2d", name="Lamp", values={"light2d": {"height": 0}}))["entity_id"]
    ui = ui_input.Ui(ed)
    await ed.call("select_entities", scene=sid, entities=[lamp])
    await ed.call("focus_window", window="inspector")
    await ui.frames(10)
    ui.focus()
    await ui.frames()
    await ed.screenshot("editor", path=str(ed.scratch.root / "light2d-inspector-notes.png"), max_width=1600)

    # height 0: the note and its fix, one undo step
    await ui.click_revealed(SET_HEIGHT, INSPECTOR)
    ed.check_eq(await height_of(ed, sid, lamp), 64.0, "Set Height 64")
    ed.check(await ui.find(SET_HEIGHT, INSPECTOR, frames=6) is None, "no height note at 64")
    await ed.call("undo", scene=sid)
    ed.check_eq(await height_of(ed, sid, lamp), 0.0, "undo puts height 0 back")
    await ed.call("redo", scene=sid)

    # a new 2D scene's ambient is white at intensity 1: bright
    before = await ambient(ed, sid)
    await ui.click_revealed(DARKEN, INSPECTOR)
    color, intensity = await ambient(ed, sid)
    ed.check(all(abs(c - e) < 0.01 for c, e in zip(color, (0.24, 0.25, 0.33))), f"Darken Ambient's colour: {color}")
    ed.check_eq(intensity, 1.0, "Darken Ambient's intensity")
    ed.check(await ui.find(DARKEN, INSPECTOR, frames=6) is None, "no ambient note under a dark ambient")
    await ui.frames(4)
    await ed.screenshot("editor", path=str(ed.scratch.root / "light2d-inspector-fixed.png"), max_width=1600)
    await ed.call("undo", scene=sid)
    color, intensity = await ambient(ed, sid)
    ed.check(all(abs(c - e) < 0.01 for c, e in zip(color, before[0])) and intensity == before[1],
             f"one undo step puts the ambient back: {color} {intensity} (was {before})")
    await ed.call("close_scene", scene=sid, unsaved="discard")
