"""HTML UI rendering features through MCP (phase 1, p1-render).

A gallery document in a UI scene: gradients (linear, radial, conic), filters (grayscale,
blur, drop-shadow, opacity), a backdrop blur, a rounded clip, a box shadow and an SVG image.
Game view screenshots are probed at listed canvas points (expected colours +-6/255 where the
colour is exact). The Scene view of the UI scene shows the document too (its canvas in world
space). Then generate_shaders cross-compiles every HTML UI shader variant (HTMLUI, the
gradient and the seven post-process variants, phase 6's YUV conversion included) for every
graphics API the editor exports, so the backends that cannot run here (D3D11, Metal, GLES) still get valid shaders.

Runs on a scratch copy of the fixture project (canvas 1280 x 720). Coordinates are canvas
pixels; screenshots map them with canvas_rect_in_image.
"""

import sys
from pathlib import Path

from harness import Editor

# the PNG decoding and canvas probes of the phase-0 test
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_html_ui_basic import Shot, decode_png, near  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Html Ui Render CI"}

TIMEOUT = 1200

S: dict = {}

DOC = "assets/ui/gallery.html"

SVG = """<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
<rect x="0" y="0" width="64" height="64" fill="#2060c0"/>
<circle cx="32" cy="32" r="16" fill="#ffffff"/>
</svg>
"""

# RCSS (what RmlUi 6.3 accepts in phase 1): decorators for gradients, colour first in shadows
HTML = """<html>
<head>
    <title>gallery</title>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0; }
        div { display: block; }
        .abs { position: absolute; }
        #linear { left: 40px; top: 40px; width: 300px; height: 100px; decorator: linear-gradient(90deg, #ff0000, #0000ff); }
        #radial { left: 380px; top: 40px; width: 100px; height: 100px;
                  decorator: radial-gradient(circle 50px at center, #ffffff, #000000); }
        #conic { left: 520px; top: 40px; width: 100px; height: 100px;
                 decorator: conic-gradient(from 0deg at center, #ff0000, #00ff00, #0000ff, #ff0000); }
        #gray { left: 40px; top: 180px; width: 120px; height: 80px; background-color: #e04020; filter: grayscale(1); }
        #blur { left: 200px; top: 180px; width: 120px; height: 80px; background-color: #ffffff; filter: blur(8px); }
        #drop { left: 360px; top: 180px; width: 100px; height: 60px; background-color: #00ff00;
                filter: drop-shadow(#000000 12px 12px 0px); }
        #group { left: 520px; top: 180px; width: 160px; height: 100px; filter: opacity(0.5); }
        #group div { position: absolute; width: 100px; height: 60px; }
        #group .r { left: 0px; top: 0px; background-color: #ff0000; }
        #group .b { left: 60px; top: 40px; background-color: #0000ff; }
        .stripe { top: 320px; width: 40px; height: 200px; }
        #s1 { left: 40px; background-color: #ff4040; } #s2 { left: 120px; background-color: #40ff40; }
        #s3 { left: 200px; background-color: #4040ff; } #s4 { left: 280px; background-color: #ffff40; }
        #glass { left: 60px; top: 360px; width: 260px; height: 120px; background-color: #ffffff20; backdrop-filter: blur(10px); }
        #clip { left: 760px; top: 40px; width: 200px; height: 160px; border-radius: 60px; overflow: hidden; background-color: #ffff00; }
        #clip div { position: absolute; left: -20px; top: -20px; width: 240px; height: 200px; background-color: #ff00ff; }
        #shadow { left: 760px; top: 280px; width: 160px; height: 100px; background-color: #40c0c0;
                  box-shadow: #ffffff 16px 16px 0px 0px; }
        #icon { left: 1000px; top: 40px; width: 128px; height: 128px; }
    </style>
</head>
<body>
    <div id="linear" class="abs"></div>
    <div id="radial" class="abs"></div>
    <div id="conic" class="abs"></div>
    <div id="gray" class="abs"></div>
    <div id="blur" class="abs"></div>
    <div id="drop" class="abs"></div>
    <div id="group" class="abs"><div class="r"></div><div class="b"></div></div>
    <div id="s1" class="abs stripe"></div><div id="s2" class="abs stripe"></div>
    <div id="s3" class="abs stripe"></div><div id="s4" class="abs stripe"></div>
    <div id="glass" class="abs"></div>
    <div id="clip" class="abs"><div></div></div>
    <div id="shadow" class="abs"></div>
    <img id="icon" class="abs" src="gallery-icon.svg"/>
</body>
</html>
"""

BG = (30, 30, 30)

# (canvas point, expected colour, tolerance, what)
PROBES = [
    ((44, 90), (253, 0, 2), 6, "linear gradient: red at the start"),
    ((336, 90), (2, 0, 253), 6, "linear gradient: blue at the end"),
    ((190, 90), (128, 0, 127), 6, "linear gradient: halfway"),
    ((430, 90), (255, 255, 255), 8, "radial gradient: white centre"),
    ((570, 45), (255, 0, 0), 30, "conic gradient: red at the top"),
    # grayscale(#E04020) = 0.2126 * 224 + 0.7152 * 64 + 0.0722 * 32 = 96
    ((100, 220), (96, 96, 96), 6, "grayscale filter"),
    ((260, 220), (255, 255, 255), 6, "blur: the centre stays white"),
    ((410, 210), (0, 255, 0), 6, "drop-shadow: the element over its shadow"),
    ((466, 246), (0, 0, 0), 6, "drop-shadow: the hard shadow, 12 px down and right"),
    # opacity(0.5) is a group: the overlap shows only the top (blue) child at 50 %
    ((540, 200), (143, 15, 15), 6, "filter opacity: red alone"),
    ((600, 230), (15, 15, 143), 6, "filter opacity: the overlap is the top child only"),
    ((50, 330), (255, 64, 64), 6, "stripe outside the glass"),
    ((765, 45), BG, 6, "rounded clip: the corner outside the radius"),
    ((860, 120), (255, 0, 255), 6, "rounded clip: the clipped child inside"),
    ((800, 330), (64, 192, 192), 6, "box-shadow: the element"),
    ((930, 392), (255, 255, 255), 6, "box-shadow: its hard shadow"),
    ((1064, 104), (255, 255, 255), 10, "svg image: the white circle"),
    ((1006, 46), (32, 96, 192), 10, "svg image: the blue square"),
]


async def shoot(ed: Editor, label: str, view: str = "game") -> Shot:
    args = {"view": view, "max_width": 1280, "scene": S["scene"]}
    shot = await ed.call_raw("screenshot", args)
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"html-ui-render-{label}.png").write_bytes(png)
    return Shot(png, shot.value)


async def html_ui_messages(ed: Editor, kind: str) -> list[str]:
    entries = await ed.console_since(types=[kind], limit=1000)
    return [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "")]


async def test_gallery_in_game_view(ed: Editor):
    await ed.console_since(None, limit=1)
    made = await ed.call("create_scene", name="HtmlUiRender", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["scene"], settings={"background_color": [0.118, 0.118, 0.118, 1.0]})
    await ed.call("write_file", path="assets/ui/gallery-icon.svg", content=SVG)
    await ed.call("write_file", path=DOC, content=HTML)
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Gallery")
    S["entity"] = ent["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["entity"], component="UIDocument",
                  values={"uidocument": {"filename": DOC}})

    shot = await shoot(ed, "game")
    for point, want, tol, what in PROBES:
        got = shot.px(*point)
        ed.check(near(got, want, tol), f"{what}: pixel at canvas {point} is {got}, expected {want} +-{tol}")

    # blurred: the box edge is a ramp, not a step
    edge = shot.px(200, 220)
    ed.check(BG[0] + 20 < edge[0] < 235, f"blur: the edge at x = 200 is {edge}, expected between the box and the background")
    # backdrop blur: the blue stripe (200..240) bleeds into the gap left of it under the glass,
    # but not outside it
    ed.check(near(shot.px(190, 340), BG, 6), f"outside the glass the stripes are sharp: {shot.px(190, 340)}")
    under = shot.px(190, 420)
    ed.check(under[2] > BG[2] + 40, f"under the glass the blue stripe is blurred into the gap: {under}")

    errors = await html_ui_messages(ed, "error")
    ed.check(not errors, f"[HTML UI] errors: {errors}")
    warnings = await html_ui_messages(ed, "warning")
    ed.check(not warnings, f"[HTML UI] warnings: {warnings}")
    S["game"] = shot


async def test_scene_view_shows_document(ed: Editor):
    """The Scene view of a UI scene draws the document's canvas in world space."""
    await ed.call("set_scene_camera", scene=S["scene"])
    shot = await ed.call_raw("screenshot", {"view": "scene", "max_width": 1280, "scene": S["scene"]})
    ed.check(not shot.is_error and shot.images, f"scene view: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / "html-ui-render-scene.png").write_bytes(png)
    w, h, ch, rows = decode_png(png)

    def count(color):
        return sum(1 for y in range(0, h, 2) for x in range(0, w, 2) if near(tuple(rows[y][x * ch:x * ch + 3]), color, 8))

    greens, magentas = count((0, 255, 0)), count((255, 0, 255))
    ed.check(greens > 20, f"the drop-shadow box (pure green) is in the Scene view: {greens} sampled pixels")
    ed.check(magentas > 20, f"the clipped box (magenta) is in the Scene view: {magentas} sampled pixels")


async def test_shaders_cross_compile(ed: Editor):
    """Every HTML UI variant for every graphics API generate_shaders accepts."""
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiRender")
    # by spec, not by ShaderKey number (the ShaderType values of the HTML UI's types moved by one
    # when origin/main's BLOOM came before them)
    posts = ("Pas", "Cmx", "Msk", "Shd", "Blr", "Unp", "Yuv")
    specs = ["htmlui", "htmlui:Tex", "htmluigradient"] + [f"htmluipost:{name}" for name in posts]
    out = ed.scratch.root / "htmlui-shaders"
    r = await ed.call("generate_shaders", output_dir=str(out), backends="all", format="binary",
                      shaders=specs, timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"generate_shaders: {r}")
    files = [p.name for p in out.rglob("*") if p.is_file()]
    # every variant for GLSL 410, GLSL 300 es, HLSL 5, MSL 2.1 (macOS, iOS) and SPIR-V
    languages = ("glsl410", "glsl300es", "hlsl5", "msl21macos", "msl21ios", "spirv10")
    stems = ["htmlui", "htmlui_Tex", "htmluigradient"] + [f"htmluipost_{name}" for name in ("Pas", "Cmx", "Msk", "Shd", "Blr", "Unp", "Yuv")]
    for stem in stems:
        for language in languages:
            ed.check(f"{stem}_{language}.sdat" in files, f"{stem}_{language}.sdat not generated: {sorted(files)}")
    errors = await html_ui_messages(ed, "error")
    ed.check(not errors, f"[HTML UI] errors: {errors}")
