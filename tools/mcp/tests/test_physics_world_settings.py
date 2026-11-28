"""The physics world settings over MCP (docs/specs/S96-physics-gaps.md WP-E, REQ-30..REQ-37, REQ-43,
REQ-45): set_scene_settings and get_scene_settings by the snake names, one undo step each, out of
range refused naming the range; edit_colliders `body` allow_fast_rotation; in Play the Lua names
read the same values, a scene setting changed live reaches the world, and fixed steps a cap drops
are counted in Time.droppedFixedSteps and editor_status physics.dropped_fixed_steps, with one
warning."""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Physics World Settings"}


async def test_world_settings_scene_lua_and_step_cap(ed: Editor):
    ed.require_tools("set_scene_settings", "get_scene_settings", "edit_colliders", "play", "wait", "eval_lua", "editor_status")
    sid = (await ed.call("create_scene", path="scenes/World.scene", name="World", type="2d"))["scene"]["id"]

    names = ["physics_2d_points_per_meter", "physics_2d_max_rotation", "physics_2d_bounce_threshold",
             "physics_2d_warm_starting", "physics_3d_velocity_iterations", "physics_3d_collision_steps"]
    got = await ed.call("get_scene_settings", scene=sid, names=names, detail=True)
    ed.check_eq(got["settings"]["physics_2d_points_per_meter"], 64, "default points per metre")
    ed.check_eq(got["settings"]["physics_2d_max_rotation"], 45, "Box2D's 0.25 pi, in degrees")
    ed.check_eq(got["settings"]["physics_2d_warm_starting"], True)
    ed.check_eq(got["settings"]["physics_3d_velocity_iterations"], 10)
    ed.check_eq(got["schema"]["physics_3d_collision_steps"]["default"], 1)
    ed.check("Unity" in got["schema"]["physics_2d_max_rotation"]["help"], f"{got['schema']['physics_2d_max_rotation']}")

    await ed.call("set_scene_settings", scene=sid, settings={"physics_2d_points_per_meter": 100, "physics_2d_max_rotation": 360,
                                                              "physics_2d_warm_starting": False})
    got = await ed.call("get_scene_settings", scene=sid, names=names)
    ed.check_eq(got["settings"]["physics_2d_points_per_meter"], 100)
    ed.check_eq(got["settings"]["physics_2d_max_rotation"], 360)
    ed.check_eq(got["settings"]["physics_2d_warm_starting"], False)
    await ed.call("undo")
    got = await ed.call("get_scene_settings", scene=sid, names=names)
    ed.check_eq(got["settings"]["physics_2d_points_per_meter"], 64, "one undo step restores them")
    ed.check_eq(got["settings"]["physics_2d_warm_starting"], True)
    await ed.call("redo")

    error = await ed.call("set_scene_settings", scene=sid, settings={"physics_3d_collision_steps": 9}, expect_error=True)
    ed.check("1 to 4" in error, f"the error names the range: {error}")
    error = await ed.call("set_scene_settings", scene=sid, settings={"physics_2d_points_per_meter": 0}, expect_error=True)
    ed.check("physics_2d_points_per_meter" in error, error)

    wheel = (await ed.call("create_entity", scene=sid, name="Wheel", type="rigidbody2d", position=[0, 300, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=wheel, body_type="dynamic", add=[{"shape": "circle", "radius": 32}],
                  body={"allow_fast_rotation": True})
    props = await ed.call("get_entity", scene=sid, entity=wheel, components=["rigidbody2d"], properties=["allowFastRotation"])
    ed.check(props["components"]["rigidbody2d"]["allowFastRotation"] is True, f"edit_colliders body allow_fast_rotation: {props}")
    await ed.call("save_scene", scene=sid)

    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=5)
        values = (await ed.call("eval_lua", code="local b = object('Wheel'):getRigidbody2D(); "
                                                 "return {scene.physics2DPointsPerMeter, scene.physics2DMaxRotation, "
                                                 "scene.physics2DWarmStarting, b.allowFastRotation, "
                                                 "scene:getPhysicsSystem().pointsToMeterScale2D, Time.maximumFixedStepsPerFrame, "
                                                 "Time.droppedFixedSteps}"))["values"][0]
        ed.check_eq(values, [100, 360, False, True, 100, 0, 0], "the Lua names read the scene's values")
        status = await ed.call("editor_status")
        ed.check_eq(status["physics"]["dropped_fixed_steps"], 0, "nothing dropped with the automatic cap")

        # 240 Hz steps in 60 fps frames, at most one a frame: the others dropped
        start = (await ed.call("eval_lua", code="Time.fixedDeltaTime = 1 / 240; Time.maximumFixedStepsPerFrame = 1; "
                                                "return Time.frameCount"))["values"][0]
        await ed.call("wait", frames=10)
        dropped, frames = (await ed.call("eval_lua", code="return {Time.droppedFixedSteps, Time.frameCount}"))["values"][0]
        frames -= start
        # each 60 fps frame holds 4 steps of 1/240 s, less a hair (the step is a float): 2 or 3 dropped
        ed.check(frames > 0 and 2 * frames <= dropped <= 3 * frames, f"2 or 3 a frame: {dropped} over {frames} frames")
        status = await ed.call("editor_status")
        ed.check_eq(status["physics"]["dropped_fixed_steps"], dropped, "editor_status counts them too")
        warnings = await ed.call("read_console", types=["warning"], contains="fixed step(s):", limit=20)
        ed.check_eq(len(warnings["entries"]), 1, f"one warning per Play: {warnings}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
