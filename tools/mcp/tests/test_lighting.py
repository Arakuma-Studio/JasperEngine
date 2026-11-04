"""S57's lighting (docs/specs/S57-forge-lighting.md AC6): the new light, camera, material, scene,
project and Capsule Shadow Caster fields through set_properties / set_scene_settings /
set_project_settings, saved, reloaded and read in Lua, one undo step each; and get_render_stats'
`lighting` figures of each feature while it draws (the tiles, the capsules, the contact mask, OIT)."""

from __future__ import annotations

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
