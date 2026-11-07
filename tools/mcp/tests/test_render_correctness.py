"""render-correctness (docs/superpowers/specs/2026-11-04-render-correctness-design.md): the
agent surfaces of the new controls.

- Project Settings > Quality > Foliage Distance per platform group (set/get_project_settings,
  range 0-10), the runtime Engine.foliageDistanceScale in Play (the desktop default, a script's
  override, reset when Play stops);
- Instanced Mesh Near Fade Start/End and a foliage layer's Min Draw Distance through
  set_properties, with undo;
- get_render_stats' gpu.robust_buffer_access (null off Vulkan).
"""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Render Correctness Test"}


async def foliage_scale(ed: Editor) -> float:
    return (await ed.call("eval_lua", code="return Engine.foliageDistanceScale"))["values"][0]


async def test_foliage_distance_settings(ed: Editor):
    keys = ["quality.foliage_distance_desktop", "quality.foliage_distance_mobile", "quality.foliage_distance_web"]
    values = (await ed.call("get_project_settings", keys=keys))["settings"]
    ed.check_eq([values[k] for k in keys], [1, 1, 1], "Foliage Distance defaults to 1 on every platform group")

    await ed.call("set_project_settings", values={"quality.foliage_distance_desktop": 0.75, "quality.foliage_distance_mobile": 0.5})
    values = (await ed.call("get_project_settings", keys=keys))["settings"]
    ed.check_eq([values[k] for k in keys], [0.75, 0.5, 1], "the scales set")
    refused = await ed.call_raw("set_project_settings", {"values": {"quality.foliage_distance_web": 11}})
    ed.check(refused.is_error, f"a scale above 10 is refused: {refused.text[:200]}")

    await ed.call("create_entity", type="cube")
    await ed.call("play", action="start")
    try:
        ed.check_eq(await foliage_scale(ed), 0.75, "Play runs the desktop scale")
        await ed.call("eval_lua", code="Engine.foliageDistanceScale = 0.25")
        ed.check_eq(await foliage_scale(ed), 0.25, "a script overrides it")
    finally:
        await ed.call("play", action="stop")
    await ed.call("play", action="start")
    try:
        ed.check_eq(await foliage_scale(ed), 0.75, "the override ends with Play")
    finally:
        await ed.call("play", action="stop")


async def test_near_fade_properties(ed: Editor):
    scene = (await ed.call("create_scene", name="NearFade", type="3D", path="scenes/NearFade"))["scene"]["id"]
    batch = (await ed.call("create_entity", scene=scene, type="cube", name="Grass"))["entity_id"]
    await ed.call("add_component", scene=scene, entity=batch, component="instancedmesh",
                  values={"instancedmesh": {"distanceFade": True, "fadeStart": 30, "fadeEnd": 40}})
    await ed.call("set_properties", scene=scene, entity=batch, values={"instancedmesh": {"nearFadeStart": 8, "nearFadeEnd": 10}})
    now = (await ed.call("get_entity", scene=scene, entity=batch, properties=["nearFadeStart", "nearFadeEnd"]))["components"]["instancedmesh"]
    ed.check_eq((now["nearFadeStart"], now["nearFadeEnd"]), (8, 10), "the near fade set")
    await ed.call("undo", scene=scene)
    now = (await ed.call("get_entity", scene=scene, entity=batch, properties=["nearFadeStart", "nearFadeEnd"]))["components"]["instancedmesh"]
    ed.check_eq((now["nearFadeStart"], now["nearFadeEnd"]), (0, 0), "one undo restores the near fade")
    refused = await ed.call_raw("set_properties", {"scene": scene, "entity": batch, "values": {"instancedmesh": {"nearFadeStart": -1}}})
    ed.check(refused.is_error, f"a negative near fade is refused: {refused.text[:200]}")


async def test_foliage_min_draw_distance(ed: Editor):
    scene = (await ed.call("create_scene", name="Foliage", type="3D", path="scenes/Foliage"))["scene"]["id"]
    terrain = (await ed.call("create_entity", scene=scene, type="terrain"))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=terrain,
                  values={"terrain": {"foliageLayers": [{"drawDistance": 300, "minDrawDistance": 40}]}})
    layers = (await ed.call("get_entity", scene=scene, entity=terrain, properties=["foliageLayers"]))["components"]["terrain"]["foliageLayers"]
    ed.check_eq((layers[0]["drawDistance"], layers[0]["minDrawDistance"]), (300, 40), f"the layer's draw range: {layers}")
    await ed.call("undo", scene=scene)
    layers = (await ed.call("get_entity", scene=scene, entity=terrain, properties=["foliageLayers"]))["components"]["terrain"].get("foliageLayers")
    ed.check_eq(layers or [], [], "one undo removes the layer")


async def test_render_stats_robustness_field(ed: Editor):
    stats = await ed.call("get_render_stats")
    ed.check_in("robust_buffer_access", stats["gpu"])
    if stats["gpu"].get("backend") != "Vulkan":
        ed.check_eq(stats["gpu"]["robust_buffer_access"], None, "robust buffer access is a Vulkan device's")
    else:
        ed.check(isinstance(stats["gpu"]["robust_buffer_access"], bool), f"the Vulkan decision: {stats['gpu']}")
