"""Editor child-scene compositing regression: documents must draw in both edit and play mode.

One scratch editor hosts the scenarios. Screenshots are retained with a failed scratch project.
"""

from harness import Editor, REPO
from htmlui_pixels import Shot, check_color

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}

CI = True
EDITOR = {"create": True, "name": "Html Ui Basic CI"}

TIMEOUT = 900

S: dict = {}

DOC = "assets/ui/basic.html"

HTML = """<html>
<head>
    <title>basic</title>
    <link type="text/css" href="basic.css"/>
</head>
<body>
    <div id="red" class="box"></div>
    <div id="blue" class="box"></div>
</body>
</html>
"""

# RCSS (phase 0): no user-agent stylesheet, so display: block is explicit; colors #RRGGBB(AA)
CSS = """body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
       font-family: sans-serif; font-size: 16px; color: #ffffff; }
div { display: block; }
.box { position: absolute; }
#red { left: 40px; top: 40px; width: 200px; height: 100px; background-color: #ff0000; }
#blue { left: 400px; top: 200px; width: 160px; height: 80px; background-color: #0000ff; }
"""

# the document of a child scene, over the blue box
LAYER = "assets/ui/layer.html"
LAYER_HTML = """<html>
<head>
    <title>layer</title>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0; }
        #layer { display: block; position: absolute; left: 480px; top: 240px; width: 200px; height: 100px;
                 background-color: #00ffff; }
    </style>
</head>
<body>
    <div id="layer"></div>
</body>
</html>
"""

RED = (255, 0, 0)
BLUE = (0, 0, 255)
CYAN = (0, 255, 255)

# canvas probes
P_RED = (140, 90)
P_BLUE = (480, 240)
P_EMPTY = (900, 500)
P_LAYER_OVER = (520, 260)   # the child scene's box over the blue box
P_LAYER_ONLY = (640, 320)


async def shoot(ed: Editor, label: str, scene=None) -> Shot:
    args = {"view": "game", "max_width": 1280}
    if scene is not None:
        args["scene"] = scene
    shot = await ed.call_raw("screenshot", args)
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"html-ui-{label}.png").write_bytes(png)
    return Shot(png, shot.value)


async def html_ui_errors(ed: Editor) -> list[str]:
    entries = await ed.console_since(types=["error"], limit=1000)
    return [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "")]


# -- tests ---------------------------------------------------------------------------------------


async def setup_document(ed: Editor):
    """A document and a contrasting scene for visibility/compositing checks."""
    await ed.console_since(None, limit=1)
    made = await ed.call("create_scene", name="HtmlUiBasic", type="UI", path="scenes/HtmlUiBasic")
    S["scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["scene"], settings={"background_color": [0.118, 0.118, 0.118, 1.0]})
    for path, content in ((DOC, HTML), ("assets/ui/basic.css", CSS)):
        await ed.call("write_file", path=path, content=content)

    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Menu")
    S["menu"] = ent["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["menu"], component="UIDocument",
                  values={"uidocument": {"filename": DOC}})

    shot = await shoot(ed, "step3", S["scene"])
    check_color(ed, shot, P_RED, RED, "red box center")
    check_color(ed, shot, P_BLUE, BLUE, "blue box center")
    check_color(ed, shot, P_EMPTY, (30, 30, 30), "transparent body shows the scene", tol=4)

    errors = await html_ui_errors(ed)
    ed.check(not errors, f"[HTML UI] errors: {errors}")


async def test_child_scene_document(ed: Editor):
    """A document of a child scene (drawn as a layer over its parent) shows over the parent's
    documents in the Game view and in play mode. The editor drew child scenes there with their
    editor overlays on, which the HTML UI takes for a Scene view frame and skips."""
    await setup_document(ed)
    await ed.call("write_file", path=LAYER, content=LAYER_HTML)
    made = await ed.call("create_scene", name="HtmlUiLayer", type="UI", path="scenes/HtmlUiLayer")
    S["layer"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["layer"], type="empty", name="LayerDoc")
    await ed.call("add_component", scene=S["layer"], entity=ent["entity_id"], component="UIDocument",
                  values={"uidocument": {"filename": LAYER}})
    await ed.call("save_scene", scene=S["layer"])
    await ed.call("set_child_scene", scene=S["scene"], child=S["layer"], start_active=True)
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("edit_build_scenes", action="add", scene="scenes/HtmlUiLayer.scene")

    shot = await shoot(ed, "child-edit", S["scene"])
    check_color(ed, shot, P_LAYER_OVER, CYAN, "Game view: the child scene's document over the parent's blue box")
    check_color(ed, shot, P_LAYER_ONLY, CYAN, "Game view: the child scene's document")
    check_color(ed, shot, P_RED, RED, "Game view: the parent's document under it")

    await ed.call("play", action="start", scene=S["scene"])
    try:
        shot = await shoot(ed, "child-play")
        check_color(ed, shot, P_LAYER_OVER, CYAN, "play: the child scene's document over the parent's blue box")
        check_color(ed, shot, P_LAYER_ONLY, CYAN, "play: the child scene's document")
        check_color(ed, shot, P_RED, RED, "play: the parent's document under it")
    finally:
        await ed.call("play", action="stop")

    errors = await html_ui_errors(ed)
    ed.check(not errors, f"[HTML UI] errors: {errors}")
