"""HTML UI shaders exported through MCP for every supported graphics backend.

The renderer's documented gallery is owned by TestRenderP1Golden; the editor Scene-view
pipeline is owned by test_html_ui_editor. This test verifies the export-facing contract.
"""

from harness import Editor, REPO

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}

CI = True
EDITOR = {"create": True, "name": "Html Ui Render CI"}

TIMEOUT = 1200


async def test_shaders_cross_compile(ed: Editor):
    """Every HTML UI variant for every graphics API generate_shaders accepts."""
    # by spec, not by ShaderKey number (the ShaderType values of the HTML UI's types moved by one
    # when origin/main's BLOOM came before them)
    posts = ("Pas", "Cmx", "Msk", "Shd", "Blr", "Unp", "Yuv", "Bmd")
    specs = ["htmlui", "htmlui:Tex", "htmluigradient", "htmluiprim"] + [f"htmluipost:{name}" for name in posts]
    out = ed.scratch.root / "htmlui-shaders"
    r = await ed.call("generate_shaders", output_dir=str(out), backends="all", format="binary",
                      shaders=specs, timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"generate_shaders: {r}")
    files = [p.name for p in out.rglob("*") if p.is_file()]
    # every variant for GLSL 410, GLSL 300 es, HLSL 5, MSL 2.1 (macOS, iOS) and SPIR-V
    languages = ("glsl410", "glsl300es", "hlsl5", "msl21macos", "msl21ios", "spirv10")
    # S16: the instanced primitives (htmlui_prim.vert/.frag) too
    stems = ["htmlui", "htmlui_Tex", "htmluigradient", "htmluiprim"] + [f"htmluipost_{name}" for name in ("Pas", "Cmx", "Msk", "Shd", "Blr", "Unp", "Yuv", "Bmd")]
    for stem in stems:
        for language in languages:
            ed.check(f"{stem}_{language}.sdat" in files, f"{stem}_{language}.sdat not generated: {sorted(files)}")


TICKER_DOC = "assets/ui/ticker.html"
TICKER_HTML = """<html>
<head>
    <title>ticker</title>
    <style>
        body { margin: 0; font-family: sans-serif; }
        #label { position: absolute; left: 20px; top: 20px; width: 400px; height: 90px; font-size: 72px;
                 line-height: 90px; color: #ffffff; background-color: #000000; }
    </style>
</head>
<body><div id="label">0</div></body>
</html>
"""

TICKER_SCRIPT = """---@class Ticker : ScriptInstance
local Ticker = { properties = {} }

function Ticker:init()
    self.doc = UIDocument(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

function Ticker:onUpdate()
    local label = self.doc:getElementById("label")
    if label then label.textContent = tostring(Engine.frameCount) end
end

return Ticker
"""


async def test_text_changing_every_frame_draws(ed: Editor):
    """S93 R33 (AC37, U9): a label set from onUpdate every frame draws its current number in each frame of a headless
    editor: the label's pixels differ between consecutive frames (OCR-free)."""
    from htmlui_pixels import Shot

    made = await ed.call("create_scene", name="HtmlUiTicker", type="UI")
    scene = made["scene"]["id"]
    await ed.call("write_file", path=TICKER_DOC, content=TICKER_HTML)
    ent = await ed.call("create_entity", scene=scene, type="empty", name="Ticker")
    await ed.call("add_component", scene=scene, entity=ent["entity_id"], component="UIDocument",
                  values={"uidocument": {"filename": TICKER_DOC}})
    await ed.call("create_script", kind="lua", name="Ticker", content=TICKER_SCRIPT, scene=scene, entity=ent["entity_id"])
    await ed.call("save_scene", scene=scene, path="scenes/HtmlUiTicker")
    await ed.call("play", action="start", scene=scene, fixed_fps=30, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        previous = None
        texts = []
        for i in range(5):
            await ed.call("wait", frames=1)
            texts.append((await ed.call("eval_lua", code="return Engine.frameCount, "
                                        "UIDocument(scene, entity('Ticker')):getElementById('label').textContent"))["values"])
            shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 1280})
            ed.check(not shot.is_error and shot.images, f"screenshot {i}: {shot.text[:300]}")
            (ed.scratch.root / f"ticker-{i}.png").write_bytes(shot.images[0][0])
            image = Shot(shot.images[0][0], shot.value)
            label = list(image.region(20, 20, 420, 110))
            ink = sum(1 for p in label if max(p) > 128)
            ed.check(ink > 100, f"frame {i}: the label's digits are drawn ({ink} light pixels)")
            if previous is not None:
                ed.check(texts[-1][1] != texts[-2][1], f"frame {i}: the script set a new number (frame count, text): {texts}")
                ed.check(label != previous, f"frame {i}: the label shows the new number (frame count, text): {texts}")
            previous = label
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
