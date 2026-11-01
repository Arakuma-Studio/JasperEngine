"""Animation Player layers and Avatar Masks over MCP (anim layers spec 10, 14 MCP tests): write_file of a
.mask (valid, broken JSON with line:col, Unity YAML refused), set_properties / add_component of layers and
entry layers with their refusals, get_animation {mask, root} coverage on Hero's humanoid rig, get_animation
{entity} layers and mask_coverage, preview_animation over a base state, and the Garden farmer flow in play
mode: eval_lua setLayerWeight, then get_runtime_entity on both forearms (the right one holds, the left one
keeps the base pose). Runs on a scratch copy of tools/fixtures/pose-animation (Hero: Mixamo names)."""

from __future__ import annotations

import json
import math

import harness
from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 900

MASK = "assets/anim/arm.mask"
BASE = "assets/anim/base.anim"
HOLD = "assets/anim/hold.anim"
S: dict = {}

# base: both forearms at 30 degrees about Z; hold: both at -70 (the mask keeps the right one only)
BASE_EULER = [0, 0, 30]
HOLD_EULER = [0, 0, -70]


def node_paths(gltf) -> list:
    """Every node's path below the model root (anim::modelNodePaths: parentless nodes under the root)."""
    nodes = json.loads(gltf.read_text())["nodes"]
    parent = {}
    for i, node in enumerate(nodes):
        for c in node.get("children", []):
            parent[c] = i

    def path(i):
        name = nodes[i].get("name", f"Node {i}")
        return path(parent[i]) + "/" + name if i in parent else name
    return [path(i) for i in range(len(nodes))]


def quat_of_euler_z(degrees: float) -> list:
    half = math.radians(degrees) / 2
    return [0.0, 0.0, math.sin(half), math.cos(half)]


def quat_close(a, b, eps=1e-3) -> bool:
    dot = abs(sum(x * y for x, y in zip(a, b)))
    return dot > 1 - eps


async def refused(ed: Editor, tool: str, args: dict, contains: str) -> str:
    r = await ed.call_raw(tool, args)
    ed.check(r.is_error, f"{tool} {json.dumps(args)[:300]} should fail, got: {r.text[:500]}")
    ed.check(contains.lower() in r.text.lower(), f"{tool} error should mention {contains!r}: {r.text[:600]}")
    return r.text


def rotation_of(got) -> list:
    rotation = harness.find_key(got, "rotation")
    if isinstance(rotation, dict):
        return rotation.get("quat")
    return rotation


async def local_rotation(ed: Editor, path) -> list:
    # scene paths start at the GameObject (clip paths start below it)
    return rotation_of(await ed.call("get_entity", entity="Hero/" + path, components=["transform"]))


# ---------------------------------------------------------------------------- setup

async def test_mask_file_and_clips(ed: Editor):
    # the forearm paths of Hero's rig (its glTF nodes, the root node first)
    paths = node_paths(ed.project / "assets" / "models" / "hero_tpose.gltf")
    S["right"] = next(p for p in paths if p.endswith("mixamorig:RightForeArm"))
    S["left"] = next(p for p in paths if p.endswith("mixamorig:LeftForeArm"))

    # write_file validates masks: refusals, then a valid one with its warnings
    await refused(ed, "write_file", {"path": "assets/anim/bad.mask", "content": '{"format": "jasperengine.mask",\n "version": 1,,}'}, "2:")
    await refused(ed, "write_file", {"path": "assets/anim/unity.mask", "content": "%YAML 1.1\n--- !u!319 &31900000\nAvatarMask:\n"},
                  "Create a JasperEngine Avatar Mask")
    mask = {"format": "jasperengine.mask", "version": 1, "default": 0,
            "humanoid": {"right_arm": 1, "right_fingers": True, "left_hand_ik": 1}}
    r = await ed.call_raw("write_file", {"path": MASK, "content": json.dumps(mask, indent=2)})
    ed.check(not r.is_error, r.text[:500])
    ed.check("Avatar Mask loads with 1 warning" in r.text and "left_hand_ik" in r.text, f"the IK part is warned about: {r.text[:500]}")

    for path, euler in ((BASE, BASE_EULER), (HOLD, HOLD_EULER)):
        tracks = [{"path": p, "channel": "rotation", "keys": [{"time": 0, "value": {"euler": euler}}, {"time": 1, "value": {"euler": euler}}]}
                  for p in (S["right"], S["left"])]
        r = await ed.call_raw("edit_animation", {"clip": path, "create": {}, "wrap": "loop", "tracks": tracks})
        ed.check(not r.is_error, r.text[:500])


async def test_mask_coverage_on_the_rig(ed: Editor):
    got = await ed.call("get_animation", mask=MASK, root="Hero")
    ed.check_eq(got["default"], 0, "default")
    ed.check_eq(got["humanoid"], {"right_arm": 1, "right_fingers": 1}, "the parts as read (true is 1)")
    ed.check(got.get("humanoid_rig") is True, f"Hero is humanoid: {json.dumps(got)[:400]}")
    coverage = {c["path"]: c for c in got["coverage"]}
    ed.check(got["coverage_total"] >= len(got["coverage"]) > 0, "coverage rows")
    right = next((c for p, c in coverage.items() if p.endswith("mixamorig:RightForeArm")), None)
    left = next((c for p, c in coverage.items() if p.endswith("mixamorig:LeftForeArm")), None)
    ed.check(right and right["weight"] == 1 and right["rule"] == "humanoid:right_arm", f"right forearm: {right}")
    ed.check(left and left["weight"] == 0 and left["rule"] == "humanoid:left_arm", f"left forearm: {left}")
    finger = next((c for p, c in coverage.items() if "RightHandIndex" in p), None)
    ed.check(finger and finger["rule"] == "humanoid:right_fingers" and finger["weight"] == 1, f"a right finger: {finger}")
    await refused(ed, "get_animation", {"mask": "assets/anim/nope.mask"}, "does not exist")

    # coverage lists at most 200 rows; coverage_total counts them all (an entity without a player:
    # every descendant)
    crowd = await ed.call("create_entity", type="empty", name="Crowd")
    crowd_id = crowd["entity"]["id"] if "entity" in crowd else crowd["id"]
    for start in (0, 105):
        await ed.call("batch", calls=[{"tool": "create_entity", "arguments": {"type": "empty", "name": f"M{k}", "parent": crowd_id}}
                                      for k in range(start, start + 105)])
    got = await ed.call("get_animation", mask=MASK, root=crowd_id)
    ed.check_eq(len(got["coverage"]), 200, "capped at 200 rows")
    ed.check(got["coverage_total"] >= 210, f"coverage_total counts every entity: {got['coverage_total']}")
    await ed.call("delete_entities", entities=[crowd_id])


async def test_layers_through_set_properties(ed: Editor):
    await ed.call("add_component", entity="Hero", component="animationplayer", values={"animationplayer": {
        "clips": [{"clip": BASE, "name": "base", "wrap": "loop"}, {"clip": HOLD, "name": "hold", "wrap": "loop"}],
        "playAutomatically": True, "defaultClip": "base"}})
    await ed.call("set_properties", entity="Hero", values={"animationplayer": {
        "layers": [{"name": "Base Layer"}, {"name": "Holding", "weight": 0, "blend": "override", "mask": MASK}],
        "clips[1].layer": 1}})
    got = await ed.call("get_entity", entity="Hero", components=["animationplayer"])
    layers = harness.find_key(got, "layers")
    ed.check_eq([(l["name"], l["mask"]) for l in layers], [("Base Layer", ""), ("Holding", MASK)], f"layers: {layers}")
    ed.check(layers[1].get("maskGuid"), "the mask's GUID follows its path")
    # refusals with their reasons
    for values, why in (({"layers[0].weight": 0.5}, "base layer"), ({"layers[0].blend": "additive"}, "base layer"),
                        ({"layers": [{"name": "A"}, {"name": "A"}]}, "unique"),
                        ({"layers[1].mask": "assets/anim/nope.mask"}, "does not exist"),
                        # one field: the list's checks
                        ({"layers[1].name": ""}, "required"), ({"layers[1].name": "Base Layer"}, "unique"),
                        ({"layers[1].weight": 1.5}, "0 to 1"),
                        ({"clips[1].layer": 7}, "add the layer first")):
        await refused(ed, "set_properties", {"entity": "Hero", "values": {"animationplayer": values}}, why)

    # a player without layers: a field of layer 0 (its implicit Base Layer) makes the list explicit
    plain = await ed.call("create_entity", type="cube", name="Plain")
    plain_id = plain["entity"]["id"] if "entity" in plain else plain["id"]
    await ed.call("add_component", entity=plain_id, component="animationplayer", values={"animationplayer": {
        "clips": [{"clip": BASE, "name": "base"}]}})
    await ed.call("set_properties", entity=plain_id, values={"animationplayer": {"layers[0].mask": MASK, "layers[0].name": "Main"}})
    layers = harness.find_key(await ed.call("get_entity", entity=plain_id, components=["animationplayer"]), "layers")
    ed.check_eq([(l["name"], l["mask"]) for l in layers], [("Main", MASK)], f"the base layer's fields on a default player: {layers}")
    await ed.call("delete_entities", entities=[plain_id])

    player = (await ed.call("get_animation", entity="Hero"))["players"][0]
    ed.check_eq([(l["index"], l["name"], l["states"]) for l in player["layers"]], [(0, "Base Layer", ["base"]), (1, "Holding", ["hold"])],
                f"layers with their states: {player['layers']}")
    ed.check_eq(player["layers"][1]["blend"], "override", "blend")
    cov = player["mask_coverage"]
    ed.check_eq([(c["layer"], c["covered"], c["total"]) for c in cov], [("Holding", 1, 2)], f"the mask covers the right forearm only: {cov}")


async def test_preview_over(ed: Editor):
    await refused(ed, "preview_animation", {"animation": "Hero", "state": "base", "over": "base", "action": "seek", "time": 0.2},
                  "base layer")
    await refused(ed, "preview_animation", {"animation": "Hero", "state": "hold", "over": "nope", "action": "seek", "time": 0.2},
                  "base states")
    r = await ed.call("preview_animation", animation="Hero", state="hold", over="base", action="seek", time=0.2)
    ed.check_eq((r["layer"], r["over"]), ("Holding", "base"), f"the preview's layer: {r}")
    try:
        right = await local_rotation(ed, S["right"])
        left = await local_rotation(ed, S["left"])
        ed.check(quat_close(right, quat_of_euler_z(HOLD_EULER[2])), f"right forearm shows the hold clip: {right}")
        ed.check(quat_close(left, quat_of_euler_z(BASE_EULER[2])), f"left forearm shows the base state: {left}")
    finally:
        await ed.call("preview_animation", animation="Hero", action="stop")


# ---------------------------------------------------------------------------- play: the farmer flow

async def test_play_layer_weight(ed: Editor):
    await ed.call("play", action="start")
    try:
        # the farmer's script: hold plays on its layer (a play there does not stop the base), at weight 0
        await ed.call("eval_lua", code="object('Hero'):getAnimationPlayer():play('hold')")
        await ed.call("wait", game_seconds=0.3)
        right = rotation_of(await ed.call("get_runtime_entity", entity="Hero/" + S["right"]))
        ed.check(quat_close(right, quat_of_euler_z(BASE_EULER[2])), f"at weight 0 the base plays everywhere: {right}")
        playing = (await ed.call("eval_lua", code="local p = object('Hero'):getAnimationPlayer() return p:isPlaying('base'), p:isPlaying('hold')"))["values"]
        ed.check_eq(playing, [True, True], "both layers play")
        await ed.call("eval_lua", code="object('Hero'):getAnimationPlayer():setLayerWeight('Holding', 1)")
        await ed.call("wait", game_seconds=0.2)
        right = rotation_of(await ed.call("get_runtime_entity", entity="Hero/" + S["right"]))
        left = rotation_of(await ed.call("get_runtime_entity", entity="Hero/" + S["left"]))
        ed.check(quat_close(right, quat_of_euler_z(HOLD_EULER[2])), f"the right forearm holds: {right}")
        ed.check(quat_close(left, quat_of_euler_z(BASE_EULER[2])), f"the left forearm keeps the base pose: {left}")
        weight = (await ed.call("eval_lua", code="return object('Hero'):getAnimationPlayer():getLayerWeight('Holding')"))["values"][0]
        ed.check(abs(weight - 1) < 1e-6, f"the runtime weight: {weight}")
        player = (await ed.call("get_animation", entity="Hero"))["players"][0]
        ed.check(abs(player["layers"][1]["live_weight"] - 1) < 1e-6 and player["layers"][1]["queue"] == [], f"live layer: {player['layers'][1]}")
    finally:
        await ed.call("play", action="stop")
