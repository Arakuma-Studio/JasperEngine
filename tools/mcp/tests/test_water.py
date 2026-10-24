"""library:water beyond the shader: the GameObject menu's 3D Object > Water (create_entity
type=water: the grid, the material preset and the underwater pass it adds, one undo step),
the Rigidbody buoyancy fields, buoyancy in Play (a box floats, one without buoyancy sinks), the
Lua surface queries (Water.getHeight / getNormal / isUnderwater, the same waves as the
shader), the automatic library:underwater pass (on under the surface, off above, the surface
height and colours written into it) and screenshots of the water from above and below
(screenshots/water_above.png, water_below.png, water_up.png in the scratch project)."""

from __future__ import annotations

from harness import Editor, find_key

EDITOR = {"create": True, "name": "WaterTest"}
TIMEOUT = 900

SCENE = "scenes/Water.scene"
M = "submeshes[0].material."


def passes_of(settings) -> list:
    found = find_key(settings, "post_process")
    return found if isinstance(found, list) else []


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def test_setup(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "get_entity", "get_scene_settings", "undo", "redo",
                     "edit_colliders", "play", "eval_lua", "wait", "screenshot", "set_main_camera", "set_transform")
    await ed.call("create_scene", name="Water", path=SCENE)


async def test_menu_entry_creates_water(ed: Editor):
    made = await ed.call("create_entity", type="water")
    water = find_key(made, "entity_id", "id")
    ed.check(water is not None, f"create_entity returned no id: {made!r}")
    entity = await ed.call("get_entity", entity=water, properties=["submeshes[0].material.*", "vertexCount"])
    ed.check_eq(find_key(entity, "name"), "Water", "named as the menu entry")
    mesh = entity["components"]["mesh"]
    ed.check_eq(mesh.get(M + "shader"), "library:water", f"water material: {mesh!r}")
    features = mesh.get(M + "shaderFeatures") or []
    for feature in ("WAVES", "REFLECTION", "UNDERSIDE", "DEPTH_FOAM", "REFRACTION"):
        ed.check(feature in features, f"preset feature {feature}: {features!r}")
    ed.check_eq(mesh.get(M + "lighting"), "pixel", "lit per pixel")

    settings = await ed.call("get_scene_settings", names=["post_process"])
    passes = passes_of(settings)
    ed.check(len(passes) == 1 and passes[0].get("shader") == "library:underwater" and passes[0].get("enabled") is False,
             f"an underwater pass, off: {settings!r}")

    # a second water adds no second pass
    second = find_key(await ed.call("create_entity", type="water", name="Pool"), "entity_id", "id")
    ed.check_eq(len(passes_of(await ed.call("get_scene_settings", names=["post_process"]))), 1, "one underwater pass")
    await ed.call("undo")
    await ed.call("undo")
    tree = await ed.call("get_scene_tree")
    ed.check(str(water) not in str(find_key(tree, "entities") or tree) or "Water" not in str(tree),
             f"undo removes the water: {tree!r}")
    ed.check_eq(passes_of(await ed.call("get_scene_settings", names=["post_process"])), [],
                "undo removes the pass it added")
    await ed.call("redo")
    ed.check_eq(len(passes_of(await ed.call("get_scene_settings", names=["post_process"]))), 1, "redo adds it again")
    del second


async def test_buoyancy_fields(ed: Editor):
    made = await ed.call("create_entity", type="cube", name="Crate", position=[0, 2, 0])
    crate = find_key(made, "entity_id", "id")
    await ed.call("edit_colliders", entity=crate, body_type="dynamic", add=[{"shape": "box", "density": 500}],
                  body={"buoyancy": 1.5, "waterDrag": 0.8})
    values = (await ed.call("get_entity", entity=crate, components=["rigidbody"]))["components"]["rigidbody"]
    ed.check_eq(values.get("buoyancy"), 1.5, f"buoyancy set: {values!r}")
    ed.check_eq(values.get("waterDrag"), 0.8)
    ed.check_eq(values.get("waterAngularDrag"), 0.01, "the default angular drag")

    made = await ed.call("create_entity", type="cube", name="Anchor", position=[3, 2, 0])
    anchor = find_key(made, "entity_id", "id")
    await ed.call("edit_colliders", entity=anchor, body_type="dynamic", add=[{"shape": "box"}])
    values = (await ed.call("get_entity", entity=anchor, components=["rigidbody"]))["components"]["rigidbody"]
    ed.check(values.get("buoyancy", 0) == 0, f"no buoyancy by default: {values!r}")

    # a bed under the water and a post through the surface (no colliders), for the screenshots
    await ed.call("create_entity", type="plane", name="Bed", position=[0, -3, 0], scale=[2, 1, 2])
    await ed.call("create_entity", type="cube", name="Post", position=[-2.5, -1, 1], scale=[0.6, 5, 0.6])

    # a camera above the water looking down at it, for the screenshots
    cam = find_key(await ed.call("create_entity", type="camera", name="View", position=[0, 4, 9],
                                 rotation={"euler": [-25, 0, 0]}), "entity_id", "id")
    await ed.call("set_main_camera", entity=cam)


async def test_play_floats_and_queries(ed: Editor):
    await ed.call("play", action="start")
    await ed.call("wait", game_seconds=6)
    crate = await ed.call("get_runtime_entity", entity="Crate", components=["transform"])
    anchor = await ed.call("get_runtime_entity", entity="Anchor", components=["transform"])
    crate_y = find_key(crate, "position")[1]
    anchor_y = find_key(anchor, "position")[1]
    ed.check(-0.7 < crate_y < 0.7, f"the buoyant crate floats at the surface: y {crate_y}")
    ed.check(anchor_y < -10, f"the other falls through the water: y {anchor_y}")

    # the surface the shader draws: waveHeight 0.1, two waves (at most 1.5 times it)
    height, normal, outside, under, over = await lua(ed, """
        return Water.getHeight(scene, 1.0, 2.0), Water.getNormal(scene, 1.0, 2.0),
               Water.getHeight(scene, 50.0, 0.0), Water.isUnderwater(scene, Vector3(0, -1, 0)),
               Water.isUnderwater(scene, Vector3(0, 1, 0))""")
    ed.check(isinstance(height, (int, float)) and abs(height) <= 0.15 + 1e-4, f"height within the waves: {height!r}")
    ed.check(isinstance(normal, list) and len(normal) == 3 and normal[1] > 0.9, f"normal nearly up: {normal!r}")
    ed.check(outside is None, f"no water at x 50: {outside!r}")
    ed.check(under is True and over is False, f"isUnderwater: {under!r} {over!r}")
    # on the shader's clock: the waves move
    first, second = await lua(ed, "local a = Water.getHeight(scene, 1, 2); local t = Water.getTime(); "
                                  "while Water.getTime() < t + 0.3 do end; return a, Water.getHeight(scene, 1, 2)")
    ed.check(first != second, f"the waves move: {first} {second}")


async def test_underwater_pass_switches(ed: Editor):
    shot = await ed.call_raw("screenshot", {"view": "game", "save_path": "screenshots/water_above.png"})
    ed.check(shot.images, "screenshot from above")
    off = await lua(ed, "return scene:isPostProcessPassEnabled(0)")
    ed.check_eq(off[0], False, "above the water: the pass is off")

    # the camera dives
    await lua(ed, "local cam = object('View'); cam.position = Vector3(0, -1.5, 6); cam:setRotation(15, 0, 0); return true")
    await ed.call("wait", frames=5)
    on, level, shallow = await lua(ed, "return scene:isPostProcessPassEnabled(0), "
                                       "scene:getPostProcessUniform(0, 'waterLevel').x, scene:getPostProcessUniform(0, 'shallowColor')")
    ed.check_eq(on, True, "under the water: the pass is on")
    ed.check(abs(level) <= 0.15 + 1e-3, f"the surface height written: {level!r}")
    # the water's manifest default (linear), not the pass's own
    ed.check(isinstance(shallow, list) and len(shallow) == 4 and shallow[1] > 0.5 and shallow[3] == 0.5,
             f"the water's shallow colour written: {shallow!r}")
    await ed.call("wait", frames=10)
    shot = await ed.call_raw("screenshot", {"view": "game", "save_path": "screenshots/water_below.png"})
    ed.check(shot.images, "screenshot from below")
    # looking up: Snell's window over the camera
    await lua(ed, "object('View'):setRotation(70, 0, 0); return true")
    await ed.call("wait", frames=5)
    shot = await ed.call_raw("screenshot", {"view": "game", "save_path": "screenshots/water_up.png"})
    ed.check(shot.images, "screenshot looking up from below")

    await lua(ed, "local cam = object('View'); cam.position = Vector3(0, 4, 9); cam:setRotation(-25, 0, 0); return true")
    await ed.call("wait", frames=5)
    off = await lua(ed, "return scene:isPostProcessPassEnabled(0)")
    ed.check_eq(off[0], False, "back above: off again")

    await ed.call("play", action="stop")
    passes = passes_of(await ed.call("get_scene_settings", names=["post_process"]))
    ed.check(passes and passes[0].get("enabled") is False, f"stop restores the edit-time chain: {passes!r}")
