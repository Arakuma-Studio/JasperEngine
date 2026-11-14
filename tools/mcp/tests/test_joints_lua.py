"""Joints made from Lua in Play (S96 REQ-25..REQ-28, AC-24, AC-25).

A script's `Joint2D(scene)` / `Joint3D(scene)` used to own a hidden root GameObject that went with
its Lua wrapper: a joint whose handle was dropped vanished at the next garbage collection. Once its
bodyA is set the joint now lives under bodyA, so it outlives the handle and goes with the body
(no "Cannot create joint" lines after); `go:addJoint2D` / `addJoint3D` make one there and
`getJoints2D` / `getJoints3D` list them.
"""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Joints Lua"}

# a dropped handle, collected: the joint must stay (red before REQ-25: it was destroyed)
MAKE_2D = """local j = Joint2D(scene)
j:setWheelJoint(entity('Frame'), entity('Wheel'), Vector2(0, 150), Vector2(0, 1))
jointId, owned = j.entity, j.entityOwned
j = nil
collectgarbage(); collectgarbage()
local parent = GameObject(scene, jointId).parent
return scene:isEntityCreated(jointId), owned, parent and parent.name or 'none', GameObject(scene, jointId).name"""

MAKE_3D = """local j = Joint3D(scene)
j.type = Joint3DType.POINT
j.bodyA = object('Frame')
j.connectedBody = object('Ball')
j.anchor = Vector3(0, -0.5, 0)
jointId, owned = j.entity, j.entityOwned
j = nil
collectgarbage(); collectgarbage()
local parent = GameObject(scene, jointId).parent
return scene:isEntityCreated(jointId), owned, parent and parent.name or 'none', GameObject(scene, jointId).name"""


async def joint_errors(ed: Editor, since) -> list:
    got = await ed.call("read_console", since=since, types=["error"], limit=200)
    return [e for e in got["entries"] if "Cannot create" in e.get("message", "")]


async def console_cursor(ed: Editor):
    return (await ed.call("read_console", limit=1))["next_cursor"]


async def test_lua_joint_lives_under_body(ed: Editor):
    sid = (await ed.call("create_scene", name="Joints2D", type="2d"))["scene"]["id"]
    frame = (await ed.call("create_entity", scene=sid, name="Frame", type="rigidbody2d", position=[0, 200, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=frame, body_type="static", add=[{"shape": "box", "size": [40, 20], "fit": False}])
    wheel = (await ed.call("create_entity", scene=sid, name="Wheel", type="rigidbody2d", position=[0, 150, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=wheel, body_type="dynamic", add=[{"shape": "circle", "radius": 10, "fit": False}])

    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        made = (await ed.call("eval_lua", scene=sid, code=MAKE_2D))["values"]
        ed.check(made[0] is True, f"the joint outlived its collected handle: {made}")
        ed.check(made[1] is False and made[2] == "Frame" and made[3] == "Joint 2D",
                 f"under its bodyA, no longer owned by the handle: {made}")
        await ed.call("wait", frames=60)
        state = (await ed.call("eval_lua", scene=sid,
                               code="local j = GameObject(scene, jointId):getJoint2D(); return j.reactionForce:length(), object('Wheel').position.y"))["values"]
        ed.check(state[0] > 0, f"the joint holds the wheel after 60 steps: {state}")
        ed.check(abs(state[1] - 150) < 64, f"the wheel hangs from the frame (a free wheel falls ~300 points): {state}")

        cursor = await console_cursor(ed)
        await ed.call("eval_lua", scene=sid, code="GameObject.destroy(object('Frame'))")
        await ed.call("wait", frames=120)
        gone = (await ed.call("eval_lua", scene=sid, code="return scene:isEntityCreated(jointId)"))["values"][0]
        ed.check(gone is False, "destroying bodyA took its joint")
        ed.check_eq(await joint_errors(ed, cursor), [], "no joint errors after its body went")
    finally:
        await ed.call("play", action="stop")


async def test_lua_joint3d_lives_under_body(ed: Editor):
    sid = (await ed.call("create_scene", name="Joints3D", type="3D"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="cube", name="Frame", position=[0, 5, 0], scale=[1, 0.2, 1])
    await ed.call("edit_colliders", scene=sid, entity="Frame", body_type="static", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=sid, type="sphere", name="Ball", position=[0, 4, 0], scale=[0.5, 0.5, 0.5])
    await ed.call("edit_colliders", scene=sid, entity="Ball", body_type="dynamic", add=[{"shape": "sphere"}])

    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        made = (await ed.call("eval_lua", scene=sid, code=MAKE_3D))["values"]
        ed.check(made[0] is True and made[1] is False and made[2] == "Frame" and made[3] == "Joint 3D",
                 f"the joint outlived its handle, under its bodyA: {made}")
        await ed.call("wait", frames=60)
        state = (await ed.call("eval_lua", scene=sid,
                               code="local j = GameObject(scene, jointId):getJoint3D(); return j.live, object('Ball').position.y"))["values"]
        ed.check(state[0] is True and state[1] > 3.5, f"the ball hangs from the frame (free: y < 0): {state}")

        cursor = await console_cursor(ed)
        await ed.call("eval_lua", scene=sid, code="GameObject.destroy(object('Frame'))")
        await ed.call("wait", frames=120)
        gone = (await ed.call("eval_lua", scene=sid, code="return scene:isEntityCreated(jointId)"))["values"][0]
        ed.check(gone is False, "destroying bodyA took its joint")
        ed.check_eq(await joint_errors(ed, cursor), [], "no joint errors after its body went")
    finally:
        await ed.call("play", action="stop")


async def test_add_joint_from_lua(ed: Editor):
    sid = (await ed.call("create_scene", name="AddJoint", type="2d"))["scene"]["id"]
    car = (await ed.call("create_entity", scene=sid, name="Car", type="rigidbody2d", position=[0, 200, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=car, body_type="static", add=[{"shape": "box", "size": [40, 20], "fit": False}])
    await ed.call("create_entity", scene=sid, name="Plain", type="empty")

    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        got = (await ed.call("eval_lua", scene=sid, code="""local car = object('Car')
local j = car:addJoint2D(Joint2DType.WHEEL)
local list = car:getJoints2D()
return j.gameObject.parent.name, j.bodyA.name, j.enableSpring, j.frequency, j.dampingRatio, #list, list[1].entity == j.entity, j.entityOwned"""))["values"]
        ed.check(got[0] == "Car" and got[1] == "Car", f"a child joint of the body, bodyA = it: {got}")
        ed.check(got[2] is True and got[3] == 2 and abs(got[4] - 0.7) < 1e-6, f"Unity's WheelJoint2D defaults (makeJoint2D): {got}")
        ed.check(got[5] == 1 and got[6] is True and got[7] is False, f"getJoints2D lists it: {got}")

        refused = (await ed.call("eval_lua", scene=sid,
                                 code="local ok, e = pcall(function() return object('Plain'):addJoint2D(Joint2DType.WHEEL) end); return ok, tostring(e)"))["values"]
        ed.check(refused[0] is False and "Rigidbody 2D" in refused[1] and "getRigidbody2D" in refused[1],
                 f"a GameObject without a body: an error naming the fix: {refused}")

        removed = (await ed.call("eval_lua", scene=sid, code="""local j = object('Car'):getJoints2D()[1]
local id = j.entity
j:destroy()
removedId = id
return #object('Car'):getJoints2D()"""))["values"]
        await ed.call("wait", frames=2)
        gone = (await ed.call("eval_lua", scene=sid, code="return scene:isEntityCreated(removedId), #object('Car'):getJoints2D()"))["values"]
        ed.check(gone[0] is False and gone[1] == 0, f"joint:destroy() removed its GameObject: {removed} {gone}")
    finally:
        await ed.call("play", action="stop")
