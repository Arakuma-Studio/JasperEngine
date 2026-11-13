"""S96 REQ-17, AC-16 (docs/specs/S96-physics-gaps.md): query_physics asks the running game's
physics world. Outside Play it fails naming play (and the edit-time world is recorded: a Lua ray
there finds no collider); in Play ray, sphere, box, capsule and point casts, overlaps and checks
in 3D and 2D return the expected GameObjects, with the ignore list, the layer mask, the trigger
rule and hit_from_inside; draw: true adds physics debug lines; the tool is in the physics group,
hidden by default."""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Query Physics"}
TIMEOUT = 600


def names(result):
    return [hit["name"] for hit in result["hits"]]


async def scene3d(ed: Editor) -> int:
    sid = (await ed.call("create_scene", name="Queries3D", type="3d"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, name="Floor", type="box", position=[0, -0.5, 0], scale=[20, 1, 20])
    await ed.call("edit_colliders", scene=sid, entity="Floor", body_type="static", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=sid, name="Player", type="box", position=[0, 0.5, 0])
    await ed.call("edit_colliders", scene=sid, entity="Player", body_type="static", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=sid, name="Crate", type="box", position=[3, 0.5, 0])
    await ed.call("edit_colliders", scene=sid, entity="Crate", body_type="static", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=sid, name="Zone", type="box", position=[-4, 0.5, 0])
    await ed.call("edit_colliders", scene=sid, entity="Zone", body_type="static", add=[{"shape": "box", "is_trigger": True}])
    await ed.call("create_entity", scene=sid, type="camera", name="Cam", position=[0, 3, 10])
    await ed.call("save_scene", scene=sid)
    return sid


async def test_query_physics_group_and_outside_play(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("query_physics" not in default, "hidden until the physics group is enabled")
    await ed.call("tool_groups", enable=["physics"])
    ed.check("query_physics" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")

    sid = await scene3d(ed)
    await ed.check_error("query_physics", shape="ray", origin=[0, 5, 0], direction=[0, -1, 0], max_distance=10,
                         contains="start Play first (play)")
    # the edit-time world: the scene's PhysicsSystem builds bodies only in Play (S96 REQ-17's inferred claim)
    edit = (await ed.call("eval_lua", scene=sid, allow_edit_mode=True,
                          code="return Physics.raycast(scene, Vector3(0, 5, 0), Vector3(0, -1, 0), 10).hit"))["values"]
    print(f"[test] edit-time Physics.raycast hit: {edit}")
    ed.check(edit == [False], f"no collider at edit time: {edit}")


async def test_query_physics_3d_in_play(ed: Editor):
    sid = await scene3d(ed)
    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        down = {"origin": [0, 5, 0], "direction": [0, -1, 0], "max_distance": 10}
        got = await ed.call("query_physics", shape="ray", **down)
        ed.check(names(got) == ["Player"] and abs(got["hits"][0]["distance"] - 4) < 1e-3, f"ray onto the Player's top: {got}")
        ed.check(abs(got["hits"][0]["normal"][1] - 1) < 1e-3 and got["hits"][0]["collider"] == 0, f"{got}")
        got = await ed.call("query_physics", shape="ray", mode="cast_all", **down)
        ed.check(names(got)[:2] == ["Player", "Floor"], f"cast_all nearest first: {got}")
        got = await ed.call("query_physics", shape="ray", ignore=["Player"], **down)
        ed.check(names(got) == ["Floor"] and abs(got["hits"][0]["distance"] - 5) < 1e-3, f"ignore: {got}")
        got = await ed.call("query_physics", shape="ray", layer_mask="nothing", **down)
        ed.check(got["hits"] == [], f"layer mask: {got}")

        got = await ed.call("query_physics", shape="sphere", radius=0.5, **down)
        ed.check(names(got) == ["Player"] and abs(got["hits"][0]["distance"] - 3.5) < 1e-3, f"sphere cast: {got}")
        got = await ed.call("query_physics", shape="box", half_extents=[0.25, 0.5, 0.25], rotation={"euler": [0, 45, 0]}, **down)
        ed.check(names(got) == ["Player"] and abs(got["hits"][0]["distance"] - 3.5) < 1e-3, f"box cast: {got}")
        got = await ed.call("query_physics", shape="capsule", point_a=[3, 5, 0], point_b=[3, 6, 0], radius=0.5,
                            direction=[0, -1, 0], max_distance=10)
        ed.check(names(got) == ["Crate"] and abs(got["hits"][0]["distance"] - 3.5) < 1e-3, f"capsule cast: {got}")

        # inside the Player: Unity's rule skips it; hit_from_inside reports it at 0
        side = {"origin": [0, 0.5, 0], "direction": [1, 0, 0], "max_distance": 10}
        got = await ed.call("query_physics", shape="ray", **side)
        ed.check(names(got) == ["Crate"] and abs(got["hits"][0]["distance"] - 2.5) < 1e-3, f"from inside: {got}")
        got = await ed.call("query_physics", shape="ray", hit_from_inside=True, **side)
        ed.check(names(got) == ["Player"] and abs(got["hits"][0]["distance"]) < 1e-3, f"hit_from_inside: {got}")

        # triggers: the Zone is hit by default (Physics.queriesHitTriggers), not with ignore
        west = {"origin": [0, 0.5, 0.0], "direction": [-1, 0, 0], "max_distance": 10, "hit_from_inside": False}
        got = await ed.call("query_physics", shape="ray", **west)
        ed.check(names(got) == ["Zone"], f"a trigger is hit by default: {got}")
        got = await ed.call("query_physics", shape="ray", triggers="ignore", **west)
        ed.check(got["hits"] == [], f"triggers ignore: {got}")

        # overlaps and checks: is anything within 2 m of the Crate's centre
        got = await ed.call("query_physics", shape="sphere", mode="overlap", origin=[3, 0.5, 0], radius=2)
        ed.check(sorted(names(got)) == ["Crate", "Floor"], f"overlap: {got}")
        got = await ed.call("query_physics", shape="box", mode="overlap", origin=[0, 0.5, 0], half_extents=[4, 0.25, 0.25])
        ed.check(sorted(names(got)) == ["Crate", "Player", "Zone"], f"box overlap: {got}")
        got = await ed.call("query_physics", shape="point", mode="overlap", origin=[3.2, 0.6, 0.1])
        ed.check(names(got) == ["Crate"], f"point overlap: {got}")
        got = await ed.call("query_physics", shape="sphere", mode="check", origin=[8, 5, 8], radius=0.5)
        ed.check(got["hit"] is False, f"check, nothing: {got}")
        got = await ed.call("query_physics", shape="capsule", mode="check", point_a=[3, 0, 0], point_b=[3, 1, 0], radius=0.1)
        ed.check(got["hit"] is True, f"check, the Crate: {got}")

        # draw: the shape and its hits in the Game view's physics lines
        view = await ed.call("configure_game_view")
        ed.check(view["physics_debug"] is False, f"{view}")
        got = await ed.call("query_physics", shape="sphere", radius=0.5, draw=True, **down)
        ed.check(got.get("drawn_lines", 0) > 0, f"lines added: {got}")
        await ed.call("wait", frames=2)
        view = await ed.call("configure_game_view")
        print(f"[test] physics debug lines after draw: {view['physics_debug_lines']}")
        ed.check(view["physics_debug_lines"] > 0, f"drawn with Physics.debugDraw off: {view}")

        # what the tool refuses names the fix
        await ed.check_error("query_physics", shape="ray", mode="overlap", origin=[0, 0, 0], contains="mode cast")
        await ed.check_error("query_physics", shape="sphere", origin=[0, 5, 0], direction=[0, -1, 0], max_distance=10,
                             contains="radius")
        await ed.check_error("query_physics", shape="ray", origin=[0, 5, 0], direction=[0, 0, 0], max_distance=10,
                             contains="direction must be")
        await ed.check_error("query_physics", shape="ray", origin=[0, 5, 0], direction=[0, -1, 0], contains="max_distance")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_query_physics_2d_in_play(ed: Editor):
    sid = (await ed.call("create_scene", name="Queries2D", type="2d"))["scene"]["id"]
    ground = (await ed.call("create_entity", scene=sid, name="Ground", type="rigidbody2d", position=[0, -32, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=ground, body_type="static", add=[{"shape": "box", "size": [640, 64], "fit": False}])
    crate = (await ed.call("create_entity", scene=sid, name="Crate", type="rigidbody2d", position=[200, 64, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=crate, body_type="static", add=[{"shape": "box", "size": [64, 64], "fit": False}])
    await ed.call("save_scene", scene=sid)
    await ed.call("play", action="start", scene=sid, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        got = await ed.call("query_physics", shape="ray", origin=[0, 200], direction=[0, -1], max_distance=1000)
        ed.check(names(got) == ["Ground"] and abs(got["hits"][0]["distance"] - 200) < 0.5, f"2D ray: {got}")
        ed.check(got["dimension"] == "2d", f"{got}")
        got = await ed.call("query_physics", shape="sphere", origin=[200, 300], radius=16, direction=[0, -1], max_distance=1000)
        ed.check(names(got) == ["Crate"] and abs(got["hits"][0]["distance"] - (300 - 96 - 16)) < 1, f"circle cast: {got}")
        got = await ed.call("query_physics", shape="box", origin=[200, 300], size=[32, 32], angle=45, direction=[0, -1],
                            max_distance=1000, mode="cast_all")
        ed.check(names(got) == ["Crate", "Ground"], f"box cast_all: {got}")
        got = await ed.call("query_physics", shape="capsule", mode="overlap", origin=[200, 20], size=[20, 120])
        ed.check(sorted(names(got)) == ["Crate", "Ground"], f"capsule overlap: {got}")
        got = await ed.call("query_physics", shape="box", mode="overlap", point_a=[150, 40], point_b=[250, 120])
        ed.check(names(got) == ["Crate"], f"area overlap: {got}")
        got = await ed.call("query_physics", shape="point", mode="check", origin=[0, -10])
        ed.check(got["hit"] is True, f"point check: {got}")
        # from inside the Crate: no hit by default (startInColliders false), at 0 with hit_from_inside
        got = await ed.call("query_physics", shape="ray", origin=[200, 64], direction=[1, 0], max_distance=100)
        ed.check(got["hits"] == [], f"2D from inside: {got}")
        got = await ed.call("query_physics", shape="ray", origin=[200, 64], direction=[1, 0], max_distance=100, hit_from_inside=True)
        ed.check(names(got) == ["Crate"] and abs(got["hits"][0]["distance"]) < 1e-3, f"2D hit_from_inside: {got}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
