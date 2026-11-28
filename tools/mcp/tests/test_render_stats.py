"""Draw submission (spec S55, docs/mcp/guide/meshes.md): get_render_stats' counters by kind of pass
(R7, AC7), GPU Instancing on and off through the project settings (R10, R11: undoable), a
Mesh's Allow GPU Instancing (set_properties), and
Lua's Scene:getRenderStats() and Mesh.allowGpuInstancing (R12) in Play. Culling
(culling-group-and-simulation-distance AC20-AC22): the `culling` block, the Game view's
culling_debug drawing and the culling monitors."""

from __future__ import annotations

import shutil

import harness
import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "RenderStats"}
TIMEOUT = 900

S: dict = {}

# a 117-vertex sphere: Models of one file share its buffers, which GPU Instancing batches
SPHERE = harness.REPO / "engine" / "tests" / "core" / "fixtures" / "gltf" / "instancing" / "sphere.gltf"

NEW_KEYS = ("main_triangles", "shadow_triangles", "draw_calls_by_pass", "instanced_draws", "batched_instances",
            "uniform_bytes_kept", "visible", "culled", "uniform_bytes", "buffer_upload_bytes")


async def spheres(ed: Editor) -> int:
    """A 3D scene of 16 Models of one glTF sphere (over the 64 vertices dynamic batching takes) on a
    floor, under the default sun (it casts shadows), seen by the game camera."""
    if "scene" in S:
        return S["scene"]
    (ed.project / "assets" / "models").mkdir(parents=True, exist_ok=True)
    shutil.copy(SPHERE, ed.project / "assets" / "models" / "sphere.gltf")
    made = await ed.call("create_scene", name="Spheres", type="3D")
    scene = made["scene"]["id"]
    S["balls"] = []
    for i in range(16):
        # each Model draws its sphere itself (no Preserve Hierarchy, which a new project places with): the
        # tests set the Mesh of the Model's own GameObject
        ball = await ed.call("create_entity", scene=scene, type="model", model="assets/models/sphere.gltf", name=f"Ball{i}",
                             position=[(i % 4) * 2 - 3, 0.5, (i // 4) * 2 - 3], select=False,
                             values={"model": {"preserveHierarchy": False}})
        S["balls"].append(ball["entity_id"])
    await ed.call("create_entity", scene=scene, type="plane", name="Floor", scale=[20, 1, 20], select=False)
    # a still scene's cached shadow slots draw nothing, and settled() waits for a still scene
    lights = harness.find_entities(await ed.call("find_entities", scene=scene, component="light"))
    await ed.call("set_properties", scene=scene, entity=harness.entity_id_of(lights[0]),
                  values={"light": {"shadowUpdateMode": "every_frame"}})
    await ed.call("wait", frames=30)
    S["scene"] = scene
    return scene


async def settled(ed: Editor) -> dict:
    """The game frame's stats once the counters hold still (the meshes loaded and their batch
    variants built)."""
    last = None
    for _ in range(60):
        await ed.call("wait", frames=5)
        stats = await ed.call("get_render_stats", view="game")
        key = (stats["draw_calls"], stats["instanced_draws"], stats["triangles"])
        if stats["drawn"] and key == last:
            return stats
        last = key
    return stats


async def test_new_counters(ed: Editor):
    scene = await spheres(ed)
    await ed.call("play", action="start", scene=scene)
    try:
        stats = await settled(ed)
        for key in NEW_KEYS:
            ed.check(key in stats, f"get_render_stats has {key}")
        by_pass = stats["draw_calls_by_pass"]
        ed.check(set(by_pass) == {"main", "shadow", "depth", "other"}, f"draw calls by pass: {by_pass}")
        ed.check(sum(by_pass.values()) <= stats["draw_calls"], f"the passes' draws are within the frame's: {by_pass} {stats['draw_calls']}")
        ed.check(by_pass["main"] >= 1 and by_pass["shadow"] >= 1, f"the camera and the shadow maps draw: {by_pass}")
        ed.check(stats["main_triangles"] > 0 and stats["shadow_triangles"] > 0, "triangles of the main view and the shadows")
        ed.check(stats["main_triangles"] + stats["shadow_triangles"] <= stats["triangles"], "triangles keeps every pass")
        # GPU Instancing is on by default: the 16 spheres batch in the colour pass and the shadow maps
        ed.check(stats["instanced_draws"] >= 1, f"instanced draws: {stats['instanced_draws']}")
        ed.check(stats["batched_instances"] >= 16, f"the spheres are instances: {stats['batched_instances']}")
        ed.check(stats["visible"] >= 17, f"the visible lists keep the meshes: {stats['visible']}")
        summary = (await ed.call_raw("get_render_stats", {"view": "game"})).text
        ed.check(" main, " in summary and " shadow; " in summary and " instanced)" in summary, f"the summary splits the draws: {summary}")
        S["on"] = stats

        # Lua's view of the same frame
        lua = (await ed.call("eval_lua", code="local s = scene:getRenderStats() return s.mainDrawCalls, s.instancedDraws, "
                                                "s.shadowTriangles > 0, object('Ball0', 'Mesh').allowGpuInstancing"))["values"]
        ed.check(lua[0] >= 1 and lua[1] >= 1 and lua[2] is True and lua[3] is True, f"Scene:getRenderStats and Mesh.allowGpuInstancing: {lua}")
    finally:
        await ed.call("play", action="stop")


async def test_gpu_instancing_setting(ed: Editor):
    scene = await spheres(ed)
    got = await ed.call("get_project_settings", keys=["graphics.gpu_instancing"])
    ed.check("gpu_instancing" in str(got), f"the key is listed: {got}")

    await ed.call("set_project_settings", values={"graphics.gpu_instancing": False})
    await ed.call("play", action="start", scene=scene)
    try:
        off = await settled(ed)
    finally:
        await ed.call("play", action="stop")
    ed.check_eq(off["instanced_draws"], 0, "off: no instanced draw")
    ed.check(off["draw_calls_by_pass"]["main"] >= 16, f"off: each sphere draws alone: {off['draw_calls_by_pass']}")
    yaml = (ed.project / "project.yaml").read_text()
    ed.check("gpuInstancing: false" in yaml, "the setting saved to project.yaml")

    # undoable (undo scope project), back on
    await ed.call("undo", scope="project")
    got = await ed.call("get_project_settings", keys=["graphics.gpu_instancing"])
    ed.check("True" in str(got) or "true" in str(got), f"undo turned it back on: {got}")
    ed.check("gpuInstancing" not in (ed.project / "project.yaml").read_text(), "the default is not written")


async def test_mesh_opt_out(ed: Editor):
    scene = await spheres(ed)
    balls = S["balls"]
    for ball in balls:
        await ed.call("set_properties", scene=scene, entity=ball, values={"mesh": {"allowGpuInstancing": False}})
    await ed.call("play", action="start", scene=scene)
    try:
        stats = await settled(ed)
    finally:
        await ed.call("play", action="stop")
    ed.check_eq(stats["instanced_draws"], 0, "a Mesh that does not allow it draws alone")
    for ball in balls:
        await ed.call("set_properties", scene=scene, entity=ball, values={"mesh": {"allowGpuInstancing": True}})


async def culling_scene(ed: Editor) -> int:
    """A camera at z 8 looking at the origin; an enemy 8 m away and two 48 m away, each with a
    Simulation Culling of 20 m (no visibility exception, no ground rule)."""
    if "culling" in S:
        return S["culling"]
    scene = (await ed.call("create_scene", name="Culling", type="3D"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="camera", name="Main Camera", position=[0, 2, 8], select=False)
    await ed.call("set_main_camera", scene=scene, entity="Main Camera")
    for name, position in (("Enemy", [0, 0.5, 0]), ("Enemy (1)", [0, 0.5, -40]), ("Enemy (2)", [3, 0.5, -40])):
        await ed.call("create_entity", scene=scene, type="box", name=name, position=position, select=False)
        await ed.call("add_component", scene=scene, entity=name, component="simulationculling",
                      values={"simulationculling": {"simulationDistance": 20, "boundsRadius": 1.5, "simulateWhenVisible": False,
                                                    "freezeWhenGroundNotLoaded": False}})
    S["culling"] = scene
    return scene


# a Lua group of three 1 m spheres 2, 13 and 68 m from the camera (surface): bands 0, 1, 2 of {5, 30}
GROUP_LUA = ("cullingTest = CullingGroup(scene); cullingTest:setBoundingDistances({5, 30}); "
             "cullingTest:setBoundingSpheres({BoundingSphere(Vector3(0, 2, 5), 1), BoundingSphere(Vector3(0, 2, -6), 1), "
             "BoundingSphere(Vector3(0, 2, -61), 1)}); return cullingTest:isValid()")


async def test_culling_block(ed: Editor):
    scene = await culling_scene(ed)
    plain = (await ed.call("create_scene", name="NoCulling", type="3D"))["scene"]["id"]
    await ed.call("play", action="start", scene=plain, fixed_fps=60)
    try:
        await ed.call("wait", frames=5)
        stats = await ed.call("get_render_stats", view="game")
        ed.check("culling" not in stats, f"no group and no component: no culling block: {stats.get('culling')}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0)

    await ed.call("play", action="start", scene=scene, fixed_fps=60)
    try:
        made = (await ed.call("eval_lua", code=GROUP_LUA))["values"]
        ed.check(made[0] is True, f"the Lua group: {made}")
        await ed.call("wait", frames=3)
        culling = (await ed.call("get_render_stats", view="game"))["culling"]
        ed.check_eq(culling["simulation"]["components"], 3, f"{culling}")
        ed.check_eq(culling["simulation"]["culled"], 2, f"the two beyond 20 m: {culling}")
        ed.check_eq(culling["groups"], 1, f"{culling}")
        ed.check_eq(culling["per_band"], [1, 1, 1], f"the group's bands: {culling}")
        far = await ed.call("get_runtime_entity", entity="Enemy (1)", include_script_state=False)
        component = harness.find_key(far, "simulationculling")
        ed.check(component["state"] == "culled" and component["reason"] == "distance", f"{component}")

        # the camera beside the far ones: they run, the near one is culled, after one frame
        await ed.call("set_runtime_property", entity="Main Camera", component="transform", property="position", value=[0, 2, -30])
        await ed.call("wait", frames=1)
        moved = (await ed.call("get_render_stats", view="game"))["culling"]
        ed.check_eq(moved["simulation"]["culled"], 1, f"{moved}")
        near = harness.find_key(await ed.call("get_runtime_entity", entity="Enemy", include_script_state=False), "simulationculling")
        ed.check_eq(near["state"], "culled", f"{near}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0)


async def test_culling_debug_screenshot(ed: Editor):
    scene = await culling_scene(ed)
    await ed.call("play", action="start", scene=scene, fixed_fps=60)
    try:
        await ed.call("eval_lua", code=GROUP_LUA)
        await ed.call("wait", frames=5)
        off = await ed.call_raw("screenshot", {"view": "game", "max_width": 640})
        ed.check(not off.is_error and off.images, off.text)
        view = await ed.call("configure_game_view", culling_debug=True)
        ed.check(view["culling_debug"] is True, f"{view}")
        await ed.call("wait", frames=3)
        on = await ed.call_raw("screenshot", {"view": "game", "max_width": 640})
        ed.check(not on.is_error and on.images, on.text)
        w, h, a = pngdiff.decode(off.images[0][0])
        w2, h2, b = pngdiff.decode(on.images[0][0])
        ed.check_eq((w, h), (w2, h2), "same size")
        changed = [i // 4 for i in range(0, len(a), 4) if max(abs(a[i + c] - b[i + c]) for c in range(3)) > 24]
        ed.check(len(changed) > 50, f"the spheres draw: {len(changed)} pixels changed")
        # around the view's centre, where the enemy at the origin and the group's spheres project;
        # the corners keep the sky
        xs = [p % w for p in changed]
        ys = [p // w for p in changed]
        ed.check(min(xs) > 0 or max(xs) < w - 1 or min(ys) > 0, "the lines are where the spheres are, not everywhere")
    finally:
        await ed.call("configure_game_view", culling_debug=False)
        await ed.call("play", action="stop", fixed_fps=0)


async def test_culling_monitors(ed: Editor):
    scene = await culling_scene(ed)
    await ed.call("play", action="start", scene=scene, fixed_fps=60)
    try:
        await ed.call("wait", frames=10)
        r = await ed.call("get_frame_times", window=5, monitors=["Simulation culled", "Script updates skipped", "Culling ms"])
        monitors = r["monitors"]
        ed.check("Simulation culled" in monitors, f"{sorted(monitors)}")
        ed.check_eq(monitors["Simulation culled"]["latest"], 2, f"{monitors['Simulation culled']}")
        ed.check("Script updates skipped" in monitors and monitors["Culling ms"]["unit"] == "ms", f"{monitors}")
        stats = (await ed.call("eval_lua", code="local s = DebugOverlay.getStats() return s.simulationCulled, s.simulationFull, "
                                                "s.cullingGroups, s.scriptUpdatesSkipped ~= nil"))["values"]
        ed.check(stats[0] == 2 and stats[1] == 1 and stats[2] == 0 and stats[3] is True, f"DebugOverlay.getStats: {stats}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0)
