"""Clip property tracks in an exported Linux player (anim-properties acceptance criterion 2): the
GameStatsShowing port of the game's GameOverPanel (stepped gameobject.active tracks with three
events, the panel's CanvasGroup alpha and anchored position, a Text colour), built with
build_project desktop and run with run_build; a probe script prints the values at fixed times
and the event handlers log what they saw.

The Desktop build compiles the engine from source (~20-60 min): set JASPER_SLOW_BUILD=1 to run it.
"""

from __future__ import annotations

import asyncio
import os
import re
import time

from harness import Editor, skip

EDITOR = {"create": True, "name": "Anim Properties Export"}
CI_EXCLUDED = ("long-running (about 35 minutes on a loaded server: its test builds and runs a real Desktop player); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

CLIP = "assets/anim/GameStatsShowing.anim"
STATS = ["puntaje", "record", "coins", "DupCoins", "Play", "Exit"]
TIMES = {"puntaje": 0.5, "record": 1.3333334, "coins": 2.1666667, "DupCoins": 3.0, "Play": 3.0, "Exit": 3.0}
BASE = "SafeArea (3)/GameOVER/"

PANEL = """local GameOverPanel = {}
local function active(name)
    local go = GameObject.find('GameOverMenu/SafeArea (3)/GameOVER/' .. name)
    return go ~= nil and go:isActive()
end
function GameOverPanel:init()
    goLog = {}
    self.t = 0
    self.marks = {0.3, 1.0, 3.6}
    self.next = 1
    RegisterEngineEvent(self, "onPostUpdate")
end
function GameOverPanel:CountThisGameScore() goLog[#goLog + 1] = 'score:' .. tostring(active('puntaje')) end
function GameOverPanel:CheckHighscore() goLog[#goLog + 1] = 'record:' .. tostring(active('record')) end
function GameOverPanel:CountMoney() goLog[#goLog + 1] = 'coins:' .. tostring(active('coins')) end
function GameOverPanel:onPostUpdate()
    self.t = self.t + Engine.deltatime
    local mark = self.marks[self.next]
    if mark == nil or self.t < mark then return end
    self.next = self.next + 1
    local group = CanvasGroup(self.scene, self.entity)
    group.entityOwned = false
    local rect = RectTransform(self.scene, self.entity)
    rect.entityOwned = false
    local on = {}
    for _, name in ipairs({'puntaje', 'record', 'coins', 'DupCoins', 'Play', 'Exit'}) do on[#on + 1] = tostring(active(name)) end
    print(string.format("goprop %.1f alpha=%.3f offset=%.2f screen=%.2f active=%s log=%s", mark, group.alpha,
        rect.positionOffset.y, self.gameObject.position.y, table.concat(on, ','), table.concat(goLog, '|')))
end
return GameOverPanel
"""

LINE = re.compile(r"goprop (?P<mark>[\d.]+) alpha=(?P<alpha>[-\d.]+) offset=(?P<offset>[-\d.]+) "
                  r"screen=(?P<screen>[-\d.]+) active=(?P<active>\S+) log=(?P<log>\S*)")


async def test_exported_player_plays_the_property_tracks(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    made = await ed.call("create_scene", name="GameOver", type="UI")
    sid = made["scene"]["id"]
    children = [{"type": "image", "name": n, "color": [1, 1, 1, 1],
                 "layout": {"anchor": "top_left", "position": [20, 40 * i], "size": [100, 30]}} for i, n in enumerate(STATS)]
    children.append({"type": "text", "name": "Money", "text": "+10",
                     "layout": {"anchor": "top_left", "position": [200, 0], "size": [100, 30]}})
    await ed.call("create_ui", scene=sid, root={
        "type": "container", "name": "GameOverMenu", "canvas_group": {"alpha": 1},
        "layout": {"anchor": "top_left", "position": [0, 0], "size": [600, 400]},
        "children": [{"type": "container", "name": "SafeArea (3)", "layout": {"anchor": "stretch"},
                      "children": [{"type": "container", "name": "GameOVER", "layout": {"anchor": "stretch"},
                                    "children": children}]}]})
    await ed.call("set_entity_state", scene=sid, entities=[f"GameOverMenu/{BASE}{n}" for n in STATS], active=False)
    (ed.project / "scripts").mkdir(parents=True, exist_ok=True)
    (ed.project / "scripts" / "GameOverPanel.lua").write_text(PANEL)
    await ed.call("attach_script", scene=sid, entity="GameOverMenu", path="scripts/GameOverPanel.lua")

    tracks = [{"path": BASE + n, "channel": "property", "component": "gameobject", "property": "active",
               "keys": [{"time": 0, "value": False}, {"time": t, "value": True}]} for n, t in TIMES.items()]
    tracks.append({"path": "", "channel": "property", "component": "canvasgroup", "property": "alpha",
                   "keys": [{"time": 0, "value": 0}, {"time": 0.333, "value": 1}]})
    # Unity's y-up m_AnchoredPosition.y 150 -> -70 -> 0, negated for the y-down layout space
    tracks.append({"path": "", "channel": "property", "component": "recttransform", "property": "positionOffset.y",
                   "keys": [{"time": 0, "value": -150}, {"time": 0.333, "value": 70}, {"time": 0.583, "value": 0}]})
    tracks.append({"path": BASE + "Money", "channel": "property", "component": "ui", "property": "color",
                   "keys": [{"time": 0, "value": "#FFFFFF"}, {"time": 0.5, "value": [0, 1, 0, 1], "ease": "step"}]})
    events = [{"time": 0.5, "function": "CountThisGameScore"}, {"time": 1.3333334, "function": "CheckHighscore"},
              {"time": 2.1666667, "function": "CountMoney"}]
    got = await ed.call("edit_animation", scene=sid, clip=CLIP, create={}, root="GameOverMenu", tracks=tracks, events=events, length=3)
    ed.check(len(got["tracks"]) == 9, f"the nine tracks: {[t.get('property') for t in got['tracks']]}")
    await ed.call("add_component", scene=sid, entity="GameOverMenu", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": CLIP}], "playAutomatically": True}})
    await ed.call("save_scene", scene=sid, path="scenes/GameOver")
    await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=sid)
    jobs = int(os.environ.get("JASPER_BUILD_JOBS", "5"))
    await ed.call("set_project_settings", values={
        "graphics.runtime_shaders": False,       # smaller, faster player build
        "preferences.build_jobs": jobs,
        "player.window_width": 640,
        "player.window_height": 480,
    })

    out = ed.scratch.root / "export-desktop"
    t0 = time.monotonic()
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=jobs, timeout_seconds=30)
    job = r
    while job["status"] == "running":
        await asyncio.sleep(15)
        job = await ed.call("get_job", job_id=r["job_id"], log_lines=10)
        if time.monotonic() - t0 > TIMEOUT:
            ed.fail(f"build still running: {job}")
    ed.check_eq(job["status"], "succeeded", f"{job}"[:2000])
    raw = await ed.call_raw("run_build", {"seconds": 6, "screenshot": False})
    ed.check(not raw.is_error and not raw.value.get("crashed"), raw.text[:1500])
    log = raw.value["log"]
    lines = log if isinstance(log, list) else str(log).splitlines()
    marks = {}
    for line in lines:
        m = LINE.search(line)
        if m:
            marks[m["mark"]] = m
    print("[test] exported player:", {k: v.group(0) for k, v in marks.items()})
    ed.check(set(marks) == {"0.3", "1.0", "3.6"}, f"the probe printed its three marks: {lines[-40:]}")

    early, rest, done = marks["0.3"], marks["1.0"], marks["3.6"]
    # 1.0 s: faded in, at rest, puntaje on (0.5 s) and record still off (1.333 s)
    ed.check(abs(float(rest["alpha"]) - 1.0) < 1e-3, f"the CanvasGroup alpha faded in: {rest.group(0)}")
    ed.check(abs(float(rest["offset"])) < 1e-3, f"the anchored position is at rest: {rest.group(0)}")
    ed.check(rest["active"] == "true,false,false,false,false,false", f"puntaje on, the others off: {rest.group(0)}")
    # 0.3 s: on its way to the overshoot below its rest (y-down: a larger screen y)
    ed.check(float(early["offset"]) > 20 and float(early["screen"]) - float(rest["screen"]) > 20,
             f"below its rest on screen at 0.3 s, as Unity's -70 overshoot: {early.group(0)} vs {rest.group(0)}")
    ed.check(0.5 < float(early["alpha"]) < 1.0, f"fading in at 0.3 s: {early.group(0)}")
    # 3.6 s: every stat on; the handlers saw the values of their frame
    ed.check(done["active"] == ",".join(["true"] * 6), f"every stat on: {done.group(0)}")
    ed.check(done["log"] == "score:true|record:true|coins:true", f"events after the values, in order: {done.group(0)}")
