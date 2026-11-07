"""Lua on Terrain in Play (T1 R75, R77-R79; AC31-AC34).

Queries work on any terrain; writes need Allow Runtime Edits, leave the authored terrain as it was
once Play stops, and reach the collider before the next physics step.
"""

from harness import Editor

EDITOR = {"create": True, "name": "Terrain Lua"}
CI = True


async def island(ed: Editor, name: str, runtime_edits: bool):
    path = f"scenes/{name}.scene"
    sid = (await ed.call("create_scene", name=name, type="3d", path=path))["scene"]["id"]
    terrain = (await ed.call("create_entity", scene=sid, name="Island", type="terrain", position=[10, 2, -4]))["entity_id"]
    await ed.call("edit_terrain", scene=sid, entity=terrain, create="test_patch", map_resolution=65,
                  ops=[{"op": "set_height", "height": 8}])
    if runtime_edits:
        await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {"allowRuntimeEdits": True}})
    await ed.call("save_scene", scene=sid, path=path)
    return sid, terrain, path


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def test_queries(ed: Editor):
    sid, terrain, _ = await island(ed, "Queries", False)
    await ed.call("play", action="start", scene=sid, auto_pause=True)
    try:
        got = await lua(ed, """
            local t = Terrain(scene, entity('Island'))
            local n = t:getNormalAt(10, -4)
            local hit = t:raycast(Vector3(10, 50, -4), Vector3(0, -1, 0), 100)
            return {t:getHeightAt(10, -4), n.y, t:contains(10, -4), t:contains(500, 0), t:isHoleAt(10, -4),
                    hit.hit, hit.point.y, hit.distance, t.heightMapWidth, t.allowRuntimeEdits}""")
        height, normal_y, inside, outside, hole, hit, hit_y, distance, width, allowed = got[0]
        ed.check(abs(height - 10.0) < 0.01, f"world height is the 8 m surface plus the terrain's 2 m: {got}")
        ed.check(abs(normal_y - 1.0) < 1e-4, f"flat: {got}")
        ed.check(inside and not outside and not hole, f"contains and holes: {got}")
        ed.check(hit and abs(hit_y - 10.0) < 0.05 and abs(distance - 40.0) < 0.05, f"the ray hits the surface: {got}")
        ed.check_eq(width, 65, "the height map's width")
        ed.check(allowed is False, "Allow Runtime Edits is off")
    finally:
        await ed.call("play", action="stop")


async def test_edit_needs_the_setting(ed: Editor):
    sid, terrain, _ = await island(ed, "Refused", False)
    await ed.call("play", action="start", scene=sid, auto_pause=True)
    try:
        message = await ed.call("eval_lua", expect_error=True, code="""
            return Terrain(scene, entity('Island')):applyOps({{op = "raise", delta = 1}})""")
        ed.check("Allow Runtime Edits" in message and "Island" in message, f"the error names the setting and the terrain: {message}")
    finally:
        await ed.call("play", action="stop")


async def test_runtime_edit_is_isolated(ed: Editor):
    sid, terrain, _ = await island(ed, "Isolated", True)
    before = (await ed.call("sample_terrain", scene=sid, entity=terrain, hash=True))["hashes"]
    await ed.call("play", action="start", scene=sid, auto_pause=True)
    try:
        got = await lua(ed, """
            local t = Terrain(scene, entity('Island'))
            local r = t:applyOps({{op = "lower", delta = 3, shape = {type = "circle", center = {10, -4}, radius = 6}}})
            return {t:getHeightAt(10, -4), r.ops[1].changed_texels, #r.warnings, r.changed[1].map}""")
        height, changed, warnings, map_name = got[0]
        ed.check(abs(height - 7.0) < 0.05, f"the next line sees the crater: {got}")
        ed.check(changed > 0 and warnings == 0 and map_name == "height", f"the result table: {got}")
        bad = await ed.call("eval_lua", expect_error=True, code="""
            return Terrain(scene, entity('Island')):applyOps({{op = "lower", delta = 1, shape = {type = "circle", radius = 2, centre = {0, 0}}}})""")
        ed.check("ops[1].shape.centre" in bad, f"a bad field is named, as Lua counts: {bad}")
    finally:
        await ed.call("play", action="stop")
    after = (await ed.call("sample_terrain", scene=sid, entity=terrain, hash=True))["hashes"]
    ed.check_eq(after, before, "stopping Play gives back the authored terrain byte for byte")
    stats = (await ed.call("sample_terrain", scene=sid, entity=terrain, stats=["height"]))["stats"]["height"]
    ed.check(abs(stats["min"] - 10.0) < 0.01, f"no crater in the edit scene: {stats}")


async def test_ball_falls_into_crater(ed: Editor):
    sid, terrain, path = await island(ed, "Crater", True)
    await ed.call("edit_colliders", scene=sid, entity=terrain, body_type="static", add=[{"shape": "heightfield"}])
    await ed.call("create_entity", scene=sid, type="sphere", name="Ball", position=[10, 10.5, -4], scale=[1, 1, 1])
    await ed.call("edit_colliders", scene=sid, entity="Ball", body_type="dynamic", add=[{"shape": "sphere", "radius": 0.5}])
    await ed.call("save_scene", scene=sid, path=path)
    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=90)
        resting = (await lua(ed, "return object('Ball').position.y"))[0]
        ed.check(abs(resting - 10.5) < 0.2, f"the ball rests on the 10 m surface: {resting}")
        await lua(ed, """
            Terrain(scene, entity('Island')):applyOps({{op = "lower", delta = 4,
                shape = {type = "circle", center = {10, -4}, radius = 5, falloff = "constant"}}})""")
        await ed.call("wait", frames=120)
        fallen = (await lua(ed, "return object('Ball').position.y"))[0]
        ed.check(abs(fallen - 6.5) < 0.3, f"the ball woke and fell to the crater floor at 6 m: {fallen}")
    finally:
        await ed.call("play", action="stop")
