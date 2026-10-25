"""HTML UI rendering statistics and paint paths through MCP (S16 R25-R27, AC18, AC19).

A list of rounded cards with box shadows in a rounded scrolling container, a dashed box and a
scaled label in a UI scene:
- inspect_ui {render_stats: true} returns every HtmlUIRenderStats field of the document's
  context (a few draws for the whole list, merged runs, instances, no stencil draw, no box-shadow
  texture, the glyph atlas in R8 pages);
- detail full shows each box's paint path ("primitive", or "mesh" with the reason) and each
  text's raster (a bitmap at the label's on-screen scale);
- the project settings graphics.html_ui_glyph_atlas_mib, graphics.html_ui_sdf_primitives and
  graphics.html_ui_transform_aware_text read and write project.yaml, reject bad values, and reach
  the renderer at once (the cards become meshes, the label a stretched bitmap).

Runs on a scratch copy of the fixture project (canvas 1280 x 720).
"""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Ui Render Stats CI"}

TIMEOUT = 900

S: dict = {}

DOC = "assets/ui/render-stats.html"

CARDS = "\n".join(f'<div class="card" id="card{i}"><div class="dot"></div><span>Card {i}</span></div>' for i in range(60))

HTML = """<!DOCTYPE html>
<html>
<head>
<style>
  html, body { margin: 0; width: 1280px; height: 720px; font-family: sans-serif; }
  #list { position: absolute; left: 40px; top: 40px; width: 700px; height: 500px; overflow: auto; padding: 14px;
          box-sizing: border-box; border-radius: 10px; background-color: #1c2030; }
  .card { position: relative; display: inline-block; width: 150px; height: 50px; margin: 0 8px 8px 0; border-radius: 6px;
          border: 1px solid #405070; background-color: #283048; box-shadow: 0 2px 6px rgba(0, 0, 0, 0.6); }
  .dot { position: absolute; left: 10px; top: 13px; width: 24px; height: 24px; border-radius: 12px; background-color: #e0a030; }
  .card span { position: absolute; left: 44px; top: 15px; color: #f0f0f0; font-size: 15px; }
  #dashed { position: absolute; left: 800px; top: 40px; width: 200px; height: 80px; border: 3px dashed #ffffff; }
  #label { position: absolute; left: 800px; top: 200px; font-size: 20px; color: #ffffff; transform: scale(2); transform-origin: 0 0; }
</style>
</head>
<body>
  <div id="list">
""" + CARDS + """
  </div>
  <div id="dashed"></div>
  <div id="label">Scaled label</div>
</body>
</html>
"""

FIELDS = ["passes", "drawCalls", "pipelineApplies", "pipelineCacheMisses", "textureBinds", "layersPushed", "filtersApplied",
          "compositesFused", "geometryLive", "geometryBytes", "geometrySubmitted", "runsMerged", "runCacheHits", "instancesDrawn",
          "verticesGathered", "stencilDraws", "stencilClipsElided", "analyticClips", "boxShadowTextures", "glyphsRasterized",
          "msdfGlyphsPending", "textRegenerations", "textEffectFallbacks", "atlasBytes", "atlasEvictions"]


async def render(ed: Editor, label: str):
    shot = await ed.call_raw("screenshot", {"view": "game", "scene": S["scene"], "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    (ed.scratch.root / f"ui-render-stats-{label}.png").write_bytes(shot.images[0][0])


async def stats(ed: Editor) -> dict:
    result = await ed.call("inspect_ui", scene=S["scene"], entity=S["entity"], selector="#list", depth=0, render_stats=True)
    ed.check("render_stats" in result, f"inspect_ui render_stats: {list(result)}")
    return result["render_stats"]


async def detail(ed: Editor, selector: str) -> dict:
    result = await ed.call("inspect_ui", scene=S["scene"], entity=S["entity"], selector=selector, detail="full")
    ed.check(result.get("details"), f"inspect_ui detail full {selector}: {result}")
    return result["details"][0]


async def test_render_stats(ed: Editor):
    made = await ed.call("create_scene", name="UiRenderStats", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("write_file", path=DOC, content=HTML)
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Cards")
    S["entity"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["entity"], values={"uidocument": {"filename": DOC}})
    await render(ed, "default")
    await render(ed, "default")

    got = await stats(ed)
    for field in FIELDS:
        ed.check(field in got and isinstance(got[field], int), f"render_stats has {field}: {got}")
    ed.check(got["drawCalls"] > 0, f"the document draws: {got}")
    # 60 cards (box and shadow in one primitives call, dot, text) in a handful of merged draws
    ed.check(got["geometrySubmitted"] >= 120, f"every box and text submitted: {got}")
    ed.check(got["drawCalls"] <= 20, f"batched into a few draws: {got}")
    ed.check(got["runsMerged"] >= 1 and got["instancesDrawn"] >= 180, f"merged instanced runs: {got}")
    # the cards are clear of the list's rounded corners: the scissor clips, no stencil
    ed.check(got["stencilDraws"] == 0, f"no stencil draw: {got}")
    ed.check(got["boxShadowTextures"] == 0, f"no box-shadow texture: {got}")
    ed.check(got["atlasBytes"] > 0 and got["atlasBytes"] % (1024 * 1024) == 0, f"R8 glyph pages of 1 MiB: {got}")


async def test_paint_and_raster(ed: Editor):
    card = await detail(ed, "#card3")
    ed.check(card.get("paint", {}).get("path") == "primitive", f"a card is primitives: {card.get('paint')}")
    dashed = await detail(ed, "#dashed")
    ed.check(dashed.get("paint") == {"path": "mesh", "reason": "border-style"}, f"a dashed border is a mesh: {dashed.get('paint')}")
    label = await detail(ed, "#label")
    raster = label.get("raster", {})
    ed.check(raster.get("mode") == "bitmap", f"the scaled label is a bitmap at rest: {raster}")
    # scale(2) times the Game view's pixels per canvas pixel: above the context's own scale
    ed.check(raster.get("scale", 0) > 1.0, f"at its on-screen scale: {raster}")
    S["labelScale"] = raster.get("scale")


async def test_project_settings(ed: Editor):
    keys = ["graphics.html_ui_glyph_atlas_mib", "graphics.html_ui_sdf_primitives", "graphics.html_ui_transform_aware_text"]
    got = await ed.call("get_project_settings", keys=keys)
    text = str(got)
    ed.check_in("0", text, "the atlas budget defaults to the platform's (0)")
    ed.check("True" in text or "true" in text, f"primitives and transform-aware text on by default: {got}")

    msg = await ed.call("set_project_settings", values={"graphics.html_ui_glyph_atlas_mib": 2}, expect_error=True)
    ed.check_in("0 or 4-256", msg)
    await ed.call("set_project_settings", values={"graphics.html_ui_glyph_atlas_mib": 32})
    await ed.call("set_project_settings", values={"graphics.html_ui_sdf_primitives": False,
                                                   "graphics.html_ui_transform_aware_text": False})
    yaml = (ed.project / "project.yaml").read_text()
    ed.check("htmlUiGlyphAtlasMiB: 32" in yaml, "the budget saved to project.yaml")
    ed.check("htmlUiSdfPrimitives: false" in yaml and "htmlUiTransformAwareText: false" in yaml, "the toggles saved to project.yaml")

    # at once: the cards become meshes and shadow textures, the label the context's bitmap stretched
    await render(ed, "off")
    await render(ed, "off")
    card = await detail(ed, "#card3")
    ed.check(card.get("paint", {}).get("path") == "mesh", f"without SDF primitives the card is a mesh: {card.get('paint')}")
    got = await stats(ed)
    ed.check(got["boxShadowTextures"] > 0, f"the shadows are textures again: {got}")
    label = await detail(ed, "#label")
    raster = label.get("raster", {})
    ed.check(raster.get("scale", 1) == 0, f"without transform-aware text the label is at the context's scale: {raster}")

    await ed.call("set_project_settings", values={"graphics.html_ui_glyph_atlas_mib": 0, "graphics.html_ui_sdf_primitives": True,
                                                   "graphics.html_ui_transform_aware_text": True})
    await render(ed, "on")
    await render(ed, "on")
    card = await detail(ed, "#card3")
    ed.check(card.get("paint", {}).get("path") == "primitive", f"primitives again: {card.get('paint')}")


async def test_console_clean(ed: Editor):
    entries = await ed.console_since(types=["error"], limit=200)
    errors = [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "")]
    ed.check(not errors, f"[HTML UI] errors: {errors}")
