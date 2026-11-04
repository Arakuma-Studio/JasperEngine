"""3D physics authoring over MCP (physics-3d spec 6.5, 9): edit_physics_material + edit_colliders
with a per-collider material and trigger read back through get_entity, the Rigidbody fields through
the body argument, set_joint 3D (a hinge with limits and a motor, local anchors, no body_b),
create_ragdoll on the pose-animation fixture's Mixamo hero (11 parts, one undo step), the world
capacity project setting, and in Play the joint and body read-outs and the physics debug draw
(configure_game_view physics_debug: lines drawn, a screenshot)."""

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
CI_EXCLUDED = ("long-running (about 3 minutes on a loaded server: joints simulated in Play on the pose-animation rig); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = 900


async def scene3d(ed: Editor, name: str) -> int:
    return (await ed.call("create_scene", name=name, type="3d"))["scene"]["id"]


async def test_material_trigger_and_body_fields(ed: Editor):
    made = await ed.call("edit_physics_material", path="assets/physics/Ice.physicsmaterial", create="3d",
                         friction=0, friction_combine="minimum")
    ed.check(made, f"{made}")
    sid = await scene3d(ed, "Materials3D")
    box = (await ed.call("create_entity", scene=sid, name="Crate", type="box", position=[0, 2, 0]))["entity_id"]
    edited = await ed.call("edit_colliders", scene=sid, entity=box, body_type="dynamic",
                           body={"linear_damping": 0.5, "mass": 3, "interpolation": "extrapolate"},
                           add=[{"shape": "box", "material": "assets/physics/Ice.physicsmaterial"},
                                {"shape": "sphere", "radius": 1.5, "is_trigger": True}])
    colliders = edited["colliders"]
    ed.check_eq(len(colliders), 2, f"{edited}")
    ed.check("Ice.physicsmaterial" in (colliders[0].get("material") or ""), f"the collider's material: {colliders[0]}")
    ed.check(colliders[1].get("is_trigger") is True, f"a trigger collider: {colliders[1]}")

    got = await ed.call("get_entity", scene=sid, entity=box, components=["rigidbody"],
                        properties=["linearDamping", "automaticMass", "mass", "interpolation",
                                    "colliders[0].material", "colliders[1].isTrigger"])
    props = got["components"]["rigidbody"]
    ed.check(abs(props["linearDamping"] - 0.5) < 1e-6 and props["automaticMass"] is False and abs(props["mass"] - 3) < 1e-6,
             f"body fields: {props}")
    ed.check_eq(props["interpolation"], "extrapolate", f"{props}")
    ed.check(props["colliders[1].isTrigger"] is True, f"{props}")

    # raw vertices: a convex hull of any number of points, no cap
    hull = await ed.call("edit_colliders", scene=sid, entity=box, clear=True,
                         add=[{"shape": "convex_hull", "vertices": [[x * 0.01, (x % 7) * 0.1, (x % 5) * 0.1] for x in range(300)]}])
    ed.check_eq(hull["colliders"][0].get("vertices"), 300, f"{hull['colliders'][0]}")


async def test_hinge_limits_motor_and_play_readouts(ed: Editor):
    sid = await scene3d(ed, "Door3D")
    await ed.call("create_entity", scene=sid, name="Floor", type="box", position=[0, -0.5, 0], scale=[20, 1, 20])
    await ed.call("edit_colliders", scene=sid, entity="Floor", body_type="static", add=[{"shape": "box"}])
    door = (await ed.call("create_entity", scene=sid, name="Door", type="box", position=[0.5, 1.2, 0], scale=[1, 2, 0.1]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=door, body_type="dynamic", add=[{"shape": "box"}])
    hinge = await ed.call("set_joint", scene=sid, name="DoorHinge", type="hinge", body_a="Door", dimension="3d",
                          anchor=[-0.5, 0, 0], axis=[0, 1, 0], limits=[0, 90],
                          motor={"enabled": True, "speed": 45, "max": 50},
                          **{"break": {"force": None, "torque": None}})
    f = hinge["fields"]
    ed.check_eq(f["anchorSpace"], "local", f"{f}")
    ed.check(f["useLimits"] is True and f["limitsMin"] == 0 and f["limitsMax"] == 90, f"{f}")
    ed.check(f["useMotor"] is True and f["motorTargetVelocity"] == 45 and f["motorMaxForce"] == 50, f"{f}")
    ed.check(f["enableCollision"] is False, f"Unity's default: {f}")
    await ed.check_error("set_joint", scene=sid, joint="Door/DoorHinge", cone_half_angle=30, contains="cone")

    await ed.call("set_project_settings", values={"physics.max_bodies": 4096})
    got = await ed.call("get_project_settings", keys=["physics.max_bodies", "physics.max_body_pairs"])
    ed.check("4096" in str(got) and "8192" in str(got), f"the world capacity setting: {got}")
    await ed.call("set_project_settings", values={"physics.max_bodies": 8192})

    await ed.call("configure_game_view", physics_debug=True)
    try:
        await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
        await ed.call("wait", frames=240)
        live = await ed.call("get_runtime_entity", entity="Door/DoorHinge", include_script_state=False)
        readout = live["joint3d"]
        ed.check(readout["live"] is True, f"{readout}")
        ed.check(80 <= readout["angle"] <= 91, f"the motor opened the door to its 90 degree limit: {readout}")
        body = (await ed.call("get_runtime_entity", entity="Door", include_script_state=False))["rigidbody"]
        ed.check(body["type"] == "dynamic" and body["mass"] > 0, f"{body}")
        view = await ed.call("configure_game_view")
        print(f"[test] physics debug lines: {view['physics_debug_lines']}")
        ed.check(view["physics_debug"] is True and view["physics_debug_lines"] > 0, f"collider lines drawn: {view}")
        shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 480})
        ed.check(not shot.is_error and shot.images, shot.text)
        lua = (await ed.call("eval_lua", scene=sid,
                             code="local j = object('Door/DoorHinge'):getJoint3D(); return j.useMotor, j.connectedBody == nil, Physics.debugDraw"))["values"]
        ed.check(lua[0] is True and lua[1] is True and lua[2] is True, f"Lua Joint3D: {lua}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
        await ed.call("configure_game_view", physics_debug=False)


async def test_create_ragdoll_on_the_hero(ed: Editor):
    await ed.call("open_scene", scene="Main Scene.scene")
    made = await ed.call("create_ragdoll", entity="Hero", total_mass=30)
    parts = made["parts"]
    ed.check_eq(len(parts), 11, f"{made}")
    ed.check_eq(made["start"], "simulated", f"no Animation Player on the hero: starts simulated: {made}")
    ed.check(abs(sum(p["mass"] for p in parts) - 30) < 1e-3, f"masses sum to the total: {parts}")
    joints = [p for p in parts if p["joint"]]
    ed.check_eq(len(joints), 10, f"{parts}")
    hips = next(p for p in parts if p["bone"] == "left_hips")
    ed.check(hips["joint"]["twist"] == [-20, 70] and hips["joint"]["swing1"] == 30, f"Unity's table: {hips}")

    got = await ed.call("get_entity", entity="Hero", components=["ragdoll"], properties=["totalMass", "simulated"])
    ed.check(abs(got["components"]["ragdoll"]["totalMass"] - 30) < 1e-6, f"{got}")
    thigh = await ed.call("get_entity", entity="mixamorig:LeftUpLeg", components=["rigidbody", "joint3d"])
    ed.check("rigidbody" in thigh["components"] and "joint3d" in thigh["components"], f"{thigh}")

    # one undo step takes the whole ragdoll away, redo brings it back
    await ed.call("undo")
    gone = await ed.call("get_entity", entity="mixamorig:LeftUpLeg")
    ed.check("rigidbody" not in gone.get("components", {}), f"undone: {gone.get('components')}")
    await ed.call("redo")
    back = await ed.call("get_entity", entity="mixamorig:LeftUpLeg", components=["rigidbody"])
    ed.check("rigidbody" in back["components"], f"redone: {back}")

    # again: rebuilt, not doubled
    again = await ed.call("create_ragdoll", entity="Hero", start="animated")
    ed.check_eq(again["start"], "animated", f"{again}")
    ed.check_eq(len(again["parts"]), 11, f"{again}")
    await ed.check_error("create_ragdoll", entity="Hero", bones={"pelvis": "NoSuchBone"}, contains="NoSuchBone")
