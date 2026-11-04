"""Hidden windows (docs/superpowers/specs/2026-11-04-platform-lifecycle-design.md D4, D7, D8): Play keeps
updating without drawing while the editor window can't be seen, unmapped or minimized by a window
manager that keeps it mapped (_NET_WM_STATE_HIDDEN, mutter), and draws again when it shows; scripts hear
Engine.onVisibilityChanged, get_play_state and editor_status say it, and a screenshot fails at once
instead of waiting for a frame that never comes. send_input {type: "app", action: "hide"} simulates it
with the window visible.

The window is the harness's Xvfb's (no window manager): xdotool unmaps it, xprop sets the property as
mutter would. A probe script counts its onUpdate and onDraw events.
"""

import asyncio
import os
import shutil
import subprocess

from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "Hidden Window", "xvfb": "harness"}

PROBE = """---@class HiddenProbe : ScriptInstance
local HiddenProbe = { properties = {} }
function HiddenProbe:init()
    hiddenProbe = { updates = 0, draws = 0, visibility = {} }
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onDraw")
    RegisterEngineEvent(self, "onVisibilityChanged")
end
function HiddenProbe:onUpdate() hiddenProbe.updates = hiddenProbe.updates + 1 end
function HiddenProbe:onDraw() hiddenProbe.draws = hiddenProbe.draws + 1 end
function HiddenProbe:onVisibilityChanged(visible) table.insert(hiddenProbe.visibility, visible) end
return HiddenProbe
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="HiddenMain", type="2D", path="scenes/HiddenMain")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Probe")
    await ed.call("create_script", kind="lua", name="HiddenProbe", content=PROBE, scene=S["scene"], entity=ent["entity_id"])
    await ed.call("create_entity", scene=S["scene"], type="sprite", name="Sprite")
    await ed.call("save_scene", scene=S["scene"])
    return S


class X:
    """xdotool and xprop on the harness's Xvfb, on the editor's main window."""

    def __init__(self, ed: Editor):
        xvfb = getattr(ed, "_xvfb", None)
        if not shutil.which("xdotool") or not shutil.which("xprop") or not xvfb or not xvfb.display:
            skip("needs xdotool, xprop and the harness's Xvfb (EDITOR xvfb harness)")
        self.env = dict(os.environ, DISPLAY=xvfb.display)
        windows = self.run("xdotool", "search", "--name", "JasperEngine").split()
        if not windows:
            skip("no editor window found")
        self.window = windows[-1]

    def run(self, *args: str) -> str:
        return subprocess.run(list(args), env=self.env, capture_output=True, text=True, timeout=20).stdout

    def unmap(self) -> None:
        self.run("xdotool", "windowunmap", "--sync", self.window)

    def map(self) -> None:
        self.run("xdotool", "windowmap", "--sync", self.window)

    def set_wm_hidden(self, hidden: bool) -> None:
        if hidden:
            self.run("xprop", "-id", self.window, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN")
        else:
            self.run("xprop", "-id", self.window, "-remove", "_NET_WM_STATE")


async def probe(ed: Editor) -> dict:
    values = (await ed.call("eval_lua", code="return hiddenProbe.updates, hiddenProbe.draws, hiddenProbe.visibility"))["values"]
    return {"updates": values[0], "draws": values[1], "visibility": values[2] or []}


async def counts_over(ed: Editor, seconds: float) -> tuple[int, int]:
    before = await probe(ed)
    await asyncio.sleep(seconds)
    after = await probe(ed)
    return after["updates"] - before["updates"], after["draws"] - before["draws"]


async def wait_visible(ed: Editor, visible: bool) -> dict:
    state = {}
    for _ in range(50):
        state = (await ed.call("get_play_state"))["platform"]
        if state["visible"] == visible:
            return state
        await asyncio.sleep(0.1)
    ed.fail(f"platform.visible never became {visible}: {state}")
    return state


async def check_hidden_then_shown(ed: Editor, hide, show) -> None:
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"], auto_pause=False)
    try:
        updates, draws = await counts_over(ed, 0.5)
        ed.check(updates > 0 and draws > 0, f"visible: the game updates and draws ({updates}, {draws})")

        hide()
        state = await wait_visible(ed, False)
        ed.check_eq(state["hidden_update_fps"], 60, "the hidden rate of a game without a frame rate of its own")
        ed.check_eq((await ed.call("editor_status"))["window"]["visible"], False, "editor_status says the window is hidden")
        # settle the last drawn frame, then measure
        await asyncio.sleep(0.2)
        updates, draws = await counts_over(ed, 1.0)
        ed.check(updates >= 30, f"hidden: the game keeps updating ({updates} updates in a second)")
        ed.check_eq(draws, 0, "hidden: nothing is drawn")

        shot = await ed.call_raw("screenshot", {"view": "game"}, timeout=10)
        ed.check(shot.is_error and "minimized or hidden" in shot.text, f"a screenshot fails at once: {shot.text[:300]}")

        show()
        await wait_visible(ed, True)
        await asyncio.sleep(0.2)
        updates, draws = await counts_over(ed, 0.5)
        ed.check(updates > 0 and draws > 0, f"shown: the game draws again ({updates}, {draws})")
        ed.check_eq((await probe(ed))["visibility"], [False, True], "onVisibilityChanged false, then true")
        ed.check_eq((await ed.call("editor_status"))["window"]["visible"], True)
    finally:
        show()
        await ed.call("play", action="stop", auto_pause=False)


async def test_editor_unmapped_keeps_play_updating(ed: Editor):
    x = X(ed)
    await check_hidden_then_shown(ed, x.unmap, x.map)


async def test_editor_net_wm_hidden_is_hidden(ed: Editor):
    x = X(ed)
    await check_hidden_then_shown(ed, lambda: x.set_wm_hidden(True), lambda: x.set_wm_hidden(False))


async def test_send_input_hide_show(ed: Editor):
    s = await setup(ed)
    await ed.call("play", action="start", scene=s["scene"], auto_pause=False)
    try:
        await ed.call("send_input", events=[{"type": "app", "action": "hide"}])
        state = await wait_visible(ed, False)
        ed.check(state["hidden_update_fps"] > 0, f"{state}")
        await asyncio.sleep(0.2)
        updates, draws = await counts_over(ed, 1.0)
        ed.check(updates >= 30, f"hidden by send_input: the game keeps updating ({updates})")
        ed.check_eq(draws, 0, "hidden by send_input: nothing is drawn")
        ed.check_eq((await ed.call("editor_status"))["window"]["visible"], True, "the editor window itself stays visible")

        shot = await ed.call_raw("screenshot", {"view": "game"}, timeout=10)
        ed.check(shot.is_error and "send_input app hide" in shot.text, f"the Game view has no new frame: {shot.text[:300]}")

        await ed.call("send_input", events=[{"type": "app", "action": "show"}])
        await wait_visible(ed, True)
        await asyncio.sleep(0.2)
        updates, draws = await counts_over(ed, 0.5)
        ed.check(draws > 0, f"shown: the game draws again ({updates}, {draws})")
        ed.check_eq((await probe(ed))["visibility"], [False, True])
        ed.check_eq((await ed.call("eval_lua", code="return Engine.isVisible"))["values"], [True])

        # a Play that stops hidden: the next one starts visible
        await ed.call("send_input", events=[{"type": "app", "action": "hide"}])
        await wait_visible(ed, False)
    finally:
        await ed.call("play", action="stop", auto_pause=False)
    await ed.call("play", action="start", scene=s["scene"], auto_pause=False)
    try:
        state = (await ed.call("get_play_state"))["platform"]
        ed.check(state["visible"], f"the next Play starts visible: {state}")
    finally:
        await ed.call("play", action="stop", auto_pause=False)
