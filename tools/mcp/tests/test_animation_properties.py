"""Clip property tracks over MCP (anim-properties spec 15, MCP tests): the GameStatsShowing port of
the game's GameOverPanel built with edit_animation (six stepped gameobject.active tracks and three
events, the panel's CanvasGroup alpha fade and its anchored position, a Text colour), played in
play mode and read back with get_runtime_entity and eval_lua at Unity's timings; get_animation
{root, list_properties} and samples in key space (sRGB colours, JSON bools); refusals with a
did-you-mean; the binding statuses missing_component and unknown_property; preview_animation's
properties; the canvasgroup.alpha track the Canvas Group feature left for this one (0.5 at 0.5 s).

A UI scene on an owned empty project: GameOverMenu (Canvas Group, Animation Player) > SafeArea (3)
> GameOVER > puntaje, record, coins, DupCoins, Play, Exit (all inactive) and Money (a Text)."""

from __future__ import annotations

import json

from harness import Editor

EDITOR = {"create": True, "name": "Anim Properties"}
CI = True

TIMEOUT = 900

CLIP = "assets/anim/GameStatsShowing.anim"
STATE: dict = {}
STATS = ["puntaje", "record", "coins", "DupCoins", "Play", "Exit"]
TIMES = {"puntaje": 0.5, "record": 1.3333334, "coins": 2.1666667, "DupCoins": 3.0, "Play": 3.0, "Exit": 3.0}
BASE = "SafeArea (3)/GameOVER/"


async def refused(ed: Editor, tool: str, args: dict, contains: str) -> str:
    r = await ed.call_raw(tool, args)
    ed.check(r.is_error, f"{tool} {json.dumps(args)[:300]} should fail, got: {r.text[:500]}")
    ed.check(contains.lower() in r.text.lower(), f"{tool} error should mention {contains!r}: {r.text[:600]}")
    return r.text


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def scene(ed: Editor) -> str:
    if "scene" in STATE:
        return STATE["scene"]
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
    # the handlers of the clip's events count and read the stats' state when they run
    (ed.project / "scripts").mkdir(parents=True, exist_ok=True)
    (ed.project / "scripts" / "GameOverPanel.lua").write_text(
        "local GameOverPanel = {}\n"
        "local function active(self, name)\n"
        "    local go = GameObject.find('GameOverMenu/SafeArea (3)/GameOVER/' .. name)\n"
        "    return go ~= nil and go:isActive()\n"
        "end\n"
        "function GameOverPanel:init() goLog = {} end\n"
        "function GameOverPanel:CountThisGameScore() goLog[#goLog + 1] = 'score:' .. tostring(active(self, 'puntaje')) end\n"
        "function GameOverPanel:CheckHighscore() goLog[#goLog + 1] = 'record:' .. tostring(active(self, 'record')) end\n"
        "function GameOverPanel:CountMoney() goLog[#goLog + 1] = 'coins:' .. tostring(active(self, 'coins')) end\n"
        "return GameOverPanel\n")
    await ed.call("attach_script", scene=sid, entity="GameOverMenu", path="scripts/GameOverPanel.lua")
    await ed.call("save_scene", scene=sid, path="scenes/GameOver")
    STATE["scene"] = sid
    return sid


async def test_list_properties(ed: Editor):
    sid = await scene(ed)
    props = ed.tool("get_animation").input_schema["properties"]
    ed.check_in("list_properties", props, "get_animation list_properties")
    edit = ed.tool("edit_animation").input_schema["properties"]["tracks"]["items"]["properties"]
    ed.check_in("property", edit["channel"]["enum"], "the property channel")
    ed.check_in("component", edit, "tracks[].component")
    got = await ed.call("get_animation", scene=sid, root="GameOverMenu", list_properties=BASE + "puntaje")
    items = {(p["component"], p["property"]): p for p in got["properties"]}
    color = items.get(("ui", "color"))
    ed.check(color and color["label"] == "Color" and color["kind"] == "color4", f"ui.color listed with label Color: {color!r}")
    ed.check(color["group"] == "Image", f"the group is the Inspector's Image: {color!r}")
    ed.check(approx(color["value"], [1, 1, 1, 1]), f"its value in sRGB: {color!r}")
    active = items.get(("gameobject", "active"))
    ed.check(active and active["kind"] == "bool" and active["value"] is False and active.get("discrete"), f"GameObject active: {active!r}")
    root = await ed.call("get_animation", scene=sid, root="GameOverMenu", list_properties="")
    keys = {(p["component"], p["property"]) for p in root["properties"]}
    ed.check(("canvasgroup", "alpha") in keys and ("recttransform", "positionOffset") in keys, f"the root's group and rect: {sorted(keys)[:20]}")
    ed.check(("script:GameOverPanel", "enabled") in keys, "the Lua script's enabled")
    await refused(ed, "get_animation", {"scene": sid, "root": "GameOverMenu", "list_properties": "SafeArea (3)/GameOVR"}, "did you mean")


def approx(a, b, eps=1e-3) -> bool:
    if isinstance(a, (list, tuple)):
        return isinstance(b, (list, tuple)) and len(a) == len(b) and all(approx(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) <= eps


async def test_build_the_game_stats_port(ed: Editor):
    sid = await scene(ed)
    tracks = [{"path": BASE + n, "channel": "property", "component": "gameobject", "property": "active",
               "keys": [{"time": 0, "value": False}, {"time": t, "value": True}]} for n, t in TIMES.items()]
    # GameOverMenu / go: the CanvasGroup alpha 0 -> 1 and the anchored position y (Unity 150 -> -70 -> 0, y-up, negated for the y-down layout); EarnAnim's text colour
    tracks.append({"path": "", "channel": "property", "component": "Canvas Group", "property": "alpha",
                   "keys": [{"time": 0, "value": 0}, {"time": 0.333, "value": 1}]})
    tracks.append({"path": "", "channel": "property", "component": "recttransform", "property": "positionOffset.y",
                   "keys": [{"time": 0, "value": -150}, {"time": 0.333, "value": 70}, {"time": 0.583, "value": 0}]})
    tracks.append({"path": BASE + "Money", "channel": "property", "component": "Text", "property": "color",
                   "keys": [{"time": 0, "value": "#FFFFFF"}, {"time": 0.5, "value": [0, 1, 0, 1], "ease": "step"}]})
    events = [{"time": 0.5, "function": "CountThisGameScore"}, {"time": 1.3333334, "function": "CheckHighscore"},
              {"time": 2.1666667, "function": "CountMoney"}]
    got = await ed.call("edit_animation", scene=sid, clip=CLIP, create={}, root="GameOverMenu", tracks=tracks, events=events, length=3)
    ed.check(len(got["tracks"]) == 9, f"6 active tracks + alpha + position + colour: {[t.get('property') for t in got['tracks']]}")
    ed.check(any("canvasgroup.alpha" in c for c in got["changes"]), f"Canvas Group resolved to canvasgroup: {got['changes']}")
    ed.check(any("Money : ui.color" in c for c in got["changes"]), f"a Text's colour is ui.color: {got['changes']}")
    bindings = got.get("bindings") or {}
    ed.check(bindings.get("resolved") == bindings.get("total") == 9, f"every track binds: {bindings!r}")
    by = {t["path"] + ":" + t.get("component", "") + "." + t.get("property", ""): t for t in got["tracks"]}
    puntaje = by[BASE + "puntaje:gameobject.active"]
    ed.check(puntaje["kind"] == "bool" and puntaje.get("discrete") and puntaje["keys"][1]["value"] is True,
             f"a stepped bool track: {puntaje!r}")
    # the file: bools as literals
    text = (ed.project / CLIP).read_text()
    ed.check('"component": "gameobject", "property": "active"' in text and "[0.5, true]" in text, f"canonical keys: {text[:600]}")
    await ed.call("add_component", scene=sid, entity="GameOverMenu", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": CLIP}], "playAutomatically": True}})
    await ed.call("save_scene", scene=sid, path="scenes/GameOver")

    # samples in key space: sRGB colours and JSON bools
    got = await ed.call("get_animation", clip=CLIP, times=[0.25, 0.5])
    samples = {t["path"] + ":" + t.get("property", ""): t["values"] for t in got["samples"]["tracks"]}
    ed.check(samples[BASE + "puntaje:active"] == [False, True], f"the step rule at 0.5: {samples[BASE + 'puntaje:active']!r}")
    ed.check(approx(samples[":alpha"][0], 0.25 / 0.333, 1e-3), f"alpha at 0.25: {samples[':alpha']!r}")
    ed.check(approx(samples[BASE + "Money:color"][1], [0, 1, 0, 1]), f"the colour in sRGB: {samples[BASE + 'Money:color']!r}")


async def test_refusals_and_statuses(ed: Editor):
    sid = await scene(ed)
    text = await refused(ed, "edit_animation", {"scene": sid, "clip": CLIP, "tracks": [
        {"path": "", "channel": "property", "component": "light", "property": "fooBar", "keys": [{"time": 0, "value": 1}]}]}, "did you mean")
    ed.check("intensity" in text, f"the component's animatable properties are listed: {text[:500]}")
    await refused(ed, "edit_animation", {"scene": sid, "clip": CLIP, "tracks": [
        {"path": "", "channel": "property", "component": "ui", "keys": [{"time": 0, "value": 1}]}]}, "list_properties")
    # a property the GameObject lacks binds as missing_component; a file's unknown one is unknown_property
    got = await ed.call("edit_animation", scene=sid, clip="assets/anim/probe.anim", create={}, root="GameOverMenu", tracks=[
        {"path": BASE + "puntaje", "channel": "property", "component": "light", "property": "intensity", "keys": [{"time": 0, "value": 1}]}])
    missing = (got.get("bindings") or {}).get("missing") or []
    ed.check(any(m["status"] == "missing_component" and m.get("property") == "light.intensity" for m in missing),
             f"missing_component: {missing!r}")
    probe = ed.project / "assets/anim/probe.anim"
    data = json.loads(probe.read_text())
    data["tracks"].append({"path": "", "channel": "property", "component": "light", "property": "fooBar", "keys": [[0, 1]]})
    probe.write_text(json.dumps(data))
    got = await ed.call("get_animation", scene=sid, clip="assets/anim/probe.anim", root="GameOverMenu")
    missing = (got.get("bindings") or {}).get("missing") or []
    ed.check(any(m["status"] == "unknown_property" for m in missing), f"unknown_property: {missing!r}")
    ed.check(any("did you mean" in w for w in got["warnings"]), f"the load warning suggests: {got['warnings']!r}")


async def test_canvas_group_alpha_track(ed: Editor):
    sid = await scene(ed)
    await ed.call("edit_animation", scene=sid, clip="assets/anim/fade.anim", create={}, tracks=[
        {"path": "", "channel": "property", "component": "canvasgroup", "property": "alpha",
         "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 1}]}])
    got = await ed.call("get_animation", clip="assets/anim/fade.anim", times=[0.5])
    value = got["samples"]["tracks"][0]["values"][0]
    ed.check(approx(value, 0.5, 1e-5), f"canvasgroup.alpha reads 0.5 at t = 0.5: {value!r}")


async def test_preview_shows_properties_and_restores(ed: Editor):
    sid = await scene(ed)
    got = await ed.call("preview_animation", scene=sid, animation="GameOverMenu", action="seek", time=0.5)
    props = {p["path"] + ":" + p["component"] + "." + p["property"]: p["value"] for p in got.get("properties", [])}
    ed.check(props.get(BASE + "puntaje:gameobject.active") is True, f"the preview shows puntaje on at 0.5: {props!r}")
    ed.check(props.get(BASE + "record:gameobject.active") is False, f"record still off: {props!r}")
    ed.check(got.get("properties_total") == len(got.get("properties", [])) and got["properties_total"] >= 9,
             f"properties_total counts every previewed property: {got.get('properties_total')!r}")
    await ed.call("preview_animation", scene=sid, animation="GameOverMenu", action="stop")
    found = await ed.call("find_entities", scene=sid, name="puntaje", active=False)
    ed.check(found["count"] == 1, f"stop restored the inactive stat: {json.dumps(found)[:400]}")


async def test_play_matches_unity_timings(ed: Editor):
    sid = await scene(ed)
    started = await ed.call("play", action="start", scene=sid)
    ed.check(started, "play started")
    try:
        await ed.call("wait", game_seconds=0.6)
        puntaje = await ed.call("get_runtime_entity", entity=f"GameOverMenu/{BASE}puntaje")
        record = await ed.call("get_runtime_entity", entity=f"GameOverMenu/{BASE}record")
        ed.check(puntaje["active"] is True, f"puntaje active at 0.6 s: {puntaje.get('active')}")
        ed.check(record["active"] is False, f"record inactive at 0.6 s: {record.get('active')}")
        await ed.call("wait", game_seconds=2.6)
        states = [(await ed.call("get_runtime_entity", entity=f"GameOverMenu/{BASE}{n}"))["active"] for n in STATS]
        ed.check(all(states), f"every stat on after 3 s: {states}")
        log = (await lua(ed, "return table.concat(goLog, '|')"))[0]
        ed.check(log == "score:true|record:true|coins:true", f"events after the values, in order: {log!r}")
        alpha = (await lua(ed, "return CanvasGroup(scene, entity('GameOverMenu')).alpha"))[0]
        ed.check(approx(alpha, 1.0, 1e-4), f"the group faded in: {alpha!r}")
        props = (await lua(ed, "local p = AnimationPlayer(scene, entity('GameOverMenu')); p.entityOwned = false; return #p:getAnimatedProperties('GameStatsShowing')"))[0]
        ed.check(props == 9, f"getAnimatedProperties lists the 9 bound tracks: {props!r}")
    finally:
        await ed.call("play", action="stop")



async def test_preview_lists_16_properties_and_counts_them_all(ed: Editor):
    sid = await scene(ed)
    await ed.call("create_ui", scene=sid, root={
        "type": "container", "name": "Many", "layout": {"anchor": "top_left", "position": [0, 0], "size": [300, 300]},
        "children": [{"type": "image", "name": f"I{i}", "color": [1, 1, 1, 1],
                      "layout": {"anchor": "top_left", "position": [0, 30 * i], "size": [20, 20]}} for i in range(6)]})
    tracks = []
    for i in range(6):
        for component, prop, value in (("gameobject", "active", True), ("ui", "enabled", True), ("ui", "color", [1, 0, 0, 1])):
            tracks.append({"path": f"I{i}", "channel": "property", "component": component, "property": prop,
                           "keys": [{"time": 0, "value": value}]})
    await ed.call("edit_animation", scene=sid, clip="assets/anim/many.anim", create={}, root="Many", tracks=tracks, length=1)
    await ed.call("add_component", scene=sid, entity="Many", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": "assets/anim/many.anim"}], "playAutomatically": False}})
    got = await ed.call("preview_animation", scene=sid, animation="Many", action="seek", time=0.5)
    try:
        ed.check(len(got.get("properties", [])) == 16, f"the first 16 listed: {len(got.get('properties', []))}")
        ed.check(got.get("properties_total") == 18, f"properties_total counts all 18: {got.get('properties_total')!r}")
    finally:
        await ed.call("preview_animation", scene=sid, animation="Many", action="stop")
