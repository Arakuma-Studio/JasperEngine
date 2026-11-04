"""Collision and trigger callbacks over MCP (physics-contacts spec 9): a Lua script defining
onCollisionEnter on a ball dropped on a floor prints once in Play; get_runtime_entity lists the
contact and the callbacks found; a 2D sensor's onTriggerEnter2D; edit_colliders takes and lists a
collider's receiver."""

from harness import Editor

EDITOR = {"create": True, "name": "Physics Contacts"}
CI = True

BALL = """local Ball = {}
function Ball:onCollisionEnter(c)
    print(string.format('contact enter %s ny=%.2f', c.gameObject.name, c:getContact(0).normal.y))
end
function Ball:onCollisionExit(c) print('contact exit ' .. c.gameObject.name) end
return Ball
"""

COIN = """local Coin = {}
function Coin:onTriggerEnter2D(other) print('coin trigger ' .. other.gameObject.name .. ' ' .. other.index .. ' ' .. tostring(other.isTrigger)) end
return Coin
"""


def lines(entries: list[dict], prefix: str) -> list[str]:
    return [e["message"] for e in entries if e["message"].startswith(prefix)]


async def test_ball_on_floor_calls_on_collision_enter(ed: Editor):
    sid = (await ed.call("create_scene", name="Contacts3D", type="3d"))["scene"]["id"]
    floor = (await ed.call("create_entity", scene=sid, name="Floor", type="cube", position=[0, -0.5, 0], scale=[20, 1, 20]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=floor, body_type="static", add=[{"shape": "box"}])
    ball = (await ed.call("create_entity", scene=sid, name="Ball", type="cube", position=[0, 2, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=ball, body_type="dynamic", add=[{"shape": "sphere", "radius": 0.5, "fit": False}])
    await ed.call("create_script", kind="lua", name="Ball", content=BALL, entity=ball)
    await ed.console_since(None)
    await ed.call("play", action="start", scene=sid, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=120)
        entered = lines(await ed.console_since(), "contact ")
        ed.check_eq(entered, ["contact enter Floor ny=1.00"], "one Enter, normal up, no Exit")
        live = await ed.call("get_runtime_entity", entity="Ball", include_script_state=False)
        physics = live.get("physics", {})
        ed.check(any(c["other_path"].endswith("Floor") and not c["trigger"] and c["since_step"] for c in physics.get("contacts", [])),
                 f"the floor contact is listed: {physics}")
        ed.check_eq(physics.get("callbacks"), ["onCollisionEnter (Ball)", "onCollisionExit (Ball)"])
        floor_live = await ed.call("get_runtime_entity", entity="Floor", include_script_state=False)
        ed.check(any(c["other_path"].endswith("Ball") for c in floor_live.get("physics", {}).get("contacts", [])),
                 f"the floor sees the ball: {floor_live.get('physics')}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    ed.check_eq(lines(await ed.console_since(), "contact "), [], "Play stop sends no Exit")
    await ed.check_alive()


async def test_sensor_trigger_2d_and_receiver(ed: Editor):
    sid = (await ed.call("create_scene", name="Contacts2D", type="2d"))["scene"]["id"]
    coin = (await ed.call("create_entity", scene=sid, name="Coin", type="rigidbody2d", position=[0, 100, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=coin, body_type="static", add=[{"shape": "circle", "radius": 20, "sensor": True}])
    await ed.call("create_script", kind="lua", name="Coin", content=COIN, entity=coin)
    detector = (await ed.call("create_entity", scene=sid, name="Detector", type="empty"))["entity_id"]
    car = (await ed.call("create_entity", scene=sid, name="Car", type="rigidbody2d", position=[0, 300, 0]))["entity_id"]
    made = await ed.call("edit_colliders", scene=sid, entity=car, body_type="dynamic",
                         add=[{"shape": "box", "size": [60, 30], "fit": False},
                              {"shape": "box", "size": [20, 20], "center": [0, 25], "fit": False, "receiver": "Detector"}])
    ed.check("receiver" not in made["colliders"][0], f"no receiver on collider 0: {made['colliders'][0]}")
    ed.check_eq(made["colliders"][1].get("receiver", {}).get("id"), detector, f"collider 1 sends to the Detector: {made['colliders'][1]}")
    got = await ed.call("get_entity", scene=sid, entity=car, components=["rigidbody2d"], properties=["colliders[1].receiver"])
    ed.check(got["components"]["rigidbody2d"]["colliders[1].receiver"] not in (None, 0), f"the Catalog path reads it: {got['components']}")
    cleared = await ed.call("edit_colliders", scene=sid, entity=car, update=[{"index": 1, "receiver": None}])
    ed.check("receiver" not in cleared["colliders"][1], f"null clears it: {cleared['colliders'][1]}")
    bad = await ed.call("edit_colliders", scene=sid, entity=car, update=[{"index": 1, "receiver": "Nobody"}], expect_error=True)
    ed.check("receiver" in bad, f"an unknown receiver is an error naming the field: {bad}")

    await ed.console_since(None)
    await ed.call("play", action="start", scene=sid, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=90)
        ed.check_eq(sorted(lines(await ed.console_since(), "coin trigger")), ["coin trigger Car 0 false", "coin trigger Car 1 false"],
                    "the coin's onTriggerEnter2D once per car collider (one callback per collider pair)")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    await ed.check_alive()


async def test_3d_receiver_roundtrip(ed: Editor):
    sid = (await ed.call("create_scene", name="Receiver3D", type="3d"))["scene"]["id"]
    target = (await ed.call("create_entity", scene=sid, name="Brain", type="empty"))["entity_id"]
    body = (await ed.call("create_entity", scene=sid, name="Hull", type="cube"))["entity_id"]
    made = await ed.call("edit_colliders", scene=sid, entity=body, body_type="dynamic", add=[{"shape": "box", "receiver": target}])
    ed.check_eq(made["colliders"][0].get("receiver", {}).get("id"), target, f"{made['colliders'][0]}")
    got = await ed.call("get_entity", scene=sid, entity=body, components=["rigidbody"], properties=["colliders[0].receiver"])
    ed.check(got["components"]["rigidbody"]["colliders[0].receiver"] not in (None, 0), f"{got['components']}")
