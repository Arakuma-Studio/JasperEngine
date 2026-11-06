"""2D physics authoring over MCP (physics-2d spec 6.5, 9): set_joint with motor / spring / limits /
break read back through get_entity, edit_colliders with a concave polygon of 40 points (no limit,
pieces reported), materials through edit_physics_material, and in Play get_runtime_entity's
joint read-outs and the car resting on its suspension. With JASPER_SLOW_BUILD=1 the same car runs
in an exported Linux Desktop player (compiles the engine) and rests as it does in Play."""

import asyncio
import math
import os
import re
import time

import yaml

from harness import Editor, skip

EDITOR = {"create": True, "name": "Physics 2D"}
CI_EXCLUDED = ("long-running (about 30 minutes on a loaded server: a test builds and runs a real Desktop player); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))
JOBS = 3   # the host is shared: never more

# on the front wheel joint: prints the car's pose once, after 150 fixed steps (2.5 s), in Play
# and in the exported player
CAR_PROBE = """local CarProbe = {}
function CarProbe:init()
    self.steps = 0
    RegisterEngineEvent(self, 'onFixedUpdate')
end
function CarProbe:onFixedUpdate()
    self.steps = self.steps + 1
    if self.steps == 150 then
        local joint = GameObject(self.scene, self.entity):getJoint2D()
        local car = joint.bodyA
        local body = car:getRigidbody2D()
        print(string.format('P2D_CAR y=%.3f translation=%.3f speed=%.4f mass=%.4f', car.position.y,
            joint.jointTranslation, body.linearVelocity:length(), body.mass))
    end
end
return CarProbe"""


async def scene2d(ed: Editor, name: str) -> int:
    return (await ed.call("create_scene", name=name, type="2d"))["scene"]["id"]


async def body(ed: Editor, sid: int, name: str, position, body_type: str, add: list, parent=None) -> int:
    args = {"scene": sid, "name": name, "type": "rigidbody2d", "position": position}
    if parent is not None:
        args["parent"] = parent
    entity = (await ed.call("create_entity", args))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=entity, body_type=body_type, add=add)
    return entity


def star(points: int, outer: float, inner: float):
    out = []
    for i in range(points):
        radius = outer if i % 2 == 0 else inner
        angle = 2 * math.pi * i / points
        out.append([round(radius * math.cos(angle), 3), round(radius * math.sin(angle), 3)])
    return out


async def test_concave_polygon_any_points(ed: Editor):
    sid = await scene2d(ed, "Concave")
    entity = (await ed.call("create_entity", scene=sid, name="Star", type="rigidbody2d"))["entity_id"]
    made = await ed.call("edit_colliders", scene=sid, entity=entity, body_type="static",
                         add=[{"shape": "polygon", "points": star(40, 100, 40)}])
    polygon = made["colliders"][0]
    ed.check_eq(len(polygon["points"]), 40, "40 points kept (no 16-point limit)")
    ed.check(polygon["pieces"] > 1, f"a concave star is split in convex pieces: {polygon}")
    ed.check(any("chain" in w for w in made.get("warnings", [])), f"a static many-point polygon suggests a chain: {made}")

    chain = await ed.call("edit_colliders", scene=sid, entity=entity, add=[{"shape": "chain", "points": [[-50, 0], [50, 0]]}])
    ed.check_eq(len(chain["colliders"]), 2, "a 2-point chain (an edge) is accepted")
    ed.check(chain["colliders"][1]["points"] == [[-50, 0], [50, 0]], f"{chain['colliders'][1]}")

    got = await ed.call("get_entity", scene=sid, entity=entity, components=["rigidbody2d"],
                        properties=["colliders[0].contactEvents", "colliders[0].points[39]"])
    props = got["components"]["rigidbody2d"]
    ed.check(props["colliders[0].contactEvents"] is True, f"new colliders report contacts: {props}")
    ed.check(abs(props["colliders[0].points[39]"][0] - star(40, 100, 40)[39][0]) < 1e-3, f"{props}")

    # the old name of the points still works
    await ed.call("set_properties", scene=sid, entity=entity, values={"rigidbody2d": {"colliders[1].vertices[1]": [60, 5]}})
    got = await ed.call("get_entity", scene=sid, entity=entity, components=["rigidbody2d"], properties=["colliders[1].points[1]"])
    ed.check(got["components"]["rigidbody2d"]["colliders[1].points[1]"] == [60, 5], f"{got}")

    await ed.check_error("edit_colliders", scene=sid, entity=entity, add=[{"shape": "polygon", "points": [[0, 0], [1, 1]]}],
                         contains="at least 3")


async def test_physics_material_tool(ed: Editor):
    made = await ed.call("edit_physics_material", path="assets/physics/Tire.physicsmaterial", create="2d", friction=2)
    ed.check(made["created"] is True, f"{made}")
    ed.check_eq(made["friction"], 2, "friction above 1 is fine (a tyre)")
    ed.check_eq(made["friction_combine"], "geometric_mean", "the 2D preset")
    edited = await ed.call("edit_physics_material", path="assets/physics/Tire.physicsmaterial", bounce_combine="minimum")
    ed.check(edited["created"] is False and edited["bounce_combine"] == "minimum", f"{edited}")
    text = (ed.project / "assets/physics/Tire.physicsmaterial").read_text()
    ed.check('"format": "jasperengine.physicsmaterial"' in text and '"friction": 2' in text, text)
    # oe-port's results name the undo step they made (S24: {id, name}); the undo below is the project's
    ed.check("Tire.physicsmaterial" in str(edited["undo"].get("name", "")), f"a project undo step: {edited['undo']}")
    await ed.call("undo", scope="project")
    ed.check('"bounceCombine": "maximum"' in (ed.project / "assets/physics/Tire.physicsmaterial").read_text(), "undo restores the file")
    await ed.check_error("edit_physics_material", path="assets/physics/Missing.physicsmaterial", friction=1, contains="create")
    await ed.check_error("edit_physics_material", path="assets/physics/Tire.physicsmaterial", bounciness=2, contains="0-1")


async def build_car(ed: Editor, sid: int) -> dict:
    """The hill-climb car (PlayerCar.prefab values): returns the wheel joints' ids by wheel name."""
    await ed.call("edit_physics_material", path="assets/physics/CarTire.physicsmaterial", create="2d", friction=2)
    await body(ed, sid, "Ground", [0, -40, 0], "static", [{"shape": "box", "size": [4000, 40], "fit": False}])
    # the chassis box sits above the wheels' bottoms; the wheels start 4 points above the ground (top -20)
    car = await body(ed, sid, "Car", [0, -14, 0], "dynamic", [{"shape": "box", "size": [200, 30], "center": [0, 45], "fit": False}])
    await ed.call("edit_colliders", scene=sid, entity=car, body={"useAutoMass": False, "mass": 1})
    wheels = {}
    for name, anchor in (("FrontWheel", [73.792, 19.84]), ("RearWheel", [-92.416, 19.84])):
        wheel = await body(ed, sid, name, [anchor[0], -14 + anchor[1], 0], "dynamic",
                           [{"shape": "circle", "radius": 21.76, "fit": False, "material": "assets/physics/CarTire.physicsmaterial"}])
        await ed.call("edit_colliders", scene=sid, entity=wheel, body={"useAutoMass": False, "mass": 1, "collisionDetection": "continuous"})
        joint = await ed.call("set_joint", scene=sid, name=name + "Joint", type="wheel", body_a="Car", body_b=name,
                              anchor_a=anchor, axis=[0, 1], spring={"frequency": 3, "damping": 0.35},
                              motor={"enabled": False, "max": 10000}, **{"break": {"force": None, "action": "destroy"}})
        ed.check(joint["created"] is True and joint["type"] == "wheel", f"{joint}")
        fields = joint["fields"]
        ed.check_eq(fields["anchorSpace"], "local", "new joints are local")
        ed.check(abs(fields["frequency"] - 3) < 1e-6 and abs(fields["dampingRatio"] - 0.35) < 1e-6, f"{fields}")
        ed.check(fields["enableSpring"] is True and fields["maxMotorTorque"] == 10000 and fields["useMotor"] is False, f"{fields}")
        wheels[name] = joint["joint"]["id"]
    return wheels


async def test_hill_climb_car_joints(ed: Editor):
    sid = await scene2d(ed, "HillCar")
    await build_car(ed, sid)

    # the joint lives under the car
    tree = await ed.call("get_entity", scene=sid, entity="Car/FrontWheelJoint", components=["joint2d"],
                         properties=["frequency", "bodyA", "anchorA", "breakAction"])
    props = tree["components"]["joint2d"]
    ed.check(abs(props["frequency"] - 3) < 1e-6, f"{props}")
    ed.check_eq(props["breakAction"], "destroy", f"{props}")

    # limits and break on another joint type, then the agent's "softer suspension"
    hinge = await ed.call("set_joint", scene=sid, name="Hinge", type="revolute", body_a="Car", body_b="FrontWheel",
                          limits=[-30, 30], **{"break": {"force": 500, "torque": 50, "action": "callback_only"}},
                          enable_collision=True)
    f = hinge["fields"]
    ed.check(f["useLimits"] is True and f["lowerAngle"] == -30 and f["upperAngle"] == 30, f"{f}")
    ed.check(f["breakForce"] == 500 and f["breakTorque"] == 50 and f["breakAction"] == "callback_only", f"{f}")
    ed.check(f["enableCollision"] is True, f"{f}")
    await ed.call("delete_entities", scene=sid, entity=hinge["joint"]["id"])
    softer = await ed.call("set_joint", scene=sid, joint="Car/RearWheelJoint", properties={"frequency": 2})
    ed.check_eq(softer["fields"]["frequency"], 2, "set_joint properties")
    await ed.call("set_joint", scene=sid, joint="Car/RearWheelJoint", properties={"frequency": 3})

    try:
        await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
        await ed.call("wait", frames=150)
        live = await ed.call("get_runtime_entity", entity="Car/FrontWheelJoint", include_script_state=False)
        readout = live["joint2d"]
        ed.check(readout["live"] is True, f"{readout}")
        ed.check(readout["joint_translation"] > 0.5, f"the car sank on its springs: {readout}")
        ed.check(len(readout["reaction_force"]) == 2, f"{readout}")
        car_live = await ed.call("get_runtime_entity", entity="Car", include_script_state=False)
        ed.check(abs(car_live["rigidbody2d"]["mass"] - 1) < 1e-4, f"explicit mass: {car_live['rigidbody2d']}")
        speed = math.hypot(*car_live["rigidbody2d"]["linear_velocity"])
        ed.check(speed < 1.0, f"at rest: {car_live['rigidbody2d']}")
        # "let the car drive itself": the motor, live
        await ed.call("set_runtime_property", entity="Car/RearWheelJoint", component="joint2d",
                      values={"useMotor": True, "motorSpeed": -720})
        x0 = (await ed.call("eval_lua", scene=sid, code="return object('Car').position.x"))["values"][0]
        await ed.call("wait", frames=60)
        x1 = (await ed.call("eval_lua", scene=sid, code="return object('Car').position.x"))["values"][0]
        ed.check(x1 - x0 > 20, f"the driven rear wheel moves the car forward: {x0} -> {x1}")
        lua = (await ed.call("eval_lua", scene=sid,
                             code="local j = object('Car/RearWheelJoint'):getJoint2D(); return j.useMotor, j.motorSpeed, j.jointSpeed < -100, j.type == Joint2DType.WHEEL"))["values"]
        ed.check(lua[0] is True and lua[1] == -720 and lua[2] is True and lua[3] is True, f"Lua Joint2D: {lua}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)



def rename_overrides(node, renames: dict) -> int:
    """Rewrites the Rigidbody 2D override names of every prefab instance entry of a scene file
    to the names of editors before the 2D caps went (numColliders, vertices[j])."""
    count = 0
    if isinstance(node, dict):
        props = node.get("properties")
        if "registryEntity" in node and isinstance(props, dict):
            for component, names in props.items():
                if "rigidbody2d" in str(component).lower().replace(" ", "") and isinstance(names, list):
                    for i, name in enumerate(names):
                        if name in renames:
                            names[i] = renames[name]
                            count += 1
        for value in node.values():
            count += rename_overrides(value, renames)
    elif isinstance(node, list):
        for value in node:
            count += rename_overrides(value, renames)
    return count


async def collider_list(ed: Editor, sid, entity) -> list:
    return (await ed.call("edit_colliders", scene=sid, entity=entity))["colliders"]


async def test_old_prefab_overrides_of_colliders(ed: Editor):
    """Overrides recorded before the caps went (numColliders, colliders[i].vertices[j]): an added
    collider, a removed one and a point edit survive a reload, and the point applies to the prefab."""
    sid = (await ed.call("create_scene", name="OldOverrides", type="2D", path="scenes/OldOverrides"))["scene"]["id"]
    crate = (await ed.call("create_entity", scene=sid, name="Crate", type="rigidbody2d"))["entity_id"]
    box = [[-10, -10], [10, -10], [10, 10], [-10, 10]]
    await ed.call("edit_colliders", scene=sid, entity=crate, body_type="dynamic",
                  add=[{"shape": "polygon", "points": box}, {"shape": "polygon", "points": [[x + 30, y] for x, y in box]}])
    path = "prefabs/Crate.prefab"
    await ed.call("create_prefab", scene=sid, entity=crate, path=path)
    for name, x in (("Added", 100), ("Removed", 200), ("Points", 300)):
        await ed.call("instantiate_prefab", scene=sid, prefab=path, name=name, position=[x, 0, 0])
    await ed.call("edit_colliders", scene=sid, entity="Added", add=[{"shape": "circle", "radius": 5, "fit": False}])
    await ed.call("edit_colliders", scene=sid, entity="Removed", remove=[1])
    await ed.call("set_properties", scene=sid, entity="Points", values={"rigidbody2d": {"colliders[0].points[1]": [60, 5]}})
    await ed.call("save_scene", scene=sid)

    file = ed.project / "scenes" / "OldOverrides.scene"
    data = yaml.safe_load(file.read_text())
    renamed = rename_overrides(data, {"colliders": "numColliders", "colliders[0].points[1]": "colliders[0].vertices[1]"})
    ed.check(renamed >= 3, f"the scene has the three overrides to make old: {renamed}")
    file.write_text(yaml.safe_dump(data, sort_keys=False))

    await ed.call("close_scene", scene=sid, unsaved="discard")
    sid = (await ed.call("open_scene", scene="scenes/OldOverrides.scene"))["scene"]["id"]
    added = await collider_list(ed, sid, "Added")
    ed.check_eq(len(added), 3, f"the added collider of an old override: {added}")
    ed.check_eq(added[2]["shape"], "circle", f"{added}")
    removed = await collider_list(ed, sid, "Removed")
    ed.check_eq(len(removed), 1, f"the removed collider stays removed: {removed}")
    edited = await collider_list(ed, sid, "Points")
    ed.check_eq(edited[0]["points"][1], [60, 5], f"the old point override: {edited}")
    overrides = (await ed.call("get_prefab_info", scene=sid, entity="Points", usage=False))["overrides"]
    ed.check("colliders[0].points[1]" in str(overrides), f"listed by its current name: {overrides}")

    await ed.call("prefab_overrides", scene=sid, entity="Points", action="apply", component="rigidbody2d",
                  properties=["colliders[0].points[1]"])
    source = await collider_list(ed, sid, "Crate")
    ed.check_eq(source[0]["points"][1], [60, 5], f"Apply to Prefab reaches the other instances: {source}")
    # the asset is written with the scene's save (its prefabs with changes): the point is in it
    await ed.call("save_scene", scene=sid)

    def has_point(node) -> bool:
        if isinstance(node, dict):
            points = node.get("points")
            if isinstance(points, list) and len(points) > 1 and [float(v) for v in points[1]] == [60.0, 5.0]:
                return True
            return any(has_point(value) for value in node.values())
        if isinstance(node, list):
            return any(has_point(value) for value in node)
        return False

    prefab = yaml.safe_load((ed.project / path).read_text())
    ed.check(has_point(prefab), f"the prefab file has the applied point: {(ed.project / path).read_text()[:3000]}")

def car_probe_line(lines) -> dict:
    for line in lines:
        m = re.search(r"P2D_CAR y=(-?[\d.]+) translation=(-?[\d.]+) speed=(-?[\d.]+) mass=(-?[\d.]+)", line)
        if m:
            return {k: float(v) for k, v in zip(("y", "translation", "speed", "mass"), m.groups())}
    return {}


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 15.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] not in ("running", "queued"):
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


async def test_exported_player_runs_the_car(ed: Editor):
    """T3 in Play and in an exported Linux Desktop player: the car rests the same on its suspension
    (the scene goes through the Factory's generated C++)."""
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    sid = await scene2d(ed, "ExportCar")
    await build_car(ed, sid)
    await ed.call("create_script", kind="lua", name="CarProbe", entity="Car/FrontWheelJoint", content=CAR_PROBE)
    await ed.call("save_project", untitled_scene_dir="scenes")

    try:
        await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
        await ed.call("wait", frames=200)
        console = await ed.call("read_console", contains="P2D_CAR", limit=5)
        play = car_probe_line([e["message"] for e in console["entries"]])
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    ed.check(play, f"the probe printed in Play: {console}")

    out = ed.scratch.root / "export-physics-2d"
    t0 = time.monotonic()
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), start_scene=sid, build_jobs=JOBS, timeout_seconds=30)
    job = r if r["status"] != "running" else await wait_job(ed, r["job_id"], TIMEOUT - 300)
    print(f"[test] desktop build: {job['status']} in {time.monotonic() - t0:.0f}s")
    ed.check_eq(job["status"], "succeeded", f"{job}")
    raw = await ed.call_raw("run_build", {"seconds": 8, "screenshot": False, "log_lines": 400})
    ed.check(not raw.is_error, raw.text)
    ran = raw.value
    ed.check(not ran["crashed"], f"the player crashed: {ran['fatal_lines']} {ran['log'][-20:]}")
    player = car_probe_line(ran["log"])
    ed.check(player, f"the probe printed in the player: {ran['log'][-40:]}")
    print(f"[test] Play {play} / player {player}")
    ed.check(abs(player["mass"] - 1) < 1e-3 and player["speed"] < 0.5, f"at rest, explicit mass: {player}")
    ed.check(player["translation"] > 1.0, f"the car sank on its springs: {player}")
    ed.check(abs(player["y"] - play["y"]) < 0.5 and abs(player["translation"] - play["translation"]) < 0.1,
             f"the player's car rests as in Play: {player} vs {play}")
