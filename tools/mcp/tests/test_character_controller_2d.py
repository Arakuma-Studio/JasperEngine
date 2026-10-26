"""S22 P5, the 2D CharacterController as an agent sees it (docs/specs/S22-gameplay.md D2, R26, R40):
the component is added like any other, never next to a Rigidbody 2D (add_component, create_entity
and edit_colliders refuse the pair), and while playing get_runtime_entity shows what its last move
and the last physics step left (grounded on the floor, the walk's velocity).

Scene CC2D (2D) with a static floor and a Player whose Lua script walks it with simpleMove.
Play runs with auto_pause and a fixed 1/60 s step, so frames are exact.
"""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "CC2D CI"}

TIMEOUT = 900

WALKER = """---@class Walker2D : ScriptInstance
local Walker2D = { properties = {} }

function Walker2D:init()
    walkSpeed = 0
    self.cc = CharacterController2D(self.scene, self.entity)
    RegisterEngineEvent(self, "onFixedUpdate")
end

function Walker2D:onFixedUpdate()
    grounded = self.cc:simpleMove(Vector2(walkSpeed, 0))
    flags = self.cc.collisionFlags
end

return Walker2D
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="CC2D", type="2D", path="scenes/CC2D")
    S["scene"] = made["scene"]["id"]
    floor = await ed.call("create_entity", scene=S["scene"], type="empty", name="Floor", position=[0, -32, 0])
    S["floor"] = floor["entity_id"]
    await ed.call("edit_colliders", scene=S["scene"], entity=S["floor"], body_type="static",
                  add=[{"shape": "box", "size": [4000, 64]}])
    # feet 1 point over the floor's top (y = 0): the capsule is 128 high, centred on the position
    player = await ed.call("create_entity", scene=S["scene"], type="empty", name="Player", position=[0, 65, 0],
                           components=["CharacterController2D"])
    S["player"] = player["entity_id"]
    await ed.call("create_script", kind="lua", name="Walker2D", content=WALKER, scene=S["scene"], entity=S["player"])
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("save_project")
    return S


async def test_component_defaults(ed: Editor):
    """Added with the 3D defaults at 64 points a metre; listed among the addable components."""
    s = await setup(ed)
    got = await ed.call("get_entity", scene=s["scene"], entity=s["player"], components=["CharacterController2D"])
    values = got["components"]["charactercontroller2d"]
    ed.check_eq((values["height"], values["radius"], values["slopeLimit"]), (128, 32, 45), f"defaults: {values}")
    ed.check(abs(values["stepOffset"] - 19.2) < 1e-4 and abs(values["skinWidth"] - 1.28) < 1e-4, f"{values}")
    ed.check_eq((values["pushBodies"], values["movingPlatforms"], values["enabled"]), (True, True, True), f"{values}")
    types = await ed.call("list_component_types")
    ed.check("charactercontroller2d" in str(types).lower(), f"listed: {str(types)[:600]}")


async def test_refuses_a_rigidbody_beside_it(ed: Editor):
    """A Character Controller 2D and a Rigidbody 2D never share a GameObject, whichever comes first."""
    s = await setup(ed)
    err = await ed.call("add_component", scene=s["scene"], entity=s["player"], component="Rigidbody2D", expect_error=True)
    ed.check_in("cannot share", err)
    err = await ed.call("edit_colliders", scene=s["scene"], entity=s["player"], body_type="dynamic",
                        add=[{"shape": "circle", "radius": 10}], expect_error=True)
    ed.check_in("cannot share", err)
    err = await ed.call("add_component", scene=s["scene"], entity=s["floor"], component="CharacterController2D",
                        expect_error=True)
    ed.check_in("cannot share", err)
    other = await ed.call("create_entity", scene=s["scene"], type="empty", name="Both")
    err = await ed.call("add_component", scene=s["scene"], entity=other["entity_id"],
                        component=["CharacterController2D", "Rigidbody2D"], expect_error=True)
    ed.check_in("cannot share", err)
    got = await ed.call("get_entity", scene=s["scene"], entity=other["entity_id"], detail="summary")
    ed.check("charactercontroller2d" not in str(got) and "rigidbody2d" not in str(got), f"nothing added: {got}")
    await ed.call("delete_entities", scene=s["scene"], entities=[other["entity_id"]])
    got = await ed.call("get_entity", scene=s["scene"], entity=s["player"], detail="summary")
    ed.check("rigidbody2d" not in str(got), f"the player has no Rigidbody 2D: {got}")


async def test_play_walks_and_reports_runtime_state(ed: Editor):
    """simpleMove on the floor: grounded, BELOW, and 2 m/s (128 points/s) for a second is 128 points."""
    s = await setup(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=s["scene"], auto_pause=True, fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"start: {started}")
    try:
        await ed.call("wait", frames=20)
        live = await ed.call("get_runtime_entity", entity=s["player"], components=["CharacterController2D"])
        state = live["components"]["charactercontroller2d"]
        ed.check_eq(state["isGrounded"], True, f"standing on the floor: {state}")
        ed.check(state["collisionFlags"] & 4, f"BELOW: {state}")
        ed.check_eq(state["groundEntity"], s["floor"], f"on the floor: {state}")
        before = (await ed.call("get_entity", scene=s["scene"], entity=s["player"], runtime=True,
                                components=["Transform"]))["components"]["transform"]["position"]
        await ed.call("eval_lua", code="walkSpeed = 128")
        await ed.call("wait", frames=60)
        await ed.call("eval_lua", code="walkSpeed = 0")
        after = (await ed.call("get_entity", scene=s["scene"], entity=s["player"], runtime=True,
                               components=["Transform"]))["components"]["transform"]["position"]
        walked = after[0] - before[0]
        ed.check(abs(walked - 128) < 4, f"128 points in 60 steps: {walked} ({before} -> {after})")
        ed.check(abs(after[1] - before[1]) < 1, f"stayed on the floor: {before} -> {after}")
        grounded = (await ed.call("eval_lua", code="return grounded"))["values"][0]
        ed.check_eq(grounded, True, "simpleMove returns isGrounded")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
