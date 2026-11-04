"""Camera shake and camera follow over MCP (spec docs/superpowers/specs/2026-10-31-camera-shake-design.md 14):
add_component camerashaker with validation, write_file of a .shake (refused when unreadable, warnings when
repaired), get_asset_info with the peaks, list_assets {type: shake}, play + eval_lua CameraShake.shake,
get_runtime_entity's camerashaker.runtime (an instance and an offset, then nothing after the fade) with
the camera's Transform never shaken, a game-view screenshot, and the camera follow flow."""

import math

from harness import Editor
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "Camera Shake"}
CI = True

BOOM = """{
  "format": "jasperengine.shake",
  "version": 1,
  "type": "one_shot",
  "strength": 0.3,
  "roughness": 12,
  "fade_in": 0.05,
  "fade_out": 0.5,
  "position_influence": [1, 1, 0],
  "rotation_influence": [10, 10, 20]
}
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="ShakeMain", type="3D", path="scenes/ShakeMain")
    S["scene"] = made["scene"]["id"]
    cam = await ed.call("create_entity", scene=S["scene"], type="camera", name="Main Camera", position=[0, 2, 8])
    S["camera"] = cam["entity_id"]
    await ed.call("set_main_camera", scene=S["scene"], entity="Main Camera")
    player = await ed.call("create_entity", scene=S["scene"], type="cube", name="Player", position=[0, 0, 0])
    S["player"] = player["entity_id"]
    await ed.call("save_scene", scene=S["scene"])
    return S


def close(a, b, tolerance=1e-3) -> bool:
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


async def runtime_of(ed: Editor, entity, component: str) -> dict:
    got = await ed.call("get_runtime_entity", entity=entity, components=[component, "transform"])
    return got["components"]


async def test_a_shaker_and_its_validation(ed: Editor):
    s = await setup(ed)
    await ed.call("add_component", scene=s["scene"], entity=s["camera"], component="camerashaker")
    got = await ed.call("get_entity", scene=s["scene"], entity=s["camera"], components=["camerashaker"])
    shaker = got["components"]["camerashaker"]
    ed.check_eq((shaker["global"], shaker["mode"], shaker["traumaPower"]), (True, "auto", 2.0), f"defaults: {shaker}")
    # the ranges are the S08 table's (oe-port's range text: "in 1..4", "at least 0")
    await ed.check_error("set_properties", scene=s["scene"], entity=s["camera"],
                         values={"camerashaker": {"traumaPower": 9}}, contains="1..4")
    await ed.check_error("set_properties", scene=s["scene"], entity=s["camera"],
                         values={"camerashaker": {"positionScale": -1}}, contains="at least 0")
    await ed.check_error("set_properties", scene=s["scene"], entity=s["camera"],
                         values={"camerashaker": {"startPreset": "kaboom"}}, contains="built-in")
    await ed.check_error("set_properties", scene=s["scene"], entity=s["camera"],
                         values={"camerashaker": {"startPreset": "assets/shakes/missing.shake"}}, contains="does not exist")
    await ed.check_error("set_properties", scene=s["scene"], entity=s["camera"],
                         values={"camerashaker": {"mode": "4d"}}, contains="2d")
    await ed.call("set_properties", scene=s["scene"], entity=s["camera"], values={"camerashaker": {"startPreset": "drunk", "mode": "3d"}})
    got = await ed.call("get_entity", scene=s["scene"], entity=s["camera"], components=["camerashaker"])
    ed.check_eq((got["components"]["camerashaker"]["startPreset"], got["components"]["camerashaker"]["mode"]), ("drunk", "3d"))
    await ed.call("set_properties", scene=s["scene"], entity=s["camera"], values={"camerashaker": {"startPreset": "", "mode": "auto"}})


async def test_b_shake_files(ed: Editor):
    s = await setup(ed)
    msg = await ed.check_error("write_file", path="assets/shakes/broken.shake", content="{\"format\": \"jasperengine.shake\", ",
                               contains="Not a valid shake preset")
    ed.check("1:" in msg, f"the error has a position: {msg}")
    ed.check(not (ed.project / "assets/shakes/broken.shake").exists(), "an unreadable preset is not written")

    lenient = await ed.call("write_file", path="assets/shakes/lenient.shake",
                            content="{\"format\": \"jasperengine.shake\", \"version\": 1, \"strength\": -1, \"wobble\": 3}")
    ed.check(len(lenient["warnings"]) == 2, f"repaired strength and the kept unknown key: {lenient}")

    await ed.call("write_file", path="assets/shakes/boom.shake", content=BOOM)
    info = await ed.call("get_asset_info", path="assets/shakes/boom.shake")
    ed.check_eq(info["shake"]["type"], "one_shot")
    ed.check(close([info["shake"]["strength"]], [0.3]), f"values: {info['shake']}")
    peak = info["peak"]
    ed.check(0 < peak["rotation"][2] <= 0.3 * 20 + 1e-4, f"rotation z peak within strength x influence: {peak}")
    ed.check_eq(peak["position"][2], 0.0, f"no z influence, no z motion: {peak}")
    ed.check(abs(peak["duration"] - (0.05 + 1 + 0.5)) < 1e-4, f"sampled span: {peak}")

    listed = await ed.call("list_assets", type="shake")
    paths = [a["path"] for a in listed["entries"]]
    ed.check("assets/shakes/boom.shake" in paths, f"list_assets type shake: {paths}")
    ed.check(all(p.endswith(".shake") for p in paths), f"only .shake files: {paths}")

    await ed.call("set_properties", scene=s["scene"], entity=s["camera"],
                  values={"camerashaker": {"startPreset": "assets/shakes/boom.shake"}})
    got = await ed.call("get_entity", scene=s["scene"], entity=s["camera"], components=["camerashaker"])
    ed.check_eq(got["components"]["camerashaker"]["startPreset"], "assets/shakes/boom.shake", "a .shake start preset reads back")
    await ed.call("set_properties", scene=s["scene"], entity=s["camera"], values={"camerashaker": {"startPreset": ""}})
    await ed.call("save_scene", scene=s["scene"])


async def test_c_shake_in_play(ed: Editor):
    s = await setup(ed)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        before = await runtime_of(ed, "Main Camera", "camerashaker")
        ed.check_eq(before["camerashaker"]["runtime"]["instances"], [], "idle before the shake")
        position = before["transform"]["position"]
        steady = await ed.call_raw("screenshot", {"view": "game", "max_width": 320})
        ed.check(not steady.is_error and steady.images, f"a steady game-view screenshot: {steady.text}")

        values = (await ed.call("eval_lua", code="local h = CameraShake.shake('assets/shakes/boom.shake', {seed = 3}) "
                                                 "return h:isValid(), h.count, h.state"))["values"]
        ed.check_eq(values, [True, 1, "fading_in"], "a valid handle on the one global shaker")
        await ed.call("wait", frames=6)
        during = await runtime_of(ed, "Main Camera", "camerashaker")
        runtime = during["camerashaker"]["runtime"]
        ed.check_eq(len(runtime["instances"]), 1, f"one instance: {runtime}")
        ed.check_eq(runtime["instances"][0]["preset"], "assets/shakes/boom.shake")
        offset = runtime["offset_position"] + runtime["offset_rotation"]
        ed.check(any(abs(v) > 1e-6 for v in offset), f"a non-zero offset: {runtime}")
        ed.check(close(during["transform"]["position"], position), "the camera's Transform is never shaken")

        shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 320})
        ed.check(not shot.is_error and shot.images, f"a game-view screenshot while shaking: {shot.text}")
        # the view moved: the box is drawn elsewhere (the Transform did not move)
        w1, h1, c1, rows1 = decode_png(steady.images[0][0])
        w2, h2, c2, rows2 = decode_png(shot.images[0][0])
        ed.check((w1, h1) == (w2, h2), "same size")
        changed = sum(1 for y in range(h1) for x in range(0, w1 * c1, c1)
                      if abs(rows1[y][x] - rows2[y][x]) + abs(rows1[y][x + 1] - rows2[y][x + 1]) > 30)
        ed.check(changed > 50, f"the shaken frame differs from the steady one ({changed} pixels)")

        await ed.call("wait", game_seconds=1.0)
        after = (await runtime_of(ed, "Main Camera", "camerashaker"))["camerashaker"]["runtime"]
        ed.check_eq(after["instances"], [], f"gone after its fade: {after}")
        ed.check(all(v == 0 for v in after["offset_position"] + after["offset_rotation"]), f"no offset left: {after}")

        # trauma, the Lua shaker object and a table preset
        values = (await ed.call("eval_lua", code=(
            "CameraShake.addTrauma(0.5) "
            "local cam = Engine.scene.camera "
            "local s = GameObject(Engine.scene, cam):getCameraShaker() "
            "local h = s:shake({type = 'sustained', strength = 0.5, roughness = 8, rotationInfluence = Vector3(0, 0, 10)}) "
            "return s.trauma, s:getInstanceCount(), h.state")))["values"]
        ed.check(abs(values[0] - 0.5) < 1e-5 and values[1] == 1, f"trauma and a table preset: {values}")
        await ed.call("wait", frames=3)
        await ed.call("eval_lua", code="CameraShake.stopAll(0)")
        await ed.call("wait", frames=2)
        stopped = (await runtime_of(ed, "Main Camera", "camerashaker"))["camerashaker"]["runtime"]
        ed.check_eq(stopped["instances"], [], f"stopAll(0) ends it: {stopped}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    errors = (await ed.call("read_console", since=cursor, types=["error"]))["entries"]
    ed.check(not errors, f"unexpected errors: {errors}")


async def test_d_camera_follow(ed: Editor):
    s = await setup(ed)
    await ed.call("add_component", scene=s["scene"], entity=s["camera"], component="camerafollow",
                  values={"camerafollow": {"target": s["player"], "offset": [0, 2, 8], "damping": [0, 0, 0], "lookAt": False}})
    await ed.check_error("set_properties", scene=s["scene"], entity=s["camera"],
                         values={"camerafollow": {"damping": [-1, 0, 0]}}, contains="damping")
    await ed.check_error("set_properties", scene=s["scene"], entity=s["camera"],
                         values={"camerafollow": {"offsetSpace": "sideways"}}, contains="target_yaw")
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        await ed.call("set_runtime_property", entity="Player", component="transform", property="position", value=[3, 0, -1])
        await ed.call("wait", frames=2)
        got = await runtime_of(ed, "Main Camera", "camerafollow")
        ed.check(close(got["transform"]["position"], [3, 2, 7]), f"rigid follow: {got['transform']['position']}")
        runtime = got["camerafollow"]["runtime"]
        ed.check(runtime["placed"] and close(runtime["desired"], [3, 2, 7]), f"follow runtime: {runtime}")

        # damped: SmoothDamp moves part of the way in one frame
        await ed.call("set_runtime_property", entity="Main Camera", component="camerafollow", property="damping", value=[0.5, 0.5, 0.5])
        await ed.call("set_runtime_property", entity="Player", component="transform", property="position", value=[10, 0, -1])
        await ed.call("wait", frames=3)
        x = (await runtime_of(ed, "Main Camera", "camerafollow"))["transform"]["position"][0]
        ed.check(3 < x < 10 and not math.isclose(x, 10), f"damped follow is on its way: {x}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    await ed.call("remove_component", scene=s["scene"], entity=s["camera"], component="camerafollow")
