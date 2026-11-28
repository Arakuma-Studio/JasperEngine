"""S96 WP-A, vehicles as an agent drives them (docs/specs/S96-physics-gaps.md AC-9, REQ-42, REQ-45): four
Wheel Colliders are added under a dynamic body with add_component, their radius set with
set_properties, each step undone and redone; in Play a Lua script gives the rear wheels motor
torque, and get_runtime_entity reports the wheels grounded while the car's z grows. The
"3D Object > Vehicle..." menu (run_command) makes a whole rig in one undo step.

Scene Vehicles (3D). Play runs with auto_pause and a fixed 1/60 s step, so frames are exact.
"""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Vehicles CI"}

TIMEOUT = 900

DRIVER = """---@class Driver : ScriptInstance
local Driver = { properties = {} }

function Driver:init()
    torque = 0
    self.wheels = {}
    for _, name in ipairs({"WheelRL", "WheelRR"}) do
        table.insert(self.wheels, GameObject.find(name):getWheelCollider())
    end
    RegisterEngineEvent(self, "onFixedUpdate")
end

function Driver:onFixedUpdate()
    for _, wheel in ipairs(self.wheels) do
        wheel.motorTorque = torque
    end
end

return Driver
"""

S: dict = {}

CORNERS = {"WheelFL": (-0.8, -0.25, 1.3), "WheelFR": (0.8, -0.25, 1.3), "WheelRL": (-0.8, -0.25, -1.3), "WheelRR": (0.8, -0.25, -1.3)}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Vehicles", type="3D", path="scenes/Vehicles")
    S["scene"] = made["scene"]["id"]
    floor = await ed.call("create_entity", scene=S["scene"], type="empty", name="Floor", position=[0, -0.5, 0])
    await ed.call("edit_colliders", scene=S["scene"], entity=floor["entity_id"], body_type="static",
                  add=[{"shape": "box", "size": [200, 1, 200], "fit": False}])
    car = await ed.call("create_entity", scene=S["scene"], type="empty", name="Car", position=[0, 1, 0])
    S["car"] = car["entity_id"]
    await ed.call("edit_colliders", scene=S["scene"], entity=S["car"], body_type="dynamic",
                  add=[{"shape": "box", "size": [1.6, 0.5, 3.6], "fit": False}])
    S["wheels"] = {}
    for name, position in CORNERS.items():
        wheel = await ed.call("create_entity", scene=S["scene"], type="empty", name=name, parent=S["car"], position=list(position))
        S["wheels"][name] = wheel["entity_id"]
    return S


async def test_add_wheels_with_undo(ed: Editor):
    """add_component WheelCollider on each wheel and set_properties radius: one undo step each, redone."""
    s = await setup(ed)
    for name, wheel in s["wheels"].items():
        await ed.call("add_component", scene=s["scene"], entity=wheel, component="WheelCollider")
        got = await ed.call("get_entity", scene=s["scene"], entity=wheel, components=["WheelCollider"])
        values = got["components"]["wheelcollider"]
        ed.check(abs(values["radius"] - 0.5) < 1e-6 and abs(values["suspensionDistance"] - 0.3) < 1e-6, f"Unity's defaults: {values}")
        ed.check(abs(values["suspensionSpring.spring"] - 35000) < 1e-3, f"spring: {values}")
        await ed.call("undo", scene=s["scene"])
        got = await ed.call("get_entity", scene=s["scene"], entity=wheel, detail="summary")
        ed.check("wheelcollider" not in str(got).lower(), f"{name}: undone: {got}")
        await ed.call("redo", scene=s["scene"])

        await ed.call("set_properties", scene=s["scene"], entity=wheel, values={"WheelCollider": {"radius": 0.35}})
        await ed.call("undo", scene=s["scene"])
        got = await ed.call("get_entity", scene=s["scene"], entity=wheel, components=["WheelCollider"])
        ed.check(abs(got["components"]["wheelcollider"]["radius"] - 0.5) < 1e-6, f"{name}: radius undone: {got}")
        await ed.call("redo", scene=s["scene"])
        got = await ed.call("get_entity", scene=s["scene"], entity=wheel, components=["WheelCollider"])
        ed.check(abs(got["components"]["wheelcollider"]["radius"] - 0.35) < 1e-6, f"{name}: radius redone: {got}")
    # a nested struct member by its path
    await ed.call("set_properties", scene=s["scene"], entity=s["wheels"]["WheelFL"],
                  values={"WheelCollider": {"sidewaysFriction.stiffness": 1.2}})
    got = await ed.call("get_entity", scene=s["scene"], entity=s["wheels"]["WheelFL"], components=["WheelCollider"])
    ed.check(abs(got["components"]["wheelcollider"]["sidewaysFriction.stiffness"] - 1.2) < 1e-6, f"{got}")
    await ed.call("create_script", kind="lua", name="Driver", content=DRIVER, scene=s["scene"], entity=s["car"])
    await ed.call("save_scene", scene=s["scene"])
    await ed.call("save_project")
    s["car_ready"] = True


async def test_play_drives_the_car(ed: Editor):
    """In Play the wheels are grounded; rear motor torque from Lua moves the car along +z."""
    s = await setup(ed)
    if "car_ready" not in s:
        await test_add_wheels_with_undo(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=s["scene"], auto_pause=True, fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"start: {started}")
    try:
        await ed.call("wait", frames=60)
        live = await ed.call("get_runtime_entity", entity=s["wheels"]["WheelRL"], components=["WheelCollider"])
        state = live["components"]["wheelcollider"]
        ed.check_eq(state["isGrounded"], True, f"resting on the floor: {state}")
        ed.check(state["sprungMass"] > 0, f"holds part of the body: {state}")
        before = (await ed.call("get_runtime_entity", entity=s["car"], components=["Transform"]))["components"]["transform"]["position"]
        await ed.call("eval_lua", code="torque = 600")
        await ed.call("wait", frames=120)
        after = (await ed.call("get_runtime_entity", entity=s["car"], components=["Transform"]))["components"]["transform"]["position"]
        ed.check(after[2] - before[2] > 1.0, f"the car drove along +z: {before} -> {after}")
        live = await ed.call("get_runtime_entity", entity=s["wheels"]["WheelRL"], components=["WheelCollider"])
        state = live["components"]["wheelcollider"]
        ed.check(state["rpm"] > 10, f"the rear wheel spins: {state}")
        ed.check(abs(state["motorTorque"] - 600) < 1e-3, f"the script's torque: {state}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_vehicle_menu_one_undo_step(ed: Editor):
    """GameObject > 3D Object > Vehicle... on a selected cube: a dynamic body and four Wheel Colliders; one undo removes them."""
    s = await setup(ed)
    cube = await ed.call("create_entity", scene=s["scene"], type="cube", name="Kart", position=[10, 1, 0], scale=[2, 1, 4])
    kart = cube["entity_id"]
    await ed.call("select_entities", scene=s["scene"], entities=[kart])
    await ed.call("run_command", id="gameobject.create.3DObject.vehicle")
    wheels = await ed.call("find_entities", scene=s["scene"], under=kart, component="WheelCollider")
    ids = [e["id"] for e in wheels["entities"]]
    ed.check_eq(len(ids), 4, f"four wheels under the kart: {wheels}")
    got = await ed.call("get_entity", scene=s["scene"], entity=kart, components=["Rigidbody"])
    body = got["components"]["rigidbody"]
    ed.check_eq(body["type"], "dynamic", f"a dynamic body: {body}")
    await ed.call("undo", scene=s["scene"])
    wheels = await ed.call("find_entities", scene=s["scene"], under=kart, component="WheelCollider")
    ed.check_eq(len(wheels["entities"]), 0, f"one undo step removes the wheels: {wheels}")
    got = await ed.call("get_entity", scene=s["scene"], entity=kart, detail="summary")
    ed.check("rigidbody" not in str(got).lower(), f"and the body: {got}")
