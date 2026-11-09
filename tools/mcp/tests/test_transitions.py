"""Screen transitions in editor Play (docs/specs/S48-pixel-fx.md R12-R17, AC12-AC14), driven from
Lua through eval_lua at a fixed 60 fps, on a project this file makes: a Menu scene whose script
adds a HUD layer scene (a white bar), a Level and a Loading scene.

  test_lua_api        Transition, TransitionHandle, TransitionKind and PixelFX.glitch exist; a bad
                      kind raises the error that names the kinds
  test_play_covers    play(): at the hold every canvas pixel is the ink, the HUD layer's too (Cover
                      UI on, R16); the handle ends after the reveal and the scene shows again; the
                      frame stats' pass list has the "transition" pass, last, only while it runs
  test_cover_ui       Project Settings' cover_ui off: at the hold the HUD stays visible and the
                      "transition" pass comes before the layer scene's
  test_load_scene     loadScene covers, calls onCovered once (before the switch), loads Level and
                      reveals it
  test_loading_scene  with a loading scene shown (loadingDelay) the transition reveals onto it, then
                      covers again when the load completes, and reveals the Level
  test_idle_free      idle (before any transition and after one ended) the frames' pass lists are
                      the same and have no transition pass (AC14; the program is made on first use:
                      engine/tests/core/pixelart/ScreenTransitionGpuTest.cpp)

The images per kind, progress and pixel size (AC12) are ScreenTransitionGpuTest's, against the CPU
twin; capture_contact_sheet (WP-E) adds the {transition} route.
"""

from __future__ import annotations

import os
import re

import pngdiff
from harness import Editor

# read at editor start: this file runs in its own process
os.environ["JASPERENGINE_READY_SIGNAL"] = "1"
os.environ["JASPERENGINE_FRAME_STATS"] = "all"

EDITOR = {"create": True, "name": "Transitions"}
CI = True
TIMEOUT = 1200

INK = (255, 0, 255)
LINE = re.compile(r"Frame stats \(frame \d+\): .*pass list: (.*)$")

HUD = """---@class TrHud : ScriptInstance
local TrHud = {}
function TrHud:init()
    local hud = Scene()
    Engine.addSceneLayer(hud)
    self.hud = hud
    local bar = Polygon(hud)
    bar:addVertex(0, 0)
    bar:addVertex(300, 0)
    bar:addVertex(0, 60)
    bar:addVertex(300, 60)
    bar:setColor(1.0, 1.0, 1.0, 1.0)
    bar.position = Vector3(20, 20, 0)
    self.bar = bar
end
return TrHud
"""

state: dict = {}


def log_length(ed: Editor) -> int:
    log = ed.scratch.root / "editor.log"
    return len(log.read_text(errors="replace").splitlines()) if log.exists() else 0


def pass_lists(ed: Editor, start: int) -> list[list[str]]:
    log = ed.scratch.root / "editor.log"
    lines = log.read_text(errors="replace").splitlines()[start:] if log.exists() else []
    out = []
    for line in lines:
        m = LINE.search(line)
        if m:
            out.append([re.sub(r" \d+x\d+.*$", "", item.strip()) for item in m.group(1).split(",") if item.strip()])
    return out


async def canvas(ed: Editor, label: str):
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"{label}.png").write_bytes(png)
    rect = shot.value["canvas_rect_in_image"]
    return pngdiff.crop(pngdiff.decode(png), round(rect["x"]), round(rect["y"]), round(rect["width"]), round(rect["height"]))


def count(image, color, tolerance: int = 2) -> int:
    width, height, rgba = image
    n = 0
    for i in range(0, width * height * 4, 4):
        if abs(rgba[i] - color[0]) <= tolerance and abs(rgba[i + 1] - color[1]) <= tolerance and abs(rgba[i + 2] - color[2]) <= tolerance:
            n += 1
    return n


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code)).get("values")


async def start_menu(ed: Editor):
    await ed.call("play", action="start", scene=state["menu"], fixed_fps=60, auto_pause=True)
    await ed.call("wait", frames=10)


async def stop(ed: Editor):
    await ed.call("play", action="stop", keep_changes="none")


async def test_setup(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "create_script", "save_scene", "play", "eval_lua", "wait", "screenshot",
                     "configure_game_view", "set_project_settings")
    await ed.call("configure_game_view", aspect="project", scale=1, maximize_on_play=True)
    menu = (await ed.call("create_scene", name="TrMenu", type="2d"))["scene"]["id"]
    holder = (await ed.call("create_entity", scene=menu, name="Hud", type="empty"))["entity_id"]
    await ed.call("create_script", kind="lua", name="TrHud", content=HUD, entity=holder, scene=menu)
    await ed.call("save_scene", scene=menu, path="TrMenu.scene")
    for name in ("TrLevel", "TrLoading"):
        sid = (await ed.call("create_scene", name=name, type="2d"))["scene"]["id"]
        await ed.call("save_scene", scene=sid, path=f"{name}.scene")
    state["menu"] = menu


async def test_lua_api(ed: Editor):
    await start_menu(ed)
    try:
        values = await lua(ed, "return type(Transition.play), type(Transition.loadScene), TransitionKind.IRIS, TransitionKind.FADE, "
                               "type(PixelFX.glitch), Transition.active, Transition.phase")
        ed.check_eq(values, ["function", "function", 0, 6, "function", False, "none"])
        bad = await ed.call_raw("eval_lua", {"code": "Transition.play('swirl', 1)"})
        ed.check(bad.is_error or "pixel_dissolve" in bad.text, f"an unknown kind names the kinds: {bad.text[:300]}")
    finally:
        await stop(ed)


async def test_play_covers(ed: Editor):
    await start_menu(ed)
    try:
        before = await canvas(ed, "before")
        white = count(before, (255, 255, 255))
        ed.check(white > 1000, f"the HUD bar shows: {white} white pixels")
        mark = log_length(ed)
        await lua(ed, "trHandle = Transition.play('fade', 1.0, {color = '#ff00ff', hold = 0.5})")
        await ed.call("wait", frames=30)   # cover 0.25 s, then the hold
        ed.check_eq(await lua(ed, "return Transition.phase, Transition.covered"), ["hold", True])
        held = await canvas(ed, "held")
        total = held[0] * held[1]
        ink = count(held, INK)
        ed.check(ink == total, f"covered: {ink} of {total} canvas pixels are the ink (the HUD's too)")
        lists = [p for p in pass_lists(ed, mark) if "transition" in p]
        ed.check(lists, "a transition pass while it runs")
        ed.check(all(p[-1] == "transition" for p in lists), f"over every scene (last): {lists[-1] if lists else None}")
        waited = await ed.call_raw("wait", {"until": "trHandle:isDone()", "timeout_seconds": 30})
        ed.check(not waited.is_error, f"the handle ends: {waited.text[:300]}")
        ed.check_eq(await lua(ed, "return Transition.active, trHandle.progress"), [False, 1])
        await ed.call("wait", frames=3)
        after = await canvas(ed, "after")
        ed.check(count(after, INK) == 0 and count(after, (255, 255, 255)) == white, "revealed: the scene and HUD as before")
    finally:
        await stop(ed)


async def test_cover_ui(ed: Editor):
    await ed.call("set_project_settings", values={"graphics.screen_transitions.cover_ui": False})
    await start_menu(ed)
    try:
        before = await canvas(ed, "cover_ui_before")
        white = count(before, (255, 255, 255))
        mark = log_length(ed)
        await lua(ed, "trHandle = Transition.play('iris', 1.0, {color = '#ff00ff', hold = 0.5})")
        await ed.call("wait", frames=30)
        held = await canvas(ed, "cover_ui_held")
        ed.check(count(held, (255, 255, 255)) == white, "Cover UI off: the HUD stays visible over the cover")
        ed.check(count(held, INK) + white >= held[0] * held[1] * 0.99, "the rest is covered")
        lists = [p for p in pass_lists(ed, mark) if "transition" in p]
        ed.check(lists and all(p[-1] != "transition" for p in lists), f"under the layer scene: {lists[-1] if lists else None}")
    finally:
        await stop(ed)
        await ed.call("set_project_settings", values={"graphics.screen_transitions.cover_ui": True})


async def test_load_scene(ed: Editor):
    await start_menu(ed)
    try:
        await lua(ed, "trCovered = 0; trWhere = nil; trHandle = Transition.loadScene('TrLevel', 'slash', 0.6, {color = '#ff00ff', "
                      "onCovered = function() trCovered = trCovered + 1; trWhere = SceneManager.currentSceneName end})")
        waited = await ed.call_raw("wait", {"until": "trHandle:isDone()", "timeout_seconds": 60})
        ed.check(not waited.is_error, f"the load's transition ends: {waited.text[:300]}")
        ed.check_eq(await lua(ed, "return trCovered, trWhere, SceneManager.currentSceneName, Transition.active"),
                    [1, "TrMenu", "TrLevel", False])
        await ed.call("wait", frames=3)
        after = await canvas(ed, "level")
        ed.check(count(after, INK) == 0, "the Level is revealed")
        missing = await lua(ed, "return Transition.loadScene('NoSuchScene').valid")
        ed.check_eq(missing, [False])
    finally:
        await stop(ed)


async def test_loading_scene(ed: Editor):
    await start_menu(ed)
    try:
        await lua(ed, """
SceneManager.setLoadingScene('TrLoading')
SceneManager.loadingDelay = 0.4
trPhases = {}
trOntoLoading = false
Engine.onUpdate:add('trlog', function()
    local phase = Transition.phase
    if trPhases[#trPhases] ~= phase then trPhases[#trPhases + 1] = phase end
    if phase == 'none' and SceneManager.loading then trOntoLoading = true end
end)
trHandle = Transition.loadScene('TrLevel', 'fade', 0.8, {color = '#ff00ff'})
""")
        waited = await ed.call_raw("wait", {"until": "trHandle:isDone()", "timeout_seconds": 60})
        ed.check(not waited.is_error, f"the load's transition ends: {waited.text[:300]}")
        values = await lua(ed, "return table.concat(trPhases, ','), trOntoLoading, SceneManager.currentSceneName")
        ed.check_eq(values[1:], [True, "TrLevel"], f"revealed onto the loading scene, then the Level: {values}")
        ed.check(values[0].startswith("none,cover,hold,reveal,none,cover,hold,reveal,none") or
                 values[0].startswith("cover,hold,reveal,none,cover,hold,reveal,none"),
                 f"cover, reveal onto the loading scene, cover again, reveal: {values[0]}")
    finally:
        await lua(ed, "Engine.onUpdate:remove('trlog')")
        await stop(ed)


async def test_idle_free(ed: Editor):
    await start_menu(ed)
    try:
        mark = log_length(ed)
        await ed.call("wait", frames=10)
        idle = pass_lists(ed, mark)
        ed.check(idle and not any("transition" in p for p in idle), f"idle: no transition pass: {idle[-1] if idle else None}")
        await lua(ed, "trHandle = Transition.play('blinds', 0.3)")
        await ed.call_raw("wait", {"until": "trHandle:isDone()", "timeout_seconds": 30})
        await ed.call("wait", frames=3)
        mark = log_length(ed)
        await ed.call("wait", frames=10)
        after = pass_lists(ed, mark)
        ed.check(after and not any("transition" in p for p in after), f"after it ended: no transition pass: {after[-1] if after else None}")
        ed.check_eq(after[-1], idle[-1], "the same passes as before any transition")
    finally:
        await stop(ed)
