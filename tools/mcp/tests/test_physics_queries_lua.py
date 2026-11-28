"""S96 REQ-10..REQ-13, AC-17 (docs/specs/S96-physics-gaps.md): every Lua shape query of Physics and
Physics2D, Rigidbody:sweepTest and Rigidbody2D:cast, called in Play with and without an options
table (ignore by GameObject, Rigidbody and entity; triggers; hitFromInside; startInColliders), the
RaycastHit fields (collider, gameObject, rigidbody, transform), the statics, the enums, and the
errors that name a bad argument. tools/lua_api/tests/test_lua_api.py checks the stubs."""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Physics Queries Lua"}
TIMEOUT = 600


async def lua(ed: Editor, code: str):
    values = (await ed.call("eval_lua", code=code))["values"]
    return values[0] if len(values) == 1 else values


async def test_lua_queries_3d(ed: Editor):
    sid = (await ed.call("create_scene", name="Lua3D", type="3d", path="scenes/Lua3D"))["scene"]["id"]
    for name, position in (("Player", [0, 0.5, 0]), ("Crate", [3, 0.5, 0]), ("Zone", [-4, 0.5, 0])):
        await ed.call("create_entity", scene=sid, name=name, type="box", position=position)
    await ed.call("edit_colliders", scene=sid, entity="Player", body_type="kinematic", add=[{"shape": "box"}])
    await ed.call("edit_colliders", scene=sid, entity="Crate", body_type="static", add=[{"shape": "box"}])
    await ed.call("edit_colliders", scene=sid, entity="Zone", body_type="static", add=[{"shape": "box", "is_trigger": True}])
    await ed.call("save_scene", scene=sid)
    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        got = await lua(ed, """
            local P, X, D = Vector3(0, 0.5, 0), Vector3(1, 0, 0), Vector3(-1, 0, 0)
            local player, crate = object("Player"), object("Crate")
            local out = {}
            local h = Physics.sphereCast(scene, Vector3(-1.5, 0.5, 0), 0.25, X, 10)
            out.sphere = h.gameObject.name; out.sphereDist = h.distance
            out.fields = (h.collider.index == 0) and (h.rigidbody ~= nil) and (h.transform.name == "Player") and (h.hit == true)
            out.sphereAll = #Physics.sphereCastAll(scene, Vector3(-1.5, 0.5, 0), 0.25, X, 10)
            out.box = Physics.boxCast(scene, Vector3(-1.5, 0.5, 0), Vector3(0.2, 0.2, 0.2), X, nil, 10).gameObject.name
            out.boxAll = #Physics.boxCastAll(scene, Vector3(-1.5, 0.5, 0), Vector3(0.2, 0.2, 0.2), X, Quaternion(0, 45, 0), 10)
            out.capsule = Physics.capsuleCast(scene, Vector3(-1.5, 0.2, 0), Vector3(-1.5, 0.8, 0), 0.2, X, 10).gameObject.name
            out.capsuleAll = #Physics.capsuleCastAll(scene, Vector3(-1.5, 0.2, 0), Vector3(-1.5, 0.8, 0), 0.2, X, 10)
            out.miss = Physics.sphereCast(scene, Vector3(0, 5, 0), 0.25, Vector3(0, 1, 0), 10) == nil
            -- ignore by GameObject, Rigidbody and entity; trigger rules
            out.ignoreGo = Physics.sphereCast(scene, Vector3(-1.5, 0.5, 0), 0.25, X, 10, Physics.DEFAULT_RAYCAST_LAYERS, {ignore = {player}}).gameObject.name
            out.ignoreBody = Physics.sphereCast(scene, Vector3(-1.5, 0.5, 0), 0.25, X, 10, nil, {ignore = {player:getRigidbody()}}).gameObject.name
            out.ignoreId = Physics.raycast(scene, Vector3(-1.5, 0.5, 0), X, 10, nil, {ignore = {entity("Player")}}).gameObject.name
            out.zone = Physics.sphereCast(scene, P, 0.25, D, 10).gameObject.name
            out.zoneIgnored = Physics.sphereCast(scene, P, 0.25, D, 10, nil, {triggers = QueryTriggerInteraction.IGNORE}) == nil
            Physics.queriesHitTriggers = false
            out.global = Physics.raycast(scene, P, D, 10).hit
            out.collide = Physics.raycast(scene, P, D, 10, nil, {triggers = QueryTriggerInteraction.COLLIDE}).gameObject.name
            Physics.queriesHitTriggers = true
            -- from inside the Player: Unity's rule, then the old result
            out.inside = Physics.raycast(scene, P, X, 10).gameObject.name
            out.insideOld = Physics.raycast(scene, P, X, 10, nil, {hitFromInside = true}).distance
            out.insideRay = Physics.raycast(scene, Ray(P, X * 10)).gameObject.name
            out.intersects = scene:getEntityName(Ray(P, X * 10):intersects(scene, RayFilter.BODY_3D).body)
            -- overlaps, checks, closestPoint, sweepTest
            out.overlap = #Physics.overlapSphere(scene, Vector3(1.5, 0.5, 0), 1.6)
            out.overlapName = Physics.overlapBox(scene, Vector3(3, 0.5, 0), Vector3(0.1, 0.1, 0.1))[1].gameObject.name
            out.overlapCapsule = #Physics.overlapCapsule(scene, Vector3(-5, 0.5, 0), Vector3(4, 0.5, 0), 0.1, nil, {triggers = QueryTriggerInteraction.IGNORE})
            out.check = Physics.checkSphere(scene, Vector3(3, 0.5, 0), 0.1)
            out.checkBox = Physics.checkBox(scene, Vector3(10, 10, 10), Vector3(1, 1, 1), Quaternion(0, 0, 0))
            out.checkCapsule = Physics.checkCapsule(scene, Vector3(0, 0, 0), Vector3(0, 1, 0), 0.1)
            out.closest = Physics.closestPoint(scene, Vector3(10, 0.5, 0), Physics.overlapSphere(scene, Vector3(3, 0.5, 0), 0.1)[1]).x
            local body = player:getRigidbody()
            out.sweep = body:sweepTest(X, 10).gameObject.name
            out.sweepDist = body:sweepTest(X, 10).distance
            out.sweepAll = #body:sweepTestAll(D, 10)
            out.sweepNone = body:sweepTest(D, 10, {triggers = QueryTriggerInteraction.IGNORE}) == nil
            out.statics = Physics.queriesHitTriggers == true and Physics.queriesHitBackfaces == false
            return out
        """)
        print(f"[test] 3D Lua queries: {got}")
        ed.check(got["sphere"] == "Player" and abs(got["sphereDist"] - 0.75) < 1e-3, f"{got}")
        ed.check(got["fields"] is True, f"RaycastHit fields: {got}")
        ed.check(got["sphereAll"] == 2 and got["boxAll"] == 2 and got["capsuleAll"] == 2, f"...All: {got}")
        ed.check(got["box"] == "Player" and got["capsule"] == "Player" and got["miss"] is True, f"{got}")
        ed.check(got["ignoreGo"] == got["ignoreBody"] == got["ignoreId"] == "Crate", f"ignore: {got}")
        ed.check(got["zone"] == "Zone" and got["zoneIgnored"] is True, f"triggers: {got}")
        ed.check(got["global"] is False and got["collide"] == "Zone", f"queriesHitTriggers: {got}")
        ed.check(got["inside"] == "Crate" and abs(got["insideOld"]) < 1e-3 and got["insideRay"] == "Crate", f"start inside: {got}")
        ed.check(got["intersects"] == "Player", f"Ray:intersects keeps its rule: {got}")
        ed.check(got["overlap"] == 2 and got["overlapName"] == "Crate" and got["overlapCapsule"] == 2, f"overlaps: {got}")
        ed.check(got["check"] is True and got["checkBox"] is False and got["checkCapsule"] is True, f"checks: {got}")
        ed.check(abs(got["closest"] - 3.5) < 1e-3, f"closestPoint: {got}")
        ed.check(got["sweep"] == "Crate" and abs(got["sweepDist"] - 2.0) < 1e-3, f"sweepTest: {got}")
        ed.check(got["sweepAll"] == 1 and got["sweepNone"] is True and got["statics"] is True, f"{got}")

        # errors name the argument
        for code, words in (
                ("Physics.sphereCast(scene, Vector3(0, 0, 0), -1, Vector3(1, 0, 0), 10)", "radius"),
                ("Physics.sphereCast(scene, Vector3(0, 0, 0), 1, Vector3(0, 0, 0), 10)", "direction"),
                ("Physics.boxCast(scene, Vector3(0, 0, 0), Vector3(1, 0, 1), Vector3(1, 0, 0), nil, 10)", "halfExtents"),
                ("Physics.overlapSphere(scene, Vector3(0, 0, 0), 0 / 0)", "radius"),
                ("Physics.raycast(scene, Vector3(0, 0, 0), Vector3(1, 0, 0), 10, nil, {ignore = {'x'}})", "options.ignore"),
                ("Physics.sphereCast(scene, Vector3(0, 0, 0), 1, Vector3(1, 0, 0), 10, nil, {triggers = 7})", "options.triggers")):
            result = await ed.call_raw("eval_lua", {"code": code})
            ed.check(result.is_error and words in result.text, f"{code}: {result.text[:300]}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_lua_queries_2d(ed: Editor):
    sid = (await ed.call("create_scene", name="Lua2D", type="2d", path="scenes/Lua2D"))["scene"]["id"]
    ground = (await ed.call("create_entity", scene=sid, name="Ground", type="rigidbody2d", position=[0, -32, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=ground, body_type="static", add=[{"shape": "box", "size": [640, 64], "fit": False}])
    crate = (await ed.call("create_entity", scene=sid, name="Crate", type="rigidbody2d", position=[200, 64, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=crate, body_type="kinematic", add=[{"shape": "box", "size": [64, 64], "fit": False}])
    await ed.call("save_scene", scene=sid)
    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        got = await lua(ed, """
            local down = Vector2(0, -1)
            local out = {}
            local h = Physics2D.circleCast(scene, Vector2(200, 300), 16, down, 1000)
            out.circle = h.gameObject.name; out.circleDist = h.distance
            out.fields = (h.collider.index == 0) and (h.rigidbody ~= nil) and (h.transform.name == "Crate")
            out.circleAll = #Physics2D.circleCastAll(scene, Vector2(200, 300), 16, down, 1000)
            out.box = Physics2D.boxCast(scene, Vector2(0, 300), Vector2(32, 32), 45, down, 1000).gameObject.name
            out.boxAll = #Physics2D.boxCastAll(scene, Vector2(200, 300), Vector2(32, 32), 0, down, 1000)
            out.capsule = Physics2D.capsuleCast(scene, Vector2(0, 300), Vector2(20, 60), CapsuleDirection2D.VERTICAL, 0, down, 1000).distance
            out.capsuleAll = #Physics2D.capsuleCastAll(scene, Vector2(200, 300), Vector2(60, 20), CapsuleDirection2D.HORIZONTAL, 0, down, 1000)
            out.overlapCircle = Physics2D.overlapCircle(scene, Vector2(200, 64), 4).gameObject.name
            out.overlapCircleAll = #Physics2D.overlapCircleAll(scene, Vector2(200, 16), 20)
            out.overlapBox = Physics2D.overlapBox(scene, Vector2(0, 50), Vector2(10, 10), 0) == nil
            out.overlapBoxAll = #Physics2D.overlapBoxAll(scene, Vector2(200, 30), Vector2(10, 100), 0)
            out.overlapCapsule = Physics2D.overlapCapsule(scene, Vector2(200, 64), Vector2(10, 30), CapsuleDirection2D.VERTICAL, 90).gameObject.name
            out.overlapCapsuleAll = #Physics2D.overlapCapsuleAll(scene, Vector2(200, 30), Vector2(10, 100), CapsuleDirection2D.VERTICAL, 0)
            out.overlapPoint = Physics2D.overlapPoint(scene, Vector2(0, -10)).gameObject.name
            out.overlapPointAll = #Physics2D.overlapPointAll(scene, Vector2(0, 10))
            out.overlapArea = Physics2D.overlapArea(scene, Vector2(150, 40), Vector2(250, 120)).gameObject.name
            out.overlapAreaAll = #Physics2D.overlapAreaAll(scene, Vector2(150, -40), Vector2(250, 120))
            -- from inside the Crate, the options and the static
            out.inside = Physics2D.raycast(scene, Vector2(200, 64), Vector2(1, 0), 100).hit
            out.startIn = Physics2D.raycast(scene, Vector2(200, 64), Vector2(1, 0), 100, nil, {startInColliders = true}).distance
            Physics2D.queriesStartInColliders = true
            out.global = Physics2D.raycast(scene, Vector2(200, 64), Vector2(1, 0), 100).gameObject.name
            Physics2D.queriesStartInColliders = false
            out.ignore = Physics2D.raycast(scene, Vector2(200, 300), down, 1000, nil, {ignore = {object("Crate")}}).gameObject.name
            local body = object("Crate"):getRigidbody2D()
            out.cast = body:cast(down, 1000).gameObject.name
            out.castDist = body:cast(down, 1000).distance
            out.castAll = #body:castAll(down, 1000)
            out.castUp = body:cast(Vector2(0, 1), 1000) == nil
            out.statics = Physics2D.queriesHitTriggers == true and Physics2D.queriesStartInColliders == false
            return out
        """)
        print(f"[test] 2D Lua queries: {got}")
        ed.check(got["circle"] == "Crate" and abs(got["circleDist"] - 188) < 1, f"{got}")
        ed.check(got["fields"] is True, f"RaycastHit fields: {got}")
        ed.check(got["circleAll"] == 2 and got["boxAll"] == 2 and got["capsuleAll"] == 2, f"...All: {got}")
        ed.check(got["box"] == "Ground" and abs(got["capsule"] - 270) < 1, f"{got}")
        ed.check(got["overlapCircle"] == "Crate" and got["overlapCircleAll"] == 2, f"{got}")
        ed.check(got["overlapBox"] is True and got["overlapBoxAll"] == 2, f"{got}")
        ed.check(got["overlapCapsule"] == "Crate" and got["overlapCapsuleAll"] == 2, f"{got}")
        ed.check(got["overlapPoint"] == "Ground" and got["overlapPointAll"] == 0, f"{got}")
        ed.check(got["overlapArea"] == "Crate" and got["overlapAreaAll"] == 2, f"{got}")
        ed.check(got["inside"] is False and abs(got["startIn"]) < 1e-3 and got["global"] == "Crate", f"startInColliders: {got}")
        ed.check(got["ignore"] == "Ground", f"ignore: {got}")
        ed.check(got["cast"] == "Ground" and abs(got["castDist"] - 32) < 1, f"Rigidbody2D:cast: {got}")
        ed.check(got["castAll"] == 1 and got["castUp"] is True and got["statics"] is True, f"{got}")

        result = await ed.call_raw("eval_lua", {"code": "Physics2D.boxCast(scene, Vector2(0, 0), Vector2(0, 1), 0, Vector2(1, 0), 10)"})
        ed.check(result.is_error and "size" in result.text, result.text[:300])
        result = await ed.call_raw("eval_lua", {"code": "Physics2D.capsuleCast(scene, Vector2(0, 0), Vector2(1, 1), 5, 0, Vector2(1, 0), 10)"})
        ed.check(result.is_error and "capsuleDirection" in result.text, result.text[:300])
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
