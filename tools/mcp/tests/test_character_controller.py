"""S22 P2, the 3D CharacterController as an agent sees it (docs/specs/S22-gameplay.md AC16, R26, R40,
R42). The component is added like any other, never next to a Rigidbody: add_component and
edit_colliders refuse the pair. In Play, a Lua script walks it with simpleMove up a stairs fixture
(6 steps of 0.2 m risers and 0.3 m treads) to the top, and get_runtime_entity reports it grounded
there with BELOW.

Scene CC3D (3D). Play runs with auto_pause and a fixed 1/60 s step, so frames are exact.
"""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "CC3D CI"}

TIMEOUT = 900

WALKER = """---@class Walker3D : ScriptInstance
local Walker3D = { properties = {} }

function Walker3D:init()
    walkSpeed = 0
    hits = 0
    self.cc = CharacterController(self.scene, self.entity)
    RegisterEngineEvent(self, "onFixedUpdate")
end

function Walker3D:onFixedUpdate()
    grounded = self.cc:simpleMove(Vector3(walkSpeed, 0, 0))
end

function Walker3D:onControllerColliderHit(hit)
    hits = hits + 1
end

return Walker3D
"""

S: dict = {}


async def static_box(ed: Editor, scene, name: str, center, size) -> int:
    made = await ed.call("create_entity", scene=scene, type="empty", name=name, position=list(center))
    await ed.call("edit_colliders", scene=scene, entity=made["entity_id"], body_type="static",
                  add=[{"shape": "box", "size": list(size), "fit": False}])
    return made["entity_id"]


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="CC3D", type="3D", path="scenes/CC3D")
    S["scene"] = made["scene"]["id"]
    S["floor"] = await static_box(ed, S["scene"], "Floor", (0, -0.5, 0), (40, 1, 40))
    end = 6 * 0.3 + 10.0
    for k in range(1, 7):
        start = (k - 1) * 0.3
        height = 0.2 * k
        await static_box(ed, S["scene"], f"Step{k}", ((start + end) * 0.5, height * 0.5, 0), (end - start, height, 4))
    # feet 1 cm over the floor, 1 m before the first riser: the capsule is 2 m high, centred on the position
    player = await ed.call("create_entity", scene=S["scene"], type="empty", name="Player", position=[-1, 1.01, 0],
                           components=["CharacterController"])
    S["player"] = player["entity_id"]
    await ed.call("create_script", kind="lua", name="Walker3D", content=WALKER, scene=S["scene"], entity=S["player"])
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("save_project")
    return S


async def test_component_defaults(ed: Editor):
    """Added with Unity's defaults."""
    s = await setup(ed)
    got = await ed.call("get_entity", scene=s["scene"], entity=s["player"], components=["CharacterController"])
    values = got["components"]["charactercontroller"]
    ed.check_eq((values["height"], values["radius"], values["slopeLimit"]), (2, 0.5, 45), f"defaults: {values}")
    ed.check(abs(values["stepOffset"] - 0.3) < 1e-6 and abs(values["skinWidth"] - 0.02) < 1e-6, f"{values}")
    ed.check_eq((values["detectCollisions"], values["characterCollisions"], values["pushBodies"]), (True, True, True), f"{values}")


async def test_refuses_a_rigidbody_beside_it(ed: Editor):
    """A Character Controller and a Rigidbody never share a GameObject, whichever comes first."""
    s = await setup(ed)
    err = await ed.call("add_component", scene=s["scene"], entity=s["floor"], component="CharacterController",
                        expect_error=True)
    ed.check_in("cannot share", err)
    err = await ed.call("add_component", scene=s["scene"], entity=s["player"], component="Rigidbody", expect_error=True)
    ed.check_in("cannot share", err)
    err = await ed.call("edit_colliders", scene=s["scene"], entity=s["player"], body_type="dynamic",
                        add=[{"shape": "sphere", "radius": 0.5, "fit": False}], expect_error=True)
    ed.check_in("cannot share", err)
    got = await ed.call("get_entity", scene=s["scene"], entity=s["player"], detail="summary")
    ed.check("'rigidbody'" not in str(got).lower(), f"the player has no Rigidbody: {got}")


async def test_play_climbs_the_stairs(ed: Editor):
    """simpleMove at 2 m/s for 4 s from 1 m before the stairs: on the top step (1.2 m), grounded."""
    s = await setup(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=s["scene"], auto_pause=True, fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"start: {started}")
    try:
        await ed.call("wait", frames=20)
        live = await ed.call("get_runtime_entity", entity=s["player"], components=["CharacterController"])
        state = live["components"]["charactercontroller"]
        ed.check_eq(state["isGrounded"], True, f"standing on the floor: {state}")
        ed.check_eq(state["groundEntity"], s["floor"], f"on the floor: {state}")
        await ed.call("eval_lua", code="walkSpeed = 2")
        await ed.call("wait", frames=240)
        await ed.call("eval_lua", code="walkSpeed = 0")
        await ed.call("wait", frames=5)
        live = await ed.call("get_runtime_entity", entity=s["player"], components=["CharacterController", "Transform"])
        state = live["components"]["charactercontroller"]
        position = live["components"]["transform"]["position"]
        ed.check_eq(state["isGrounded"], True, f"grounded on the top: {state}")
        ed.check(state["collisionFlags"] & 4, f"BELOW: {state}")
        foot = position[1] - 1.0
        ed.check(abs(foot - 1.2) < 0.05, f"the feet on the top step (1.2 m): {foot} ({position})")
        ed.check(position[0] > 1.8, f"past the last riser: {position}")
        hits = (await ed.call("eval_lua", code="return hits"))["values"][0]
        ed.check(hits > 0, f"onControllerColliderHit ran: {hits}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
