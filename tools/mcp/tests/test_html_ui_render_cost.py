"""S92 (HTML UI render cost) through MCP: the composite stage and MSAA settings (AC6, AC14, AC16).

- inspect_ui {render_stats: true} and get_render_stats list the S92 statistics fields;
- set_scene_settings carries html_ui_msaa_samples (0 = the project's, 1, 2, 4, 8) and
  html_ui_render_mode (overlay, camera), with undo; the screen document's target follows the
  scene's count (render_stats samples), Auto (the project default) is 4 on desktop;
- a texture-mode document's msaaSamples drives its own target; the same edit on a screen document
  returns a `next` naming the scene setting;
- Lua's scene.htmlUiMsaaSamples and scene.htmlUiRenderMode in Play, printed back and seen in render_stats;
- the editor's Scene view keeps drawing a 2D scene's documents as the world-space canvas (it scales
  with the Scene view's zoom), while game views fill the viewport (HtmlUIScene.* in the htmlui suite).
"""

from harness import Editor
from htmlui_pixels import count_near, decode_png

CI = True
EDITOR = {"create": True, "name": "Html Ui Render Cost CI"}

TIMEOUT = 900

S: dict = {}

DOC = "assets/ui/render-cost.html"
TEX = "assets/ui/render-cost-texture.html"
BLUE = (0x30, 0x60, 0xc0)

HTML = """<!DOCTYPE html>
<html>
<head>
<style>
  html, body { margin: 0; width: 100%; height: 100%; background-color: #3060c0; }
  #box { position: absolute; left: 40px; top: 40px; width: 200px; height: 120px; border-radius: 16px;
         background-color: #e0a030; transform: rotate(10deg); }
</style>
</head>
<body><div id="box"></div></body>
</html>
"""

TEX_HTML = """<!DOCTYPE html>
<html><head><style>html, body { margin: 0; width: 256px; height: 256px; background-color: #20a040; }</style></head>
<body><div>texture</div></body></html>
"""

# R6 (package A adds them, B2 fills the layer cache ones): listed by both tools through forEachField
S92_FIELDS = ["framesKept", "liveSources", "keptMismatches", "compositeTiles", "compositePixels", "layerPixels",
              "layerCacheHits", "layerCacheMisses", "layerCacheBytes", "samples", "targetBytes", "depthStencilStores"]


async def render(ed: Editor, scene: int, view: str = "game", **args):
    shot = await ed.call_raw("screenshot", {"view": view, "scene": scene, "max_width": 640, **args})
    ed.check(not shot.is_error and shot.images, f"no {view} screenshot: {shot.text[:300]}")
    return shot.images[0][0]


async def stats(ed: Editor, entity: int) -> dict:
    result = await ed.call("inspect_ui", scene=S["scene"], entity=entity, depth=0, render_stats=True)
    ed.check("render_stats" in result, f"inspect_ui render_stats: {list(result)}")
    return result["render_stats"]


async def settings(ed: Editor) -> dict:
    got = await ed.call("get_scene_settings", scene=S["scene"], names=["html_ui_msaa_samples", "html_ui_render_mode"])
    return got["settings"]


async def test_scene_settings_and_stats(ed: Editor):
    made = await ed.call("create_scene", name="RenderCost", type="UI", path="scenes/RenderCost.scene")
    S["scene"] = made["scene"]["id"]
    await ed.call("write_file", path=DOC, content=HTML)
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Hud")
    S["screen"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["screen"], values={"uidocument": {"filename": DOC}})
    await render(ed, S["scene"])
    await render(ed, S["scene"])

    # AC6: every S92 field, from both tools
    got = await stats(ed, S["screen"])
    for field in S92_FIELDS:
        ed.check(field in got and isinstance(got[field], int), f"render_stats has {field}: {got}")
    html_ui = (await ed.call("get_render_stats")).get("html_ui", {})
    ed.check(all(field in html_ui for field in S92_FIELDS), f"get_render_stats html_ui lists the S92 fields: {html_ui}")

    # defaults: Screen Space - Overlay, the project's count (Auto: 4 on desktop)
    ed.check_eq(await settings(ed), {"html_ui_msaa_samples": 0, "html_ui_render_mode": "overlay"})
    project = await ed.call("get_project_settings", keys=["graphics.html_ui_msaa_samples"])
    ed.check_in("0", str(project), "a new project is Auto")
    ed.check_eq(got["samples"], 4, "Auto on desktop")

    # AC16: the scene's count drives the screen target; undo restores the project's
    await ed.call("set_scene_settings", scene=S["scene"], settings={"html_ui_msaa_samples": 1})
    await render(ed, S["scene"])
    await render(ed, S["scene"])
    ed.check_eq((await stats(ed, S["screen"]))["samples"], 1, "the scene's 1 sample")
    await ed.call("undo", scene=S["scene"])
    await render(ed, S["scene"])
    await render(ed, S["scene"])
    ed.check_eq((await stats(ed, S["screen"]))["samples"], 4, "undo: the project's Auto again")
    msg = await ed.call("set_scene_settings", scene=S["scene"], settings={"html_ui_msaa_samples": 3}, expect_error=True)
    ed.check_in("0 (the project's), 1, 2, 4 or 8", msg)

    # the render mode round-trips through the scene file, with undo
    await ed.call("set_scene_settings", scene=S["scene"], settings={"html_ui_render_mode": "camera"})
    ed.check_eq((await settings(ed))["html_ui_render_mode"], "camera")
    await ed.call("save_scene", scene=S["scene"])
    ed.check("htmlUiRenderMode: camera" in (ed.project / "scenes/RenderCost.scene").read_text(), "saved to the scene file")
    await ed.call("undo", scene=S["scene"])
    ed.check_eq((await settings(ed))["html_ui_render_mode"], "overlay")
    await ed.call("save_scene", scene=S["scene"])
    ed.check("htmlUiRenderMode" not in (ed.project / "scenes/RenderCost.scene").read_text(), "the default is not written")


async def test_document_msaa(ed: Editor):
    await ed.call("write_file", path=TEX, content=TEX_HTML)
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Panel")
    S["texture"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["texture"],
                  values={"uidocument": {"filename": TEX, "renderMode": "texture", "textureWidth": 256, "textureHeight": 256}})
    await render(ed, S["scene"])
    await render(ed, S["scene"])
    ed.check_eq((await stats(ed, S["texture"]))["samples"], 4, "Scene Default: the project's Auto")
    done = await ed.call("set_properties", scene=S["scene"], entity=S["texture"], values={"uidocument": {"msaaSamples": 2}})
    ed.check("next" not in done, f"a texture document takes its own count: {done}")
    await render(ed, S["scene"])
    await render(ed, S["scene"])
    ed.check_eq((await stats(ed, S["texture"]))["samples"], 2, "the document's own count")
    ed.check_eq((await stats(ed, S["screen"]))["samples"], 4, "the screen documents keep the scene's")

    # a screen document: the scene setting decides, and the result says so
    done = await ed.call("set_properties", scene=S["scene"], entity=S["screen"], values={"uidocument": {"msaaSamples": 8}})
    ed.check_in("html_ui_msaa_samples", done.get("next", ""), "the next step names the scene setting")
    await ed.call("undo", scene=S["scene"])


async def test_lua_scene_settings(ed: Editor):
    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60)
    try:
        got = await ed.call("eval_lua", code="local s = getScene(); s.htmlUiMsaaSamples = 1; s.htmlUiRenderMode = UIRenderMode.CAMERA; "
                                             "print('s92 msaa=' .. s.htmlUiMsaaSamples .. ' camera=' .. tostring(s.htmlUiRenderMode == UIRenderMode.CAMERA))")
        ed.check_in("s92 msaa=1 camera=true", str(got.get("printed")), "Lua sets and reads the scene's HTML UI settings")
        # the next frame draws the screen document with the scene's count
        await ed.call("wait", frames=3)
        ed.check_eq((await stats(ed, S["screen"]))["samples"], 1, "Lua's count reaches the document target")
    finally:
        await ed.call("play", action="stop")
    # Play's changes stay in Play
    ed.check_eq(await settings(ed), {"html_ui_msaa_samples": 0, "html_ui_render_mode": "overlay"})


async def test_scene_view_keeps_world_canvas(ed: Editor):
    """AC14: the Scene view draws a 2D scene's documents as the canvas rectangle of world space."""
    made = await ed.call("create_scene", name="Canvas2D", type="2d")
    sid = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=sid, type="ui_document", name="Hud")
    await ed.call("set_properties", scene=sid, entity=ent["entity_id"], values={"uidocument": {"filename": DOC}})
    size = await ed.call("get_project_settings", keys=["graphics.canvas_width", "graphics.canvas_height"])
    text = str(size)
    width = 1280 if "1280" in text else 640
    height = 720 if "720" in text else 360
    await render(ed, sid, "game")

    async def blue_share(zoom: float) -> float:
        await ed.call("set_scene_camera", scene=sid, center=[width / 2, height / 2], zoom=zoom)
        await render(ed, sid, "scene")
        png = await render(ed, sid, "scene")
        w, h, _, _ = decode_png(png)
        return count_near(png, BLUE, tol=24) / max(1, (w // 4) * (h // 4))

    near_share = await blue_share(1.0)
    far_share = await blue_share(4.0)
    ed.check(near_share > 0.05, f"the canvas shows in the Scene view: {near_share:.3f}")
    # a quarter of the side at four times the units per pixel: about a sixteenth of the pixels
    ed.check(far_share < near_share / 4, f"the canvas scales with the Scene view's zoom (world space): {near_share:.3f} -> {far_share:.3f}")
