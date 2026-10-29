"""The Animator over MCP and Lua (docs/specs/S21-animation.md R29-R31, AC14): an Animator Controller
built with edit_animator_controller (idle, a locomotion blend tree on Speed, jump on a trigger, an
any-state hit), all or nothing, one project undo step, its canonical JSON file; the Animator
component assigned with the generic tools; the edit-mode preview through control_animator
(evaluated, then the scene restored) and sample_animation_pose; an Avatar Mask and an Animation
Montage (A -> B sections, an event); get_animator's settings, rig (humanoid), clips (retargeted
onto the T-pose hero from the animated hero's file) and diagnostics; play mode: parameters and
triggers drive the transitions, a montage plays its sections and calls a Lua script method,
Lua's onStateEnter / onMontageEnded fire; one call through the CLI. Runs on a scratch copy of
tools/fixtures/pose-animation (Hero: Mixamo names, Robot: Blender names)."""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import Editor, REPO, free_port, wait_for_port

TIMEOUT = 1500
PORT = free_port()
TOKEN = "s21-animator-token"
EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation", "extra_args": ["--http-port", str(PORT)],
          "settings": {"mcp": {"http_token": TOKEN}}}

CONTROLLER = "assets/anim/hero.controller"
MASK = "assets/anim/upper.mask"
MONTAGE = "assets/anim/wave.montage"
WALK = "assets/models/hero_animated.gltf#Walk"
IDLE = "assets/models/hero_animated.gltf#Idle"
WAVE = "assets/models/hero_animated.gltf#Wave"
TOOLS = ("get_animator", "edit_animator_controller", "control_animator", "edit_avatar_mask", "edit_montage", "sample_animation_pose")
S: dict = {}


def file_json(ed: Editor, rel: str) -> dict:
    return json.loads((ed.project / rel).read_text())


def approx(a, b, eps=1e-4) -> bool:
    if isinstance(a, dict):
        return isinstance(b, dict) and a.keys() == b.keys() and all(approx(a[k], b[k], eps) for k in a)
    if isinstance(a, (list, tuple)):
        return len(a) == len(b) and all(approx(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) <= eps


async def refused(ed: Editor, tool: str, args: dict, contains: str) -> str:
    r = await ed.call_raw(tool, args)
    ed.check(r.is_error, f"{tool} {json.dumps(args)[:300]} should fail, got: {r.text[:500]}")
    ed.check(contains.lower() in r.text.lower(), f"{tool} error should mention {contains!r}: {r.text[:600]}")
    return r.text


def states_of(controller: dict, layer: int = 0) -> dict:
    return {s["name"]: s for s in controller["layers"][layer]["state_machine"]["states"]}


async def live(ed: Editor) -> dict:
    got = await ed.call("get_animator", entity="Hero")
    ed.check("live" in got, f"get_animator has the live state: {json.dumps(got)[:800]}")
    return got["live"]


async def local_rotation(ed: Editor, entity) -> list:
    got = await ed.call("get_entity", entity=entity, components=["transform"])
    return harness.find_key(got, "rotation")


# ---------------------------------------------------------------------------- the tools

async def test_tools_listed(ed: Editor):
    ed.require_tools(*TOOLS)
    for name in TOOLS:
        tool = ed.tool(name)
        ed.check(tool.input_schema.get("type") == "object", f"{name} has an object schema")
    words = ed.tool("edit_animator_controller").description + json.dumps(ed.tool("edit_animator_controller").input_schema)
    for op in ("_parameter", "_state", "_transition", "set_blend_tree", "_sub_state_machine", "set_default_state", "jasper://docs/animator"):
        ed.check(op in words, f"edit_animator_controller names {op}")


async def test_build_controller(ed: Editor):
    ops = [
        {"op": "add_parameter", "name": "Speed", "type": "float"},
        {"op": "add_parameter", "name": "Jump", "type": "trigger"},
        {"op": "add_parameter", "name": "Hit", "type": "trigger"},
        {"op": "add_state", "name": "Idle", "clip": IDLE, "default": True, "position": [300, 100]},
        {"op": "add_state", "name": "Locomotion", "position": [300, 220], "blend_tree": {
            "type": "simple_1d", "parameter": "Speed",
            "children": [{"clip": IDLE, "threshold": 0}, {"clip": WALK, "threshold": 1}]}},
        {"op": "add_state", "name": "Jump", "clip": WAVE, "loop": "once", "position": [520, 220]},
        {"op": "add_state", "name": "Hit", "clip": WAVE, "loop": "once", "tag": "damage", "position": [520, 100]},
        {"op": "add_transition", "from": "Idle", "to": "Locomotion",
         "conditions": [{"parameter": "Speed", "mode": "greater", "threshold": 0.1}], "has_exit_time": False, "duration": 0.2},
        {"op": "add_transition", "from": "Locomotion", "to": "Idle",
         "conditions": [{"parameter": "Speed", "mode": "less", "threshold": 0.1}], "has_exit_time": False, "duration": 0.2},
        {"op": "add_transition", "from": "Locomotion", "to": "Jump", "conditions": [{"parameter": "Jump", "mode": "if"}],
         "has_exit_time": False, "duration": 0.1},
        {"op": "add_transition", "from": "Jump", "to": "Locomotion", "has_exit_time": True, "exit_time": 0.9, "duration": 0.1},
        {"op": "add_transition", "from": "Any State", "to": "Hit", "conditions": [{"parameter": "Hit", "mode": "if"}],
         "duration": 0.05},
        {"op": "add_transition", "from": "Hit", "to": "Idle", "has_exit_time": True, "exit_time": 1.0, "duration": 0.1},
    ]
    r = await ed.call_raw("edit_animator_controller", {"path": CONTROLLER, "create": True, "ops": ops})
    ed.check(not r.is_error, r.text[:1500])
    out = r.structured
    ed.check_eq(out["diagnostics"], [], "a clean controller")
    states = states_of(out["controller"])
    ed.check_eq(sorted(states), ["Hit", "Idle", "Jump", "Locomotion"], "the four states")
    ed.check_eq(out["controller"]["layers"][0]["state_machine"]["default_state"], "Idle", "Idle is the default")
    ed.check(states["Locomotion"]["motion"].get("blend_tree", {}).get("type") == "simple_1d", f"a 1D tree: {states['Locomotion']}")
    ed.check_eq(states["Hit"].get("tag"), "damage", "the tag")
    # the file: canonical JSON with its format, version and GUID
    text = (ed.project / CONTROLLER).read_text()
    data = json.loads(text)
    ed.check_eq((data["format"], data["version"]), ("jasperengine.controller", 1), "format and version")
    ed.check(re.fullmatch(r"[0-9a-f]{32}", data.get("guid", "")), f"a GUID: {data.get('guid')}")
    S["guid"] = data["guid"]
    S["text"] = text
    # the parameters are typed as written
    ed.check_eq([(p["name"], p["type"]) for p in data["parameters"]], [("Speed", "float"), ("Jump", "trigger"), ("Hit", "trigger")],
                "parameters")


async def test_all_or_nothing_and_undo(ed: Editor):
    # a failing op writes nothing, the earlier ops of the call included
    await refused(ed, "edit_animator_controller", {"path": CONTROLLER, "ops": [
        {"op": "add_state", "name": "Extra"}, {"op": "add_transition", "from": "Nowhere", "to": "Extra"}]}, "ops[1]")
    # an op's fields are checked (the listing keeps them free-form)
    await refused(ed, "edit_animator_controller", {"path": CONTROLLER, "ops": [{"op": "add_state", "name": "X", "speeed": 2}]}, "speeed")
    await refused(ed, "edit_animator_controller", {"path": CONTROLLER, "ops": [{"op": "add_state", "name": "X", "speed": "fast"}]}, "number")
    await refused(ed, "edit_animator_controller", {"path": CONTROLLER, "ops": [{"op": "add_stat", "name": "X"}]}, "add_state")
    ed.check_eq((ed.project / CONTROLLER).read_text(), S["text"], "nothing was written")
    await refused(ed, "edit_animator_controller", {"path": CONTROLLER, "ops": [{"op": "add_parameter", "name": "Speed", "type": "int"}]},
                  "Speed")
    await refused(ed, "edit_animator_controller", {"path": "assets/anim/hero.anim", "create": True}, ".controller")
    # one edit, one project undo step
    await ed.call("edit_animator_controller", path=CONTROLLER, ops=[{"op": "add_state", "name": "Extra"}])
    ed.check("Extra" in states_of((await ed.call("get_animator", controller=CONTROLLER))), "the state is added")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / CONTROLLER).read_text(), S["text"], "undo restores the file exactly")
    await ed.call("redo", scope="project")
    ed.check("Extra" in states_of((await ed.call("get_animator", controller=CONTROLLER))), "redo adds it back")
    await ed.call("edit_animator_controller", path=CONTROLLER, ops=[{"op": "remove_state", "name": "Extra"}])
    # diagnostics name the unknown parameter of a condition
    out = await ed.call("edit_animator_controller", path=CONTROLLER, ops=[
        {"op": "add_transition", "from": "Idle", "to": "Jump", "conditions": [{"parameter": "Speed", "mode": "greater", "threshold": 5}]},
        {"op": "rename_parameter", "name": "Speed", "new_name": "Velocity"}])
    states = states_of(out["controller"])
    conditions = [c["parameter"] for t in states["Idle"]["transitions"] for c in t.get("conditions", [])]
    ed.check("Speed" not in conditions and "Velocity" in conditions, f"a rename follows into the conditions: {conditions}")
    tree = states["Locomotion"]["motion"]["blend_tree"]
    ed.check_eq(tree["parameter"], "Velocity", "and into the blend tree")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / CONTROLLER).read_text(), S["text"], "undone")


async def test_assign_and_inspect(ed: Editor):
    await ed.call("add_component", entity="Hero", component="animator", values={"animator": {"controller": CONTROLLER}})
    got = await ed.call("get_animator", entity="Hero")
    ed.check_eq(got["controller"], CONTROLLER, "the controller is assigned")
    ed.check(got.get("rig", {}).get("humanoid") is True, f"the hero is a humanoid: {json.dumps(got.get('rig'))[:500]}")
    clips = got.get("rig", {}).get("clips", [])
    ed.check(clips and all(c.get("loaded") for c in clips), f"every clip loads: {json.dumps(clips)[:800]}")
    ed.check(not got.get("bind_error") and not got.get("controller_error"), f"no error: {json.dumps(got)[:800]}")
    # the component in the generic tools: settings round trip
    await ed.call("set_properties", entity="Hero", values={"animator": {"speed": 1.5}})
    comp = await ed.call("get_entity", entity="Hero", components=["animator"])
    ed.check(approx(harness.find_key(comp, "speed"), 1.5), f"speed set: {json.dumps(comp)[:600]}")
    await ed.call("set_properties", entity="Hero", values={"animator": {"speed": 1}})


async def test_edit_preview_restores(ed: Editor):
    arm = "Hero/mixamorig:Hips/mixamorig:Spine/mixamorig:Spine1/mixamorig:Spine2/mixamorig:LeftShoulder/mixamorig:LeftArm"
    rest = await local_rotation(ed, arm)
    r = await ed.call_raw("control_animator", {"entity": "Hero", "preview": "start", "parameters": {"Speed": 1}, "advance": 1.0})
    ed.check(not r.is_error, r.text[:1200])
    layers = r.structured["live"]["layers"]
    ed.check(layers and layers[0]["state"].endswith("Locomotion"), f"Speed 1 leads to Locomotion: {layers}")
    moved = await local_rotation(ed, arm)
    ed.check(not approx(moved, rest, 1e-3), f"the preview poses the arm: {rest} -> {moved}")
    pose = await ed.call("sample_animation_pose", entity="Hero", bones=["left_hand", "right_foot", "nope"])
    ed.check("left_hand" in pose["bones"] and "right_foot" in pose["bones"], f"slot names resolve: {json.dumps(pose)[:600]}")
    ed.check_eq(pose.get("unknown_bones"), ["nope"], "an unknown bone is listed")
    await ed.call("control_animator", entity="Hero", preview="stop")
    ed.check(approx(await local_rotation(ed, arm), rest, 1e-5), "stop restores the scene")
    # sample in edit mode: a state at a time, the scene untouched
    pose = await ed.call("sample_animation_pose", entity="Hero", state="Jump", time=0.25, bones=["left_hand"])
    ed.check(pose["state"].endswith("Jump"), f"the state asked for: {pose['state']}")
    ed.check(approx(await local_rotation(ed, arm), rest, 1e-5), "sampling leaves the scene as it was")


async def test_mask_and_montage(ed: Editor):
    out = await ed.call("edit_avatar_mask", path=MASK, create=True, body_parts={"left_leg": False, "right_leg": False, "root": False},
                        paths={"mixamorig:Hips/mixamorig:Spine": 0.5})
    data = file_json(ed, MASK)
    ed.check_eq((data["format"], data["version"]), ("jasperengine.mask", 1), "the mask's format")
    ed.check(data["body_parts"]["left_leg"] is False and data["body_parts"]["head"] is True, f"body parts: {data['body_parts']}")
    await ed.call("edit_avatar_mask", path=MASK, paths={"mixamorig:Hips/mixamorig:Spine": None})
    ed.check_eq(file_json(ed, MASK).get("paths", []), [], "null removes a path")
    # an upper-body layer with the mask and the montage slot
    await ed.call("edit_animator_controller", path=CONTROLLER, ops=[
        {"op": "add_layer", "name": "Upper", "mask": MASK, "slot": "UpperBody", "weight": 1},
        {"op": "add_state", "layer": "Upper", "name": "Empty"}])
    out = await ed.call("edit_montage", path=MONTAGE, create=True, clip=WAVE, slot="UpperBody", rate=1,
                        blend_in={"time": 0.1, "ease": "sine_in_out"}, blend_out={"time": 0.1},
                        sections=[{"name": "A", "start": 0}, {"name": "B", "start": 0.3}],
                        events=[{"time": 0.1, "function": "onWaved", "args": ["hello", 3]}])
    data = file_json(ed, MONTAGE)
    ed.check_eq((data["format"], data["slot"]), ("jasperengine.montage", "UpperBody"), "the montage's format and slot")
    # A plays on into the following section (no "next" written), B, the last, ends it
    sections = {s["name"]: s for s in data["sections"]}
    ed.check(sections["A"].get("next") in (None, "B") and sections["B"].get("next") in (None, ""), f"A then B: {data['sections']}")
    await refused(ed, "edit_montage", {"path": MONTAGE, "sections": [{"name": "A", "start": 0}, {"name": "A", "start": 0.2}]}, "A")


# ---------------------------------------------------------------------------- play mode (Lua)

SCRIPT = """local Hero = {}

function Hero:init()
    self.animator = GameObject(self.scene, self.entity):getAnimator()
    self.animator.onStateEnter = function(layer, state)
        Log.print("enter " .. layer .. " " .. state)
    end
    self.animator.onMontageEnded = function(path, interrupted)
        Log.print("montage ended " .. path .. " " .. tostring(interrupted))
    end
end

function Hero:update()
    if self.animator:isHumanoid() and not self.said then
        self.said = true
        Log.print("humanoid " .. tostring(self.animator:hasBone("left_hand")))
    end
end

function Hero:onWaved(word, count)
    Log.print("waved " .. tostring(word) .. " " .. tostring(count))
end

return Hero
"""


async def test_play_mode(ed: Editor):
    await ed.call("create_script", kind="lua", name="HeroAnimator", content=SCRIPT, entity="Hero")
    await ed.console_since(None)
    await ed.call("play", action="start")
    try:
        await ed.call("wait", game_seconds=0.3)
        state = (await live(ed))["layers"][0]["state"]
        ed.check(state.endswith("Idle"), f"the default state plays first: {state}")
        r = await ed.call("control_animator", entity="Hero", parameters={"Speed": 1})
        await ed.call("wait", game_seconds=0.5)
        state = (await live(ed))["layers"][0]["state"]
        ed.check(state.endswith("Locomotion"), f"Speed > 0.1 moves to Locomotion: {state}")
        await ed.call("control_animator", entity="Hero", triggers=["Hit"])
        await ed.call("wait", game_seconds=0.2)
        layer = (await live(ed))["layers"][0]
        ed.check(layer["state"].endswith("Hit") or layer.get("next", "").endswith("Hit"), f"any state -> Hit on the trigger: {layer}")
        # the trigger is consumed: Hit runs once, then back to Idle -> Locomotion (Speed is still 1)
        await ed.call("wait", game_seconds=2.0)
        state = (await live(ed))["layers"][0]["state"]
        ed.check(not state.endswith("Hit"), f"the trigger fired once: {state}")
        # a montage on the upper body: A then B, its event reaches the script
        r = await ed.call("control_animator", entity="Hero", montage={"path": MONTAGE})
        ed.check(r["live"].get("montage", {}).get("path", "").endswith("wave.montage"), f"the montage plays: {json.dumps(r['live'])[:600]}")
        await ed.call("wait", game_seconds=0.45)
        montage = (await live(ed)).get("montage")
        ed.check(montage is None or montage.get("section") == "B", f"A -> B: {montage}")
        await ed.call("wait", game_seconds=2.0)
        ed.check("montage" not in (await live(ed)), "the montage ended")
        console = " ".join(str(e.get("message")) for e in await ed.console_since("last"))
        ed.check("enter Base Layer Locomotion" in console, f"onStateEnter: {console[-1200:]}")
        ed.check("waved hello 3" in console, f"the montage event called the script method: {console[-1200:]}")
        ed.check("montage ended" in console and "false" in console, f"onMontageEnded(not interrupted): {console[-1200:]}")
        ed.check("humanoid true" in console, f"isHumanoid and hasBone(slot): {console[-1200:]}")
    finally:
        await ed.call("play", action="stop")


async def test_from_player_and_cli(ed: Editor):
    # Create Animator Controller from Animation Player: a state per clip
    await ed.call("add_component", entity="Robot", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": WALK, "name": "Walk"}, {"clip": IDLE, "name": "Idle"}]}})
    out = await ed.call("edit_animator_controller", path="assets/anim/robot.controller", create=True, from_player="Robot")
    states = states_of(out["controller"])
    ed.check_eq(sorted(states), ["Idle", "Walk"], "a state per player clip")
    ed.check_eq(out["controller"]["layers"][0]["state_machine"]["default_state"], "Walk", "the first clip without a default")
    # one call through the CLI (S31): get_animator on the controller, attached by port and token,
    # with a HOME of its own (no other editor's instance file answers) in an empty directory
    await wait_for_port(PORT)
    root = Path(tempfile.mkdtemp(prefix="jasper-animator-cli-"))
    (root / "home").mkdir()
    (root / "cwd").mkdir()
    env = {k: v for k, v in os.environ.items() if k not in ("JASPER_MCP_URL", "JASPER_MCP_TOKEN", "DISPLAY", "XDG_CONFIG_HOME",
                                                          "WAYLAND_DISPLAY")}
    env["HOME"] = str(root / "home")
    args = harness.editor_command() + ["call", "--port", str(PORT), "--token", TOKEN, "--json", "get_animator", "--controller", CONTROLLER]
    p = subprocess.run(args, cwd=str(root / "cwd"), env=env, capture_output=True, text=True, timeout=300)
    ed.check(p.returncode == 0, f"the CLI call exits 0: {p.returncode} {p.stderr[-800:]} {p.stdout[-800:]}")
    envelope = json.loads(p.stdout.strip().splitlines()[-1])
    ed.check("Locomotion" in json.dumps(envelope), f"the CLI returns the controller: {json.dumps(envelope)[:800]}")
    ed.check(not any((root / "cwd").iterdir()), "the CLI writes nothing to its working directory")
