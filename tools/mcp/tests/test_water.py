"""library:water beyond the shader: the GameObject menu's 3D Object > Water (create_entity
type=water: the grid, the material preset and the underwater pass it adds, one undo step),
the Rigidbody buoyancy fields, buoyancy in Play (a box floats, one without buoyancy sinks), the
Lua surface queries (Water.getHeight / getNormal / isUnderwater, the same waves as the
shader), the automatic library:underwater pass (on under the surface, off above, the surface
height and colours written into it), screenshots of the water from above and below
(screenshots/water_above.png, water_below.png, water_up.png in the scratch project), and that a
library:water material without textures keeps the flat, grey defaults it had (no ripples:
the preset's BUILTIN_TEXTURES alone picks the engine's ripples and noise)."""

from __future__ import annotations

import asyncio

from harness import Editor, find_key

from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "WaterTest"}
CI = True
TIMEOUT = 900

SCENE = "scenes/Water.scene"
M = "submeshes[0].material."


def passes_of(settings) -> list:
    found = find_key(settings, "post_process")
    return found if isinstance(found, list) else []


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def test_menu_entry_creates_water(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "get_entity", "get_scene_settings", "undo", "redo",
                     "edit_colliders", "play", "eval_lua", "wait", "screenshot", "set_main_camera", "set_transform")
    await ed.call("create_scene", name="Water", path=SCENE)

    made = await ed.call("create_entity", type="water")
    water = find_key(made, "entity_id", "id")
    ed.check(water is not None, f"create_entity returned no id: {made!r}")
    entity = await ed.call("get_entity", entity=water, properties=["submeshes[0].material.*", "vertexCount"])
    ed.check_eq(find_key(entity, "name"), "Water", "named as the menu entry")
    mesh = entity["components"]["mesh"]
    ed.check_eq(mesh.get(M + "shader"), "library:water", f"water material: {mesh!r}")
    features = mesh.get(M + "shaderFeatures") or []
    for feature in ("BUILTIN_TEXTURES", "WAVES", "GERSTNER", "REFLECTION", "UNDERSIDE", "DEPTH_FOAM", "REFRACTION"):
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

    # the surface the shader draws: waveHeight 0.1, four Gerstner waves (at most 1.94 times it)
    height, normal, outside, under, over = await lua(ed, """
        return Water.getHeight(scene, 1.0, 2.0), Water.getNormal(scene, 1.0, 2.0),
               Water.getHeight(scene, 50.0, 0.0), Water.isUnderwater(scene, Vector3(0, -1, 0)),
               Water.isUnderwater(scene, Vector3(0, 1, 0))""")
    ed.check(isinstance(height, (int, float)) and abs(height) <= 0.194 + 1e-4, f"height within the waves: {height!r}")
    ed.check(isinstance(normal, list) and len(normal) == 3 and normal[1] > 0.8, f"normal nearly up: {normal!r}")
    ed.check(outside is None, f"no water at x 50: {outside!r}")
    ed.check(under is True and over is False, f"isUnderwater: {under!r} {over!r}")
    # on the scene clock, advanced once a frame: still within a frame, moving over frames
    first, time, same = await lua(ed, "local a = Water.getHeight(scene, 1, 2); local t = Water.getTime(scene); "
                                      "local s = os.clock(); while os.clock() < s + 0.05 do end; return a, t, Water.getTime(scene)")
    ed.check_eq(same, time, "one water time within a frame")
    await ed.call("wait", frames=20)
    second, later = await lua(ed, "return Water.getHeight(scene, 1, 2), Water.getTime(scene)")
    ed.check(later > time, f"the clock moves frame to frame: {time} {later}")
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


async def test_caustics_and_absorption_screenshot(ed: Editor):
    # every feature, from above, stopped (the game view through the main camera)
    await ed.call("set_properties", entity="Water", values={"mesh": {
        M + "shaderFeatures": ["BUILTIN_TEXTURES", "WAVES", "REFLECTION", "UNDERSIDE", "DEPTH_FOAM", "ABSORPTION", "REFRACTION", "CAUSTICS"],
        M + "shaderParams": {"absorptionDepth": 4, "causticsStrength": 2}}})
    shot = await ed.call_raw("screenshot", {"view": "game", "save_path": "screenshots/water_caustics.png"})
    ed.check(shot.images, "screenshot with caustics")
    r = await ed.call("check_shader", timeout=300)
    errors = [f for f in r.get("findings", []) if f.get("severity") == "error"]
    ed.check(not errors, f"the water's variant has no check error: {errors!r}")


def roughness(png: bytes) -> int:
    """How many horizontal neighbours (green) in the middle half of an image differ by more than 4
    (a smooth gradient: almost none)."""
    w, h, channels, rows = decode_png(png)
    steps = 0
    for row in rows[h // 4: 3 * h // 4]:
        for x in range(w // 4, 3 * w // 4):
            steps += abs(row[x * channels + 1] - row[(x + 1) * channels + 1]) > 4
    return steps


async def test_old_materials_keep_flat_defaults(ed: Editor):
    # the manifest: unset textures stay flat and grey, BUILTIN_TEXTURES picks the built-in ones
    entry = (await ed.call("list_shaders", name="water"))["shaders"][0]
    textures = {t["uniform"]: t for t in entry.get("textures", [])}
    ed.check_eq(textures["u_normalTexture"]["default"], "normal", f"old normal default: {textures!r}")
    ed.check_eq(textures["u_noiseTexture"]["default"], "gray", "old noise default")
    ed.check_eq(textures["u_normalTexture"].get("default_with"), {"feature": "BUILTIN_TEXTURES", "default": "ripples"})
    ed.check_eq(textures["u_noiseTexture"].get("default_with"), {"feature": "BUILTIN_TEXTURES", "default": "noise"})

    # rendered: a plain library:water plane (no textures, no features, unlit) is a smooth
    # gradient, as before (pixel-identical to the editor before these features); BUILTIN_TEXTURES
    # ripples it
    await ed.call("create_scene", name="Plain", path="scenes/Plain.scene")
    plane = find_key(await ed.call("create_entity", type="plane", name="Pond"), "entity_id", "id")
    await ed.call("set_properties", entity=plane, values={"mesh": {M + "shader": "library:water"}})
    # at a slant, where the fresnel colour shows the normals
    cam = find_key(await ed.call("create_entity", type="camera", name="Slant", position=[0, 1.5, 4],
                                 rotation={"euler": [-30, 0, 0]}), "entity_id", "id")
    await ed.call("set_main_camera", entity=cam)
    plain = await ed.call_raw("screenshot", {"view": "game", "max_width": 400, "save_path": "screenshots/water_plain.png"})
    ed.check(plain.images, "screenshot of the plain water")
    flat = roughness(plain.images[0][0])
    ed.check(flat <= 20, f"no ripples without textures ({flat} sharp neighbour steps)")

    await ed.call("set_properties", entity=plane, values={"mesh": {M + "shaderFeatures": ["BUILTIN_TEXTURES"]}})
    # the new variant compiles in the background: the old one draws until it is ready
    ripples = 0
    for _ in range(40):
        rippled = await ed.call_raw("screenshot", {"view": "game", "max_width": 400, "save_path": "screenshots/water_builtin.png"})
        ed.check(rippled.images, "screenshot with the built-in textures")
        ripples = roughness(rippled.images[0][0])
        if ripples > 200:
            break
        await asyncio.sleep(0.5)
    ed.check(ripples > 200 and ripples > 10 * max(flat, 1), f"BUILTIN_TEXTURES ripples it ({ripples} sharp steps against {flat})")
