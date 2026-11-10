"""library:water beyond the shader: the GameObject menu's 3D Object > Water (create_entity
type=water: the grid, the material preset and the underwater pass it adds, one undo step),
the Rigidbody buoyancy fields, buoyancy in Play (a box floats, one without buoyancy sinks), the
Lua surface queries (Water.getHeight / getNormal / isUnderwater, the same waves as the
shader), the automatic library:underwater pass (on under the surface, off above, the surface
height and colours written into it), screenshots of the water from above and below
(screenshots/water_above.png, water_below.png, water_up.png in the scratch project), and that a
library:water material without textures keeps the flat, grey defaults it had (no ripples:
the preset's BUILTIN_TEXTURES alone picks the engine's ripples and noise). The water-reflections
spec: the waves hold while Play is paused or the time scale is 0 and follow a stopped scene's
Always Refresh, a fork with @role water (made by hand or by fork_shader source) keeps the
buoyancy, the queries and the underwater switch, and the planar reflection settings (undoable,
in get_render_stats, a screenshot of the reflection)."""

from __future__ import annotations

import asyncio
from pathlib import Path

from harness import Editor, find_key

from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "WaterTest"}
CI = True
TIMEOUT = 900

SCENE = "scenes/Water.scene"
REPO = Path(__file__).resolve().parents[3]

M = "submeshes[0].material."


def passes_of(settings) -> list:
    found = find_key(settings, "post_process")
    return found if isinstance(found, list) else []


async def lua(ed: Editor, code: str, edit: bool = False):
    """Lua's return values; edit=True runs it against the stopped scene."""
    extra = {"allow_edit_mode": True} if edit else {}
    return (await ed.call("eval_lua", code=code, **extra))["values"]


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
    ed.check(abs(level) <= 0.194 + 1e-3, f"the surface height written: {level!r}")
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


def surface(sample: dict) -> float:
    return sample["samples"][0]["surface_height"]


async def test_transparent_under_water_screenshot(ed: Editor):
    """A see-through red box under the refracting water shows through it, tinted
    (screenshots/water_glass_under.png; water-reflections F2, F3)."""
    made = await ed.call("create_entity", type="cube", name="Glass", position=[1.5, -1.2, 2.0])
    glass = find_key(made, "entity_id", "id")
    await ed.call("set_properties", entity=glass, values={"mesh": {
        M + "shader": "library:unlit", M + "blendMode": "alpha",
        M + "shaderParams": {"baseColor": [1.0, 0.1, 0.1, 0.5]}}})
    shot = await ed.call_raw("screenshot", {"view": "game", "save_path": "screenshots/water_glass_under.png"})
    ed.check(shot.images, "screenshot of the box under the water")
    await ed.call("delete_entities", entities=[glass])


async def test_waves_hold_while_paused(ed: Editor):
    """The waves run on the scene clock: Pause and time scale 0 hold them with the bodies, time
    scale 0.5 halves them (water-reflections F9)."""
    point = {"points": [[1.5, 0, 2.5]]}
    await ed.call("play", action="start")
    try:
        await ed.call("wait", frames=10)
        await ed.call("play", action="pause")
        a = await ed.call("sample_water", **point)
        await ed.call("wait", seconds=0.5)
        b = await ed.call("sample_water", **point)
        ed.check(b["shader_time"] > a["shader_time"], f"the editor drew frames meanwhile: {a['shader_time']} {b['shader_time']}")
        ed.check_eq(surface(b), surface(a), "paused: the surface holds")
        ed.check_eq(b.get("water_time"), a.get("water_time"), "paused: the water clock holds")

        await ed.call("play", action="resume")
        await ed.call("wait", frames=20)
        c = await ed.call("sample_water", **point)
        ed.check(c["water_time"] > b["water_time"], f"resumed: the clock runs again: {b['water_time']} {c['water_time']}")

        await ed.call("play", action="set", time_scale=0)
        await ed.call("wait", frames=2)
        d = await ed.call("sample_water", **point)
        await ed.call("wait", frames=20)
        e = await ed.call("sample_water", **point)
        ed.check_eq(surface(e), surface(d), "time scale 0: the surface holds")
        ed.check_eq(e["water_time"], d["water_time"], "time scale 0: the water clock holds")

        await ed.call("play", action="set", time_scale=0.5)
        await ed.call("wait", frames=2)
        before = await lua(ed, "return Water.getTime(scene), Time.unscaledTime")
        await ed.call("wait", frames=60)
        after = await lua(ed, "return Water.getTime(scene), Time.unscaledTime")
        ratio = (after[0] - before[0]) / max(after[1] - before[1], 1e-6)
        ed.check(0.4 < ratio < 0.6, f"time scale 0.5: the water clock runs at half speed ({ratio:.3f})")
    finally:
        await ed.call("play", action="set", time_scale=1)
        await ed.call("play", action="stop")


async def test_edit_mode_animate_water(ed: Editor):
    """A stopped scene's clock follows the Scene view's Always Refresh (on by default), frame by
    frame of the Scene view (a headless editor draws one for each screenshot)."""
    async def scene_frames():
        await ed.call_raw("screenshot", {"view": "scene"})
        await asyncio.sleep(0.5)
        await ed.call_raw("screenshot", {"view": "scene"})

    first = (await lua(ed, "return Water.getTime(scene)", edit=True))[0]
    await scene_frames()
    second = (await lua(ed, "return Water.getTime(scene)", edit=True))[0]
    ed.check(second > first, f"Always Refresh on: the clock moves in edit mode ({first} {second})")
    await ed.call("set_scene_camera", display={"animate_water": False})
    try:
        held = (await lua(ed, "return Water.getTime(scene)", edit=True))[0]
        await scene_frames()
        still = (await lua(ed, "return Water.getTime(scene)", edit=True))[0]
        ed.check_eq(still, held, "Always Refresh off: the clock holds")
    finally:
        await ed.call("set_scene_camera", display={"animate_water": True})


async def test_play_restarts_scene_clock(ed: Editor):
    """Play starts the scene clock at 0, as an exported game does, whatever the edit-mode clock
    ran to (spec 7)."""
    point = {"points": [[1.5, 0, 2.5]]}
    edit = (await ed.call("sample_water", **point))["water_time"]
    for _ in range(30):
        if edit > 3.0:
            break
        await ed.call_raw("screenshot", {"view": "scene"})
        await asyncio.sleep(0.3)
        edit = (await ed.call("sample_water", **point))["water_time"]
    ed.check(edit > 3.0, f"the edit-mode clock ran: {edit}")
    await ed.call("play", action="start")
    try:
        await ed.call("wait", frames=2)
        played = (await ed.call("sample_water", **point))["water_time"]
        ed.check(played < 2.0, f"Play restarts the scene clock: {played} after an edit-mode {edit}")
    finally:
        await ed.call("play", action="stop")


def with_role(source: str, shader_line: str, role: str) -> str:
    """A library entry file as a hand-made fork has it, with the @role line (already there in this
    engine's library)."""
    if f"@role {role}" in source:
        return source
    return source.replace(shader_line, f"{shader_line}\n// @role {role}", 1)


async def test_fork_keeps_buoyancy_queries_underwater(ed: Editor):
    """A copy of library:water and library:underwater with @role (written by hand) keeps the engine's
    water: the crate floats, Water.getHeight answers, the underwater pass switches (water-reflections F10)."""
    ed.require_tools("write_file", "set_properties", "set_scene_settings", "play", "eval_lua", "wait")
    library = REPO / "engine" / "shaderlib" / "library"
    await ed.call("write_file", path="shaders/HandWater.frag",
                  content=with_role((library / "water.frag").read_text(), "// @shader mesh", "water"))
    await ed.call("write_file", path="shaders/HandWater.vert", content=(library / "water.vert").read_text())
    await ed.call("write_file", path="shaders/HandUnderwater.frag",
                  content=with_role((library / "underwater.frag").read_text(), "// @shader postprocess", "underwater"))
    await ed.call("write_file", path="shaders/HandUnderwater.vert",
                  content=(REPO / "engine" / "shaderlib" / "fullscreen.vert").read_text())
    chain = passes_of(await ed.call("get_scene_settings", names=["post_process"]))
    try:
        await ed.call("set_properties", entity="Water", values={"mesh": {M + "shader": "shaders/HandWater"}})
        chain[0]["shader"] = "shaders/HandUnderwater"
        await ed.call("set_scene_settings", settings={"post_process": chain})
        await ed.call("play", action="start")
        await ed.call("wait", game_seconds=5)
        crate = await ed.call("get_runtime_entity", entity="Crate", components=["transform"])
        crate_y = find_key(crate, "position")[1]
        ed.check(crate_y > -0.7, f"the crate floats in the fork: y {crate_y}")
        height = (await lua(ed, "return Water.getHeight(scene, 1.0, 2.0)"))[0]
        ed.check(isinstance(height, (int, float)), f"Water.getHeight answers over the fork: {height!r}")
        await lua(ed, "local cam = object('View'); cam.position = Vector3(0, -1.5, 6); cam:setRotation(15, 0, 0); return true")
        await ed.call("wait", frames=5)
        on = (await lua(ed, "return scene:isPostProcessPassEnabled(0)"))[0]
        ed.check_eq(on, True, "the forked underwater pass switches on under the forked water")
    finally:
        await ed.call("play", action="stop")
        await ed.call("set_properties", entity="Water", values={"mesh": {M + "shader": "library:water"}})
        chain[0]["shader"] = "library:underwater"
        await ed.call("set_scene_settings", settings={"post_process": chain})


async def test_fork_shader_from_library(ed: Editor):
    """fork_shader source=library:water target=material: the files with @role, the material on them,
    features kept, one undo step; check stays quiet."""
    before = (await ed.call("get_entity", entity="Water", properties=["submeshes[0].material.*"]))["components"]["mesh"]
    features = before.get(M + "shaderFeatures") or []
    ed.check(features, f"the water has features to keep: {before!r}")
    made = await ed.call("fork_shader", source="library:water", target="material", entity="Water", name="ForkedWater")
    base = made["custom_shader"]
    ed.check_eq(made.get("submeshes"), [0], f"the water's submesh: {made!r}")
    frag = await ed.call_raw("read_file", {"path": made["frag_path"]})
    ed.check("@role water" in frag.text, f"the fork keeps the role: {frag.text[:300]}")
    mesh = (await ed.call("get_entity", entity="Water", properties=["submeshes[0].material.*"]))["components"]["mesh"]
    ed.check_eq(mesh.get(M + "shader"), base, "the material draws with the fork")
    ed.check_eq(mesh.get(M + "shaderFeatures") or [], features, "its features are kept")
    found = await ed.call("check_shader", timeout=300)
    role = [f for f in found.get("findings", []) if f.get("code") == "water-fork-without-role"]
    ed.check(not role, f"no role warning for the fork: {role!r}")
    await ed.call("undo")
    mesh = (await ed.call("get_entity", entity="Water", properties=["submeshes[0].material.*"]))["components"]["mesh"]
    ed.check_eq(mesh.get(M + "shader"), "library:water", "undo puts the library shader back")
    await ed.check_error("fork_shader", source="library:underwater", target="material", entity="Water", contains="postprocess")


async def test_planar_reflection_settings(ed: Editor):
    """The scene's Planar Reflections: settable and undoable, a pass per water height in
    get_render_stats, none with the resolution off; a screenshot of the reflection."""
    await ed.call("set_properties", entity="Water", values={"mesh": {
        M + "shaderFeatures": ["BUILTIN_TEXTURES", "WAVES", "GERSTNER", "REFLECTION", "PLANAR_REFLECTION", "UNDERSIDE"]}})
    defaults = (await ed.call("get_scene_settings", names=["planar_reflection_resolution", "max_planar_reflections",
                                                             "planar_reflection_layers"]))["settings"]
    ed.check_eq(defaults["planar_reflection_resolution"], "half", f"half by default: {defaults!r}")
    ed.check_eq(defaults["planar_reflection_layers"], "everything")
    await ed.call("set_scene_settings", settings={"planar_reflection_resolution": "quarter", "max_planar_reflections": 1,
                                                "planar_reflection_far_clip": 60, "planar_reflection_shadows": False})
    got = (await ed.call("get_scene_settings", names=["planar_reflection_resolution", "max_planar_reflections"]))["settings"]
    ed.check_eq(got["planar_reflection_resolution"], "quarter")
    ed.check_eq(got["max_planar_reflections"], 1)
    await ed.check_error("set_scene_settings", settings={"max_planar_reflections": 9}, contains="1 to 4")
    await ed.call("undo")
    got = (await ed.call("get_scene_settings", names=["planar_reflection_resolution"]))["settings"]
    ed.check_eq(got["planar_reflection_resolution"], "half", "undo restores the resolution")
    await ed.call("redo")
    # the same settings in Lua
    lua_values = await lua(ed, "return scene.planarReflectionResolution == PlanarReflectionResolution.QUARTER, "
                               "scene.maxPlanarReflections, scene.planarReflectionShadows", edit=True)
    ed.check_eq(lua_values, [True, 1, False], "Scene's Lua properties read the settings")

    # drawn: a pass for the water's height, at a quarter of the view
    stats = {}
    for _ in range(40):
        await ed.call_raw("screenshot", {"view": "game", "save_path": "screenshots/water_planar.png"})
        stats = await ed.call("get_render_stats")
        if stats.get("planar_reflections", {}).get("passes"):
            break
        await asyncio.sleep(0.5)
    ed.check_eq(stats.get("planar_reflections", {}).get("passes"), 1, f"one reflection pass: {stats.get('planar_reflections')!r}")
    in_use = (await ed.call("get_scene_settings"))["planar_reflections_in_use"]
    ed.check(in_use["passes"] == 1 and in_use["width"] > 0, f"in use: {in_use!r}")

    scene_shot = await ed.call_raw("screenshot", {"view": "scene", "save_path": "screenshots/water_planar_scene_view.png"})
    ed.check(scene_shot.images, "Scene view screenshot of the reflection")

    await ed.call("set_scene_settings", settings={"planar_reflection_resolution": "off"})
    await ed.call_raw("screenshot", {"view": "game"})
    await ed.call_raw("screenshot", {"view": "game", "save_path": "screenshots/water_planar_off.png"})
    stats = await ed.call("get_render_stats")
    ed.check(not stats.get("planar_reflections", {}).get("passes"), f"off: no pass: {stats.get('planar_reflections')!r}")
    shaders = (await ed.call("list_shaders", name="water"))["shaders"][0]
    ed.check_eq(shaders.get("role"), "water", "list_shaders names the role")
    ed.check("planar reflection" in shaders.get("cost_note", ""), f"and the planar reflection cost: {shaders.get('cost_note')!r}")
    await ed.call("set_scene_settings", settings={"planar_reflection_resolution": "half", "max_planar_reflections": 2,
                                                "planar_reflection_far_clip": 0, "planar_reflection_shadows": True})


def roughness(png: bytes) -> int:
    """How many horizontal neighbours (green) in the middle half of an image differ by more than 4
    (a smooth gradient: almost none)."""
    w, h, channels, rows = decode_png(png)
    steps = 0
    for row in rows[h // 4: 3 * h // 4]:
        for x in range(w // 4, 3 * w // 4):
            steps += abs(row[x * channels + 1] - row[(x + 1) * channels + 1]) > 4
    return steps


async def test_planar_reflection_not_flat_check(ed: Editor):
    """check_shader warns about a planar reflection on sloped water (it mirrors about the top of
    its bounds), not on the flat one."""
    await ed.call("set_properties", entity="Water", values={"mesh": {
        M + "shaderFeatures": ["BUILTIN_TEXTURES", "WAVES", "REFLECTION", "PLANAR_REFLECTION"]}})

    async def not_flat():
        found = await ed.call("check_shader", timeout=300)
        return [f for f in found.get("findings", []) if f.get("code") == "planar-reflection-not-flat"]

    ed.check(not await not_flat(), "flat water: no warning")
    await ed.call("set_transform", entity="Water", rotation={"euler": [20, 0, 0]})
    try:
        sloped = await not_flat()
        ed.check(len(sloped) == 1 and "Water" in sloped[0].get("message", ""), f"sloped water: one warning: {sloped!r}")
    finally:
        await ed.call("set_transform", entity="Water", rotation={"euler": [0, 0, 0]})
    ed.check(not await not_flat(), "flat again: no warning")


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
