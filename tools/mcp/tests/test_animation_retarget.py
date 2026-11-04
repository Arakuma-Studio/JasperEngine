"""Humanoid retargeting (anim-retarget spec 14, test 14) on the pose-animation fixture: the hero's
walk (hero_animated.gltf, Mixamo names, T-pose) played by the Robot (Blender names, A-pose, other
proportions).

- set_properties clips[0].retarget on the Robot's Animation Player entry of the hero's walk
- get_animation {clip, root: Robot, times}: the entry's retarget makes it report the humanoid
  realisation (`realised`, every slot the robot has, with angles) and `retarget_report` (scale
  ratio between 0.5 and 2, chest folded into spine on the robot without an upper chest)
- play: the robot's thigh.L turns away from its rest and matches `realised` (state time held)
- edit_animation create {from, humanoid}: a version 2 clip with humanoid tracks; humanoid tracks
  by bone, {angles} keys, path refused, a did-you-mean for a wrong bone
- set_humanoid without a path (the Avatar alone: animation_type, twist), get_asset_info avatar
- review fixes: angle-based tolerances, Lua resetFootIK and addClip type checks, keys sent back
  as get_animation prints them ({quat, angles}, tangents), angles by DOF name, an unnormalized
  quaternion, get_asset_info's humanoid tracks by bone, a finger on a body slot's bone refused,
  an Animation Type: Humanoid model's clip realised (with the entry's Mirror) without retarget
- the tool definitions stay within their budget (test_html_ui_tools checks the totals)

Runs on a scratch copy of tools/fixtures/pose-animation.
"""

import json
import math

import harness
from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation"}
CI = True
TIMEOUT = 900

WALK = "assets/models/hero_animated.gltf#Walk"
S: dict = {}


def quat_degrees(a, b) -> float:
    """The angle between two rotations (x, y, z, w), in degrees."""
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(x * x for x in b))
    dot = min(1.0, abs(sum(x * y for x, y in zip(a, b))) / (na * nb))
    return math.degrees(2.0 * math.acos(dot))


def quat_close(a, b, degrees=0.05) -> bool:
    return quat_degrees(a, b) <= degrees


def rotation_of(got) -> list:
    rotation = harness.find_key(got, "rotation")
    if isinstance(rotation, dict):
        return rotation.get("quat")
    return rotation


async def refused(ed: Editor, tool: str, args: dict, contains: str) -> str:
    r = await ed.call_raw(tool, args)
    ed.check(r.is_error, f"{tool} {json.dumps(args)[:300]} should fail, got: {r.text[:500]}")
    ed.check(contains.lower() in r.text.lower(), f"{tool} error should mention {contains!r}: {r.text[:600]}")
    return r.text


async def test_retarget_entry(ed: Editor):
    await ed.call("add_component", entity="Robot", component="animationplayer", values={"animationplayer": {
        "clips": [{"clip": WALK, "wrap": "loop"}], "playAutomatically": True}})
    await ed.call("set_properties", entity="Robot", values={"animationplayer": {"clips[0].retarget": True, "clips[0].footIK": True}})
    got = await ed.call("get_entity", entity="Robot", components=["animationplayer"])
    clips = harness.find_key(got, "clips")
    ed.check(clips and clips[0].get("retarget") is True and clips[0].get("footIK") is True, f"the entry flags: {clips}")

    r = await ed.call("get_animation", clip=WALK, root="Robot", times=[0.5])
    report = r.get("retarget_report")
    ed.check(report is not None and "error" not in report, f"retarget_report: {json.dumps(report)[:600]}")
    ratio = report["scale_ratio"]
    ed.check(0.5 < ratio < 2.0, f"scale ratio {ratio}")
    ed.check(report["target"]["model"].endswith("robot_apose.gltf"), f"target: {report['target']}")
    ed.check(report["source"]["model"].endswith("hero_animated.gltf"), f"source: {report['source']}")
    realised = r.get("realised")
    ed.check(isinstance(realised, list) and len(realised) == 1, f"realised: {json.dumps(realised)[:400]}")
    bones = {b["bone"]: b for b in realised[0]["bones"]}
    # the robot maps every slot but upper_chest (README): 21 slots, each with its joint angles
    slots = [b for b in bones.values() if "angles" in b]
    ed.check(len(slots) >= 21, f"{len(slots)} slots realised: {sorted(bones)}")
    ed.check("left_upper_leg" in bones and bones["left_upper_leg"]["path"].endswith("thigh.L"), f"left_upper_leg: {bones.get('left_upper_leg')}")
    ed.check(realised[0]["hips_local_position"] is not None, "the body position")
    for b in slots:
        ed.check(all(math.isfinite(v) for v in b["angles"].values()), f"{b['bone']} angles {b['angles']}")
    S["thigh"] = bones["left_upper_leg"]["local_rotation"]["quat"]
    rest = rotation_of(await ed.call("get_entity", entity="thigh.L", components=["transform"]))
    S["rest"] = rest
    ed.check(quat_degrees(S["thigh"], rest) > 1.0, f"the realised thigh is not its rest: {S['thigh']} vs {rest}")
    # the binding report names the humanoid tracks as bound on the robot's rig
    ed.check(r["bindings"]["resolved"] > 0, f"bindings: {json.dumps(r['bindings'])[:400]}")


async def test_play_matches_the_realisation(ed: Editor):
    await ed.call("play", action="start")
    try:
        # the state held at 0.5 s (speed 0); Foot IK overridden off, then back to the entry's
        # (on, as get_animation's realisation used it)
        await ed.call("eval_lua", code="local p = object('Robot'):getAnimationPlayer() p.speed = 0 "
                                       "local s = p:getState('Walk') s.time = 0.5 s.footIK = false")
        info = (await ed.call("eval_lua", code="local s = object('Robot'):getAnimationPlayer():getState('Walk') "
                                               "local off = s.footIK s:resetFootIK() return s.isHumanoid, off, s.footIK, s.mirror"))["values"]
        ed.check_eq(info, [True, False, True, False], "the state is humanoid, its Foot IK overridden then reset")
        r = await ed.call_raw("eval_lua", {"code": "object('Robot'):getAnimationPlayer():addClip('anim/x.anim', nil, {mirror = 1})"})
        ed.check(r.is_error and "mirror is true or false" in r.text, f"addClip options are type-checked: {r.text[:300]}")
        r = await ed.call_raw("eval_lua", {"code": "object('Robot'):getAnimationPlayer():addClip('anim/x.anim', nil, {layer = 1.5})"})
        ed.check(r.is_error and "whole number" in r.text, f"a fractional layer is refused: {r.text[:300]}")
        await ed.call("wait", game_seconds=0.2)
        thigh = rotation_of(await ed.call("get_runtime_entity", entity="thigh.L", components=["transform"], include_script_state=False))
        ed.check(quat_degrees(thigh, S["rest"]) > 1.0, f"the thigh moved: {thigh}")
        ed.check(quat_close(thigh, S["thigh"], 0.05), f"play matches realised within 0.05 deg: {quat_degrees(thigh, S['thigh'])}")
        humanoid = (await ed.call("eval_lua", code="local i = object('Robot', 'Model'):getHumanoidInfo() "
                                                   "return i.humanoid, i.animationType, i.humanScale > 0, #i.missing"))["values"]
        ed.check(humanoid[0] is True and humanoid[1] == "generic" and humanoid[2] is True, f"getHumanoidInfo: {humanoid}")
    finally:
        await ed.call("play", action="stop")


async def test_convert_and_humanoid_tracks(ed: Editor):
    r = await ed.call("edit_animation", clip="assets/anim/walk_h.anim", create={"from": WALK, "humanoid": {}})
    ed.check(r.get("humanoid") and r["humanoid"]["mapped_slots"] >= 21, f"the conversion: {json.dumps(r.get('humanoid'))[:400]}")
    text = (ed.project / "assets" / "anim" / "walk_h.anim").read_text()
    ed.check('"version": 2,' in text and '"channel": "humanoid"' in text, f"a version 2 humanoid clip: {text[:300]}")
    tracks = [t for t in r["tracks"] if t["channel"] == "humanoid"]
    ed.check(tracks and all("bone" in t and "path" not in t for t in tracks), f"humanoid tracks by bone: {tracks[:2]}")
    key = tracks[0]["keys"][0]["value"]
    ed.check("quat" in key, f"a humanoid key: {key}")

    # what get_animation prints goes back in as it is: {quat, angles: {dof: degrees}}, tangents
    arm_track = next(t for t in tracks if t["bone"] == "left_upper_arm")
    printed = arm_track["keys"][:3]
    r = await ed.call("edit_animation", clip="assets/anim/walk_h.anim", tracks=[
        {"channel": "humanoid", "bone": "left_upper_arm", "mode": "merge", "keys": printed}])
    again = next(t for t in r["tracks"] if t.get("bone") == "left_upper_arm")
    for before, after in zip(printed, again["keys"][:3]):
        ed.check(quat_close(before["value"]["quat"], after["value"]["quat"], 0.01), f"sent back unchanged: {before} -> {after}")
    # angles by DOF name; a wrong name names the slot's DOF order; quat and angles must agree
    r = await ed.call("edit_animation", clip="assets/anim/walk_h.anim", tracks=[
        {"channel": "humanoid", "bone": "left_lower_arm", "keys": [{"time": 0, "value": {"angles": {"bend": 30}}}]}])
    elbow = next(t for t in r["tracks"] if t.get("bone") == "left_lower_arm")
    ed.check(abs(elbow["keys"][0]["value"]["angles"]["bend"] - 30) < 0.01, f"angles by name: {elbow['keys'][0]}")
    text = await refused(ed, "edit_animation", {"clip": "assets/anim/walk_h.anim", "tracks": [
        {"channel": "humanoid", "bone": "left_upper_arm", "keys": [{"time": 0, "value": {"angles": {"lift": 10}}}]}]}, "raise, forward, twist")
    await refused(ed, "edit_animation", {"clip": "assets/anim/walk_h.anim", "tracks": [
        {"channel": "humanoid", "bone": "left_upper_arm", "keys": [{"time": 0, "value": {"quat": [0, 0, 0, 1], "angles": [60, 0, 0]}}]}]},
                  "different rotations")
    # a quaternion off unit length (rounded output) is normalized, not refused
    r = await ed.call("edit_animation", clip="assets/anim/walk_h.anim", tracks=[
        {"channel": "humanoid", "bone": "jaw", "keys": [{"time": 0, "value": [0, 0, 0.2, 0.99]}]}])
    jaw = next(t for t in r["tracks"] if t.get("bone") == "jaw")
    q = jaw["keys"][0]["value"]["quat"]
    ed.check(abs(math.sqrt(sum(x * x for x in q)) - 1) < 1e-4, f"normalized: {q}")
    # get_asset_info names the humanoid tracks by bone
    info = await ed.call("get_asset_info", path="assets/anim/walk_h.anim")
    humanoid_tracks = [t for t in info["clip"]["tracks"] if t["channel"] == "humanoid"]
    ed.check(humanoid_tracks and all(t.get("bone") and "path" not in t for t in humanoid_tracks), f"asset info tracks: {humanoid_tracks[:2]}")

    # humanoid tracks written by agents
    r = await ed.call("edit_animation", clip="assets/anim/walk_h.anim", tracks=[
        {"channel": "humanoid", "bone": "left_upper_arm", "keys": [{"time": 0, "value": {"angles": [60, 0, 0]}}]}])
    arm = next(t for t in r["tracks"] if t.get("bone") == "left_upper_arm")
    ed.check(abs(arm["keys"][0]["value"]["angles"]["raise"] - 60) < 0.01, f"angles in, angles out: {arm['keys'][0]}")
    await refused(ed, "edit_animation", {"clip": "assets/anim/walk_h.anim", "tracks": [
        {"channel": "humanoid", "bone": "left_upper_arm", "path": "Hero", "keys": [{"time": 0, "value": [0, 0, 0, 1]}]}]}, "no path")
    await refused(ed, "edit_animation", {"clip": "assets/anim/walk_h.anim", "tracks": [
        {"channel": "humanoid", "bone": "left_uper_arm", "keys": [{"time": 0, "value": [0, 0, 0, 1]}]}]}, "did you mean 'left_upper_arm'")
    # a clip without a source model is refused
    await ed.call("edit_animation", clip="assets/anim/plain.anim", create={}, tracks=[
        {"path": "pelvis", "channel": "rotation", "keys": [{"time": 0, "value": [0, 0, 0, 1]}]}])
    await refused(ed, "edit_animation", {"clip": "assets/anim/plain_h.anim", "create": {"from": "assets/anim/plain.anim", "humanoid": {}}},
                  "names no source model")


async def test_avatar(ed: Editor):
    info = await ed.call("get_asset_info", path="assets/models/robot_apose.gltf")
    avatar = info.get("avatar")
    ed.check(avatar and avatar["humanoid"] is True and avatar["animation_type"] == "generic", f"avatar: {json.dumps(avatar)[:400]}")
    ed.check("upper_chest" in avatar["missing"], f"the robot has no upper chest: {avatar['missing']}")
    r = await ed.call("edit_pose_animation", ops=[{"op": "set_humanoid", "model": "assets/models/hero_animated.gltf",
                                                   "animation_type": "humanoid", "twist": {"left_lower_arm": 15}}])
    ed.check(r["sidecars"], f"a sidecar written: {r}")
    text = (ed.project / "assets" / "models" / "hero_animated.gltf.humanoid").read_text()
    ed.check('"animation_type": "humanoid"' in text and '"left_lower_arm": 15' in text, f"the sidecar: {text}")
    info = await ed.call("get_asset_info", path="assets/models/hero_animated.gltf")
    ed.check(info["avatar"]["animation_type"] == "humanoid" and info["avatar"]["twist"].get("left_lower_arm") == 15,
             f"avatar after: {json.dumps(info['avatar'])[:400]}")
    # the model's clips are humanoid now (Animation Type: Humanoid): the robot plays the walk
    # without retarget, get_animation realises it there and gives no retarget advice
    await ed.call("set_properties", entity="Robot", values={"animationplayer": {"clips[0].retarget": False}})
    r = await ed.call("get_animation", clip=WALK, root="Robot", times=[0.5])
    ed.check(isinstance(r.get("realised"), list) and r["realised"][0]["bones"], f"realised on the robot: {json.dumps(r)[:600]}")
    report = r["retarget_report"]
    ed.check(report.get("mirror") is False and report.get("entry") == 0, f"the entry's Mirror and index: {report}")
    ed.check("rig_mismatch" not in (r.get("bindings") or {}), f"no retarget advice: {json.dumps(r.get('bindings'))[:600]}")
    plain = {b["bone"]: b for b in r["realised"][0]["bones"]}
    # Mirror alone (everything else the same): the left thigh takes the right thigh's motion
    await ed.call("set_properties", entity="Robot", values={"animationplayer": {"clips[0].mirror": True}})
    r = await ed.call("get_animation", clip=WALK, root="Robot", times=[0.5])
    ed.check(r["retarget_report"].get("mirror") is True, f"the entry's Mirror: {r['retarget_report']}")
    mirrored = {b["bone"]: b for b in r["realised"][0]["bones"]}
    left, right = mirrored["left_upper_leg"]["angles"], plain["right_upper_leg"]["angles"]
    ed.check(quat_degrees(mirrored["left_upper_leg"]["local_rotation"]["quat"], plain["left_upper_leg"]["local_rotation"]["quat"]) > 1.0,
             "Mirror changes the realised left thigh")
    ed.check(abs(left["forward"] - right["forward"]) < 0.5, f"mirrored: the left thigh swings as the right did: {left} vs {right}")
    await ed.call("set_properties", entity="Robot", values={"animationplayer": {"clips[0].mirror": False}})
    await refused(ed, "edit_pose_animation", {"ops": [{"op": "set_humanoid", "model": "assets/models/hero_animated.gltf",
                                                       "extra_bones": {"left_pinky_proximal": "x"}}]}, "unknown bone")
    # a finger the runtime would drop (a body slot's bone) is refused, not saved
    arm = info["avatar"]["mapped"]["left_upper_arm"]
    await refused(ed, "edit_pose_animation", {"ops": [{"op": "set_humanoid", "model": "assets/models/hero_animated.gltf",
                                                       "extra_bones": {"left_index_proximal": arm}}]}, "body slot")
    # back to the bones-only sidecar: undo
    await ed.call("undo")
