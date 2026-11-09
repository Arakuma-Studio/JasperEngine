"""Pixel FX in the editor's UI (docs/specs/S48-pixel-fx.md R39, R40, AC24), driven in the harness's
Xvfb through find_ui_item: the post-processing Inspector's Glitch row (one undo step, its rows, the
Burst now button only while playing) and the Game view's Transition menu playing a transition over
the edit-mode Game view."""

from __future__ import annotations

import asyncio

import pngdiff
import harness
from harness import Editor
import ui_input

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Pixel FX Inspector"}
TIMEOUT = 900

INSPECTOR = "Inspector"
# copies of the screenshots outside the container's scratch (fetched for review)
EVIDENCE = harness.REPO / "build" / "test-results" / "pixel_fx"


def keep(name: str, png: bytes) -> None:
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    (EVIDENCE / name).write_bytes(png)
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
    for row in ("glitch_rgb_split", "glitch_tear_shift", "glitch_burst_rate", "glitch_seed"):
        ed.check(await ui.find("##pp_" + row, INSPECTOR, frames=20) is not None, f"its {row} row shows")
    ed.check(await ui.find("Burst now", INSPECTOR, frames=6) is None, "no Burst now while stopped")
    await ed.screenshot("editor", path=str(ed.scratch.root / "glitch-rows.png"), max_width=1600)
    keep("glitch-rows.png", (ed.scratch.root / "glitch-rows.png").read_bytes())
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
        keep("glitch-burst-now.png", (ed.scratch.root / "glitch-burst-now.png").read_bytes())
        await ui.click(ui.centre(button["rect"]))
        await ui.frames()
        ed.check(await glitch_enabled(ed, sid), "the burst leaves the saved settings as they were")
    finally:
        await ed.call("play", action="stop", keep_changes="none")


async def test_game_view_transition_button(ed: Editor):
    sid = (await ed.call("create_scene", name="TransitionButton", type="3d"))["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    await ed.call("set_project_settings", values={"graphics.screen_transitions.color": "#ff00ff",
                                                  "graphics.screen_transitions.duration": 20.0})
    ui = ui_input.Ui(ed)
    await ed.call("focus_window", window="game")
    await ui.frames(10)
    ui.focus()
    await ui.click_item("##GameTransition", GAME)
    await ui.frames()
    await ui.click_item("Fade")

    def ink_pixels(png: bytes) -> tuple[int, int]:
        width, height, rgba = pngdiff.decode(png)
        ink = sum(1 for i in range(0, width * height * 4, 4) if rgba[i] > 240 and rgba[i + 1] < 15 and rgba[i + 2] > 240)
        return ink, width * height

    async def shot() -> bytes:
        await ui.frames(3)
        taken = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
        ed.check(not taken.is_error and taken.images, f"a Game view screenshot: {taken.text[:200]}")
        return taken.images[0][0]

    # the editor's clock: polled until the cover holds (the duration is long: 45 % covering), then
    # until the view is revealed again
    covered = None
    for _ in range(60):
        png = await shot()
        ink, total = ink_pixels(png)
        if ink > total * 0.9:
            covered = png
            break
        await asyncio.sleep(0.5)
    ed.check(covered is not None, f"the edit-mode Game view shows the transition covering it (last: {ink} of {total} ink pixels)")
    (ed.scratch.root / "transition-preview.png").write_bytes(covered)
    keep("transition-preview.png", covered)
    revealed = False
    for _ in range(120):
        ink, total = ink_pixels(await shot())
        if ink == 0:
            revealed = True
            break
        await asyncio.sleep(0.5)
    ed.check(revealed, f"revealed again: {ink} ink pixels")
    await ed.call("set_project_settings", values={"graphics.screen_transitions.color": "#0b0a10",
                                                  "graphics.screen_transitions.duration": 0.6})
