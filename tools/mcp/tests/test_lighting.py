"""S57's lighting (docs/specs/S57-forge-lighting.md AC6): the new light, camera, material, scene,
project and Capsule Shadow Caster fields through set_properties / set_scene_settings /
set_project_settings, saved, reloaded and read in Lua, one undo step each; and get_render_stats'
`lighting` figures of each feature while it draws (the tiles, the capsules, the contact mask, OIT)."""

from __future__ import annotations

import math

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Lighting"}
TIMEOUT = 900


async def make_scene(ed: Editor, name: str) -> int:
    made = await ed.call("create_scene", name=name, type="3D")
    return made["scene"]["id"]


async def sun_of(ed: Editor, scene: int) -> int:
    found = await ed.call("find_entities", scene=scene, component="light")
    entities = harness.find_entities(found)
    ed.check(entities, "the new 3D scene has its sun")
    return harness.entity_id_of(entities[0])


async def camera_of(ed: Editor, scene: int) -> int:
    """a camera GameObject made the scene's main camera (a new scene draws through the default
    camera, which has no GameObject)"""
    made = await ed.call("create_entity", scene=scene, type="camera", name="Main Camera", position=[0, 2, 8], select=False)
    camera = made["entity_id"]
    await ed.call("set_main_camera", scene=scene, entity=camera)
    return camera


async def named(ed: Editor, scene: int, name: str) -> int:
    entities = harness.find_entities(await ed.call("find_entities", scene=scene, name=name))
    ed.check(entities, f"the scene has {name}")
    return harness.entity_id_of(entities[0])


async def entity(ed: Editor, scene: int, kind: str, name: str, position, scale=None, values=None) -> int:
    args = {"scene": scene, "type": kind, "name": name, "position": position, "select": False}
    if scale is not None:
        args["scale"] = scale
    if values:
        args["values"] = values
    made = await ed.call("create_entity", **args)
    return made["entity_id"]


async def lighting_stats(ed: Editor, frames: int = 6) -> dict:
    await ed.call("wait", frames=frames)
    stats = await ed.call("get_render_stats")
    ed.check("lighting" in stats, f"get_render_stats has no lighting: {sorted(stats)}")
    return stats["lighting"]


async def test_lighting_fields_round_trip(ed: Editor):
    ed.require_tools("set_properties", "set_scene_settings", "set_project_settings", "get_entity", "eval_lua", "add_component")
    scene = await make_scene(ed, "LightingFields")
    sun = await sun_of(ed, scene)
    camera = await camera_of(ed, scene)
    body = await entity(ed, scene, "cube", "Body", [0, 1, 0], scale=[0.6, 2, 0.4])

    await ed.call("set_properties", scene=scene, entity=sun, values={"light": {
        "shadows": True, "contactShadows": True, "contactShadowLength": 0.3, "contactShadowThickness": 0.08,
        "contactShadowSampleCount": 16}})
    await ed.call("set_properties", scene=scene, entity=camera, values={"camera": {
        "transparencyMode": "order_independent", "oitWeightScale": 0.5}})
    await ed.call("add_component", scene=scene, entity=body, component="capsuleshadowcaster")
    await ed.call("set_properties", scene=scene, entity=body, values={"capsuleshadowcaster": {
        "castShadows": "capsules_only",
        "capsules": [{"bone": "", "radius": 0.3, "length": 0.6, "offset": [0, 0.3, 0], "direction": "y_axis"},
                     {"bone": "", "radius": 0.12, "length": 0.7, "offset": [0.15, -0.45, 0], "direction": "y_axis"}]}})
    await ed.call("set_scene_settings", scene=scene, settings={"capsule_shadows_enabled": True, "capsule_shadow_intensity": 0.8,
                                                               "capsule_shadow_softness": 0.4})
    await ed.call("set_project_settings", values={"graphics.light_mode": "tiled"})

    await ed.call("save_scene", scene=scene, path="scenes/LightingFields.scene")
    text = (ed.project / "scenes/LightingFields.scene").read_text()
    for key in ("contactShadows: true", "contactShadowLength: 0.3", "contactShadowSampleCount: 16", "oitWeightScale: 0.5",
                "capsuleShadowsEnabled: true", "capsuleShadowSoftness: 0.4"):
        ed.check(key in text, f"{key} saved")
    ed.check("light_mode" in (ed.project / "project.yaml").read_text() or "lightMode" in (ed.project / "project.yaml").read_text(),
             "the project's light mode saved")

    # reloaded, read back through Lua
    await ed.call("close_scene", scene=scene, unsaved="discard")
    opened = await ed.call("open_scene", scene="scenes/LightingFields.scene")
    scene = opened["scene"]["id"]
    sun = await sun_of(ed, scene)
    camera = await named(ed, scene, "Main Camera")
    body = await named(ed, scene, "Body")
    fields = {"contact": "l.contactShadows", "length": "l.contactShadowLength", "thickness": "l.contactShadowThickness",
              "samples": "l.contactShadowSampleCount", "mode": "c.transparencyMode", "OIT": "TransparencyMode.ORDER_INDEPENDENT",
              "scale": "c.oitWeightScale", "cast": "k.castShadows", "ONLY": "CapsuleCastShadows.CAPSULES_ONLY",
              "capsules": "k.capsuleCount", "radius": "k:getCapsuleRadius(0)", "enabled": "scene.capsuleShadowsEnabled",
              "intensity": "scene.capsuleShadowIntensity", "softness": "scene.capsuleShadowSoftness"}
    code = (f"local l = object({sun}, 'Light') local c = object({camera}, 'Camera') "
            f"local k = object({body}, 'CapsuleShadowCaster') local out = {{}} ")
    for key, expression in fields.items():
        code += (f"do local ok, value = pcall(function() return {expression} end) "
                 f"if ok then out['{key}'] = value else out['{key}'] = 'ERROR ' .. tostring(value) end end ")
    code += "return out"
    v = (await ed.call("eval_lua", scene=scene, allow_edit_mode=True, code=code))["values"][0]
    errors = {k: x for k, x in v.items() if isinstance(x, str) and x.startswith("ERROR")}
    ed.check(not errors, f"every S57 field reads in Lua: {errors}")
    ed.check_eq(v["contact"], True)
    ed.check(abs(v["length"] - 0.3) < 1e-5 and abs(v["thickness"] - 0.08) < 1e-5, f"contact values {v}")
    ed.check_eq(v["samples"], 16)
    ed.check_eq(v["mode"], v["OIT"])
    ed.check(abs(v["scale"] - 0.5) < 1e-5, f"weight scale {v}")
    ed.check_eq(v["cast"], v["ONLY"])
    ed.check_eq(v["capsules"], 2)
    ed.check(abs(v["radius"] - 0.3) < 1e-5, f"capsule radius {v}")
    ed.check(v["enabled"] is True and abs(v["intensity"] - 0.8) < 1e-5 and abs(v["softness"] - 0.4) < 1e-5, f"scene values {v}")
    project = await ed.call("get_project_settings", keys=["graphics.light_mode"])
    ed.check_eq(harness.find_key(project, "graphics.light_mode"), "tiled")

    # one undo step reverts a field
    await ed.call("set_properties", scene=scene, entity=sun, values={"light": {"contactShadowLength": 0.9}})
    await ed.call("undo")
    got = await ed.call("get_entity", scene=scene, entity=sun, components=["light"])
    ed.check(abs(harness.find_key(got, "contactShadowLength") - 0.3) < 1e-5, "undo reverts the contact length")
    await ed.call("set_properties", scene=scene, entity=camera, values={"camera": {"transparencyMode": "sorted"}})
    await ed.call("undo")
    got = await ed.call("get_entity", scene=scene, entity=camera, components=["camera"])
    ed.check_eq(harness.find_key(got, "transparencyMode"), "order_independent", "undo reverts the transparency mode")
    await ed.call("set_project_settings", values={"graphics.light_mode": "automatic"})


async def test_lighting_render_stats(ed: Editor):
    ed.require_tools("get_render_stats", "set_properties", "set_scene_settings", "set_project_settings")
    scene = await make_scene(ed, "LightingStats")
    sun = await sun_of(ed, scene)
    await entity(ed, scene, "plane", "Ground", [0, 0, 0], scale=[2, 1, 2])
    body = await entity(ed, scene, "cube", "Body", [0, 1, 0], scale=[0.6, 2, 0.4])
    for i in range(12):
        await entity(ed, scene, "point_light", f"Lamp{i}", [-5 + i, 0.6, (i % 3) - 1],
                     values={"light": {"range": 2.0, "intensity": 2.0}})
    await ed.call("set_scene_camera", scene=scene, position=[0, 8, 10], target=[0, 0, 0])

    # Per Object: no tiles; a scene of only suns would stay so even when Tiled
    await ed.call("set_project_settings", values={"graphics.light_mode": "per_object"})
    lighting = await lighting_stats(ed)
    ed.check_eq(lighting["light_mode"], "per_object")
    ed.check_eq(lighting["tiles"]["active"], False, "Per Object builds no tiles")
    ed.check_eq(lighting["contact_shadows"]["active"], False)
    ed.check_eq(lighting["oit"]["active"], False)

    # Tiled: the tiles list the twelve lamps and the sun
    await ed.call("set_project_settings", values={"graphics.light_mode": "tiled"})
    lighting = await lighting_stats(ed, 10)
    tiles = lighting["tiles"]
    ed.check_eq(tiles["active"], True, f"Tiled builds tiles: {lighting}")
    ed.check_eq(tiles.get("lights"), 13, f"the view's lights {tiles}")
    ed.check(tiles["max_per_tile"] >= 2 and tiles["average_per_tile"] >= 1.0, f"tile figures {tiles}")

    # contact shadows and capsule shadows on the shadowed sun
    await ed.call("set_properties", scene=scene, entity=sun, values={"light": {"shadows": True, "contactShadows": True}})
    await ed.call("add_component", scene=scene, entity=body, component="capsuleshadowcaster")
    await ed.call("set_properties", scene=scene, entity=body, values={"capsuleshadowcaster": {
        "capsules": [{"bone": "", "radius": 0.3, "length": 0.6}]}})
    await ed.call("set_scene_settings", scene=scene, settings={"capsule_shadows_enabled": True})
    lighting = await lighting_stats(ed, 10)
    for _ in range(60):
        if lighting["contact_shadows"]["width"] > 0:
            break
        lighting = await lighting_stats(ed, 10)
    ed.check_eq(lighting["capsule_shadows"]["active"], True, f"capsules {lighting}")
    ed.check_eq(lighting["capsule_shadows"]["capsules"], 1)
    ed.check_eq(lighting["contact_shadows"]["active"], True, f"contact {lighting}")
    ed.check(lighting["contact_shadows"]["width"] > 0, f"the contact mask {lighting}")
    ed.check("passes" in lighting, "the lighting passes' times")
    await ed.call("set_project_settings", values={"graphics.light_mode": "automatic"})


# ---- S99 W7: Extract Sun and the Gradient ambient (AC7)

def _face_direction(face: int, u: float, v: float) -> tuple[float, float, float]:
    """the engine's cube face convention (IBLEnvironment): 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z"""
    x, y, z = [(1.0, -v, -u), (-1.0, -v, u), (u, 1.0, v), (u, -1.0, -v), (u, -v, 1.0), (-u, -v, -1.0)][face]
    length = math.sqrt(x * x + y * y + z * z)
    return x / length, y / length, z / length


def _write_sun_cross(path, sun: tuple[float, float, float], tile: int = 64, radius_degrees: float = 2.0,
                     radiance: float = 3000.0) -> None:
    """a 4:3 cube cross .hdr (flat RGBE scanlines): a blue sky over a dark ground and a sun disc"""
    cos_radius = math.cos(math.radians(radius_degrees))
    width, height = tile * 4, tile * 3
    pixels = [[(0.0, 0.0, 0.0)] * width for _ in range(height)]
    tiles = [(2, 1), (0, 1), (1, 0), (1, 2), (1, 1), (3, 1)]
    for face, (tx, ty) in enumerate(tiles):
        for y in range(tile):
            for x in range(tile):
                total = [0.0, 0.0, 0.0]
                for s in range(9):
                    u = 2.0 * (x + (s % 3 + 0.5) / 3.0) / tile - 1.0
                    v = 2.0 * (y + (s // 3 + 0.5) / 3.0) / tile - 1.0
                    d = _face_direction(face, u, v)
                    if d[0] * sun[0] + d[1] * sun[1] + d[2] * sun[2] >= cos_radius:
                        c = (radiance, radiance * 0.97, radiance * 0.9)
                    elif d[1] >= 0.0:
                        c = (0.3 * (0.6 + 0.4 * d[1]), 0.5 * (0.6 + 0.4 * d[1]), 0.9 * (0.6 + 0.4 * d[1]))
                    else:
                        c = (0.12, 0.1, 0.08)
                    for k in range(3):
                        total[k] += c[k] / 9.0
                pixels[ty * tile + y][tx * tile + x] = tuple(total)
    out = bytearray(f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n".encode("ascii"))
    for row in pixels:
        for r, g, b in row:
            v = max(r, g, b)
            if v < 1e-32:
                out += bytes(4)
                continue
            mantissa, exponent = math.frexp(v)
            scale = mantissa * 256.0 / v
            out += bytes([int(r * scale), int(g * scale), int(b * scale), exponent + 128])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))


def _angle_degrees(a, b) -> float:
    dot = sum(x * y for x, y in zip(a, b)) / (math.sqrt(sum(x * x for x in a)) * math.sqrt(sum(y * y for y in b)))
    return math.degrees(math.acos(max(-1.0, min(1.0, dot))))


async def test_extract_sun_creates_and_aligns_the_main_light(ed: Editor):
    """set_scene_settings extract_sun: the sun of the Skybox's HDR cross creates the main Directional
    Light (none in the scene) pointing from it, one undo step; then aligns an existing light in lux,
    with Remove Sun Disc on the sky's import settings in the same undo step. A sky without a file
    (the default sky) is refused naming the fix."""
    elevation, azimuth = math.radians(40.0), math.radians(30.0)
    sun = (math.cos(elevation) * math.sin(azimuth), math.sin(elevation), math.cos(elevation) * math.cos(azimuth))
    _write_sun_cross(ed.project / "assets/sky/sun_cross.hdr", sun)
    await ed.call("sync_files", paths=["assets/sky/sun_cross.hdr"])
    scene = await make_scene(ed, "ExtractSun")
    sky = await named(ed, scene, "Skybox")
    await ed.check_error("set_scene_settings", scene=scene, extract_sun={}, contains="HDR")
    await ed.check_error("set_scene_settings", scene=scene, extract_sun={"remove_disc": True}, contains="remove_sun_disc")

    await ed.call("delete_entities", scene=scene, entity=await sun_of(ed, scene))
    await ed.call("set_properties", scene=scene, entity=sky, values={"skybox": {"texture": {"cube": "assets/sky/sun_cross.hdr"}}})
    made = await ed.call("set_scene_settings", scene=scene, extract_sun={})
    result = made["sun"]
    ed.check(result["created"], f"a light was created: {result}")
    ed.check(result["peak_ratio"] >= 20.0, f"a distinct sun: {result}")
    ed.check(_angle_degrees(result["direction"], [-c for c in sun]) < 1.5, f"the light points from the sun: {result}")
    ed.check(result["intensity"] > 0.0 and result["unit"] == "engine", f"an intensity: {result}")
    ed.check("remove_sun_disc" in made.get("next", ""), f"the next step names Remove Sun Disc: {made}")
    light = harness.entity_id_of(result["light"]) if isinstance(result["light"], dict) else result["light"]
    got = await ed.call("get_entity", scene=scene, entity=light, components=["light"])
    ed.check(_angle_degrees(harness.find_key(got, "direction"), [-c for c in sun]) < 1.5, f"the light's direction: {got}")
    ed.check(harness.find_key(got, "shadows") is True, f"the sun casts shadows: {got}")

    await ed.call("undo", scene=scene, scope="timeline")
    ed.check_eq(harness.find_entities(await ed.call("find_entities", scene=scene, component="light")), [], "undo removes the created light")

    # an existing light in lux, aligned (not a second one), the sky's disc removal in the same step
    lamp = await entity(ed, scene, "directional_light", "Key Light", [0, 5, 0])
    await ed.call("set_light_unit", scene=scene, entity=lamp, unit="lux")
    before = harness.find_key(await ed.call("get_entity", scene=scene, entity=lamp, components=["light"]), "direction")
    aligned = (await ed.call("set_scene_settings", scene=scene, extract_sun={"remove_sun_disc": True}))["sun"]
    ed.check(not aligned["created"], f"the existing light is aligned: {aligned}")
    ed.check_eq(aligned["unit"], "lux", "a light in lux keeps lux")
    ed.check(_angle_degrees(aligned["direction"], [-c for c in sun]) < 1.5, f"aligned to the sun: {aligned}")
    ed.check(aligned["remove_sun_disc"] and aligned["remove_sun_disc"][0].endswith("sun_cross.hdr"), f"Remove Sun Disc set: {aligned}")
    meta = ed.project / "assets/sky/sun_cross.hdr.meta"
    ed.check("removeSunDisc: true" in meta.read_text(), "the import setting is written")
    lights = harness.find_entities(await ed.call("find_entities", scene=scene, component="light"))
    ed.check_eq(len(lights), 1, "no second light")

    await ed.call("undo", scene=scene, scope="timeline")
    restored = harness.find_key(await ed.call("get_entity", scene=scene, entity=lamp, components=["light"]), "direction")
    ed.check(_angle_degrees(restored, before) < 0.01, f"undo restores the light's direction: {restored} {before}")
    ed.check("removeSunDisc" not in (meta.read_text() if meta.exists() else ""), "one undo step removes the import setting too")


async def test_gradient_ambient_fields_and_lua(ed: Editor):
    """Environment Lighting Source Gradient through set_scene_settings: saved only away from the
    defaults, read back, readable and settable from Lua, one undo step."""
    scene = await make_scene(ed, "GradientAmbient")
    initial = await ed.call("get_scene_settings", scene=scene, names=["ambient_mode"])
    ed.check_eq(harness.find_key(initial, "ambient_mode"), "color", "the default source is Color")
    await ed.call("set_scene_settings", scene=scene, settings={
        "ambient_mode": "gradient", "ambient_sky_color": [0.2, 0.4, 0.9], "ambient_ground_color": "#804020"})
    got = await ed.call("get_scene_settings", scene=scene, names=["ambient_mode", "ambient_sky_color", "ambient_ground_color"])
    ed.check_eq(harness.find_key(got, "ambient_mode"), "gradient")
    sky = harness.find_key(got, "ambient_sky_color")
    ed.check(abs(sky[2] - 0.9) < 1e-3, f"sky color read back: {sky}")
    await ed.call("save_scene", scene=scene, path="scenes/GradientAmbient.scene")
    text = (ed.project / "scenes/GradientAmbient.scene").read_text()
    ed.check("ambientMode: gradient" in text and "ambientSkyColor" in text, "the scene file has the gradient")
    ed.check("ambientEquatorColor" not in text, "a default colour is not written")
    await ed.check_error("set_scene_settings", scene=scene, settings={"ambient_mode": "colour"}, contains="gradient")

    # Lua reads and writes them (scene.ambientMode, scene.ambient*Color, sRGB)
    code = ("local out = {source = scene.ambientMode == AmbientMode.GRADIENT, sky = scene.ambientSkyColor.z} "
            "scene.ambientEquatorColor = Vector3(0.5, 0.25, 0.125) scene.ambientMode = AmbientMode.COLOR "
            "out.equator = scene.ambientEquatorColor.y out.after = scene.ambientMode == AmbientMode.COLOR return out")
    v = (await ed.call("eval_lua", scene=scene, allow_edit_mode=True, code=code))["values"][0]
    ed.check(v["source"] is True and abs(v["sky"] - 0.9) < 1e-3, f"Lua reads the gradient: {v}")
    ed.check(abs(v["equator"] - 0.25) < 1e-3 and v["after"] is True, f"Lua sets the gradient: {v}")
    await ed.call("set_scene_settings", scene=scene, settings={"ambient_mode": "gradient"})

    await ed.call("undo", scene=scene)
    ed.check_eq(harness.find_key(await ed.call("get_scene_settings", scene=scene, names=["ambient_mode"]), "ambient_mode"),
                "color", "one undo step reverts the source")
