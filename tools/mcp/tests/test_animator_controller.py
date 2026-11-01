"""Animator Controllers over MCP (anim layers spec 11.5, 14 MCP tests): edit_animator builds the GranjeroAnim
port (Idle / Running on Speed > / < 0.01 with 0.15 s fixed fades, a Watering dead end, a Holding layer with
an arm mask), its validation warnings (the dead end, an unused parameter), all-or-nothing errors naming the
op, undo / redo in the project history, get_animation {controller}, set_properties controller and
get_animation {entity} (edit mode and play: after setFloat('Speed', 2) the base layer is in Running),
create.from_entity (one state per clip, set as the player's controller), write_file of .controller text
(Unity YAML refused, warnings returned) and the tool budget of edit_animator. Runs on a scratch copy of
tools/fixtures/pose-animation (Hero: Mixamo names)."""

from __future__ import annotations

import json

import harness
from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation"}
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


async def test_tool_definition(ed: Editor):
    tool = ed.tool("edit_animator")
    size = len(tool.name) + len(tool.description or "") + len(json.dumps(tool.input_schema, separators=(",", ":")))
    ed.check(size < 3500, f"edit_animator's definition is {size} chars (under 3500: the op details live in the guide)")
    ed.check("jasper://docs/animation" in tool.description, "it points to the guide")
    got = await ed.session.read_resource("jasper://docs/animation")
    guide = "".join(getattr(c, "text", "") or "" for c in got.contents)
    # (the guide is served as an index of its sections)
    ed.check("Animator Controllers" in guide and "Layers and masks" in guide, f"the guide has both sections: {guide[:600]}")


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
    r = await ed.call_raw("write_file", {"path": "assets/anim/arm.mask", "content": json.dumps(
        {"format": "jasperengine.mask", "version": 1, "default": 0, "humanoid": {"right_arm": 1}})})
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
    r = await ed.call_raw("edit_animator", {"controller": CONTROLLER, "create": {}, "ops": ops})
    ed.check(not r.is_error, r.text[:800])
    out = r.structured
    ed.check(out["created"] and out["guid"], "created with a GUID")
    ed.check_eq([p["name"] for p in out["parameters"]], ["Speed", "Watering", "Unused"], "parameters")
    base, holding = out["layers"]
    ed.check_eq(([s["name"] for s in base["states"]], base["default_state"]), (["Idle", "Running", "Watering"], "Idle"), "base states")
    ed.check(all("position" in s for s in base["states"]), "positions laid out on a grid")
    ed.check_eq([(t["index"], t["from"], t["to"]) for t in base["transitions"]], [(0, "Idle", "Running"), (1, "Running", "Idle"), (2, "Idle", "Watering")],
                "transitions with indices")
    ed.check_eq((holding["name"], holding["weight"], holding["mask"]), ("Holding", 0, "assets/anim/arm.mask"), "the Holding layer")
    ed.check(warned(out, "layers[0].states[2]", "dead end"), f"Watering is a dead end: {out['warnings']}")
    ed.check(warned(out, "parameters[2]", "unused"), f"Unused is unused: {out['warnings']}")
    ed.check("one project undo step" in r.text, r.text[:300])
    S["text"] = (ed.project / CONTROLLER).read_text()


async def test_all_or_nothing_and_undo(ed: Editor):
    before = (ed.project / CONTROLLER).read_text()
    text = await refused(ed, "edit_animator", {"controller": CONTROLLER, "ops": [
        {"op": "add_state", "name": "Jump"},
        {"op": "add_transition", "from": "Idle", "to": "Runing"}]}, "ops[1].to")
    ed.check("Running" in text and "nothing was changed" in text, f"the error lists the states: {text[:400]}")
    ed.check_eq((ed.project / CONTROLLER).read_text(), before, "nothing written")
    await refused(ed, "edit_animator", {"controller": CONTROLLER, "ops": [{"op": "remove_layer", "layer": 0}]}, "base layer")
    await refused(ed, "edit_animator", {"controller": CONTROLLER, "ops": [{"op": "add_parameter", "name": "Speed", "type": "float"}]}, "already")
    await refused(ed, "edit_animator", {"controller": CONTROLLER, "create": {}}, "exists")
    await refused(ed, "edit_animator", {"controller": "assets/anim/missing.controller", "ops": []}, "create")

    # rename a parameter: every condition follows; then undo / redo in the project history
    r = await ed.call("edit_animator", controller=CONTROLLER, ops=[{"op": "update_parameter", "name": "Speed", "rename": "Velocity"}])
    conditions = [c["parameter"] for t in r["layers"][0]["transitions"] for c in t["conditions"]]
    ed.check("Speed" not in conditions and conditions.count("Velocity") == 2, f"the rename followed: {conditions}")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / CONTROLLER).read_text(), before, "undo restores the file")
    await ed.call("redo", scope="project")
    ed.check("Velocity" in (ed.project / CONTROLLER).read_text(), "redo")
    await ed.call("undo", scope="project")

    # remove_parameter clears the blend trees reading it, as the Animator window does
    r = await ed.call("edit_animator", controller="assets/anim/mix.controller", create={}, ops=[
        {"op": "add_parameter", "name": "Blend", "type": "float"},
        {"op": "add_state", "name": "Mix", "blend_tree": {"parameter": "Blend", "children": [
            {"clip": "assets/anim/idle.anim", "threshold": 0}, {"clip": "assets/anim/running.anim", "threshold": 1}]}}])
    ed.check_eq(r["layers"][0]["states"][0]["blend_tree"]["parameter"], "Blend", "the tree reads Blend")
    r = await ed.call("edit_animator", controller="assets/anim/mix.controller", ops=[{"op": "remove_parameter", "name": "Blend"}])
    ed.check_eq(r["layers"][0]["states"][0]["blend_tree"]["parameter"], "", "remove_parameter cleared the tree's parameter")
    # clip "" leaves the state empty: no clip, no tree
    r = await ed.call("edit_animator", controller="assets/anim/mix.controller", ops=[{"op": "update_state", "state": "Mix", "clip": ""}])
    mix = r["layers"][0]["states"][0]
    ed.check(mix.get("empty") is True and "blend_tree" not in mix and "clip" not in mix, f"an empty state: {mix}")

    # misspelled keys are refused with the allowed ones, and numbers out of range (all or nothing)
    MIX = "assets/anim/mix.controller"
    before = (ed.project / MIX).read_text()
    for ops, why in (([{"op": "add_state", "name": "X", "clipp": "assets/anim/idle.anim"}], "ops[0].clipp: unknown key (allowed: op, layer, name, clip"),
                     ([{"op": "add_state", "name": "X"}, {"op": "add_transition", "from": "Mix", "to": "X", "durration": 0.2}],
                      "ops[1].durration: unknown key"),
                     ([{"op": "add_state", "name": "X"}, {"op": "add_transition", "from": "Mix", "to": "X",
                       "conditions": [{"parameter": "P", "mode": "if", "treshold": 1}]}], "ops[1].conditions[0].treshold: unknown key"),
                     ([{"op": "add_state", "name": "T", "blend_tree": {"parameter": "P", "children": [{"clip": "assets/anim/idle.anim", "treshold": 0}]}}],
                      "ops[0].blend_tree.children[0].treshold: unknown key (allowed: clip, threshold, speed, mirror)"),
                     ([{"op": "add_layer", "name": "L", "weight": 1.5}], "ops[0].weight: must be from 0 to 1"),
                     ([{"op": "add_state", "name": "X"}, {"op": "add_transition", "from": "Mix", "to": "X", "duration": -1}], "ops[1].duration: must be 0 or more"),
                     ([{"op": "add_state", "name": "X"}, {"op": "add_transition", "from": "Mix", "to": "X", "offset": 2}], "ops[1].offset: must be from 0 to 1"),
                     ([{"op": "add_state", "name": "X", "cycle_offset": -0.5}], "ops[0].cycle_offset: must be from 0 to 1"),
                     ([{"op": "add_parameter", "name": "N", "type": "int", "default": 1.5}], "ops[0].default: expected a whole number"),
                     # parameter names are Lua identifiers (the reader would drop the others)
                     ([{"op": "add_parameter", "name": "Move Speed", "type": "float"}], "ops[0].name: \"Move Speed\" is not a Lua identifier"),
                     ([{"op": "add_parameter", "name": "end", "type": "bool"}], "is not a Lua identifier"),
                     ([{"op": "add_parameter", "name": "Ok", "type": "bool"}, {"op": "update_parameter", "name": "Ok", "rename": "2fast"}],
                      "ops[1].rename: \"2fast\" is not a Lua identifier")):
        await refused(ed, "edit_animator", {"controller": MIX, "ops": ops}, why)
    await refused(ed, "edit_animator", {"controller": "assets/anim/new.controller", "create": {"from_entitty": "Hero"}}, "create.from_entitty: unknown key")
    ed.check_eq((ed.project / MIX).read_text(), before, "nothing written by the refusals")

    # move_layer, update_transition, remove_state with its transitions
    r = await ed.call("edit_animator", controller=MIX, ops=[
        {"op": "add_layer", "name": "A"}, {"op": "add_layer", "name": "B", "weight": 0.5}, {"op": "move_layer", "layer": "B", "to": 1},
        {"op": "add_parameter", "name": "Go", "type": "bool"},
        {"op": "add_state", "name": "Idle", "clip": "assets/anim/idle.anim"}, {"op": "add_state", "name": "Run", "clip": "assets/anim/running.anim"},
        {"op": "add_transition", "from": "Idle", "to": "Run"}, {"op": "add_transition", "from": "Run", "to": "Idle"},
        {"op": "add_transition", "from": "Mix", "to": "Idle"},
        {"op": "update_transition", "index": 0, "has_exit_time": False, "duration": 0.4, "fixed_duration": True, "offset": 0.25,
         "conditions": [{"parameter": "Go", "mode": "if"}]}])
    ed.check_eq([l["name"] for l in r["layers"]], ["Base Layer", "B", "A"], "move_layer put B above the base")
    t = r["layers"][0]["transitions"][0]
    ed.check_eq((t["from"], t["to"], t["has_exit_time"], t["duration"], t["fixed_duration"], t["offset"], t["conditions"]),
                ("Idle", "Run", False, 0.4, True, 0.25, [{"parameter": "Go", "mode": "if"}]), f"update_transition: {t}")
    r = await ed.call("edit_animator", controller=MIX, ops=[{"op": "remove_state", "state": "Run"}])
    ed.check_eq([s["name"] for s in r["layers"][0]["states"]], ["Mix", "Idle"], "Run removed")
    ed.check_eq([(t["from"], t["to"]) for t in r["layers"][0]["transitions"]], [("Mix", "Idle")], "its transitions went with it")

    got = await ed.call("get_animation", controller=CONTROLLER)
    ed.check_eq([l["name"] for l in got["layers"]], ["Base Layer", "Holding"], "get_animation {controller}")
    ed.check(warned(got, "layers[0].states[2]", "dead end"), "with its warnings")


async def test_play_drives_the_controller(ed: Editor):
    await ed.call("add_component", entity="Hero", component="animationplayer", values={"animationplayer": {"controller": CONTROLLER}})
    player = (await ed.call("get_animation", entity="Hero"))["players"][0]
    ed.check_eq(player["controller"]["path"], CONTROLLER, f"the player's controller: {player.get('controller')}")
    ed.check(any("dead end" in w["message"] for w in player["controller"]["warnings"]), "its warnings in get_animation {entity}")
    await ed.call("play", action="start")
    try:
        await ed.call("wait", game_seconds=0.2)
        live = (await ed.call("get_animation", entity="Hero"))["players"][0]["controller"]
        ed.check_eq(live["layers"][0]["current"], "Idle", f"the default state: {live}")
        await ed.call("eval_lua", code="object('Hero'):getAnimator():setFloat('Speed', 2)")
        await ed.call("wait", game_seconds=0.3)
        live = (await ed.call("get_animation", entity="Hero"))["players"][0]["controller"]
        ed.check_eq(live["layers"][0]["current"], "Running", f"Speed 2 moved it to Running: {live}")
        ed.check(abs(live["parameters"]["Speed"] - 2) < 1e-6, f"parameters: {live['parameters']}")
        ed.check_eq(live["layers"][1]["current"], "Hold", "the Holding layer's default state")
        state = (await ed.call("eval_lua", code="return object('Hero'):getAnimator():getCurrentStateName(0)"))["values"][0]
        ed.check_eq(state, "Running", "Lua getCurrentStateName")
    finally:
        await ed.call("play", action="stop")


async def test_create_from_entity_and_write_file(ed: Editor):
    box = (await ed.call("create_entity", type="cube", name="Box"))
    box_id = box["entity"]["id"] if "entity" in box else box["id"]
    await ed.call("add_component", entity=box_id, component="animationplayer", values={"animationplayer": {
        "clips": [{"clip": "assets/anim/idle.anim", "name": "Idle"}, {"clip": "assets/anim/running.anim", "name": "Run"}], "defaultClip": "Run"}})
    # a new controller file stays in the assets folder
    await refused(ed, "edit_animator", {"controller": "box.controller", "create": {"from_entity": box_id}}, "inside the assets")
    ed.check(not (ed.project / "box.controller").exists(), "nothing written outside the assets folder")
    # a failing op: no file, no change to the player, nothing on the undo stack (the next undo is
    # the add_component above)
    await refused(ed, "edit_animator", {"controller": "assets/anim/box.controller", "create": {"from_entity": box_id},
                                        "ops": [{"op": "add_transition", "from": "Idle", "to": "Nope"}]}, "ops[0].to")
    ed.check(not (ed.project / "assets/anim/box.controller").exists(), "no file")
    ed.check(not harness.find_key(await ed.call("get_entity", entity=box_id, components=["animationplayer"]), "controller"), "no controller set")
    await ed.call("undo", scope="scene")
    got = await ed.call("get_entity", entity=box_id)
    ed.check("animationplayer" not in json.dumps(got).lower(), f"the undo took back the add_component: {json.dumps(got)[:300]}")
    await ed.call("redo", scope="scene")
    r = await ed.call_raw("edit_animator", {"controller": "assets/anim/box.controller", "create": {"from_entity": box_id},
                                             "ops": [{"op": "add_parameter", "name": "Go", "type": "trigger"}]})
    ed.check(not r.is_error, r.text[:600])
    out = r.structured
    ed.check_eq(([s["name"] for s in out["layers"][0]["states"]], out["layers"][0]["default_state"]), (["Idle", "Run"], "Run"),
                "one state per clip, the default clip's the default")
    ed.check("scene undo step" in r.text, r.text[:300])
    got = await ed.call("get_entity", entity=box_id, components=["animationplayer"])
    ed.check_eq(harness.find_key(got, "controller"), "assets/anim/box.controller", "set as the player's controller")
    ed.check(out["users"], "it has its user")

    await refused(ed, "write_file", {"path": "assets/anim/unity.controller", "content": "%YAML 1.1\n--- !u!91 &9100000\nAnimatorController:\n"},
                  "Unity .controller")
    text = json.dumps({"format": "jasperengine.controller", "version": 1, "parameters": [{"name": "X", "type": "float"}],
                       "layers": [{"name": "Base Layer", "states": [{"name": "A"}]}]})
    r = await ed.call_raw("write_file", {"path": "assets/anim/hand.controller", "content": text})
    ed.check(not r.is_error and "unused" in r.text, f"written with its validation warnings: {r.text[:500]}")


async def test_live_transition_progress(ed: Editor):
    # a 5 s transition: get_animation {entity} shows it under way with its progress
    await ed.call("edit_animator", controller="assets/anim/long.controller", create={}, ops=[
        {"op": "add_parameter", "name": "Go", "type": "bool"},
        {"op": "add_state", "name": "A", "clip": "assets/anim/idle.anim"}, {"op": "add_state", "name": "B", "clip": "assets/anim/running.anim"},
        {"op": "add_transition", "from": "A", "to": "B", "has_exit_time": False, "duration": 5, "fixed_duration": True,
         "conditions": [{"parameter": "Go", "mode": "if"}]}])
    await ed.call("set_properties", entity="Hero", values={"animationplayer": {"controller": "assets/anim/long.controller"}})
    await ed.call("play", action="start")
    try:
        await ed.call("eval_lua", code="object('Hero'):getAnimator():setBool('Go', true)")
        await ed.call("wait", game_seconds=0.5)
        layer = (await ed.call("get_animation", entity="Hero"))["players"][0]["controller"]["layers"][0]
        ed.check_eq(layer["current"], "A", f"the source state while it blends: {layer}")
        transition = layer.get("transition") or {}
        ed.check(transition.get("to") == "B" and 0 < transition.get("progress", -1) < 0.5, f"the transition under way: {layer}")
    finally:
        await ed.call("play", action="stop")
