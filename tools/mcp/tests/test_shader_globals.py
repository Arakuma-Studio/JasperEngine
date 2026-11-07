"""Global shader values (shader globals spec 9, docs/mcp/guide/shaders.md "Global values").

The fixture tools/fixtures/shader-globals declares the globals in project.yaml and has the forks:
globals_color (its fragment stage outputs the TestColor global), checker (samples the
_CheckerTex texture global) and vertex_lights (the game's four vertex lights in u_vs_globals, set
every frame by scripts/VertexLightsUpdater.lua, the port of VertexLightsUpdater.cs).

- get/set_project_settings graphics.shader_globals: the list, glsl_vs / glsl_fs, one global by
  key (upsert, rename, null), the errors, undo;
- edit mode: the default red on screen, changed to green by set_project_settings at once;
- Play: Lua Shader.setGlobalColor turns it blue, the getters and ids, Stop restores the default;
- a library:unlit material whose baseColor is bound to FogTint follows it (get_shader_uniforms
  lists the binding and the globals), unbinding gives its own colour back;
- a texture global (a 4x4 checker of 2x2 cells: S14 R6 rescales what is no multiple of 4 for block
  compression, which would blur it), unset (white), set from Lua;
- the vertex lights: the lit side follows the light the updater pushes (left/right halves);
- check_shader: global-undeclared, global-type-mismatch, globals-block-too-large;
- JASPER_SLOW_BUILD=1: a Desktop export shows the default colour (the generated main declares
  the globals).
"""

from __future__ import annotations

import os
from pathlib import Path

import harness
from harness import Editor, find_key, skip

from htmlui_pixels import decode_png

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "shader-globals"}
CI_EXCLUDED = ("long-running (about 50 minutes on a loaded server: a test builds and runs a real Desktop player); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "1800"))
SCENE = "scenes/Globals.scene"
S: dict = {}


async def shot(ed: Editor, label: str):
    r = await ed.call_raw("screenshot", {"view": "game", "scene": SCENE, "max_width": 400})
    ed.check(not r.is_error and r.images, f"{label}: no screenshot: {r.text[:300]}")
    data = r.images[0][0]
    if ed.scratch:
        (ed.scratch.root / f"globals-{label}.png").write_bytes(data)
    return decode_png(data)


def pixel(image, fx: float, fy: float):
    w, h, ch, rows = image
    x, y = min(w - 1, int(w * fx)), min(h - 1, int(h * fy))
    return tuple(rows[y][x * ch:x * ch + 3])


def near(color, want, tol: int = 24) -> bool:
    return all(abs(int(c) - w) <= tol for c, w in zip(color, want))


async def wait_centre(ed: Editor, want, label: str, frames: int = 300):
    """The centre pixel reaches `want` (shaders build in the background)."""
    color = None
    for _ in range(max(1, frames // 10)):
        image = await shot(ed, label)
        color = pixel(image, 0.5, 0.5)
        if near(color, want):
            return color
        await ed.call("wait", frames=10)
    ed.check(False, f"{label}: the centre is {color}, expected {want}")
    return color


def brightness(image, x0: float, x1: float, y0: float = 0.3, y1: float = 0.7) -> float:
    w, h, ch, rows = image
    total, n = 0, 0
    for y in range(int(h * y0), int(h * y1)):
        row = rows[y]
        for x in range(int(w * x0), int(w * x1)):
            total += row[x * ch] + row[x * ch + 1] + row[x * ch + 2]
            n += 1
    return total / max(1, n)


async def new_box(ed: Editor, name: str, values: dict, scale=(4, 4, 0.2)) -> int:
    made = await ed.call("create_entity", type="box", name=name, position=[0, 0, 0])
    eid = find_key(made, "entity_id", "id")
    await ed.call("set_transform", entity=eid, scale=list(scale))
    if values:
        await ed.call("set_properties", entity=eid, values=values)
    return eid


async def settings(ed: Editor, *keys: str) -> dict:
    return await ed.call("get_project_settings", keys=list(keys))


async def test_setup(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "set_properties", "screenshot", "set_main_camera", "delete_entities",
                     "get_scene_tree", "get_project_settings", "set_project_settings", "play", "eval_lua", "wait",
                     "check_shader", "get_shader_uniforms", "attach_script", "undo", "set_transform")
    await ed.call("create_scene", name="Globals", path=SCENE)
    # the fixture has no scene: the untitled one the editor opened goes (an export would list it)
    status = await ed.call("list_scenes")
    for scene in find_key(status, "scenes") or []:
        if isinstance(scene, dict) and scene.get("name") != "Globals":
            await ed.call("close_scene", scene=str(scene["id"]), unsaved="discard")
    tree = await ed.call("get_scene_tree", scene=SCENE, format="json")

    def walk(nodes):
        for node in nodes or []:
            yield node
            yield from walk(node.get("children"))
    for node in list(walk(find_key(tree, "entities", "roots", "children") or [])):
        await ed.call("delete_entities", entities=[str(node["id"])])
    cam = find_key(await ed.call("create_entity", type="camera", name="View", position=[0, 0, 10]), "entity_id", "id")
    await ed.call("set_main_camera", entity=cam)
    S["camera"] = cam


async def test_declarations_and_glsl(ed: Editor):
    got = await settings(ed, "graphics.shader_globals")
    listed = got["settings"]["graphics.shader_globals"]
    names = [g["name"] for g in listed]
    ed.check_eq(names[:3], ["TestColor", "FogTint", "_CheckerTex"], "the fixture's list, in order")
    ed.check_eq(listed[0], {"name": "TestColor", "type": "color", "default": "#FF0000FF"}, "a colour as sRGB hex")
    ed.check_in("uniform u_vs_globals {", got["glsl_vs"])
    ed.check_in("    vec4 _LightPositionX;", got["glsl_vs"])
    ed.check_in("uniform texture2D _CheckerTex;\nuniform sampler _CheckerTex_smp;", got["glsl_fs"])
    ed.check("_CheckerTex" not in got["glsl_vs"], "textures in the fragment block only")

    # one global by key: upsert, read, rename (bindings follow), remove
    await ed.call("set_project_settings", values={"graphics.shader_globals.Extra": {"type": "float_array", "array_size": 3, "default": [0.5]}})
    one = (await settings(ed, "graphics.shader_globals.Extra"))["settings"]["graphics.shader_globals.Extra"]
    ed.check_eq(one, {"type": "float_array", "array_size": 3, "default": [0.5, 0, 0]}, "upserted")
    await ed.call("set_project_settings", values={"graphics.shader_globals.Extra": {"name": "Extra2", "default": [1, 2, 3]}})
    one = (await settings(ed, "graphics.shader_globals.Extra2"))["settings"]["graphics.shader_globals.Extra2"]
    ed.check_eq(one["default"], [1, 2, 3], "renamed, the type kept")
    ed.check_eq((await settings(ed, "graphics.shader_globals.Extra"))["settings"]["graphics.shader_globals.Extra"], None, "the old name is gone")
    # the project history undoes it
    await ed.call("undo", scope="project")
    ed.check((await settings(ed, "graphics.shader_globals.Extra"))["settings"]["graphics.shader_globals.Extra"] is not None, "undo: the old name back")
    await ed.call("set_project_settings", values={"graphics.shader_globals.Extra": None})
    names = [g["name"] for g in (await settings(ed, "graphics.shader_globals"))["settings"]["graphics.shader_globals"]]
    ed.check("Extra" not in names and "Extra2" not in names, f"removed: {names}")

    # errors say what is wrong and change nothing
    for values, what in (({"graphics.shader_globals.u_time": {"type": "float"}}, "u_"),
                         ({"graphics.shader_globals.A": {"type": "vec4"}}, "type is one of"),
                         ({"graphics.shader_globals.A": {"type": "color", "default": "red"}}, "#RRGGBB"),
                         ({"graphics.shader_globals": [{"name": "A"}]}, "needs a type"),
                         ({"graphics.shader_globals": [{"name": "A", "type": "float"}, {"name": "A", "type": "float"}]}, "twice")):
        r = await ed.call_raw("set_project_settings", {"values": values})
        ed.check(r.is_error and what in r.text, f"{values} refused naming {what}: {r.text[:300]}")
    ed.check_eq(len((await settings(ed, "graphics.shader_globals"))["settings"]["graphics.shader_globals"]), len(listed), "unchanged")


async def test_edit_mode_default_and_change(ed: Editor):
    box = await new_box(ed, "Colour", {"mesh": {"customShader": "shaders/globals_color"}})
    S["colour"] = box
    await wait_centre(ed, (255, 0, 0), "default-red")
    # a new default shows at once in edit mode
    await ed.call("set_project_settings", values={"graphics.shader_globals.TestColor": {"default": "#00FF00"}})
    await wait_centre(ed, (0, 255, 0), "default-green", frames=20)


async def test_play_sets_and_stop_resets(ed: Editor):
    await ed.call("play", action="start", scene=SCENE)
    try:
        r = await ed.call("eval_lua", code="Shader.setGlobalColor('TestColor', Vector4(0, 0, 1, 1)); return Shader.getGlobalColor('TestColor').z")
        ed.check_eq(r["values"][0], 1, f"set and read back as sRGB: {r}")
        await wait_centre(ed, (0, 0, 255), "play-blue", frames=30)
        r = await ed.call("eval_lua", code="local id = Shader.propertyToID('TestColor'); return {id == Shader.propertyToID('TestColor'), "
                                            "Shader.hasGlobal(id), Shader.getGlobalFloat('NoSuchGlobal') == nil, Shader.hasGlobal('FogTint'), "
                                            "Shader.setGlobalMatrix('TestColor', Matrix4()) == false}")
        ed.check_eq(r["values"][0], [True, True, True, True, True], f"ids, hasGlobal, unknown nil, kinds: {r}")
        r = await ed.call_raw("eval_lua", {"code": "Shader.setGlobalVector('TestColor', 'red')"})
        ed.check("setGlobalVector" in r.text and "Vector4" in r.text, f"a wrong argument names the function: {r.text[:300]}")
        # a name nobody declared exists from its first set
        r = await ed.call("eval_lua", code="Shader.setGlobalFloat('_Made', 2); return {Shader.getGlobalFloat('_Made'), #Shader.getGlobalNames()}")
        ed.check_eq(r["values"][0], [2, 12], f"made by its first set, beside the 11 declared: {r}")
    finally:
        await ed.call("play", action="stop")
    await wait_centre(ed, (0, 255, 0), "stop-default", frames=30)


async def test_library_parameter_bound_to_a_global(ed: Editor):
    await ed.call("delete_entities", entities=[str(S["colour"])])
    box = await new_box(ed, "Tinted", {"mesh": {"submeshes[0].material.shader": "library:unlit",
                                                "submeshes[0].material.shaderParams.baseColor": "#00FF00"}})
    await wait_centre(ed, (0, 255, 0), "unlit-own")
    await ed.call("set_properties", entity=box, values={"mesh": {"submeshes[0].material.shaderGlobalBindings.baseColor": "FogTint"}})
    await wait_centre(ed, (128, 128, 128), "bound-fog", frames=30)
    u = await ed.call("get_shader_uniforms", entity=box, submesh=0)
    ed.check_eq(u["global_bindings"], {"baseColor": "FogTint"}, "the binding")
    # the global changes, the material follows (no fork)
    await ed.call("set_project_settings", values={"graphics.shader_globals.FogTint": {"default": "#FF00FF"}})
    await wait_centre(ed, (255, 0, 255), "bound-magenta", frames=30)
    # a texture global is refused
    r = await ed.call_raw("set_properties", {"entity": box, "values": {"mesh": {"submeshes[0].material.shaderGlobalBindings.baseColor": "_CheckerTex"}}})
    ed.check(r.is_error and "texture2d" in r.text, f"a texture global cannot feed a colour: {r.text[:300]}")
    # undo (the scene history) and null unbind give its own colour back
    await ed.call("undo")
    await wait_centre(ed, (0, 255, 0), "undo-own", frames=30)
    await ed.call("redo")
    await wait_centre(ed, (255, 0, 255), "redo-bound", frames=30)
    await ed.call("set_properties", entity=box, values={"mesh": {"submeshes[0].material.shaderGlobalBindings.baseColor": None}})
    await wait_centre(ed, (0, 255, 0), "unbound", frames=30)
    u = await ed.call("get_shader_uniforms", entity=box, submesh=0)
    ed.check_eq(u["global_bindings"], {}, "no binding left")
    # bound again, then driven from Lua in Play (and Lua's own binding API)
    await ed.call("set_properties", entity=box, values={"mesh": {"submeshes[0].material.shaderGlobalBindings.baseColor": "FogTint"}})
    await ed.call("play", action="start", scene=SCENE)
    try:
        await ed.call("eval_lua", code="Shader.setGlobalColor('FogTint', Vector4(1, 1, 0, 1))")
        await wait_centre(ed, (255, 255, 0), "play-yellow", frames=30)
        r = await ed.call("eval_lua", code="local mesh = object('Tinted', 'Mesh'); local m = mesh:getMaterial(0); "
                                            "local was = m:getShaderGlobalBinding('baseColor'); "
                                            "m:setShaderGlobalBinding('baseColor', nil); mesh:setMaterial(0, m); return was")
        ed.check_eq(r["values"][0], "FogTint", f"Lua reads the binding: {r}")
        r = await ed.call("eval_lua", code="local m = object('Tinted', 'Mesh'):getMaterial(0); "
                                            "local ok, err = pcall(function() m:setShaderGlobalBinding('baseColor', 5) end); "
                                            "return {ok, tostring(err), m:getShaderGlobalBinding('baseColor')}")
        ed.check(r["values"][0][0] is False and "setShaderGlobalBinding" in r["values"][0][1],
                 f"a number is an error, not an unbind: {r}")
        await wait_centre(ed, (0, 255, 0), "lua-unbound", frames=30)
    finally:
        await ed.call("play", action="stop")
    await ed.call("delete_entities", entities=[str(box)])


async def test_texture_global(ed: Editor):
    box = await new_box(ed, "Checker", {"mesh": {"customShader": "shaders/checker"}})
    S["checker"] = box

    def colours(image):
        reds = blues = whites = 0
        w, h, ch, rows = image
        for y in range(int(h * 0.3), int(h * 0.7), 3):
            for x in range(int(w * 0.3), int(w * 0.7), 3):
                p = tuple(rows[y][x * ch:x * ch + 3])
                reds += near(p, (255, 0, 0), 40)
                blues += near(p, (0, 0, 255), 40)
                whites += near(p, (255, 255, 255), 30)
        return reds, blues, whites

    found = (0, 0, 0)
    for _ in range(30):
        found = colours(await shot(ed, "checker"))
        if found[0] > 20 and found[1] > 20:
            break
        await ed.call("wait", frames=10)
    ed.check(found[0] > 20 and found[1] > 20, f"the checker default: red and blue {found}")
    # no default: the white fallback
    await ed.call("set_project_settings", values={"graphics.shader_globals._CheckerTex": {"default": ""}})
    for _ in range(10):
        found = colours(await shot(ed, "checker-white"))
        if found[2] > 100:
            break
        await ed.call("wait", frames=5)
    ed.check(found[2] > 100 and found[0] == 0, f"white without a texture: {found}")
    # set from Lua in Play
    await ed.call("play", action="start", scene=SCENE)
    try:
        await ed.call("eval_lua", code="Shader.setGlobalTexture('_CheckerTex', 'textures/checker.png')")
        for _ in range(30):
            found = colours(await shot(ed, "checker-lua"))
            if found[0] > 20 and found[1] > 20:
                break
            await ed.call("wait", frames=10)
        ed.check(found[0] > 20 and found[1] > 20, f"set from Lua: {found}")
    finally:
        await ed.call("play", action="stop")
    await ed.call("delete_entities", entities=[str(box)])


async def test_missing_texture_and_destroyed_render_targets(ed: Editor):
    box = await new_box(ed, "Checker2", {"mesh": {"customShader": "shaders/checker"}})
    cursor = (await ed.call("read_console", limit=1)).get("next_cursor", 0)
    # a default whose file is missing: the white fallback, warned once however many frames draw it
    await ed.call("set_project_settings", values={"graphics.shader_globals._CheckerTex": {"default": "textures/missing.png"}})
    await wait_centre(ed, (255, 255, 255), "missing-white", frames=30)
    await ed.call("wait", frames=20)
    logs = await ed.call("read_console", since=cursor, contains="could not be loaded", limit=50)
    ed.check_eq(len(logs["entries"]), 1, f"the missing file is warned once: {logs['entries']}")

    await ed.call("play", action="start", scene=SCENE)
    try:
        cursor = (await ed.call("read_console", limit=1)).get("next_cursor", 0)
        # a Lua Framebuffer collected while the global holds it
        await ed.call("eval_lua", code="local f = Framebuffer(); Shader.setGlobalTexture('_CheckerTex', f); f = nil; "
                                       "collectgarbage(); collectgarbage()")
        await ed.call("wait", frames=5)
        logs = await ed.call("read_console", since=cursor, contains="render target was destroyed", limit=50)
        ed.check(len(logs["entries"]) >= 1, f"the collected render target is let go: {logs}")
        # a camera's render target, the camera destroyed while the global binds it
        await ed.call("eval_lua", code="local c = Camera(scene); c.name = 'RTCam'; c.renderToTexture = true")
        await ed.call("wait", until="object('RTCam', 'Camera'):getFramebuffer() ~= nil", timeout_seconds=20)
        await ed.call("eval_lua", code="Shader.setGlobalTexture('_CheckerTex', object('RTCam', 'Camera'):getFramebuffer())")
        await ed.call("wait", frames=3)
        cursor = (await ed.call("read_console", limit=1)).get("next_cursor", 0)
        await ed.call("eval_lua", code="GameObject.destroy(object('RTCam'))")
        await ed.call("wait", frames=10)
        logs = await ed.call("read_console", since=cursor, contains="render target was destroyed", limit=50)
        ed.check(len(logs["entries"]) >= 1, f"the destroyed camera's target is let go: {logs}")
        # it keeps drawing (the default: the missing file's white)
        await wait_centre(ed, (255, 255, 255), "after-camera", frames=20)
    finally:
        await ed.call("play", action="stop")
    await ed.call("set_project_settings", values={"graphics.shader_globals._CheckerTex": {"default": "textures/checker.png"}})
    await ed.call("delete_entities", entities=[str(box)])


async def test_rename_with_bindings_undo_redo_and_play(ed: Editor):
    box = await new_box(ed, "Renamed", {"mesh": {"submeshes[0].material.shader": "library:unlit",
                                                 "submeshes[0].material.shaderGlobalBindings.baseColor": "FogTint"}})
    await ed.call("set_project_settings", values={"graphics.shader_globals.FogTint": {"name": "Fog2"}})
    u = await ed.call("get_shader_uniforms", entity=box, submesh=0)
    ed.check_eq(u["global_bindings"], {"baseColor": "Fog2"}, "the binding follows the rename")
    await ed.call("undo", scope="project")
    u = await ed.call("get_shader_uniforms", entity=box, submesh=0)
    ed.check_eq(u["global_bindings"], {"baseColor": "FogTint"}, "undo renames it back")
    await ed.call("redo", scope="project")
    u = await ed.call("get_shader_uniforms", entity=box, submesh=0)
    ed.check_eq(u["global_bindings"], {"baseColor": "Fog2"}, "redo")
    await ed.call("play", action="start", scene=SCENE)
    try:
        r = await ed.call_raw("undo", {"scope": "project"})
        ed.check(r.is_error or "Stop" in r.text, f"a rename is not undone while playing: {r.text[:300]}")
        r = await ed.call_raw("set_project_settings", {"values": {"graphics.shader_globals.Fog2": {"name": "Fog3"}}})
        ed.check(r.is_error, f"no settings change while playing: {r.text[:300]}")
    finally:
        await ed.call("play", action="stop")
    u = await ed.call("get_shader_uniforms", entity=box, submesh=0)
    ed.check_eq(u["global_bindings"], {"baseColor": "Fog2"}, "Stop changed nothing")
    await ed.call("undo", scope="project")
    names = [g["name"] for g in (await settings(ed, "graphics.shader_globals"))["settings"]["graphics.shader_globals"]]
    ed.check("FogTint" in names, f"back to FogTint: {names}")
    await ed.call("delete_entities", entities=[str(box)])


async def test_prefab_overrides_of_bindings(ed: Editor):
    box = await new_box(ed, "Crate", {"mesh": {"submeshes[0].material.shader": "library:unlit"}})
    path = "prefabs/Crate.prefab"
    await ed.call("create_prefab", entity=box, path=path)
    made = await ed.call("instantiate_prefab", prefab=path, name="CrateCopy", position=[0, 0, 0])
    copy = find_key(made, "entity_id", "id")
    await ed.call("set_properties", entity=copy, values={"mesh": {"submeshes[0].material.shaderGlobalBindings.baseColor": "FogTint"}})
    info = await ed.call("get_prefab_info", entity=copy, usage=False)
    ed.check("submeshes[0].material.shaderGlobalBindings" in str(info["overrides"]), f"a material override: {info['overrides']}")
    # a component's bindings are overrides too
    pts = find_key(await ed.call("create_entity", type="points", name="Dots"), "entity_id", "id")
    await ed.call("create_prefab", entity=pts, path="prefabs/Dots.prefab")
    dots = find_key(await ed.call("instantiate_prefab", prefab="prefabs/Dots.prefab", name="DotsCopy"), "entity_id", "id")
    await ed.call("set_properties", entity=dots, values={"points": {"shaderGlobalBindings.tint": "FogTint"}})
    info = await ed.call("get_prefab_info", entity=dots, usage=False)
    ed.check("shaderGlobalBindings" in str(info["overrides"]), f"a component override: {info['overrides']}")
    # saved and reopened, then applied to the prefab file
    await ed.call("save_scene", scene=SCENE)
    await ed.call("create_scene", name="Spare", path="scenes/Spare.scene")  # (the last scene cannot close)
    await ed.call("close_scene", scene=SCENE, unsaved="discard")
    await ed.call("open_scene", scene=SCENE)
    await ed.call("close_scene", scene="scenes/Spare.scene", unsaved="discard")
    await ed.call("select_scene", scene=SCENE)
    u = await ed.call("get_shader_uniforms", entity="CrateCopy", submesh=0)
    ed.check_eq(u["global_bindings"], {"baseColor": "FogTint"}, "the override survives the reload (override bit)")
    await ed.call("prefab_overrides", entity="CrateCopy", action="apply", component="mesh",
                  properties=["submeshes[0].material.shaderGlobalBindings"])
    u = await ed.call("get_shader_uniforms", entity="Crate", submesh=0)
    ed.check_eq(u["global_bindings"], {"baseColor": "FogTint"}, "the other instance follows")
    await ed.call("save_scene", scene=SCENE)
    ed.check_in("shaderGlobalBindings", (ed.project / path).read_text(), "applied to the prefab file")
    for name in ("Crate", "CrateCopy", "Dots", "DotsCopy"):
        await ed.call("delete_entities", entities=[name])


async def test_values_persist_across_a_scene_load_and_post_process_bindings(ed: Editor):
    await ed.call("save_scene", scene=SCENE)
    await ed.call("create_scene", name="Other", path="scenes/Other.scene")
    await ed.call("save_scene", scene="scenes/Other.scene")
    await ed.call("select_scene", scene=SCENE)
    # a gameboy pass whose four shades follow TestColor: the whole view takes it
    await ed.call("set_scene_settings", scene=SCENE, settings={"post_process": [
        {"shader": "library:gameboy", "shaderGlobalBindings": {s: "TestColor" for s in ("shade0", "shade1", "shade2", "shade3")}}]})
    got = (await ed.call("get_scene_settings", scene=SCENE, names=["post_process"]))["settings"]["post_process"]
    ed.check_eq(got[0]["shaderGlobalBindings"]["shade0"], "TestColor", f"{got}")
    await ed.call("set_scene_camera", scene=SCENE, display={"show_post_processing": True})
    await ed.call("play", action="start", scene=SCENE)
    try:
        await ed.call("eval_lua", code="Shader.setGlobalColor('TestColor', Vector4(1, 0, 1, 1))")
        image = None
        for _ in range(30):
            image = await shot(ed, "post-bound")
            if near(pixel(image, 0.1, 0.1), (255, 0, 255), 40):
                break
            await ed.call("wait", frames=10)
        ed.check(near(pixel(image, 0.1, 0.1), (255, 0, 255), 40), f"the pass's shades follow the global: {pixel(image, 0.1, 0.1)}")
        # a scene load keeps the values (like Unity)
        await ed.call("eval_lua", code="SceneManager.loadScene('Other')")
        await ed.call("wait", until="SceneManager.currentSceneName == 'Other'", timeout_seconds=30)
        r = await ed.call("eval_lua", code="local c = Shader.getGlobalColor('TestColor'); return {c.x, c.y, c.z}")
        ed.check_eq(r["values"][0], [1, 0, 1], f"the value outlives the scene load: {r}")
    finally:
        await ed.call("play", action="stop")
    await ed.call("set_scene_settings", scene=SCENE, settings={"post_process": []})


async def test_vertex_lights_follow_the_updater(ed: Editor):
    made = await ed.call("create_entity", type="sphere", name="Lit", position=[0, 0, 0])
    sphere = find_key(made, "entity_id", "id")
    await ed.call("set_transform", entity=sphere, scale=[5, 5, 5])
    await ed.call("set_properties", entity=sphere, values={"mesh": {"customShader": "shaders/vertex_lights"}})
    # edit mode: the defaults put the one white light on the left
    left = right = 0.0
    for _ in range(30):
        image = await shot(ed, "lights-default")
        left, right = brightness(image, 0.25, 0.45), brightness(image, 0.55, 0.75)
        if left > right + 30:
            break
        await ed.call("wait", frames=10)
    ed.check(left > right + 30, f"lit from the left by the defaults: left {left:.0f}, right {right:.0f}")

    # Play: the ported updater pushes the light GameObjects; Light0 on the right
    for i, x in enumerate((3, 30, 30, 30)):
        await ed.call("create_entity", type="empty", name=f"Light{i}", position=[x, 0, 2])
    await ed.call("attach_script", entity=sphere, path="scripts/VertexLightsUpdater.lua")
    await ed.call("play", action="start", scene=SCENE)
    try:
        await ed.call("wait", frames=5)
        image = await shot(ed, "lights-right")
        left, right = brightness(image, 0.25, 0.45), brightness(image, 0.55, 0.75)
        ed.check(right > left + 30, f"the updater's light on the right: left {left:.0f}, right {right:.0f}")
        r = await ed.call("eval_lua", code="return Shader.getGlobalVector('_LightPositionX').x")
        ed.check_eq(r["values"][0], 3, f"the updater pushed Light0's x: {r}")
        # move the light: the next frames draw it on the other side
        await ed.call("eval_lua", code="object('Light0').position = Vector3(-3, 0, 2)")
        await ed.call("wait", frames=3)
        image = await shot(ed, "lights-moved")
        left, right = brightness(image, 0.25, 0.45), brightness(image, 0.55, 0.75)
        ed.check(left > right + 30, f"moved to the left: left {left:.0f}, right {right:.0f}")
    finally:
        await ed.call("play", action="stop")
    image = await shot(ed, "lights-stopped")
    ed.check(brightness(image, 0.25, 0.45) > brightness(image, 0.55, 0.75) + 30, "Stop: the defaults again")
    u = await ed.call("get_shader_uniforms", entity=sphere)
    members = {g["member"]: g for g in u["globals"] if g["stage"] == "vertex"}
    ed.check_eq(sorted(members), sorted(["_LightPositionX", "_LightPositionY", "_LightPositionZ", "_LightRadius",
                                         "_VertexLightColor0", "_VertexLightColor1", "_VertexLightColor2", "_VertexLightColor3"]),
                "get_shader_uniforms lists what the vertex stage reads")
    ed.check(members["_LightRadius"]["declared"] and members["_LightRadius"]["type"] == "vector4", f"{members['_LightRadius']}")
    S["sphere"] = sphere


async def test_check_shader_reports_globals(ed: Editor):
    shaders = ed.project / "assets" / "shaders"
    (shaders / "bad_globals.vert").write_text("#version 450\n#include \"includes/surface_vs.glsl\"\n")
    (shaders / "bad_globals.frag").write_text(
        "#version 450\n// @shader mesh\nuniform u_fs_globals {\n    vec4 NotDeclared;\n    vec3 TestColor;\n    vec2 _LightRadius;\n} globals;\n"
        "out vec4 g_finalColor;\n#include \"includes/output.glsl\"\n"
        "void main() { g_finalColor = finalizeColor(globals.NotDeclared + vec4(globals.TestColor, 1.0) + vec4(globals._LightRadius, 0.0, 0.0)); }\n")
    (shaders / "huge_globals.vert").write_text("#version 450\n#include \"includes/surface_vs.glsl\"\n")
    (shaders / "huge_globals.frag").write_text(
        "#version 450\n// @shader mesh\nuniform u_fs_globals {\n    vec4 big[65];\n} globals;\nout vec4 g_finalColor;\n"
        "#include \"includes/output.glsl\"\nvoid main() { g_finalColor = finalizeColor(globals.big[0] + globals.big[64]); }\n")
    await ed.call("sync_files", paths=["assets/shaders/bad_globals.frag", "assets/shaders/huge_globals.frag"])
    r = await ed.call("check_shader", shader="shaders/bad_globals", shader_type="mesh", timeout=300)
    codes = [f.get("code") for f in r["findings"]]
    ed.check_eq(r["failed"], 0, f"warnings only: {r}")
    ed.check_eq(codes.count("global-undeclared"), 1, f"NotDeclared: {r['findings']}")
    ed.check_eq(codes.count("global-type-mismatch"), 2, f"vec3 TestColor, vec2 _LightRadius: {r['findings']}")
    ed.check(any("NotDeclared" in f["message"] for f in r["findings"]), f"named: {r['findings']}")
    r = await ed.call("check_shader", shader="shaders/huge_globals", shader_type="mesh", timeout=300)
    ed.check_eq(r["failed"], 1, f"a block over 1024 bytes fails: {r}")
    ed.check(any(f.get("code") == "globals-block-too-large" for f in r["findings"]), f"{r['findings']}")
    # the forks of the fixture are clean
    for fork in ("shaders/globals_color", "shaders/checker", "shaders/vertex_lights"):
        r = await ed.call("check_shader", shader=fork, shader_type="mesh", timeout=300)
        ed.check_eq((r["failed"], [f for f in r["findings"] if f.get("code", "").startswith("global")]), (0, []), f"{fork}: {r}")


async def test_desktop_export_declares_the_globals(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    sphere = S.get("sphere")
    if sphere:
        await ed.call("delete_entities", entities=[str(sphere)])
    await new_box(ed, "Exported", {"mesh": {"customShader": "shaders/globals_color"}})
    await ed.call("save_scene", scene=SCENE)
    await ed.call("edit_build_scenes", action="add", scene=SCENE)
    await ed.call("edit_build_scenes", action="set_start", scene=SCENE)
    out = ed.scratch.root / "export-desktop"
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=2, timeout_seconds=30)
    job = r
    while job.get("status") == "running":
        await ed.call("wait", seconds=15)
        job = await ed.call("get_job", job_id=r["job_id"])
    ed.check_eq(job["status"], "succeeded", f"{job}")
    main = next(Path(out).rglob("main.cpp"), None)
    if main:
        ed.check_in("ShaderGlobals::declare({", main.read_text(), "the generated main declares the globals")
    raw = await ed.call_raw("run_build", {"seconds": 8, "screenshot_after_seconds": 6})
    ed.check(not raw.is_error and raw.images, raw.text[:500])
    image = decode_png(raw.images[0][0])
    ed.check(near(pixel(image, 0.5, 0.5), (0, 255, 0), 40), f"the exported game draws the default (green): {pixel(image, 0.5, 0.5)}")
