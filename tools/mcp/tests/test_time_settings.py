"""S95 REQ-13 (AC-14): Project Settings > Time as an agent sees it. time.fixed_timestep and
time.maximum_delta_time are set before the scripts of Play run; a step a script sets in Play does
not leak into the next Play (Play stop gives the editor the defaults back). A scene's own
fixed_timestep (set_scene_settings, one undo step) is the step while it is the played scene."""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Time Settings"}
TIMEOUT = 600


async def step(ed: Editor, edit_mode: bool = False) -> float:
    return (await ed.call("eval_lua", code="return Time.fixedDeltaTime", allow_edit_mode=edit_mode))["values"][0]


async def play(ed: Editor, scene: int) -> None:
    started = await ed.call("play", action="start", scene=scene)
    ed.check_eq(started["state"], "playing", f"start: {started}")
    await ed.call("wait", frames=2)


async def test_time_settings(ed: Editor):
    ed.require_tools("set_project_settings", "get_project_settings", "set_scene_settings", "get_scene_settings", "play",
                     "eval_lua", "undo")
    made = await ed.call("create_scene", name="TimeCI", type="2D", path="scenes/TimeCI")
    sid = made["scene"]["id"]
    await ed.call("save_scene", scene=sid)

    keys = ["time.fixed_timestep", "time.maximum_delta_time"]
    got = (await ed.call("get_project_settings", keys=keys))["settings"]
    ed.check_eq((round(got["time.fixed_timestep"], 9), got["time.maximum_delta_time"]), (round(1 / 60, 9), 0.25), "defaults")
    await ed.call("set_project_settings", values={"time.fixed_timestep": 0.02, "time.maximum_delta_time": 0.1})
    got = (await ed.call("get_project_settings", keys=keys))["settings"]
    ed.check_eq((got["time.fixed_timestep"], got["time.maximum_delta_time"]), (0.02, 0.1), "set")
    refused = await ed.call_raw("set_project_settings", {"values": {"time.fixed_timestep": 0}})
    ed.check(refused.is_error, f"a zero step is refused: {refused.text}")

    await play(ed, sid)
    try:
        ed.check_eq(round(await step(ed), 6), 0.02, "Play starts at the project's step")
        maximum = (await ed.call("eval_lua", code="return Time.maximumDeltaTime"))["values"][0]
        ed.check_eq(round(maximum, 6), 0.1, "and its frame cap")
        await ed.call("eval_lua", code="Time.fixedDeltaTime = 1 / 120")
        ed.check_eq(round(await step(ed), 6), round(1 / 120, 6), "a script sets it at runtime")
    finally:
        await ed.call("play", action="stop")
    ed.check_eq(round(await step(ed, edit_mode=True), 6), round(1 / 60, 6), "Play stop gives the editor its step back")

    await play(ed, sid)
    try:
        ed.check_eq(round(await step(ed), 6), 0.02, "the script's step did not leak into the next Play")
    finally:
        await ed.call("play", action="stop")

    # the scene's own step while it is the played scene
    await ed.call("set_scene_settings", scene=sid, settings={"fixed_timestep": 0.0025})
    ed.check_eq((await ed.call("get_scene_settings", scene=sid, names=["fixed_timestep"]))["settings"]["fixed_timestep"],
                0.0025, "scene setting")
    bad = await ed.call_raw("set_scene_settings", {"scene": sid, "settings": {"fixed_timestep": -1}})
    ed.check(bad.is_error, f"a negative step is refused: {bad.text}")
    await play(ed, sid)
    try:
        ed.check_eq(round(await step(ed), 6), 0.0025, "the scene's override")
    finally:
        await ed.call("play", action="stop")
    await ed.call("undo")
    ed.check_eq((await ed.call("get_scene_settings", scene=sid, names=["fixed_timestep"]))["settings"]["fixed_timestep"],
                0, "one undo step: back to the project's")
