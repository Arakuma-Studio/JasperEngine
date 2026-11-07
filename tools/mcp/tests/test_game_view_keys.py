"""Keys in the editor's Game view during Play, from REAL X input (xdotool on the harness' Xvfb): runtime-core
spec 6.8. Escape in the focused Game view is the game's key, as in Unity (a pause menu reads it), and the
view keeps the focus: ImGui's keyboard navigation took Escape to leave the view's canvas, so the game
never saw it and the keys after it went nowhere. The other keys a game uses for menus (Tab, the arrows,
Space, Enter) reach it the same way.

A Lua script counts the key downs it reads in onUpdate; Play runs freely (no auto_pause) while the keys
are pressed.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Game Keys", "xvfb": "harness"}
TIMEOUT = 600

COUNTER = """---@class KeyCounter : ScriptInstance
local KeyCounter = { properties = {} }

function KeyCounter:init()
    keys = { escape = 0, tab = 0, left = 0, space = 0, enter = 0 }
    RegisterEngineEvent(self, "onUpdate")
end

function KeyCounter:onUpdate()
    if Input.getKeyDown(Input.KEY_ESCAPE) then keys.escape = keys.escape + 1 end
    if Input.getKeyDown(Input.KEY_TAB) then keys.tab = keys.tab + 1 end
    if Input.getKeyDown(Input.KEY_LEFT) then keys.left = keys.left + 1 end
    if Input.getKeyDown(Input.KEY_SPACE) then keys.space = keys.space + 1 end
    if Input.getKeyDown(Input.KEY_ENTER) then keys.enter = keys.enter + 1 end
end

return KeyCounter
"""


def xdo(ed: Editor, *args: str) -> str:
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    if not shutil.which("xdotool"):
        raise harness.TestFailure("xdotool is not installed: the Game view's real-key test needs it with the harness' Xvfb")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout


async def press(ed: Editor, key: str) -> None:
    """A real key press (down, then up a few frames later), then a few frames."""
    xdo(ed, "keydown", key)
    await ed.call("wait", frames=3)
    xdo(ed, "keyup", key)
    await ed.call("wait", frames=4)


async def keys(ed: Editor) -> dict:
    return (await ed.call("eval_lua", code="return keys"))["values"][0]


async def test_escape_in_the_focused_game_view_reaches_the_game(ed: Editor):
    made = await ed.call("create_scene", name="Keys", type="3D", path="scenes/Keys")
    scene = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=scene, type="empty", name="Counter")
    await ed.call("create_script", kind="lua", name="KeyCounter", content=COUNTER, scene=scene, entity=ent["entity_id"])
    await ed.call("save_scene", scene=scene)
    started = await ed.call("play", action="start", scene=scene)
    ed.check_eq(started["state"], "playing", f"start: {started}")
    try:
        await ed.call("focus_window", window="game")
        ids = xdo(ed, "search", "--onlyvisible", "--name", "JasperEngine").split()
        ed.check(ids, "the editor's X window")
        xdo(ed, "windowfocus", "--sync", ids[-1])
        await ed.call("wait", frames=6)
        await asyncio.sleep(0.2)

        await press(ed, "Escape")
        ed.check_eq((await keys(ed))["escape"], 1, "the game read Escape")
        # the view kept the focus: the keys after Escape reach the game too
        for key in ("Tab", "Left", "space", "Return"):
            await press(ed, key)
        got = await keys(ed)
        ed.check_eq((got["tab"], got["left"], got["space"], got["enter"]), (1, 1, 1, 1),
                    f"the Game view kept the focus after Escape: {got}")
        await press(ed, "Escape")
        ed.check_eq((await keys(ed))["escape"], 2, "a second Escape reaches it as well")
    finally:
        await ed.call("play", action="stop")
