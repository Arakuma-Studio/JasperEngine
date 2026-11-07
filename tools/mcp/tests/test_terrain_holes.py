"""T3's holes end to end: cut_holes and fill_holes through T1's edit_terrain, what the readers answer without a
reload, the Hole brush and its overlay (R27-R30, R41; AC34-AC36). edit_terrain is T1's: until it has landed these tests
report it missing through require_tools, and the same semantics run in TerrainHolesConsumers.* and TerrainHoles.*.
"""

from harness import Editor

EDITOR = {"create": True, "name": "Terrain Holes"}
CI = True

S = {}


async def setup(ed: Editor):
    if S:
        return S
    S["scene"] = (await ed.call("list_scenes"))["scenes"][0]["id"]
    made = await ed.call("create_entity", scene=S["scene"], type="terrain", name="Ground", select=False)
    S["entity"] = made["entity_id"]
    # holes are cut on a height map (T1's edit_terrain refuses a terrain without one)
    await ed.call("edit_terrain", scene=S["scene"], entity=S["entity"], create="test_patch", map_resolution=65)
    return S


async def cut(ed, s, radius=8.0, tool="cut_holes", **extra):
    op = {"op": tool, "shape": {"type": "circle", "center": [0, 0], "radius": radius}}
    return await ed.call("edit_terrain", scene=s["scene"], entity=s["entity"], ops=[op], **extra)


async def test_ops_semantics(ed: Editor):
    ed.require_tools("edit_terrain", "sample_terrain")
    s = await setup(ed)

    # dry_run changes nothing
    await cut(ed, s, dry_run=True)
    ed.check_eq((await ed.call("sample_terrain", scene=s["scene"], entity=s["entity"], grid=0))["info"]["holes"], None)

    before = await ed.call("sample_terrain", scene=s["scene"], entity=s["entity"], grid=9)
    first = await cut(ed, s)
    op = first["ops"][0]
    ed.check_eq(op["map_created"], True)
    ed.check(op["changed_texels"] > 0, "the cut changed texels")
    ed.check(op["collision_samples_open"] > 0, "a wide hole opens the collider")

    # idempotent: a second identical call changes 0 texels
    ed.check_eq((await cut(ed, s))["ops"][0]["changed_texels"], 0)

    # one undo per call
    await ed.call("undo")
    await ed.call("undo")
    ed.check_eq((await ed.call("sample_terrain", scene=s["scene"], entity=s["entity"], grid=0))["info"]["holes"], None)
    await ed.call("redo")

    # a small hole draws and opens nothing: it says so
    small = await cut(ed, s, radius=0.6)
    ed.check_in("hole_no_collision", [w.get("code") for w in small.get("warnings", [])] + [""], "small hole warns or was inside")

    # the last fill removes the map, and heights stand as they were
    filled = await ed.call("edit_terrain", scene=s["scene"], entity=s["entity"], ops=[{"op": "fill_holes"}])
    ed.check_eq(filled["ops"][0]["map_removed"], True)
    after = await ed.call("sample_terrain", scene=s["scene"], entity=s["entity"], grid=9)
    ed.check_eq(after["grid"]["world_heights"], before["grid"]["world_heights"])


async def test_refresh_after_cut(ed: Editor):
    ed.require_tools("edit_terrain", "sample_terrain", "pick_at")
    s = await setup(ed)
    await cut(ed, s)
    point = (await ed.call("sample_terrain", scene=s["scene"], entity=s["entity"], points=[[0, 0]], grid=0))["samples"][0]
    ed.check_eq(point["hole"], True)
    ed.check_eq(point["collision_gap"], True)
    ed.check_eq(point["nav_excluded"], True)
    await ed.call("edit_terrain", scene=s["scene"], entity=s["entity"], ops=[{"op": "fill_holes"}])
    point = (await ed.call("sample_terrain", scene=s["scene"], entity=s["entity"], points=[[0, 0]], grid=0))["samples"][0]
    ed.check_eq(point["hole"], False)
    ed.check_eq(point["collision_gap"], False)
