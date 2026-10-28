"""Pose Animator runtime (pose animation phase 2, spec section 4): the component on the hero of the
pose fixture project, playing a small .poseanim written here in the file format of spec 2.1.

- add_component / set_properties: the component and its properties (animation, autoplay, speed,
  weight); scene save + reload keeps them (get_entity yaml, key `poseanimator`)
- edit mode: the animator does nothing (bones keep their rest pose)
- play mode: bone rotations change over time; eval_lua drives the PoseAnimator API (getPoseCount,
  findPose, getPoseName, goToPose reaching the key pose, setWeight(0) giving the rest pose back,
  stop showing pose 0, one-shot end and onFinish); Stop restores the edit-time bones
- a missing file logs one error and disables the animator
- export: the .poseanim and humanoid sidecar ship with their original bytes

Runs on a scratch copy of tools/fixtures/pose-animation.
"""

import asyncio
import json
import time

from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 900

S: dict = {}

# the Pose Animator's animation as MCP takes and shows it (project-relative, as the pose tools name
# it) and as the component stores it (relative to the assets folder: scene file, Lua, generated code)
ANIM = "assets/anims/raise.poseanim"
ANIM_STORED = "anims/raise.poseanim"
ONE_SHOT = "anims/oneshot.poseanim"
# long enough (2.12 s) to be seen half way through between two tool calls on a loaded machine
LONG_SHOT = "anims/longshot.poseanim"
HAND = "mixamorig:LeftHand"
ARM = "mixamorig:LeftArm"


def pose(name, raise_deg, hold=0.1):
    angles = {}
    if raise_deg:
        angles = {"left_upper_arm.raise": raise_deg, "right_upper_arm.raise": raise_deg}
    return {"name": name, "hold": hold, "grounded": True, "lift": 0, "transition": None,
            "offset": {"left": 0, "forward": 0}, "angles": angles}


def animation(poses, loop=True):
    return {
        "format": "jasperengine.poseanim", "version": 1, "preview_model": "models/hero_tpose.gltf",
        "frame_rate": 30, "loop": loop,
        "feel": {"transition": {"curve": "spring", "duration": 0.12, "overshoot": 0.25, "anticipation": 0.06},
                 "step_rate": 0, "overlap": 0.05, "looseness": 0.2,
                 "weights": {"spine": 1, "head": 1, "arms": 1, "legs": 0}},
        "humanize": {"enabled": False, "joint_limits": True, "moving_holds": 0.08, "inbetweens": 0.4,
                     "life": 0.6, "seed": 1, "head_follow": 0.5, "torso_follow": 0.4, "springiness": 0.5},
        "poses": poses,
    }


def write_asset(ed: Editor, rel: str, data: dict):
    path = ed.project / "assets" / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


def close(a, b, tol):
    return abs(a - b) <= tol


async def hand_y(ed: Editor, **args) -> float:
    v = await lua(ed, f"return object('Hero', 'Model'):getBone('{HAND}'):getWorldPosition().y", **args)
    return float(v[0])


async def arm_rotation(ed: Editor):
    r = await ed.call("get_runtime_entity", entity=ARM, components=["transform"], include_script_state=False)
    text = json.dumps(r)
    found = None

    def walk(value):
        nonlocal found
        if isinstance(value, dict):
            for k, v in value.items():
                if k == "rotation" and found is None:
                    found = v
                walk(v)
        elif isinstance(value, list):
            for v in value:
                walk(v)
    walk(r)
    ed.check(found is not None, f"no rotation in get_runtime_entity: {text[:600]}")
    return found


async def frames(ed: Editor, seconds: float):
    await ed.call("wait", seconds=seconds)


async def test_add_component(ed: Editor):
    write_asset(ed, ANIM_STORED, animation([pose("Down", 0, hold=0.3), pose("T", 90, hold=0.3), pose("Up", 150, hold=0.3)]))
    write_asset(ed, ONE_SHOT, animation([pose("Down", 0, hold=0.05), pose("T", 90, hold=0.05)], loop=False))
    write_asset(ed, LONG_SHOT, animation([pose("Down", 0, hold=1.0), pose("T", 90, hold=1.0)], loop=False))

    types = await ed.call("list_component_types")
    ed.check("poseanimator" in json.dumps(types).lower(), f"poseanimator not listed: {json.dumps(types)[:400]}")

    S["edit_hand_y"] = await hand_y(ed, allow_edit_mode=True)
    await ed.call("add_component", entity="Hero", component="poseanimator",
                  values={"poseanimator": {"animation": ANIM, "speed": 1, "weight": 1}})
    got = await ed.call("get_entity", entity="Hero", components=["poseanimator"])
    values = json.dumps(got)
    ed.check(ANIM in values, f"animation not set: {values[:600]}")

    # the path convention: project-relative in and out (as the pose tools), stored relative to the
    # assets folder; the stored form of an existing file is taken too; anything else is refused
    async def animation_value():
        got = await ed.call("get_entity", entity="Hero", components=["poseanimator"])
        found = []

        def walk(value):
            if isinstance(value, dict):
                for k, v in value.items():
                    if k == "animation" and isinstance(v, str):
                        found.append(v)
                    walk(v)
            elif isinstance(value, list):
                for v in value:
                    walk(v)
        walk(got)
        return found[0] if found else None
    ed.check_eq(await animation_value(), ANIM, "shown project-relative")
    await ed.call("set_properties", entity="Hero", values={"poseanimator": {"animation": ONE_SHOT}})
    ed.check_eq(await animation_value(), "assets/" + ONE_SHOT, "the assets-relative form of an existing file")
    await ed.call("set_properties", entity="Hero", values={"poseanimator": {"animation": ANIM}})
    ed.check_eq(await animation_value(), ANIM, "the project-relative form")
    for bad, why in (("anims/none.poseanim", "inside the assets"), ("assets/models/hero_tpose.gltf", ".poseanim"),
                     ("../outside.poseanim", "project-relative")):
        r = await ed.call_raw("set_properties", {"entity": "Hero", "values": {"poseanimator": {"animation": bad}}})
        ed.check(r.is_error and why in r.text, f"{bad}: {r.text[:300]}")
    ed.check_eq(await animation_value(), ANIM, "a refused path changes nothing")

    # the animator does not run in edit mode
    ed.check(close(await hand_y(ed, allow_edit_mode=True), S["edit_hand_y"], 1e-4), "edit mode moved the bones")


async def test_scene_round_trip(ed: Editor):
    await ed.call("set_properties", entity="Hero", values={"poseanimator": {"autoplay": True, "speed": 1.5, "weight": 0.75}})
    await ed.call("save_scene", scene="Main Scene")
    text = (ed.project / "Main Scene.scene").read_text()
    ed.check("poseanimator:" in text and ANIM_STORED in text, "no poseanimator key in the saved scene")

    # closed and opened again: decoded from the file
    await ed.call("create_scene", name="Other", type="3D")
    await ed.call("close_scene", scene="Main Scene")
    await ed.call("open_scene", scene="Main Scene.scene")
    y = await ed.call("get_entity", entity="Hero", detail="yaml", components=["poseanimator"])
    blob = json.dumps(y)
    ed.check("poseanimator" in blob and ANIM_STORED in blob, f"yaml after reload: {blob[:800]}")
    ed.check("1.5" in blob and "0.75" in blob, f"speed/weight after reload: {blob[:800]}")
    await ed.call("set_properties", entity="Hero", values={"poseanimator": {"speed": 1, "weight": 1}})


async def test_play_bones_move(ed: Editor):
    started = await ed.call("play", action="start")
    ed.check_eq(started["state"], "playing", f"play: {started}")
    deadline = time.monotonic() + 60
    while True:
        v = await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():isLoaded()")
        if v[0]:
            break
        ed.check(time.monotonic() < deadline, "the animator never loaded")
        await frames(ed, 0.2)

    rotations = []
    for _ in range(4):
        rotations.append(await arm_rotation(ed))
        await frames(ed, 0.17)
    ed.check(len({json.dumps(r) for r in rotations}) >= 3, f"the arm does not move over time: {rotations}")


async def test_lua_api(ed: Editor):
    v = await lua(ed, """
        local a = object('Hero', 'Model'):getPoseAnimator()
        return a:getPoseCount(), a:findPose('T'), a:findPose('nope'), a:getPoseName(2), a:getAnimation(),
               a:isPlaying(), a:getLength() > 0.5
    """)
    ed.check_eq(v, [3, 1, -1, "Up", ANIM_STORED, True, True], "PoseAnimator API")


async def test_go_to_pose(ed: Editor):
    # arms down (pose 0, neutral) then out to the side (pose "T": raise 90 = the T-pose)
    ok = await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():goToPose(0, 0.1, 0, 0)")
    ed.check_eq(ok, [True], "goToPose(0)")
    await frames(ed, 1.0)
    playing = await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():isPlaying()")
    ed.check_eq(playing, [False], "playback stops for a live transition")
    down = await hand_y(ed)
    ed.check(down < S["edit_hand_y"] - 0.35, f"pose 0 has the arms down: hand y {down}, rest {S['edit_hand_y']}")

    ok = await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():goToPose('T', 0.1, 0, 0)")
    ed.check_eq(ok, [True], "goToPose('T')")
    await frames(ed, 1.0)
    side = await hand_y(ed)
    ed.check(close(side, S["edit_hand_y"], 0.03), f"pose T is the T-pose: hand y {side}, rest {S['edit_hand_y']}")
    S["down_hand_y"] = down

    bad = await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():goToPose('nope')")
    ed.check_eq(bad, [False], "goToPose of an unknown pose")


async def test_weight_zero_leaves_the_rest_pose(ed: Editor):
    await lua(ed, "object('Hero', 'Model'):getPoseAnimator():goToPose(0, 0.05, 0, 0)")
    await frames(ed, 0.6)
    ed.check(await hand_y(ed) < S["edit_hand_y"] - 0.35, "arms down before the weight change")
    await lua(ed, "object('Hero', 'Model'):getPoseAnimator():setWeight(0)")
    await frames(ed, 0.4)
    ed.check(close(await hand_y(ed), S["edit_hand_y"], 1e-3), "weight 0: the bones are back in the rest pose")
    await lua(ed, "object('Hero', 'Model'):getPoseAnimator():setWeight(0.5)")
    await frames(ed, 0.4)
    half = await hand_y(ed)
    ed.check(S["down_hand_y"] + 0.05 < half < S["edit_hand_y"] - 0.05, f"weight 0.5 is in between: {half}")
    await frames(ed, 0.4)
    ed.check(close(await hand_y(ed), half, 1e-3), "weight 0.5 is stable over frames")
    await lua(ed, "object('Hero', 'Model'):getPoseAnimator():setWeight(1)")


async def arm_angle(ed: Editor) -> float:
    """degrees of the upper arm's local rotation (the hero's rest is identity)"""
    v = await lua(ed, f"local q = object('Hero', 'Model'):getBone('{ARM}').rotation; "
                      "return 2 * math.deg(math.acos(math.min(1, math.abs(q.w))))")
    return float(v[0])


async def test_pause_freezes_go_to_pose(ed: Editor):
    await lua(ed, "object('Hero', 'Model'):getPoseAnimator():goToPose('T', 0.05, 0, 0)")
    await frames(ed, 0.5)
    await lua(ed, "local a = object('Hero', 'Model'):getPoseAnimator(); a:goToPose(0, 1.0, 0, 0)")
    await frames(ed, 0.25)
    await lua(ed, "object('Hero', 'Model'):getPoseAnimator():pause()")
    frozen = await hand_y(ed)
    ed.check(S["down_hand_y"] + 0.05 < frozen < S["edit_hand_y"] - 0.02, f"paused half way down: {frozen}")
    await frames(ed, 0.5)
    ed.check(close(await hand_y(ed), frozen, 1e-4), "pause froze the transition")
    await lua(ed, "object('Hero', 'Model'):getPoseAnimator():goToPose(0, 0.05, 0, 0)")
    await frames(ed, 0.5)
    ed.check(await hand_y(ed) < S["edit_hand_y"] - 0.35, "a new goToPose runs again")


async def test_empty_path_restores_the_bones(ed: Editor):
    ed.check(await hand_y(ed) < S["edit_hand_y"] - 0.35, "arms down before")
    await lua(ed, "object('Hero', 'Model'):getPoseAnimator():setAnimation('')")
    await frames(ed, 0.3)
    ed.check(close(await hand_y(ed), S["edit_hand_y"], 1e-3), "no animation: the bones are back in the rest pose")
    await lua(ed, f"object('Hero', 'Model'):getPoseAnimator():setAnimation('{ANIM_STORED}')")
    deadline = time.monotonic() + 30
    while not (await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():isLoaded()"))[0]:
        ed.check(time.monotonic() < deadline, "the animator never loaded again")
        await frames(ed, 0.2)


async def test_negative_speed_one_shot_starts_at_the_end(ed: Editor):
    try:
        await lua(ed, f"""
            local a = object('Hero', 'Model'):getPoseAnimator()
            a:setSpeed(-1)
            a:play()
            a:setAnimation('{LONG_SHOT}')
        """)
        deadline = time.monotonic() + 30
        while not (await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():isLoaded()"))[0]:
            ed.check(time.monotonic() < deadline, "the one-shot never loaded")
            await frames(ed, 0.05)
        v = await lua(ed, "local a = object('Hero', 'Model'):getPoseAnimator(); return a:getTime(), a:getLength(), a:isPlaying()")
        ed.check(v[2] and 0.5 * v[1] < v[0] < v[1], f"backwards from the end: time {v[0]} of {v[1]}, playing {v[2]}")
        await frames(ed, 0.2)
        later = await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():getTime()")
        ed.check(later[0] < v[0], f"time goes down: {v[0]} -> {later[0]}")
        deadline = time.monotonic() + 30
        while (await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():isPlaying()"))[0]:
            ed.check(time.monotonic() < deadline, "the backwards one-shot never ended")
            await frames(ed, 0.2)
        v = await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():getTime()")
        ed.check_eq(v, [0], "the backwards one-shot ends at time 0")
    finally:
        # the next tests play forwards whatever happened here
        await lua(ed, f"local a = object('Hero', 'Model'):getPoseAnimator(); a:setSpeed(1); a:setAnimation('{ANIM_STORED}'); a:play()")
    deadline = time.monotonic() + 30
    while not (await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():isLoaded()"))[0]:
        ed.check(time.monotonic() < deadline, "the animator never loaded again")
        await frames(ed, 0.2)


async def test_stop_and_one_shot(ed: Editor):
    v = await lua(ed, """
        local a = object('Hero', 'Model'):getPoseAnimator()
        a:stop()
        return a:isPlaying(), a:getTime()
    """)
    ed.check_eq(v, [False, 0], "stop: not playing, time 0")
    await frames(ed, 0.3)
    ed.check(await hand_y(ed) < S["edit_hand_y"] - 0.35, "stop shows pose 0 (arms down)")

    await lua(ed, f"""
        local a = object('Hero', 'Model'):getPoseAnimator()
        finished = 0
        a.onFinish = function() finished = finished + 1 end
        a:setAnimation('{ONE_SHOT}')
        a:play()
    """)
    deadline = time.monotonic() + 30
    while True:
        v = await lua(ed, "local a = object('Hero', 'Model'):getPoseAnimator(); return finished, a:isPlaying(), a:isLoaded(), a:getTime(), a:getLength()")
        if v[0] >= 1:
            break
        ed.check(time.monotonic() < deadline, f"the one-shot never finished: {v}")
        await frames(ed, 0.2)
    await frames(ed, 0.3)
    v = await lua(ed, "local a = object('Hero', 'Model'):getPoseAnimator(); return finished, a:isPlaying(), math.abs(a:getTime() - a:getLength()) < 1e-4")
    ed.check_eq(v, [1, False, True], "one-shot: onFinish once, stopped at the end")
    # the last pose (T) stays
    ed.check(close(await hand_y(ed), S["edit_hand_y"], 0.03), "the one-shot holds its last pose")


async def test_stop_play_restores_the_scene(ed: Editor):
    await ed.call("play", action="stop")
    ed.check(close(await hand_y(ed, allow_edit_mode=True), S["edit_hand_y"], 1e-4), "Stop restores the bones")
    got = json.dumps(await ed.call("get_entity", entity="Hero", components=["poseanimator"]))
    ed.check(ANIM in got and ONE_SHOT not in got, f"Stop restores the component: {got[:500]}")


async def test_on_finish_does_not_survive_stop(ed: Editor):
    """play 1 sets onFinish; play 2 does not: its one-shot end calls nothing (no released closure)"""
    async def one_shot_play(register: bool):
        await ed.call("play", action="start")
        code = f"local a = object('Hero', 'Model'):getPoseAnimator(); "
        if register:
            code += "a.onFinish = function() Log.print('pose-on-finish-called') end; "
        code += f"a:setAnimation('{ONE_SHOT}'); a:play()"
        await lua(ed, code)
        deadline = time.monotonic() + 30
        while True:
            v = await lua(ed, "local a = object('Hero', 'Model'):getPoseAnimator(); return a:isLoaded() and not a:isPlaying()")
            if v[0]:
                break
            ed.check(time.monotonic() < deadline, "the one-shot never ended")
            await frames(ed, 0.2)
        await ed.call("play", action="stop")

    def calls(entries):
        return [e for e in entries if "pose-on-finish-called" in e.get("message", "")]

    await one_shot_play(True)
    first = calls(await ed.console_since(None, limit=1000))
    ed.check_eq(len(first), 1, f"play 1: onFinish called once: {first}")
    await one_shot_play(False)
    second = calls(await ed.console_since(None, limit=1000))
    ed.check_eq(len(second), 1, f"play 2 called play 1's onFinish: {second}")


async def test_clip_under_the_animator(ed: Editor):
    """a clip (Animation) playing on the upper arm, the animator over it: weight 1 = the pose,
    0 = the clip, 0.5 = half way (the T key's upper arm is the hero's rest, identity)"""
    made = await ed.call("edit_animation", target="Hero", name="ArmClip", autoplay=True, loop=True, duration=1,
                         tracks=[{"target": ARM, "channel": "rotation",
                                  "keys": [{"time": 0, "value": [0, 0, -0.5, 0.8660254]},
                                           {"time": 1, "value": [0, 0, -0.5, 0.8660254]}]}])
    ed.check(made, "edit_animation")
    await ed.call("play", action="start")
    deadline = time.monotonic() + 60
    while not (await lua(ed, "return object('Hero', 'Model'):getPoseAnimator():isLoaded()"))[0]:
        ed.check(time.monotonic() < deadline, "the animator never loaded")
        await frames(ed, 0.2)
    await lua(ed, "object('Hero', 'Model'):getPoseAnimator():goToPose('T', 0.05, 0, 0)")
    await frames(ed, 0.5)
    angles = {}
    for weight in (1, 0, 0.5):
        await lua(ed, f"object('Hero', 'Model'):getPoseAnimator():setWeight({weight})")
        await frames(ed, 0.3)
        angles[weight] = await arm_angle(ed)
    ed.check(angles[1] < 3, f"weight 1: the pose (T): {angles}")
    ed.check(close(angles[0], 60, 1.5), f"weight 0: the clip's 60 degrees: {angles}")
    ed.check(close(angles[0.5], angles[0] / 2, 2.5), f"weight 0.5: half way between the clip and the pose: {angles}")
    await frames(ed, 0.3)
    ed.check(close(await arm_angle(ed), angles[0.5], 0.2), f"weight 0.5 over a clip is stable: {angles}")
    await ed.call("play", action="stop")
    await ed.call("undo")


async def test_missing_file_logs_one_error(ed: Editor):
    # a file that does not exist (yet): only the project-relative form names it
    await ed.call("add_component", entity="Robot", component="poseanimator",
                  values={"poseanimator": {"animation": "assets/anims/missing.poseanim"}})
    await ed.call("play", action="start")
    await frames(ed, 1.0)
    entries = await ed.console_since(None, types=["error"], limit=1000)
    errors = [e.get("message", "") for e in entries if "missing.poseanim" in e.get("message", "")]
    ed.check_eq(len(errors), 1, f"one error for the missing file: {errors}")
    v = await lua(ed, "return object('Robot', 'Model'):getPoseAnimator():isLoaded(), object('Hero', 'Model'):getPoseAnimator():isLoaded()")
    ed.check_eq(v, [False, True], "the broken animator is disabled, the other plays")

    # a good path loads it again: the A-pose robot (Blender rest rotations, twist bones) in a T-pose
    await lua(ed, f"object('Robot', 'Model'):getPoseAnimator():setAnimation('{ANIM_STORED}')")
    deadline = time.monotonic() + 30
    while not (await lua(ed, "return object('Robot', 'Model'):getPoseAnimator():isLoaded()"))[0]:
        ed.check(time.monotonic() < deadline, "the robot's animator never loaded")
        await frames(ed, 0.2)
    await lua(ed, "object('Robot', 'Model'):getPoseAnimator():goToPose('T', 0.1, 0, 0)")
    await frames(ed, 1.0)
    v = await lua(ed, """
        local m = object('Robot', 'Model')
        return m:getBone('hand.L'):getWorldPosition().y, m:getBone('upper_arm.L'):getWorldPosition().y
    """)
    ed.check(close(v[0], v[1], 0.03), f"robot T-pose: the hand at shoulder height: {v}")

    # the same path set again after a failure (Inspector / set_properties) loads again
    await lua(ed, "object('Robot', 'Model'):getPoseAnimator():setAnimation('anims/missing.poseanim')")
    await frames(ed, 0.5)
    write_asset(ed, "anims/missing.poseanim", animation([pose("Down", 0), pose("T", 90)]))
    await ed.call("set_properties", entity="Robot", values={"poseanimator": {"animation": "assets/anims/missing.poseanim"}})
    deadline = time.monotonic() + 30
    while not (await lua(ed, "return object('Robot', 'Model'):getPoseAnimator():isLoaded()"))[0]:
        ed.check(time.monotonic() < deadline, "the fixed file never loaded through set_properties")
        await frames(ed, 0.2)
    await ed.call("play", action="stop")
    (ed.project / "assets" / "anims" / "missing.poseanim").unlink()
    await ed.call("remove_component", entity="Robot", component="poseanimator")


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


async def test_export_preserves_pose_assets(ed: Editor):
    # a humanoid sidecar next to the model (overrides nothing) ships like any asset
    write_asset(ed, "models/hero_tpose.gltf.humanoid", {"format": "jasperengine.humanoid", "version": 1, "bones": {}})
    await ed.call("save_scene")
    out = ed.scratch.root / "export-pose"
    raw = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "platform": "linux",
                                              "overwrite": True, "timeout_seconds": 900})
    r = raw.value
    if isinstance(r, dict) and r.get("status") == "running":
        r = await wait_job(ed, r["job_id"], 900)
    ed.check_eq(r["status"], "succeeded", raw.text[:2000])
    # The exported program must reference the authored asset. This checks a user-facing
    # path byte and survives renames of generated variables, members and helper functions.
    ed.check(any(f'"{ANIM_STORED}"' in source.read_text(errors="ignore") for source in out.rglob("*.cpp")),
             "the exported program references the selected pose animation")
    assets = out / "project" / "assets"
    for rel in (ANIM_STORED, "models/hero_tpose.gltf.humanoid"):
        ed.check_eq((assets / rel).read_bytes(), (ed.project / "assets" / rel).read_bytes(),
                    f"the exported {rel} retains the asset bytes")
