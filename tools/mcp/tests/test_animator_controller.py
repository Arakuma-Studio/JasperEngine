"""Animator Controllers over MCP (upstream anim layers spec 11.5, 14 MCP tests, on S21's tools and Animator:
docs/specs/S21-animation.md "Upstream absorption"): edit_animator_controller builds the GranjeroAnim port (Idle /
Running on Speed > / < 0.01 with 0.15 s fixed fades, a Watering dead end, a Holding layer with an arm mask), its
validation warnings (the dead end, an unused parameter), all-or-nothing errors naming the op, refused keys and
out-of-range numbers, undo / redo in the project history, a parameter rename and removal following their readers,
get_animation {controller}; play mode through the Animator (after setFloat('Speed', 2) the base layer is in
Running) and through a disabled Animation Player that hands over to it (upstream's controller-on-player scripts);
from_player (one state per clip, an Animator added and the player handing over, one scene step); write_file of
.controller text (upstream's version 1 read converted, Unity YAML refused, warnings returned); a long transition's
live progress; and the tool's budget. Runs on a scratch copy of tools/fixtures/pose-animation (Hero: Mixamo
names)."""

from __future__ import annotations

import json

import harness
from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation"}
CI_EXCLUDED = ("long-running (about 3 minutes on a loaded server: controllers played on the pose-animation rig); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = 900

CONTROLLER = "assets/anim/granjero.controller"
S: dict = {}


async def refused(ed: Editor, tool: str, args: dict, contains: str) -> str:
    r = await ed.call_raw(tool, args)
    ed.check(r.is_error, f"{tool} {json.dumps(args)[:300]} should fail, got: {r.text[:500]}")
    ed.check(contains.lower() in r.text.lower(), f"{tool} error should mention {contains!r}: {r.text[:600]}")
    return r.text


def warned(result: dict, path: str, text: str) -> bool:
    return any(w["path"] == path and text in w["message"] for w in result["warnings"])


def machine(controller: dict, layer: int = 0) -> dict:
    return controller["layers"][layer]["state_machine"]


def transitions(controller: dict, layer: int = 0) -> list:
    """(from, to, conditions) in list order, Any State's first"""
    m = machine(controller, layer)
    out = [("Any State", t["to"], t["conditions"]) for t in m.get("any_state", [])]
    for s in m["states"]:
        out += [(s["name"], t["to"], t["conditions"]) for t in s["transitions"]]
    return out


async def live(ed: Editor) -> dict:
    got = await ed.call("get_animator", entity="Hero")
    ed.check("live" in got, f"get_animator has the live state: {json.dumps(got)[:800]}")
    return got["live"]


async def test_tool_definition(ed: Editor):
    tool = ed.tool("edit_animator_controller")
    size = len(tool.name) + len(tool.description or "") + len(json.dumps(tool.input_schema, separators=(",", ":")))
    ed.check(size < 1600, f"edit_animator_controller's definition is {size} chars (the op fields live in the guide)")
    ed.check("jasper://docs/animator" in tool.description, "it points to the guide")
    got = await ed.session.read_resource("jasper://docs/animator")
    guide = "".join(getattr(c, "text", "") or "" for c in got.contents)
    # (the guide is served as an index of its sections)
    ed.check("Masks and montages" in guide and "Controller ops" in guide, f"the guide has the ops and the masks: {guide[:600]}")


async def test_build_the_granjero_port(ed: Editor):
    nodes = json.loads((ed.project / "assets" / "models" / "hero_tpose.gltf").read_text())["nodes"]
    parent = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}

    def path(i):
        return path(parent[i]) + "/" + nodes[i]["name"] if i in parent else nodes[i]["name"]
    S["arm"] = next(path(i) for i, n in enumerate(nodes) if n.get("name") == "mixamorig:RightForeArm")
    for name, euler, wrap in (("idle", 0, "loop"), ("running", 50, "loop"), ("watering", -40, "once"), ("holding", 80, "loop")):
        r = await ed.call_raw("edit_animation", {"clip": f"assets/anim/{name}.anim", "create": {}, "wrap": wrap, "tracks": [
            {"path": S["arm"], "channel": "rotation", "keys": [{"time": 0, "value": {"euler": [0, 0, euler]}}, {"time": 1, "value": {"euler": [0, 0, euler]}}]}]})
        ed.check(not r.is_error, r.text[:400])
    r = await ed.call_raw("edit_avatar_mask", {"path": "assets/anim/arm.mask", "create": True, "default": 0,
                                               "body_parts": {"right_arm": 1, "right_hand": 1}})
    ed.check(not r.is_error, r.text[:300])

    ops = [
        {"op": "add_parameter", "name": "Speed", "type": "float", "default": 0},
        {"op": "add_parameter", "name": "Watering", "type": "bool"},
        {"op": "add_parameter", "name": "Unused", "type": "int", "default": 3},
        {"op": "add_state", "name": "Idle", "clip": "assets/anim/idle.anim"},
        {"op": "add_state", "name": "Running", "clip": "assets/anim/running.anim"},
        {"op": "add_state", "name": "Watering", "clip": "assets/anim/watering.anim"},
        {"op": "add_transition", "from": "Idle", "to": "Running", "has_exit_time": False, "duration": 0.15, "fixed_duration": True,
         "conditions": [{"parameter": "Speed", "mode": "greater", "threshold": 0.01}]},
        {"op": "add_transition", "from": "Running", "to": "Idle", "has_exit_time": False, "duration": 0.15,
         "conditions": [{"parameter": "Speed", "mode": "less", "threshold": 0.01}]},
        {"op": "add_transition", "from": "Idle", "to": "Watering", "has_exit_time": False, "conditions": [{"parameter": "Watering", "mode": "if"}]},
        {"op": "add_layer", "name": "Holding", "weight": 0, "mask": "assets/anim/arm.mask"},
        {"op": "add_state", "layer": "Holding", "name": "Hold", "clip": "assets/anim/holding.anim"},
    ]
    r = await ed.call_raw("edit_animator_controller", {"path": CONTROLLER, "create": True, "ops": ops})
    ed.check(not r.is_error, r.text[:800])
    out = r.structured
    ed.check(out["changes"][0] == "created", f"created: {out['changes']}")
    c = out["controller"]
    ed.check_eq([p["name"] for p in c["parameters"]], ["Speed", "Watering", "Unused"], "parameters")
    base = machine(c)
    ed.check_eq(([s["name"] for s in base["states"]], base["default_state"]), (["Idle", "Running", "Watering"], "Idle"), "base states")
    ed.check(all("position" in s for s in base["states"]), "positions laid out on a grid")
    ed.check_eq([(f, t) for f, t, _ in transitions(c)], [("Idle", "Running"), ("Idle", "Watering"), ("Running", "Idle")],
                "transitions by source state")
    holding = c["layers"][1]
    ed.check_eq((holding["name"], holding["weight"], holding["mask"]), ("Holding", 0, "anim/arm.mask"), "the Holding layer")
    ed.check(warned(out, "layers[0].state_machine.states[2]", "dead end"), f"Watering is a dead end: {out['warnings']}")
    ed.check(warned(out, "parameters[2]", "unused"), f"Unused is unused: {out['warnings']}")
    ed.check("one project undo step" in r.text, r.text[:300])
    S["text"] = (ed.project / CONTROLLER).read_text()
    ed.check('"version": 2' in S["text"], "written as version 2")


async def test_all_or_nothing_and_undo(ed: Editor):
    before = (ed.project / CONTROLLER).read_text()
    text = await refused(ed, "edit_animator_controller", {"path": CONTROLLER, "ops": [
        {"op": "add_state", "name": "Jump"},
        {"op": "add_transition", "from": "Idle", "to": "Runing"}]}, "ops[1]")
    ed.check("nothing was written" in text, f"all or nothing: {text[:400]}")
    ed.check_eq((ed.project / CONTROLLER).read_text(), before, "nothing written")
    await refused(ed, "edit_animator_controller", {"path": CONTROLLER, "ops": [{"op": "add_parameter", "name": "Speed", "type": "float"}]}, "exists")
    await refused(ed, "edit_animator_controller", {"path": "assets/anim/missing.controller", "ops": []}, "create")

    # rename a parameter: every condition follows; then undo / redo in the project history
    r = await ed.call("edit_animator_controller", path=CONTROLLER, ops=[{"op": "rename_parameter", "name": "Speed", "new_name": "Velocity"}])
    conditions = [c["parameter"] for _, _, cs in transitions(r["controller"]) for c in cs]
    ed.check("Speed" not in conditions and conditions.count("Velocity") == 2, f"the rename followed: {conditions}")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / CONTROLLER).read_text(), before, "undo restores the file")
    await ed.call("redo", scope="project")
    ed.check("Velocity" in (ed.project / CONTROLLER).read_text(), "redo")
    await ed.call("undo", scope="project")

    # remove_parameter: the conditions reading it go and the blend trees reading it are cleared
    MIX = "assets/anim/mix.controller"
    r = await ed.call("edit_animator_controller", path=MIX, create=True, ops=[
        {"op": "add_parameter", "name": "Blend", "type": "float"},
        {"op": "add_parameter", "name": "Go", "type": "bool"},
        {"op": "add_state", "name": "Mix", "blend_tree": {"parameter": "Blend", "children": [
            {"clip": "assets/anim/idle.anim", "threshold": 0}, {"clip": "assets/anim/running.anim", "threshold": 1}]}},
        {"op": "add_state", "name": "Rest"},
        {"op": "add_transition", "from": "Mix", "to": "Rest", "conditions": [{"parameter": "Blend", "mode": "greater", "threshold": 0.5},
                                                                        {"parameter": "Go", "mode": "if"}]}])
    ed.check_eq(machine(r["controller"])["states"][0]["motion"]["blend_tree"]["parameter"], "Blend", "the tree reads Blend")
    r = await ed.call("edit_animator_controller", path=MIX, ops=[{"op": "remove_parameter", "name": "Blend"}])
    ed.check_eq(machine(r["controller"])["states"][0]["motion"]["blend_tree"]["parameter"], "", "remove_parameter cleared the tree's parameter")
    ed.check_eq([c["parameter"] for c in machine(r["controller"])["states"][0]["transitions"][0]["conditions"]], ["Go"],
                "its condition went, the other stays")

    # misspelled fields are refused, and numbers out of range (all or nothing)
    before = (ed.project / MIX).read_text()
    for ops, why in (([{"op": "add_state", "name": "X", "clipp": "assets/anim/idle.anim"}], 'ops[0]: unknown field "clipp"'),
                     ([{"op": "add_state", "name": "X"}, {"op": "add_transition", "from": "Mix", "to": "X", "durration": 0.2}],
                      'ops[1]: unknown field "durration"'),
                     ([{"op": "add_layer", "name": "L", "weight": 1.5}], 'ops[0]: "weight" must be from 0 to 1'),
                     ([{"op": "add_state", "name": "X"}, {"op": "add_transition", "from": "Mix", "to": "X", "duration": -1}],
                      'ops[1]: "duration" must be 0 or more'),
                     ([{"op": "add_state", "name": "X"}, {"op": "add_transition", "from": "Mix", "to": "X", "offset": 2}],
                      'ops[1]: "offset" must be from 0 to 1'),
                     ([{"op": "add_state", "name": "X", "cycle_offset": -0.5}], 'ops[0]: "cycle_offset" must be from 0 to 1'),
                     ([{"op": "add_parameter", "name": "N", "type": "int", "default": 1.5}], "must be a whole number"),
                     ([{"op": "remove_layer", "layer": 0}], "keeps one layer")):
        await refused(ed, "edit_animator_controller", {"path": MIX, "ops": ops}, why)
    ed.check_eq((ed.project / MIX).read_text(), before, "nothing written by the refusals")

    # a parameter name that is not a Lua identifier: kept (scripts pass names as strings), warned
    r = await ed.call("edit_animator_controller", path=MIX, ops=[{"op": "add_parameter", "name": "Move Speed", "type": "float"}])
    ed.check(warned(r, "parameters[1].name", "Lua"), f"a non-Lua name is warned about: {r['warnings']}")

    # set_layer, set_transition, remove_state with the transitions into it
    r = await ed.call("edit_animator_controller", path=MIX, ops=[
        {"op": "add_layer", "name": "A"}, {"op": "add_layer", "name": "B", "weight": 0.5},
        {"op": "add_state", "name": "Idle", "clip": "assets/anim/idle.anim"}, {"op": "add_state", "name": "Run", "clip": "assets/anim/running.anim"},
        {"op": "add_transition", "from": "Idle", "to": "Run"}, {"op": "add_transition", "from": "Run", "to": "Idle"},
        {"op": "add_transition", "from": "Mix", "to": "Idle"},
        {"op": "set_transition", "from": "Idle", "index": 0, "has_exit_time": False, "duration": 0.4, "fixed_duration": True, "offset": 0.25,
         "conditions": [{"parameter": "Go", "mode": "if"}]}])
    ed.check_eq([l["name"] for l in r["controller"]["layers"]], ["Base Layer", "A", "B"], "layers")
    idle = next(s for s in machine(r["controller"])["states"] if s["name"] == "Idle")
    t = idle["transitions"][0]
    ed.check_eq((t["to"], "exit_time" in t, round(t["duration"], 5), t.get("fixed_duration", True), round(t["offset"], 5), t["conditions"]),
                ("Run", False, 0.4, True, 0.25, [{"parameter": "Go", "mode": "if"}]), f"set_transition: {t}")
    r = await ed.call("edit_animator_controller", path=MIX, ops=[{"op": "remove_state", "name": "Run"}])
    names = [s["name"] for s in machine(r["controller"])["states"]]
    ed.check("Run" not in names, f"Run removed: {names}")
    ed.check(all(to != "Run" for _, to, _ in transitions(r["controller"])), "the transitions into it went with it")

    got = await ed.call("get_animation", controller=CONTROLLER)
    ed.check_eq([l["name"] for l in got["layers"]], ["Base Layer", "Holding"], "get_animation {controller}")
    ed.check(warned(got, "layers[0].state_machine.states[2]", "dead end"), "with its warnings")


async def test_play_drives_the_controller(ed: Editor):
    await ed.call("add_component", entity="Hero", component="animator", values={"animator": {"controller": CONTROLLER}})
    got = await ed.call("get_animator", entity="Hero")
    ed.check_eq(got["controller"], CONTROLLER, f"the Animator's controller: {got.get('controller')}")
    await ed.call("play", action="start")
    try:
        await ed.call("wait", game_seconds=0.2)
        state = await live(ed)
        ed.check_eq(state["layers"][0]["state"], "Base Layer.Idle", f"the default state: {state}")
        await ed.call("eval_lua", code="object('Hero'):getAnimator():setFloat('Speed', 2)")
        await ed.call("wait", game_seconds=0.3)
        state = await live(ed)
        ed.check_eq(state["layers"][0]["state"], "Base Layer.Running", f"Speed 2 moved it to Running: {state}")
        ed.check(abs(state["parameters"]["Speed"] - 2) < 1e-6, f"parameters: {state['parameters']}")
        ed.check_eq(state["layers"][1]["state"], "Holding.Hold", "the Holding layer's default state")
        name = (await ed.call("eval_lua", code="return object('Hero'):getAnimator():getCurrentStateInfo(0).name"))["values"][0]
        ed.check_eq(name, "Running", "Lua getCurrentStateInfo")
    finally:
        await ed.call("play", action="stop")

    # upstream's scripts: a disabled Animation Player beside the Animator hands over to it
    await ed.call("add_component", entity="Hero", component="animationplayer", values={"animationplayer": {"enabled": False}})
    player = (await ed.call("get_animation", entity="Hero"))["players"][0]
    ed.check(player.get("animator", {}).get("hands_over") is True, f"get_animation says the player hands over: {player}")
    await ed.call("play", action="start")
    try:
        await ed.call("wait", game_seconds=0.2)
        await ed.call("eval_lua", code="object('Hero'):getAnimationPlayer():setFloat('Speed', 2)")
        await ed.call("wait", game_seconds=0.3)
        ed.check_eq((await live(ed))["layers"][0]["state"], "Base Layer.Running", "the player's setFloat reached the Animator")
        values = (await ed.call("eval_lua", code="local p = object('Hero'):getAnimationPlayer() "
                                                 "return p:getCurrentStateName(0), p:isPlaying('Running')"))["values"]
        ed.check_eq(values, ["Running", True], "the player's state reads are the Animator's")
    finally:
        await ed.call("play", action="stop")
    await ed.call("undo", scope="scene")


async def test_from_player_and_write_file(ed: Editor):
    box = (await ed.call("create_entity", type="cube", name="Box"))
    box_id = box["entity"]["id"] if "entity" in box else box["id"]
    await ed.call("add_component", entity=box_id, component="animationplayer", values={"animationplayer": {
        "clips": [{"clip": "assets/anim/idle.anim", "name": "Idle"}, {"clip": "assets/anim/running.anim", "name": "Run"}], "defaultClip": "Run"}})
    # a new controller file stays in the assets folder
    await refused(ed, "edit_animator_controller", {"path": "../box.controller", "create": True, "from_player": box_id}, "inside the project")
    ed.check(not (ed.project / "box.controller").exists(), "nothing written outside the assets folder")
    # a failing op: no file, no Animator, nothing on the undo stack (the next undo is the
    # add_component above)
    await refused(ed, "edit_animator_controller", {"path": "assets/anim/box.controller", "create": True, "from_player": box_id,
                                                   "ops": [{"op": "add_transition", "from": "Idle", "to": "Nope"}]}, "ops[0]")
    ed.check(not (ed.project / "assets/anim/box.controller").exists(), "no file")
    got = await ed.call("get_entity", entity=box_id)
    ed.check('"animator"' not in json.dumps(got).lower(), f"no Animator: {json.dumps(got)[:300]}")
    await ed.call("undo", scope="scene")
    got = await ed.call("get_entity", entity=box_id)
    ed.check("animationplayer" not in json.dumps(got).lower(), f"the undo took back the add_component: {json.dumps(got)[:300]}")
    await ed.call("redo", scope="scene")
    r = await ed.call_raw("edit_animator_controller", {"path": "assets/anim/box.controller", "create": True, "from_player": box_id,
                                                       "ops": [{"op": "add_parameter", "name": "Go", "type": "trigger"}]})
    ed.check(not r.is_error, r.text[:600])
    out = r.structured
    ed.check_eq(([s["name"] for s in machine(out["controller"])["states"]], machine(out["controller"])["default_state"]), (["Idle", "Run"], "Run"),
                "one state per clip, the default clip's the default")
    ed.check("scene undo step" in r.text, r.text[:300])
    got = await ed.call("get_animator", entity=box_id)
    ed.check_eq(got["controller"], "assets/anim/box.controller", "an Animator plays it")
    player = harness.find_key(await ed.call("get_entity", entity=box_id, components=["animationplayer"]), "enabled")
    ed.check(player is False, "the player hands over (disabled)")
    ed.check(out["users"], "it has its user")
    await ed.call("undo", scope="scene")
    ed.check(not (ed.project / "assets/anim/box.controller").exists(), "one scene step: the file goes with the undo")
    await ed.call("redo", scope="scene")

    await refused(ed, "write_file", {"path": "assets/anim/unity.controller", "content": "%YAML 1.1\n--- !u!91 &9100000\nAnimatorController:\n"},
                  "Unity .controller")
    # upstream's version 1 text: read converted, validated
    text = json.dumps({"format": "jasperengine.controller", "version": 1, "parameters": [{"name": "X", "type": "float"}],
                       "layers": [{"name": "Base Layer", "states": [{"name": "A"}]}]})
    r = await ed.call_raw("write_file", {"path": "assets/anim/hand.controller", "content": text})
    ed.check(not r.is_error and "unused" in r.text and "read converted" in r.text, f"written with its notes and warnings: {r.text[:500]}")
    got = await ed.call("get_animation", controller="assets/anim/hand.controller")
    ed.check_eq((got["read_as"], [s["name"] for s in machine(got)["states"]]), ("upstream_v1", ["A"]), f"read converted: {got}")


async def test_live_transition_progress(ed: Editor):
    # a 5 s transition: get_animator shows it under way with its progress
    await ed.call("edit_animator_controller", path="assets/anim/long.controller", create=True, ops=[
        {"op": "add_parameter", "name": "Go", "type": "bool"},
        {"op": "add_state", "name": "A", "clip": "assets/anim/idle.anim"}, {"op": "add_state", "name": "B", "clip": "assets/anim/running.anim"},
        {"op": "add_transition", "from": "A", "to": "B", "has_exit_time": False, "duration": 5, "fixed_duration": True,
         "conditions": [{"parameter": "Go", "mode": "if"}]}])
    await ed.call("set_properties", entity="Hero", values={"animator": {"controller": "assets/anim/long.controller"}})
    await ed.call("play", action="start")
    try:
        await ed.call("eval_lua", code="object('Hero'):getAnimator():setBool('Go', true)")
        await ed.call("wait", game_seconds=0.5)
        layer = (await live(ed))["layers"][0]
        ed.check_eq(layer["state"], "Base Layer.A", f"the source state while it blends: {layer}")
        ed.check(layer.get("next") == "Base Layer.B" and 0 < layer.get("transition_progress", -1) < 0.5, f"the transition under way: {layer}")
    finally:
        await ed.call("play", action="stop")
