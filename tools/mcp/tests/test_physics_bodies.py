"""S96 WP-C (docs/specs/S96-physics-gaps.md REQ-18, REQ-20, REQ-24, AC-20, AC-23): edit_colliders
decompose turns a torus's Mesh collider into convex hulls (same pose and material, one undo step)
so the body can be dynamic and fall in Play; a capsule's half_height 0 is accepted (a sphere) and a
negative one refused naming the field; body attach_to_parent_body makes a child's collider part of
its parent's body (query_physics names the child, its body the parent)."""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Physics Bodies"}
TIMEOUT = 600


async def scene3d(ed: Editor, name: str) -> int:
    sid = (await ed.call("create_scene", name=name, type="3d", path=f"scenes/{name}"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, name="Floor", type="box", position=[0, -0.5, 0], scale=[20, 1, 20])
    await ed.call("edit_colliders", scene=sid, entity="Floor", body_type="static", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=sid, type="camera", name="Cam", position=[0, 3, 10])
    return sid


async def test_capsule_half_height_zero(ed: Editor):
    sid = await scene3d(ed, "Capsules")
    await ed.call("create_entity", scene=sid, name="Pill", type="sphere", position=[0, 2, 0])
    made = await ed.call("edit_colliders", scene=sid, entity="Pill", body_type="dynamic",
                         add=[{"shape": "capsule", "half_height": 0, "radius": 0.5, "fit": False}])
    ed.check_eq(made["colliders"][0].get("half_height"), 0, f"a capsule with no cylinder part: {made['colliders'][0]}")
    await ed.check_error("edit_colliders", scene=sid, entity="Pill", update=[{"index": 0, "half_height": -1}], contains="half_height")
    await ed.call("save_scene", scene=sid)
    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=180)
        live = await ed.call("get_runtime_entity", entity="Pill", include_script_state=False)
        y = live["components"]["transform"]["position"][1]
        ed.check(abs(y - 0.5) < 0.01, f"rests on the floor at its radius: {live['components']['transform']}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_decompose_mesh_collider(ed: Editor):
    sid = await scene3d(ed, "Decompose")
    await ed.call("create_entity", scene=sid, name="Ring", type="torus", position=[0, 3, 0], rotation=[90, 0, 0])
    made = await ed.call("edit_colliders", scene=sid, entity="Ring", body_type="static",
                         add=[{"shape": "mesh", "center": [0, 0.1, 0], "density": 500, "is_trigger": False}])
    ed.check_eq(made["colliders"][0]["shape"], "mesh", f"{made}")
    await ed.check_error("edit_colliders", scene=sid, entity="Ring", decompose={"index": 0, "max_hulls": 0}, contains="max_hulls")
    await ed.check_error("edit_colliders", scene=sid, entity="Floor", decompose={"index": 0}, contains="not a Mesh collider")

    got = await ed.call("edit_colliders", scene=sid, entity="Ring", decompose={"index": 0, "quality": "low", "max_hulls": 16})
    ed.check_eq(got["status"], "succeeded", f"{got}")
    hulls = got["colliders"]
    ed.check(2 <= len(hulls) <= 16, f"2 to 16 convex hulls: {len(hulls)}")
    ed.check(all(h["shape"] == "convex_hull" and h.get("vertices", 0) >= 4 for h in hulls), f"{hulls}")
    ed.check(all(abs(h["center"][1] - 0.1) < 1e-6 and h["density"] == 500 for h in hulls), f"the old pose and density: {hulls}")
    ed.check_eq(got["undo_name"], "Decompose Collider (Ring)", f"{got}")

    await ed.call("undo", scene=sid)
    listed = await ed.call("edit_colliders", scene=sid, entity="Ring")
    ed.check_eq([c["shape"] for c in listed["colliders"]], ["mesh"], "one undo brings the Mesh collider back")
    await ed.call("redo", scene=sid)
    listed = await ed.call("edit_colliders", scene=sid, entity="Ring")
    ed.check_eq(len(listed["colliders"]), len(hulls), "redo")

    await ed.call("edit_colliders", scene=sid, entity="Ring", body_type="dynamic")
    await ed.call("save_scene", scene=sid)
    await ed.console_since(None)
    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=30)
        live = await ed.call("get_runtime_entity", entity="Ring", include_script_state=False)
        y = live["components"]["transform"]["position"][1]
        ed.check(y < 2.9, f"the decomposed ring is dynamic and falls: {live['components']['transform']}")
        errors = [e["message"] for e in await ed.console_since() if "static-only" in e["message"]]
        ed.check_eq(errors, [], "no static-only refusal")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_attach_to_parent_body(ed: Editor):
    sid = await scene3d(ed, "Compound")
    await ed.call("create_entity", scene=sid, name="Car", type="box", position=[0, 3, 0])
    await ed.call("edit_colliders", scene=sid, entity="Car", body_type="dynamic", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=sid, name="Wheel", type="sphere", parent="Car", position=[2, 0, 0])
    made = await ed.call("edit_colliders", scene=sid, entity="Car/Wheel", body_type="static",
                         add=[{"shape": "sphere", "radius": 0.5, "fit": False}], body={"attach_to_parent_body": True})
    ed.check(made["body_added"] is True, f"{made}")
    got = await ed.call("get_entity", scene=sid, entity="Car/Wheel", components=["rigidbody"], properties=["attachToParentBody"])
    ed.check(got["components"]["rigidbody"]["attachToParentBody"] is True, f"{got['components']}")
    await ed.call("save_scene", scene=sid)
    await ed.call("tool_groups", enable=["physics"])
    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        hit = await ed.call("query_physics", shape="ray", origin=[2, 10, 0], direction=[0, -1, 0], max_distance=20)
        first = hit["hits"][0]
        ed.check(first["name"].endswith("Wheel") and first["collider"] == 0 and first.get("body", "").endswith("Car"),
                 f"the child's collider, in its parent's body: {hit}")
        # the wheel moves with the car's body: both fall together
        await ed.call("wait", frames=30)
        car = (await ed.call("get_runtime_entity", entity="Car", include_script_state=False))["components"]["transform"]["position"]
        ed.check(car[1] < 2.95, f"the compound falls: {car}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
