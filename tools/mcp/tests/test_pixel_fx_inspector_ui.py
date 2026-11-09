"""Pixel FX in the editor's UI (docs/specs/S48-pixel-fx.md R39, R40, AC24), driven in the harness's
Xvfb through find_ui_item: the post-processing Inspector's Glitch row (one undo step, its rows, the
Burst now button only while playing) and the Game view's Transition menu playing a transition over
the edit-mode Game view."""

from __future__ import annotations

import asyncio

import pngdiff
from harness import Editor
import ui_input

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Pixel FX Inspector"}
TIMEOUT = 900

INSPECTOR = "Inspector"
GAME = "Game"


async def glitch_enabled(ed: Editor, sid: int) -> bool:
    values = (await ed.call("get_scene_settings", scene=sid, names=["post_processing.glitch_enabled"]))["settings"]
    return bool(values["post_processing.glitch_enabled"])


async def test_glitch_rows(ed: Editor):
    sid = (await ed.call("create_scene", name="GlitchRows", type="3d"))["scene"]["id"]
    ui = ui_input.Ui(ed)                    # skips without xdotool or the harness's display
    await ed.call("select_scene", scene=sid)
    await ed.call("select_entities", entities=[])
    await ed.call("focus_window", window="inspector")
    await ui.frames(10)
    ui.focus()

    ed.check(not await glitch_enabled(ed, sid), "Glitch starts off")
    await ui.click_revealed("Glitch", INSPECTOR)
    await ui.frames()
    ed.check(await glitch_enabled(ed, sid), "the Glitch row's checkbox turns it on")
    for row in ("RGB Split", "Tear Shift", "Burst Rate", "Seed"):
        ed.check(await ui.find(row, INSPECTOR, frames=20) is not None, f"its {row} row shows")
    ed.check(await ui.find("Burst now", INSPECTOR, frames=6) is None, "no Burst now while stopped")
    await ed.screenshot("editor", path=str(ed.scratch.root / "glitch-rows.png"), max_width=1600)
    await ed.call("undo")
    ed.check(not await glitch_enabled(ed, sid), "one undo step turns it off again")

    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": {"glitch_enabled": True, "glitch_intensity": 0.0}})
    await ed.call("save_scene", scene=sid, path="GlitchRows.scene")
    await ed.call("play", action="start", scene=sid)
    try:
        await ed.call("select_entities", entities=[])
        await ui.frames(10)
        button = await ui.reveal("Burst now", INSPECTOR)
        ed.check(button is not None, "Burst now shows while playing")
        await ed.screenshot("editor", path=str(ed.scratch.root / "glitch-burst-now.png"), max_width=1600)
        await ui.click(ui.centre(button["rect"]))
        await ui.frames()
        ed.check(await glitch_enabled(ed, sid), "the burst leaves the saved settings as they were")
    finally:
        await ed.call("play", action="stop", keep_changes="none")


async def test_game_view_transition_button(ed: Editor):
    sid = (await ed.call("create_scene", name="TransitionButton", type="3d"))["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    await ed.call("set_project_settings", values={"graphics.screen_transitions.color": "#ff00ff",
                                                  "graphics.screen_transitions.duration": 3.0})
    ui = ui_input.Ui(ed)
    await ed.call("focus_window", window="game")
    await ui.frames(10)
    ui.focus()
    await ui.click_item("##GameTransition", GAME)
    await ui.frames()
    await ui.click_item("Fade")
    # the cover runs 45 % of 3 s on the editor's clock; at the hold every pixel is the ink
    await asyncio.sleep(1.6)
    await ui.frames(6)
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
    ed.check(not shot.is_error and shot.images, f"a Game view screenshot: {shot.text[:200]}")
    png = shot.images[0][0]
    (ed.scratch.root / "transition-preview.png").write_bytes(png)
    width, height, rgba = pngdiff.decode(png)
    ink = sum(1 for i in range(0, width * height * 4, 4) if rgba[i] > 240 and rgba[i + 1] < 15 and rgba[i + 2] > 240)
    ed.check(ink > width * height // 2, f"the edit-mode Game view shows the transition: {ink} of {width * height} pixels are the ink")
    await asyncio.sleep(2.0)
    await ui.frames(6)
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
    width, height, rgba = pngdiff.decode(shot.images[0][0])
    ink = sum(1 for i in range(0, width * height * 4, 4) if rgba[i] > 240 and rgba[i + 1] < 15 and rgba[i + 2] > 240)
    ed.check(ink == 0, f"revealed again: {ink} ink pixels")
    await ed.call("set_project_settings", values={"graphics.screen_transitions.color": "#0b0a10",
                                                  "graphics.screen_transitions.duration": 0.6})
