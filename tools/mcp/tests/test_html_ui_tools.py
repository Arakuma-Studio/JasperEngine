"""The HTML/CSS UI MCP tools (phase 4, p4-mcp-tools, spec §3.6 and §5.6): inspect_ui, assert_ui,
create_ui_document, send_input's target / to_target / document, import_files' fonts / icons / URLs /
from_document, the jasper://examples/html-ui resources and the tool budget.

Besides §5.6's cases: invalid selectors everywhere, documents not loaded yet or failing to load,
texture-mode documents, play vs edit mode, stacked documents (the pointer passes through a
transparent upper page), the gamepad focus seen by inspect_ui, unicode paths, a 3000-element
document (output cut within the budget) and fast successive reloads.

Runs on a scratch copy of the fixture project (canvas 1280 x 720). The importers read
JASPERENGINE_IMPORT_MIRROR, built here before the editor starts (no network).
"""

from __future__ import annotations

import atexit
import base64
import json
import os
import re
import shutil
import sys
import tempfile
import time
import urllib.parse
from pathlib import Path

from mcp.shared.exceptions import MCPError

import harness
from harness import Editor, REPO

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import pngdiff  # noqa: E402
import tool_budget  # noqa: E402
from test_html_ui_checks import GOOD_CSS, GOOD_HTML, LLM_CSS, LLM_HTML, normalized, run_json, scratch_project  # noqa: E402

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}
CI = True

TIMEOUT = 900

S: dict = {}

CYAN = (0x00, 0xC8, 0xFF)
RED = (0xFF, 0x3B, 0x30)

LIBRARY = harness.REPO / "docs" / "mcp" / "guide" / "html-ui-examples"
MANIFEST = json.loads((LIBRARY / "examples.json").read_text(encoding="utf-8"))

# §3.6.7 and phase 5 (spec 3.13): the HTML UI tools' caps. The limits themselves live in
# tools/mcp/budget.json (S31 R21), the single source of the tool budget: its exceptions entries
# for these tools record the caps of the HTML UI specs, the default listing its total.
HTML_UI_TOOLS = ("inspect_ui", "assert_ui", "create_ui_document", "send_input", "import_files",
                 "list_ui_effects", "set_ui_effect_time")

CYAN = (0x00, 0xC8, 0xFF)
RED = (0xFF, 0x3B, 0x30)

# ---------------------------------------------------------------------------- the import mirror (§3.9)

FONT_CSS_URL = "https://fonts.googleapis.com/css2?family=Karla:wght@400"
FONT_FILE_URL = "https://fonts.gstatic.com/s/karla/v1/karla.ttf"
DOC_FONT_URL = "https://fonts.googleapis.com/css2?family=Karla:wght@400;700&display=swap"
DOC_FONT_700_URL = "https://fonts.gstatic.com/s/karla/v1/karla-700.ttf"
ANIM_URL = "https://assets.lottiefiles.com/packages/tools/anim.json"
MISSING_URL = "https://assets.lottiefiles.com/packages/tools/missing.json"
SWORD_URL = "https://example.com/sword.png"
KARLA = harness.REPO / "libs" / "imgui" / "misc" / "fonts" / "Karla-Regular.ttf"
LOTTIE = json.dumps({"v": "5.7.4", "fr": 30, "ip": 0, "op": 30, "w": 64, "h": 64, "layers": []})


def mirror_file(root: Path, url: str, content: bytes) -> None:
    parts = urllib.parse.urlsplit(url)
    path_and_query = parts.path + ("?" + parts.query if parts.query else "")
    target = root / parts.hostname / urllib.parse.quote(path_and_query, safe="")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)


def png(width: int, height: int, rgb: tuple) -> bytes:
    return pngdiff.encode(width, height, bytes(list(rgb) + [255]) * (width * height))


def font_face(family: str, weight: int, url: str) -> str:
    return (f"@font-face {{\n  font-family: '{family}';\n  font-style: normal;\n  font-weight: {weight};\n"
            f"  src: url({url}) format('truetype');\n}}\n")


MIRROR = Path(tempfile.mkdtemp(prefix="jasper-tools-mirror-"))
atexit.register(shutil.rmtree, MIRROR, ignore_errors=True)
mirror_file(MIRROR, FONT_CSS_URL, font_face("Karla", 400, FONT_FILE_URL).encode())
mirror_file(MIRROR, FONT_FILE_URL, KARLA.read_bytes())
mirror_file(MIRROR, DOC_FONT_URL, (font_face("Karla", 400, FONT_FILE_URL) + font_face("Karla", 700, DOC_FONT_700_URL)).encode())
mirror_file(MIRROR, DOC_FONT_700_URL, KARLA.read_bytes())
mirror_file(MIRROR, ANIM_URL, LOTTIE.encode())
mirror_file(MIRROR, SWORD_URL, png(8, 8, (200, 200, 200)))
os.environ["JASPERENGINE_IMPORT_MIRROR"] = str(MIRROR)

# ---------------------------------------------------------------------------- documents

FLAGS_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Flags</title>
  <link rel="stylesheet" href="flags.css">
</head>
<body>
  <div id="box"><span id="label">A label far too long for its one hundred pixel box</span></div>
  <div id="clipper"><div id="child"></div></div>
  <div id="hidden-panel"><p id="inside">Hidden</p></div>
</body>
</html>
"""

FLAGS_CSS = """body { margin: 0; font-family: sans-serif; color: #ffffff; }
#box { position: absolute; left: 20px; top: 20px; width: 100px; height: 30px; white-space: nowrap; }
#clipper { position: absolute; left: 20px; top: 100px; width: 50px; height: 50px; overflow: hidden; }
#child { width: 200px; height: 20px; background-color: #ff0000; }
#hidden-panel { display: none; }
"""

ASSERT_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Asserts</title>
  <link rel="stylesheet" href="asserts.css">
</head>
<body>
  <div id="parent">
    <button id="small" class="btn">S</button>
    <button id="big" class="btn">Big</button>
    <div id="narrow">Purchase Legendary Sword</div>
    <div id="wide">Fits</div>
    <div id="a" class="sq"></div>
    <div id="b" class="sq"></div>
    <div id="c" class="sq"></div>
    <div id="out"></div>
    <div id="far"></div>
    <p id="lowcontrast">Grey on grey</p>
    <p id="highcontrast">White on black</p>
    <div id="gone">Gone</div>
  </div>
</body>
</html>
"""

ASSERT_CSS = """body { margin: 0; font-family: sans-serif; font-size: 16px; color: #ffffff; }
#parent { position: absolute; left: 0; top: 0; width: 600px; height: 400px; }
.btn { position: absolute; margin: 0; padding: 0; border: 0; color: #ffffff; background-color: #000000; }
#small { left: 10px; top: 10px; width: 30px; height: 20px; }
#big { left: 60px; top: 10px; width: 100px; height: 50px; }
#narrow { position: absolute; left: 10px; top: 80px; width: 60px; height: 24px; white-space: nowrap; }
#wide { position: absolute; left: 10px; top: 120px; width: 300px; height: 24px; }
.sq { position: absolute; width: 40px; height: 40px; background-color: #336699; }
#a { left: 200px; top: 10px; }
#b { left: 220px; top: 30px; }
#c { left: 300px; top: 10px; }
#out { position: absolute; left: 560px; top: 300px; width: 100px; height: 40px; }
#far { position: absolute; left: 1250px; top: 300px; width: 100px; height: 40px; }
#lowcontrast { position: absolute; left: 10px; top: 160px; width: 200px; margin: 0; background-color: #888888; color: #777777; }
#highcontrast { position: absolute; left: 10px; top: 200px; width: 200px; margin: 0; background-color: #000000; color: #ffffff; }
#gone { display: none; }
"""

VIEWPORT_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Viewport</title>
  <link rel="stylesheet" href="viewport.css">
</head>
<body>
  <div class="row"><div id="c1" class="cell"></div><div id="c2" class="cell"></div></div>
</body>
</html>
"""

VIEWPORT_CSS = """body { margin: 0; }
.row { position: absolute; left: 0; top: 0; width: 1000px; height: 40px; display: flex; flex-direction: row; }
.cell { width: 100px; height: 40px; flex-shrink: 0; background-color: #446688; }
@media (max-width: 800px) { .row { flex-direction: column; } }
"""

CONTRAST_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Contrast</title>
  <link rel="stylesheet" href="contrast.css">
</head>
<body>
  <span id="grey">Score 1200</span>
  <div class="dark"><span id="white">Ammo 30</span></div>
</body>
</html>
"""

CONTRAST_CSS = """body { margin: 0; font-family: sans-serif; font-size: 16px; }
#grey { position: absolute; left: 20px; top: 20px; color: #808080; }
.dark { position: absolute; left: 20px; top: 80px; padding: 8px; background-color: rgba(0, 0, 0, 0.85); }
#white { color: #ffffff; }
"""

CLICKS_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Clicks</title>
  <link rel="stylesheet" href="clicks.css">
</head>
<body>
  <div class="column">
    <button id="ok">OK</button>
    <button id="open">Open</button>
    <button id="ghost">Ghost</button>
  </div>
  <div id="panel" class="hidden"><button class="close">Close</button></div>
</body>
</html>
"""

CLICKS_CSS = """body { margin: 0; font-family: sans-serif; color: #ffffff; }
.column { position: absolute; left: 100px; top: 100px; width: 200px; display: flex; flex-direction: column; gap: 20px; }
button { height: 50px; color: #ffffff; background-color: #404040; border: 0; font-size: 18px; }
button:focus { background-color: #4060c0; }
#panel { position: absolute; left: 500px; top: 100px; width: 300px; height: 200px; background-color: #202838; }
#panel .close { margin: 20px; width: 120px; }
.hidden { display: none; }
#ghost { opacity: 0; }
"""

CLICKS_LUA = """---@class ClicksProbe : ScriptInstance
local ClicksProbe = {}

function ClicksProbe:init()
    self.doc = UIDocument(self.scene, self.entity)
    RegisterEvent(self, self.doc.onLoad, "bind")
    self:bind()
end

function ClicksProbe:bind()
    local doc = self.doc
    doc:getElementById("ok"):addEventListener("click", function() print("tools: ok") end)
    doc:getElementById("ghost"):addEventListener("click", function() print("tools: ghost") end)
    doc:getElementById("open"):addEventListener("click", function() doc:getElementById("panel").classList:remove("hidden") end)
    doc:querySelector("#panel .close"):addEventListener("click", function()
        print("tools: close")
        doc:getElementById("panel").classList:add("hidden")
    end)
end

return ClicksProbe
"""

# a transparent page over the clicks: only a badge in the corner (the pointer passes through the rest)
OVERLAY_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Overlay</title>
  <style>
    body { margin: 0; font-family: sans-serif; color: #ffffff; }
    #badge { position: absolute; right: 20px; top: 20px; width: 120px; height: 40px; background-color: #a03030; }
  </style>
</head>
<body>
  <div id="badge">Badge</div>
</body>
</html>
"""


# ---------------------------------------------------------------------------- helpers

def disk_write(ed: Editor, rel: str, text) -> Path:
    """An external edit (the agent's own Write tool), stamped surely after the previous one."""
    time.sleep(0.02)
    path = ed.project / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(text, bytes):
        path.write_bytes(text)
    else:
        path.write_text(text, encoding="utf-8")
    return path


def line_col(text: str, needle: str) -> tuple[int, int]:
    for i, line in enumerate(text.split("\n")):
        if needle in line:
            return i + 1, line.index(needle) + 1
    raise AssertionError(f"no line with {needle!r}")


async def lua(ed: Editor, code: str, **args) -> list:
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def document(ed: Editor, html_rel: str, html: str, css_rel: str | None = None, css: str | None = None,
                   scene: int | None = None, **create) -> dict:
    """The files written by the agent (disk), then create_ui_document on them (kept, not overwritten)."""
    disk_write(ed, html_rel, html)
    if css_rel:
        disk_write(ed, css_rel, css)
    r = await ed.call("create_ui_document", scene=scene or S["scene"], html=html_rel, **create)
    ed.check(html_rel in r["files_kept"], f"the agent's html is kept: {r}")
    return r


def color_pixels(img, rgb: tuple, tolerance: int = 40) -> list[tuple[int, int]]:
    w, h, data = img
    out = []
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            i = row + x * 4
            if abs(data[i] - rgb[0]) <= tolerance and abs(data[i + 1] - rgb[1]) <= tolerance and abs(data[i + 2] - rgb[2]) <= tolerance:
                out.append((x, y))
    return out


def bounds(points: list[tuple[int, int]]) -> tuple[int, int, int, int]:
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1


def image_rect(info: dict, box: list, image_width: int) -> tuple[float, float, float, float]:
    """A canvas rect [x, y, w, h] mapped into the tool's image (its structuredContent image.canvas_rect)."""
    cx, cy, cw, ch = info["canvas_rect"]
    scale = image_width / cw
    return (box[0] - cx) * scale, (box[1] - cy) * scale, box[2] * scale, box[3] * scale


async def console_lines(ed: Editor) -> list[str]:
    return [e.get("message", "") for e in await ed.console_since(limit=500)]


# ---------------------------------------------------------------------------- schemas and budget

async def ensure_scene(ed: Editor):
    if S.get("editor") is ed:
        return
    S.clear()
    S["editor"] = ed
    made = await ed.call("create_scene", name="HtmlUiTools", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiTools")


async def ensure_fixture(ed: Editor, kind: str):
    await ensure_scene(ed)
    if kind in S:
        return S[kind]
    if kind == "other_scene":
        other = await ed.call("create_scene", name="HtmlUiOther", type="UI")
        S[kind] = other["scene"]["id"]
        await ed.call("create_ui_document", scene=S[kind], html="assets/ui/other.html")
    elif kind == "texture":
        made = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/terminal.html",
                             properties={"renderMode": "texture"})
        S[kind] = made["entity"]["id"]
    else:
        html, css = {
            "panel": (GOOD_HTML, GOOD_CSS),
            "viewport": (VIEWPORT_HTML, VIEWPORT_CSS),
            "asserts": (ASSERT_HTML, ASSERT_CSS),
        }[kind]
        made = await document(ed, f"assets/ui/{kind}.html", html, f"assets/ui/{kind}.css", css)
        S[kind] = made["entity"]["id"]
    return S[kind]


async def test_tools_listed_with_annotations(ed: Editor):
    await ensure_scene(ed)
    for name in ("inspect_ui", "assert_ui"):
        a = ed.tool(name).annotations
        ed.check(a and a.read_only_hint is True and a.idempotent_hint is True, f"{name} annotations: {a}")
    a = ed.tool("create_ui_document").annotations
    ed.check(a and not a.read_only_hint and not a.destructive_hint, f"create_ui_document annotations: {a}")
    enum = ed.tool("create_ui_document").input_schema["properties"]["template"]["enum"]
    ed.check_eq(set(enum), {e["name"] for e in MANIFEST["examples"]} | {"blank"}, "template enum")
    kinds = ed.tool("assert_ui").input_schema["properties"]["checks"]["items"]["properties"]["type"]["enum"]
    ed.check_eq(kinds, ["exists", "count", "visible", "text_fits", "no_overlap", "within_parent", "within_viewport",
                        "contrast", "min_size", "virtual_window", "tooltip", "selected"], "assert_ui check types")

    # allowedWhilePlaying: false is the registry's refusal
    await ed.call("play", action="start", scene=S["scene"])
    try:
        r = await ed.call_raw("create_ui_document", {"html": "assets/ui/during_play.html"})
        ed.check(r.is_error and "not available while a scene is playing" in r.text, f"create_ui_document in play: {r.text}")
        # the read tools work during play (nothing to inspect yet)
        r = await ed.call_raw("inspect_ui", {})
        ed.check(r.is_error and "create_ui_document" in r.text, f"inspect_ui in play without documents: {r.text}")
    finally:
        await ed.call("play", action="stop")
    ed.check(not (ed.project / "assets/ui/during_play.html").exists(), "nothing written during play")


async def test_budget(ed: Editor):
    budget = tool_budget.load_budget()
    rows = {t.name: tool_budget.measure(t, False) for t in ed.tool_list}
    for name in HTML_UI_TOOLS:
        limit = tool_budget.tool_limit(budget, name)
        ed.check(rows[name]["total"] <= limit, f"{name}: {rows[name]['total']} chars, over its limit {limit}")
    core = [r for r in rows.values() if r["group"] == "core"]
    total = sum(r["total"] for r in core)
    limit = budget["default_listing"]["chars"]
    print(f"     [budget] {len(core)} core tools, {total} chars; " +
          ", ".join(f"{n} {rows[n]['total']}" for n in HTML_UI_TOOLS))
    ed.check(total <= limit, f"the default listing totals {total} chars, over {limit} (tools/mcp/budget.json)")
    ed.check(len(ed.tool("inspect_ui").description) <= 520 and len(ed.tool("assert_ui").description) <= 520 and
             len(ed.tool("create_ui_document").description) <= 480, "description caps (§3.6.1-3.6.3)")


# ---------------------------------------------------------------------------- create_ui_document

async def test_create_blank(ed: Editor):
    await ensure_scene(ed)
    raw = await ed.call_raw("create_ui_document", {"scene": S["scene"], "html": "assets/ui/menu.html"})
    ed.check(not raw.is_error, raw.text)
    r = raw.structured
    ed.check_eq(r["files_written"], ["assets/ui/menu.html", "assets/ui/menu.css"], "blank writes the html and its css")
    ed.check(r["script"] is None, f"blank has no script by default: {r['script']}")
    ed.check_eq((r["entity"]["name"], r["template"], r["html"]), ("Menu", "blank", "assets/ui/menu.html"), "result")
    doc = r["document"]
    ed.check_eq((doc["load_count"], doc["diagnostics_count"]["error"], doc["diagnostics_count"]["warning"]), (1, 0, 0),
                f"loaded once, clean: {doc}")
    ed.check_eq(r["diagnostics"], [], "no diagnostics")
    html = (ed.project / "assets/ui/menu.html").read_text()
    ed.check('href="menu.css"' in html and "<h1>Menu</h1>" in html, f"the blank html: {html}")
    entity = await ed.call("get_entity", scene=S["scene"], entity=r["entity"]["id"])
    ed.check_eq(entity["components"]["uidocument"]["filename"], "assets/ui/menu.html", "filename reads back project-relative")

    # one scene undo step removes the GameObject, one project step the files
    await ed.call("undo", scene=S["scene"])
    found = await ed.call("find_entities", scene=S["scene"], name="Menu")
    ed.check_eq(found["count"], 0, "undo (scene) removes the GameObject")
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / "assets/ui/menu.html").exists() and not (ed.project / "assets/ui/menu.css").exists(),
             "undo scope project removes both files")


async def test_create_template_renames(ed: Editor):
    await ensure_scene(ed)
    r = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/title_screen.html", template="main_menu")
    ed.check_eq(set(r["files_written"]), {"assets/ui/title_screen.html", "assets/ui/title_screen.css",
                                          "assets/ui/title_screen_assets/logo.svg", "scripts/TitleScreenUI.lua"}, "files")
    ed.check_eq(r["script"], {"path": "scripts/TitleScreenUI.lua", "attached": True}, "script")
    lua_text = (ed.project / "scripts/TitleScreenUI.lua").read_text()
    ed.check("---@class TitleScreenUI" in lua_text and "MainMenuUI" not in lua_text, "the Lua class is renamed")
    html = (ed.project / "assets/ui/title_screen.html").read_text()
    ed.check('href="title_screen.css"' in html and "main_menu.css" not in html, "the html links its renamed css")
    ed.check("title_screen_assets/" in html + (ed.project / "assets/ui/title_screen.css").read_text(), "assets renamed")
    ed.check_eq(r["document"]["diagnostics_count"]["error"] + r["document"]["diagnostics_count"]["warning"], 0, "clean")
    entity = await ed.call("get_entity", scene=S["scene"], entity=r["entity"]["id"])
    ed.check_eq(entity["components"]["script"]["scripts"][0]["className"], "TitleScreenUI", "attached")


async def test_create_existing_kept(ed: Editor):
    await ensure_scene(ed)
    disk_write(ed, "assets/ui/kept.html", GOOD_HTML.replace("panel.css", "kept.css"))
    disk_write(ed, "assets/ui/kept.css", GOOD_CSS)
    r = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/kept.html")
    ed.check_eq(r["files_kept"], ["assets/ui/kept.html"], "the existing html is kept")
    ed.check_eq(r["files_written"], [], "nothing written from the template")
    tree = await ed.call("inspect_ui", entity=r["entity"]["id"], selector=".title")
    ed.check_eq(tree["elements"][0]["text"], "Settings", "the document shows the agent's html")

    r = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/kept.html", overwrite=True, name="Replaced")
    ed.check("assets/ui/kept.html" in r["files_written"], f"overwrite replaces it: {r}")
    ed.check('<main class="screen">' in (ed.project / "assets/ui/kept.html").read_text(), "the blank template replaced it")

    r = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/scripted.html", script=True)
    ed.check_eq(r["script"], {"path": "scripts/ScriptedUI.lua", "attached": True}, "blank with script: true")
    ed.check("---@class ScriptedUI" in (ed.project / "scripts/ScriptedUI.lua").read_text(), "BlankUI renamed")

    # an existing html and an example template: the html stays, the template's Lua is still written and attached
    # (the HUD's script expects the HUD's markup: the GameObject goes before anything plays)
    disk_write(ed, "assets/ui/own_hud.html", OVERLAY_HTML)
    r = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/own_hud.html", template="hud")
    ed.check_eq((r["files_kept"], r["files_written"]), (["assets/ui/own_hud.html"], ["scripts/OwnHudUI.lua"]), "only the Lua")
    ed.check_eq(r["script"], {"path": "scripts/OwnHudUI.lua", "attached": True}, "attached")
    await ed.call("delete_entities", scene=S["scene"], entities=[r["entity"]["id"]])


async def test_create_errors(ed: Editor):
    await ensure_scene(ed)
    before = (await ed.call("find_entities", scene=S["scene"]))["count"]
    r = await ed.call_raw("create_ui_document", {"scene": S["scene"], "html": "outside.html"})
    ed.check(r.is_error and "inside the assets directory (assets/)" in r.text, f"outside the assets: {r.text}")
    r = await ed.call_raw("create_ui_document", {"scene": S["scene"], "html": "assets/ui/styles.css"})
    ed.check(r.is_error and ".html" in r.text, f"a .css as html: {r.text}")
    r = await ed.call_raw("create_ui_document", {"scene": S["scene"], "html": "assets/ui/bad_props.html", "properties": {"colr": 1}})
    ed.check(r.is_error and "colr" in r.text and "sortOrder" in r.text and "renderMode" in r.text, f"unknown property: {r.text}")
    r = await ed.call_raw("create_ui_document", {"scene": S["scene"], "html": "assets/ui/bad_props.html", "template": "nope"})
    ed.check(r.is_error and "main_menu" in r.text, f"unknown template: {r.text}")
    r = await ed.call_raw("create_ui_document", {"scene": S["scene"], "html": "../escape.html"})
    ed.check(r.is_error, f"a path leaving the project: {r.text}")
    ed.check_eq((await ed.call("find_entities", scene=S["scene"]))["count"], before, "no GameObject left behind")
    ed.check(not (ed.project / "assets/ui/bad_props.html").exists() and not (ed.project / "assets/ui/bad_props.css").exists(),
             "no file left behind")


async def test_create_unicode_path(ed: Editor):
    await ensure_scene(ed)
    r = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/menú ü/título.html")
    ed.check_eq(r["files_written"], ["assets/ui/menú ü/título.html", "assets/ui/menú ü/título.css"], "unicode files")
    ed.check_eq(r["entity"]["name"], "Título", "the name keeps the letters")
    ed.check_eq(r["document"]["html"], "assets/ui/menú ü/título.html", "document html")
    tree = await ed.call("inspect_ui", entity=r["entity"]["id"], selector="h1")
    ed.check_eq(tree["elements"][0]["text"], "Título", "the heading")
    f = await ed.call("inspect_ui", file="assets/ui/menú ü/título.css")
    ed.check(f["diagnostics"] == [] and f["documents_using"][0]["entity"]["id"] == r["entity"]["id"], f"file mode: {f}")


# ---------------------------------------------------------------------------- inspect_ui

async def test_inspect_tree_default(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "panel")
    r = await ed.call_raw("inspect_ui", {"entity": S["panel"]})
    ed.check(not r.is_error, r.text)
    data = r.structured
    elements = data["elements"]
    ed.check(data["truncated"] is False, "the complete small document is returned")
    # One returned ref must be usable as a selector by a caller.
    ref = next(e["ref"] for e in elements if e.get("id") == "ok")
    again = await ed.call("inspect_ui", entity=S["panel"], selector=ref, depth=0)
    ed.check_eq([e.get("id") for e in again["elements"]], ["ok"], "the returned ref selects the same element")
    for e in elements:
        if e["parent"] >= 0:
            ed.check_eq(elements[e["parent"]]["depth"], e["depth"] - 1, f"parent link of {e['ref']}")
    ok = next(e for e in elements if e.get("id") == "ok")
    ed.check_eq((ok["tag"], ok["class"], ok["text"]), ("button", ["btn", "primary"], "OK"), "the #ok element")

    r = await ed.call("inspect_ui", entity=S["panel"], limit=3)
    ed.check(len(r["elements"]) == 3 and r["truncated"] is True, f"limit: {len(r['elements'])}, {r['truncated']}")
    r = await ed.call("inspect_ui", entity=S["panel"], depth=1)
    ed.check_eq({e["depth"] for e in r["elements"]}, {0, 1}, "depth 1")
    ed.check(not any(e["tag"] == "h1" for e in r["elements"]), "depth 1 stops above h1")


async def test_inspect_full_styles_sources(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "panel")
    r = await ed.call_raw("inspect_ui", {"entity": S["panel"], "selector": "#ok", "detail": "full"})
    ed.check(not r.is_error, r.text)
    detail = r.structured["details"][0]
    ed.check_eq(detail["ref"], "#ok", "detail ref")
    for key in ("content", "padding", "border", "margin", "padding_edges", "border_edges", "margin_edges", "scroll"):
        ed.check(key in detail["box"], f"box model {key}")
    ed.check_eq(detail["box"]["border_edges"], [1, 1, 1, 1], "border edges of .btn")
    ed.check_eq(detail["attributes"], {"class": "btn primary", "id": "ok"}, "attributes")
    styles = {s["property"]: s for s in detail["styles"]}
    line, _ = line_col(GOOD_CSS, ".btn.primary")
    col = GOOD_CSS.split("\n")[line - 1].index("background-color") + 1
    bg = styles["background-color"]
    ed.check_eq((bg["source"], bg.get("selector")), (f"assets/ui/panel.css:{line}:{col}", ".btn.primary"), "background-color source")
    ed.check_eq(bg["value"], "rgb(63, 169, 245)", "var(--accent) computed")
    ed.check(f"background-color: rgb(63, 169, 245)  <- assets/ui/panel.css:{line}:{col} (.btn.primary)" in r.text, "text line")
    ed.check(len(r.structured["details"]) == 1, "one element")

    r = await ed.call("inspect_ui", entity=S["panel"], selector=".list > li", detail="full", styles=["color", "letter-spacing"])
    ed.check(len(r["details"]) == 3, f"three items: {len(r['details'])}")
    styles = {s["property"]: s for s in r["details"][0]["styles"]}
    ed.check(styles["color"]["source"].startswith("inherited from body"), f"inherited: {styles['color']}")
    ed.check_eq(styles["letter-spacing"]["source"], "default", "an unset property")

    r = await ed.call_raw("inspect_ui", {"entity": S["panel"], "selector": "#ok", "styles": ["colr"]})
    ed.check(r.is_error and "did you mean color" in r.text, f"unknown style: {r.text}")

    # styles in tree mode: on every element
    r = await ed.call_raw("inspect_ui", {"entity": S["panel"], "selector": ".btn", "styles": ["color", "font-size"]})
    ed.check(all(e["styles"]["color"] == "rgb(255, 255, 255)" for e in r.structured["elements"] if e["tag"] == "button"),
             f"styles per element: {r.structured['elements']}")
    # (a <button> has the user agent's 13.33px, as in Chromium)
    ed.check("{color: rgb(255, 255, 255); font-size: 13.33px}" in r.text, f"styles in the outline: {r.text}")


async def test_inspect_hit_test(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "panel")
    ok = (await ed.call("inspect_ui", entity=S["panel"], selector="#ok", depth=0))["elements"][0]["box"]
    center = [ok[0] + ok[2] / 2, ok[1] + ok[3] / 2]
    r = await ed.call("inspect_ui", entity=S["panel"], at=center)
    ed.check_eq(r["hit"][0]["ref"], "#ok", f"the button is hit: {r['hit']}")
    ed.check_eq(r["hit"][-1]["ref"], "body", "the stack ends at body")
    r = await ed.call_raw("inspect_ui", {"entity": S["panel"], "at": [1200, 700]})
    ed.check(r.structured["hit"] == [] and "passes through" in r.text, f"nothing at a transparent point: {r.text}")


async def test_inspect_flags(ed: Editor):
    await ensure_scene(ed)
    made = await document(ed, "assets/ui/flags.html", FLAGS_HTML, "assets/ui/flags.css", FLAGS_CSS)
    r = await ed.call_raw("inspect_ui", {"entity": made["entity"]["id"]})
    flags = {e.get("id"): e.get("flags", []) for e in r.structured["elements"]}
    ed.check("overflow-x" in flags["box"], f"#box overflows: {flags}")
    ed.check("clipped" in flags["child"], f"#child is clipped: {flags}")
    ed.check("hidden" in flags["hidden-panel"] and "hidden" in flags["inside"], f"hidden: {flags}")
    ed.check(re.search(r"div#box .*!overflow-x", r.text) and re.search(r"div#child .*!clipped", r.text) and
             re.search(r"div#hidden-panel .*\bhidden\b", r.text), f"flags in the text: {r.text}")
    S["flags"] = made["entity"]["id"]


async def test_inspect_viewport(ed: Editor):
    await ensure_scene(ed)
    made = await document(ed, "assets/ui/viewport.html", VIEWPORT_HTML, "assets/ui/viewport.css", VIEWPORT_CSS)
    S["viewport"] = made["entity"]["id"]

    def boxes(r):
        return {e["id"]: e["box"] for e in r["elements"] if e.get("id")}
    wide = boxes(await ed.call("inspect_ui", entity=S["viewport"], selector=".cell"))
    ed.check(wide["c2"][0] == 100 and wide["c2"][1] == 0, f"side by side at 1280x720: {wide}")
    r = await ed.call("inspect_ui", entity=S["viewport"], selector=".cell", viewport=[720, 1280])
    tall = boxes(r)
    ed.check(tall["c2"][0] == 0 and tall["c2"][1] == 40, f"stacked at 720x1280: {tall}")
    ed.check_eq(r["document"]["laid_out_at"], [720, 1280], "the laid-out size")
    ed.check_eq(boxes(await ed.call("inspect_ui", entity=S["viewport"], selector=".cell")), wide, "restored after")
    r = await ed.call_raw("inspect_ui", {"entity": S["viewport"], "viewport": [720, 1280], "screenshot": True})
    ed.check(r.is_error and "without viewport" in r.text, f"viewport + screenshot: {r.text}")


async def test_inspect_file_mode(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "panel")
    disk_write(ed, "assets/ui/shop_llm.html", LLM_HTML)
    disk_write(ed, "assets/ui/shop_llm.css", LLM_CSS)
    r = await ed.call("inspect_ui", file="assets/ui/shop_llm.html")
    with scratch_project(ed.project) as copy:
        _, data = run_json(copy, "--ui", "--files", "assets/ui/shop_llm.html")
    ed.check(data["findings"], "the CLI finds the mistakes")
    ed.check_eq(normalized(r["diagnostics"]), normalized(data["findings"]), "inspect_ui {file} == check --ui --files")
    ed.check(all(set(d) == {"severity", "code", "message", "suggestion", "path", "line", "col"} for d in r["diagnostics"]),
             "diagnostic entries")
    r = await ed.call("inspect_ui", file="assets/ui/panel.css")
    ed.check_eq([u["entity"]["id"] for u in r["documents_using"]], [S["panel"]], "documents_using lists the live document")
    ed.check(r["diagnostics"] == [], "a clean stylesheet")
    r = await ed.call_raw("inspect_ui", {"file": "assets/ui/panel.css", "selector": ".x"})
    ed.check(r.is_error and "drop selector" in r.text, f"file + selector: {r.text}")
    disk_write(ed, "assets/ui/not-ui.lua", "return {}\n")
    r = await ed.call_raw("inspect_ui", {"file": "assets/ui/not-ui.lua"})
    ed.check(r.is_error and ".html" in r.text, f"a .lua file: {r.text}")
    r = await ed.call_raw("inspect_ui", {"file": "assets/ui/nope.css"})
    ed.check(r.is_error, f"a missing file: {r.text}")


async def test_inspect_screenshot(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "panel")
    await ed.call("select_scene", scene=S["scene"])
    r = await ed.call_raw("inspect_ui", {"entity": S["panel"], "selector": "#ok", "screenshot": True, "max_width": 0})
    ed.check(not r.is_error and r.images, f"an image: {r.text[-300:]}")
    img = pngdiff.decode(r.images[0][0])
    info = r.structured["image"]
    ed.check_eq((info["width"], info["height"]), (img[0], img[1]), "image size")
    ok = r.structured["elements"][0]["box"]
    cyan = color_pixels(img, CYAN, 30)
    ed.check(cyan, "cyan outline pixels")
    x, y, w, h = bounds(cyan)
    ex, ey, ew, eh = image_rect(info, ok, img[0])
    ed.check(abs(w - ew) <= 3 and abs(h - eh) <= 3 and abs(x - ex) <= 3 and abs(y - ey) <= 3,
             f"the cyan rectangle {x, y, w, h} is #ok's box {ok} in the image {ex, ey, ew, eh}")
    (ed.scratch.root / "inspect_ok.png").write_bytes(r.images[0][0])

    # a render-to-texture document: inspectable, no image
    made = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/terminal.html", properties={"renderMode": "texture"})
    ed.check_eq(made["document"]["render_target"], "texture", "texture mode")
    r = await ed.call_raw("inspect_ui", {"entity": made["entity"]["id"], "screenshot": True})
    ed.check(not r.is_error and not r.images and "texture" in r.structured.get("image_note", ""), f"no image: {r.text[-300:]}")
    ed.check(any(e["tag"] == "h1" for e in r.structured["elements"]), "the texture document's tree")
    ed.check_eq(r.structured["document"]["viewport"], [1024, 1024], "its own viewport (the texture size)")
    S["texture"] = made["entity"]["id"]

    # a document of a scene the Game view does not show: no image
    other = await ed.call("create_scene", name="HtmlUiOther", type="UI")
    S["other_scene"] = other["scene"]["id"]
    made = await ed.call("create_ui_document", scene=S["other_scene"], html="assets/ui/other.html")
    await ed.call("select_scene", scene=S["scene"])
    r = await ed.call_raw("inspect_ui", {"scene": S["other_scene"], "screenshot": True})
    ed.check(not r.is_error and not r.images and "HtmlUiOther" in r.structured.get("image_note", ""), f"other scene: {r.text[-300:]}")


async def test_document_resolution(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "panel")
    await ensure_fixture(ed, "texture")
    await ensure_fixture(ed, "other_scene")
    empty = await ed.call("create_scene", name="HtmlUiEmpty", type="UI")
    r = await ed.call_raw("inspect_ui", {"scene": empty["scene"]["id"]})
    ed.check(r.is_error and "create_ui_document" in r.text, f"no document: {r.text}")
    r = await ed.call_raw("assert_ui", {"scene": S["scene"], "checks": [{"type": "exists", "selector": "body"}]})
    ed.check(r.is_error and "Several UI Documents: pass entity" in r.text and "HtmlUiTools/Panel (assets/ui/panel.html)" in r.text,
             f"several documents: {r.text}")
    plain = await ed.call("create_entity", scene=S["scene"], type="empty", name="Plain")
    r = await ed.call_raw("inspect_ui", {"scene": S["scene"], "entity": "Plain"})
    ed.check(r.is_error and 'GameObject "Plain" has no UI Document component' in r.text and "Panel (assets/ui/panel.html)" in r.text,
             f"no component: {r.text}")
    # an explicit scene with one document
    r = await ed.call_raw("inspect_ui", {"scene": S["other_scene"]})
    ed.check(not r.is_error, r.text)
    # an entity name of another scene resolves without scene
    r = await ed.call_raw("inspect_ui", {"entity": "Other"})
    ed.check(not r.is_error and r.structured["document"]["html"] == "assets/ui/other.html", f"entity of another scene: {r.text}")
    await ed.call("delete_entities", scene=S["scene"], entities=[plain["entity_id"]])

    # a document whose html is missing: the load diagnostics after waiting
    disk_write(ed, "assets/ui/vanishing.html", GOOD_HTML.replace("panel.css", "panel.css"))
    made = await ed.call("create_ui_document", scene=S["other_scene"], html="assets/ui/vanishing.html")
    (ed.project / "assets/ui/vanishing.html").unlink()
    await ed.call("sync_files", paths=["assets/ui/vanishing.html"])
    t0 = time.monotonic()
    r = await ed.call_raw("inspect_ui", {"scene": S["other_scene"], "entity": made["entity"]["id"]})
    ed.check(r.is_error and "did not load" in r.text and "assets/ui/vanishing.html" in r.text, f"missing html: {r.text}")
    ed.check(time.monotonic() - t0 < 20, "the wait is bounded")
    await ed.call("delete_entities", scene=S["other_scene"], entities=[made["entity"]["id"]])

    # a document still loading: the call waits for it (the file written in the same breath as the GameObject)
    made = await ed.call("create_entity", scene=S["other_scene"], type="ui_document", name="Late")
    disk_write(ed, "assets/ui/late.html", GOOD_HTML)
    await ed.call("set_properties", scene=S["other_scene"], entity=made["entity_id"], values={"uidocument": {"filename": "assets/ui/late.html"}})
    r = await ed.call("inspect_ui", scene=S["other_scene"], entity="Late", selector=".title")
    ed.check_eq(r["elements"][0]["text"], "Settings", "waited for the load")
    await ed.call("delete_entities", scene=S["other_scene"], entities=[made["entity_id"]])


async def test_invalid_selector(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "panel")
    r = await ed.call_raw("inspect_ui", {"entity": S["panel"], "selector": "div[["})
    ed.check(r.is_error and 'selector "div[[" is not valid' in r.text, f"invalid selector: {r.text}")
    r = await ed.call_raw("inspect_ui", {"entity": S["panel"], "selector": "p::before"})
    ed.check(r.is_error and "not valid" in r.text, f"a pseudo-element: {r.text}")
    r = await ed.call_raw("assert_ui", {"entity": S["panel"], "checks": [{"type": "visible", "selector": "div[["}]})
    ed.check(r.is_error and "not valid" in r.text, f"assert_ui invalid selector: {r.text}")
    r = await ed.call_raw("assert_ui", {"entity": S["panel"], "checks": [{"type": "no_overlap", "selector": ".btn", "with": "a[["}]})
    ed.check(r.is_error and "not valid" in r.text, f"assert_ui invalid with: {r.text}")
    r = await ed.call_raw("assert_ui", {"entity": S["panel"], "checks": [{"type": "bogus", "selector": ".btn"}]})
    ed.check(r.is_error and "text_fits" in r.text, f"unknown check type: {r.text}")
    r = await ed.call_raw("assert_ui", {"entity": S["panel"], "checks": [{"type": "count", "selector": ".btn"}]})
    ed.check(r.is_error and "count, min or max" in r.text, f"count without numbers: {r.text}")


async def test_inspect_huge_document(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "other_scene")
    rows = "\n".join(f'    <div class="row"><span>Row {i}</span></div>' for i in range(501))
    html = GOOD_HTML.replace('<div class="panel">', f'<div class="big">\n{rows}\n  </div>\n  <div class="panel">')
    made = await document(ed, "assets/ui/huge.html", html, None, None, scene=S["other_scene"])
    t0 = time.monotonic()
    r = await ed.call_raw("inspect_ui", {"scene": S["other_scene"], "entity": made["entity"]["id"]})
    seconds = time.monotonic() - t0
    ed.check(not r.is_error and r.structured["truncated"] is True and len(r.structured["elements"]) == 200, "default limit 200")
    ed.check("limit 200 reached" in r.text, "the text says it was cut")
    r = await ed.call_raw("inspect_ui", {"scene": S["other_scene"], "entity": made["entity"]["id"], "limit": 1000, "depth": 16})
    ed.check_eq(len(r.structured["elements"]), 1000, "the requested public limit")
    ed.check(len(r.text) < 12800, f"the documented text budget is enforced: {len(r.text)} chars")
    ed.check(re.search(r"\.\.\. \(\d+ more elements: narrow with selector/depth\)", r.text), "the outline is cut with a note")
    print(f"     [timing] inspect_ui on a 1000-element document: {seconds:.2f}s")
    await ed.call("delete_entities", scene=S["other_scene"], entities=[made["entity"]["id"]])


async def test_concurrent_reloads(ed: Editor):
    """External css edits in quick succession, each followed at once by a call: every call sees the latest
    file, each edit reloads the document exactly once, two documents sharing the css reload together."""
    await ensure_scene(ed)
    await ensure_fixture(ed, "panel")
    shared = await ed.call("create_ui_document", scene=S["scene"], html="assets/ui/panel_copy.html")
    disk_write(ed, "assets/ui/panel_copy.html", GOOD_HTML)
    await ed.call("sync_files", paths=["assets/ui/panel_copy.html"])
    start = {e: (await ed.call("inspect_ui", entity=e, depth=0, limit=1))["document"]["load_count"] for e in (S["panel"], shared["entity"]["id"])}
    colors = ["#ff0000", "#00ff00"]
    for i, color in enumerate(colors):
        disk_write(ed, "assets/ui/panel.css", GOOD_CSS.replace("color: var(--accent); }", f"color: {color}; }}"))
        r = await ed.call_raw("inspect_ui", {"entity": S["panel"], "selector": ".title", "styles": ["color"]})
        rgb = tuple(int(color[k:k + 2], 16) for k in (1, 3, 5))
        ed.check_eq(r.structured["elements"][0]["styles"]["color"], f"rgb({rgb[0]}, {rgb[1]}, {rgb[2]})", f"edit {i}")
        ext = r.structured.get("external_changes") or {}
        reloaded = sorted(item["entity"]["id"] for item in ext.get("ui_reloaded", []))
        current = ext.get("ui_current", 0)
        ed.check(len(reloaded) + current == 2, f"both documents reading the css are reloaded once: {ext}")
    end = {e: (await ed.call("inspect_ui", entity=e, depth=0, limit=1))["document"]["load_count"] for e in start}
    ed.check(all(end[e] == start[e] + len(colors) for e in start), f"one reload per edit: {start} -> {end}")
    disk_write(ed, "assets/ui/panel.css", GOOD_CSS)
    await ed.call("inspect_ui", entity=S["panel"], depth=0, limit=1)
    await ed.call("delete_entities", scene=S["scene"], entities=[shared["entity"]["id"]])


# ---------------------------------------------------------------------------- assert_ui

ALL_CHECKS = [
    # (check, passes, expected failure ref, a number in the message)
    ({"type": "exists", "selector": "#big"}, True, None, None),
    ({"type": "exists", "selector": ".nope"}, False, ".nope", "nothing"),
    ({"type": "count", "selector": ".btn", "count": 2}, True, None, None),
    ({"type": "count", "selector": ".btn", "min": 3}, False, ".btn", "3"),
    ({"type": "visible", "selector": "#big"}, True, None, None),
    ({"type": "visible", "selector": "#gone"}, False, "#gone", "display: none"),
    ({"type": "text_fits", "selector": "#wide"}, True, None, None),
    ({"type": "text_fits", "selector": "#narrow"}, False, "#narrow", "60px"),
    ({"type": "no_overlap", "selector": "#a", "with": "#c"}, True, None, None),
    ({"type": "no_overlap", "selector": "#a", "with": "#b"}, False, "#a", "20px x 20px"),
    ({"type": "within_parent", "selector": "#big"}, True, None, None),
    ({"type": "within_parent", "selector": "#out"}, False, "#out", "60px right"),
    ({"type": "within_viewport", "selector": "#big"}, True, None, None),
    ({"type": "within_viewport", "selector": "#far"}, False, "#far", "70px right"),
    ({"type": "contrast", "selector": "#highcontrast"}, True, None, None),
    ({"type": "contrast", "selector": "#lowcontrast"}, False, "#lowcontrast", "4.5:1"),
    ({"type": "min_size", "selector": "#big"}, True, None, None),
    ({"type": "min_size", "selector": "#small"}, False, "#small", "30x20px"),
]


async def test_assert_all_kinds_pass_and_fail(ed: Editor):
    await ensure_scene(ed)
    made = await document(ed, "assets/ui/asserts.html", ASSERT_HTML, "assets/ui/asserts.css", ASSERT_CSS)
    S["asserts"] = made["entity"]["id"]
    r = await ed.call_raw("assert_ui", {"entity": S["asserts"], "checks": [c for c, *_ in ALL_CHECKS], "screenshot": "never"})
    ed.check(not r.is_error, f"a failed assertion is not a tool error: {r.text}")
    data = r.structured
    failing = sum(1 for _, ok, *_ in ALL_CHECKS if not ok)
    ed.check_eq((data["passed"], data["failed"], data["total"]), (False, failing, len(ALL_CHECKS)), "summary")
    ed.check(r.text.startswith(f"assert_ui: {len(ALL_CHECKS) - failing}/{len(ALL_CHECKS)} passed."), r.text[:100])
    for (check, ok, ref, number), result in zip(ALL_CHECKS, data["checks"]):
        ed.check_eq((result["type"], result["selector"], result["passed"]), (check["type"], check["selector"], ok), f"{check}")
        ed.check_eq(result["viewport"], [1280, 720], "the current viewport")
        if ok:
            ed.check(result["failures"] == [], f"{check}: {result}")
            continue
        failure = result["failures"][0]
        ed.check_eq(failure["ref"], ref, f"{check} failure ref")
        ed.check(number in failure["message"], f"{check}: {number!r} in {failure['message']!r}")
        ed.check(len(failure["rect"]) == 4, "rect")
        ed.check(f'FAIL {check["type"]} "{check["selector"]}" @1280x720: {ref}: ' in r.text, f"the text line of {check}")
    overlap = next(c for c in data["checks"] if c["type"] == "no_overlap" and not c["passed"])["failures"][0]
    ed.check_eq((overlap["other_ref"], [round(v) for v in overlap["other_rect"]]), ("#b", [220, 30, 40, 40]), "overlap partner")


async def test_assert_zero_matches_fail(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "asserts")
    r = await ed.call_raw("assert_ui", {"entity": S["asserts"], "checks": [{"type": "text_fits", "selector": ".buton"}],
                                        "screenshot": "never"})
    ed.check(not r.is_error and r.structured["passed"] is False, f"zero matches fail: {r.text}")
    ed.check("matched nothing" in r.structured["checks"][0]["failures"][0]["message"], r.text)
    r = await ed.call("assert_ui", entity=S["asserts"], checks=[{"type": "count", "selector": ".buton", "max": 0}])
    ed.check(r["passed"] is True, "count max 0 passes on zero")


async def test_assert_viewports(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "viewport")
    checks = [{"type": "within_viewport", "selector": ".row"}, {"type": "min_size", "selector": ".row", "width": 10}]
    r = await ed.call_raw("assert_ui", {"entity": S["viewport"], "checks": checks, "viewports": [[1280, 720], [720, 1280]],
                                        "max_width": 0})
    results = {(c["index"], tuple(c["viewport"])): c for c in r.structured["checks"]}
    ed.check(results[(0, (1280, 720))]["passed"] and not results[(0, (720, 1280))]["passed"],
             f"within_viewport passes at 1280x720 only: {r.text}")
    ed.check("280px right" in results[(0, (720, 1280))]["failures"][0]["message"], r.text)
    ed.check(not results[(1, (1280, 720))]["passed"] and not results[(1, (720, 1280))]["passed"], "min_size fails at both")
    ed.check_eq(r.structured["total"], 4, "checks x viewports")
    ed.check("@720x1280" in r.text and "Failures at other viewports have no image" in r.text, f"the note: {r.text}")
    ed.check(r.images, "an image of the current-viewport failure")
    after = await ed.call("inspect_ui", entity=S["viewport"], selector=".row", depth=0)
    ed.check_eq(after["elements"][0]["box"], [0, 0, 1000, 40], "the layout is restored")
    r = await ed.call_raw("assert_ui", {"entity": S["viewport"], "checks": checks, "viewports": [[1, 1]] * 7})
    ed.check(r.is_error and "1 to 6" in r.text, f"at most 6 viewports: {r.text}")


async def test_assert_screenshot_modes(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "asserts")
    await ed.call("select_scene", scene=S["scene"])
    failing = [{"type": "min_size", "selector": "#small"}]
    passing = [{"type": "visible", "selector": "#big"}, {"type": "min_size", "selector": "#big"}]
    r = await ed.call_raw("assert_ui", {"entity": S["asserts"], "checks": failing, "screenshot": "never"})
    ed.check(not r.structured["passed"] and not r.images, "never: no image")
    r = await ed.call_raw("assert_ui", {"entity": S["asserts"], "checks": passing, "screenshot": "on_failure"})
    ed.check(r.structured["passed"] and not r.images, "on_failure on a passing set: no image")
    r = await ed.call_raw("assert_ui", {"entity": S["asserts"], "checks": passing, "screenshot": "always", "max_width": 0})
    ed.check(r.structured["passed"] and r.images, f"always: an image {r.text[-200:]}")
    img = pngdiff.decode(r.images[0][0])
    cyan = color_pixels(img, CYAN, 30)
    x, y, w, h = bounds(cyan)
    ex, ey, ew, eh = image_rect(r.structured["image"], [60, 10, 100, 50], img[0])
    ed.check(abs(w - ew) <= 3 and abs(h - eh) <= 3, f"cyan outline of #big: {x, y, w, h} vs {ex, ey, ew, eh}")
    r = await ed.call_raw("assert_ui", {"entity": S["asserts"], "checks": failing, "max_width": 0})
    ed.check(r.images, "on_failure with a failure: an image")
    img = pngdiff.decode(r.images[0][0])
    x, y, w, h = bounds(color_pixels(img, RED, 30))
    ex, ey, ew, eh = image_rect(r.structured["image"], [10, 10, 30, 20], img[0])
    ed.check(abs(w - ew) <= 3 and abs(h - eh) <= 3 and abs(x - ex) <= 3 and abs(y - ey) <= 3, f"red #small: {x, y, w, h} vs {ex, ey, ew, eh}")
    (ed.scratch.root / "assert_small.png").write_bytes(r.images[0][0])
    # max_width scales the image down
    r = await ed.call_raw("assert_ui", {"entity": S["asserts"], "checks": failing, "max_width": 200})
    ed.check(pngdiff.decode(r.images[0][0])[0] == 200, "max_width")


async def test_assert_contrast_background(ed: Editor):
    await ensure_scene(ed)
    made = await document(ed, "assets/ui/contrast.html", CONTRAST_HTML, "assets/ui/contrast.css", CONTRAST_CSS)
    entity = made["entity"]["id"]
    r = await ed.call("assert_ui", entity=entity, checks=[{"type": "contrast", "selector": "#grey"}])
    check = r["checks"][0]
    ed.check(check["passed"] and check["inconclusive"] == ["#grey"], f"a transparent HUD label is inconclusive: {check}")
    r = await ed.call_raw("assert_ui", {"entity": entity, "screenshot": "never", "checks": [
        {"type": "contrast", "selector": "#grey", "background": [0.5, 0.5, 0.5]},
        {"type": "contrast", "selector": "#white", "background": [0.5, 0.5, 0.5]}]})
    grey, white = r.structured["checks"]
    ed.check(not grey["passed"] and grey["inconclusive"] == [] and "below 4.5:1" in grey["failures"][0]["message"],
             f"grey text over grey fails: {grey}")
    ed.check(white["passed"] and white["inconclusive"] == [], f"white on the dark panel passes: {white}")
    r = await ed.call("assert_ui", entity=entity, checks=[{"type": "contrast", "selector": "#grey", "background": [0, 0, 0],
                                                           "min_ratio": 7}])
    ed.check(not r["checks"][0]["passed"] and "below 7" in r["checks"][0]["failures"][0]["message"], f"min_ratio: {r['checks']}")
    await ed.call("delete_entities", scene=S["scene"], entities=[entity])


# ---------------------------------------------------------------------------- play: models, send_input target

async def play_scene(ed: Editor) -> None:
    """The play tests' scene (clicks.html with its Lua probe under a transparent overlay), made by
    the first test that needs it: each play test runs alone (-k) as well as after the others."""
    await ensure_scene(ed)
    if "play" in S:
        return
    made = await ed.call("create_scene", name="HtmlUiPlay", type="UI")
    S["play"] = made["scene"]["id"]
    clicks = await document(ed, "assets/ui/clicks.html", CLICKS_HTML, "assets/ui/clicks.css", CLICKS_CSS, scene=S["play"])
    S["clicks"] = clicks["entity"]["id"]
    await ed.call("create_script", kind="lua", name="ClicksProbe", content=CLICKS_LUA, scene=S["play"], entity=S["clicks"])
    overlay = await document(ed, "assets/ui/overlay.html", OVERLAY_HTML, scene=S["play"], sort_order=10)
    S["overlay"] = overlay["entity"]["id"]
    S["overlay_sort_order"] = overlay["document"]["sort_order"]
    await ed.call("save_scene", scene=S["play"], path="scenes/HtmlUiPlay")


async def test_inspect_models_play(ed: Editor):
    await ensure_scene(ed)
    await play_scene(ed)
    made = await ed.call("create_scene", name="HtmlUiHud", type="UI")
    S["hud_scene"] = made["scene"]["id"]
    hud = await ed.call("create_ui_document", scene=S["hud_scene"], html="assets/ui/hud_test.html", template="hud")
    S["hud"] = hud["entity"]["id"]
    ed.check_eq(hud["script"], {"path": "scripts/HudTestUI.lua", "attached": True}, "the hud's Lua")
    r = await ed.call_raw("inspect_ui", {"scene": S["hud_scene"], "entity": S["hud"], "models": True})
    ed.check(r.structured["models"] == {} and "only in play" in r.text, f"edit mode: {r.text[-300:]}")
    hidden = await ed.call("inspect_ui", scene=S["play"], entity=S["clicks"], selector="#panel", depth=0)
    ed.check("hidden" in hidden["elements"][0]["flags"], "the panel is hidden in edit mode")
    await ed.call("play", action="start", scene=S["hud_scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=30)
        r = await ed.call_raw("inspect_ui", {"entity": S["hud"], "models": True})
        ed.check(not r.is_error, r.text)
        hud = r.structured["models"].get("hud")
        ed.check(isinstance(hud, dict) and isinstance(hud.get("health"), (int, float)), f"the hud model: {r.structured['models']}")
        ed.check(r.structured["document"]["scene"] == S["hud_scene"], "the running scene")
        ed.check('"hud"' in r.text and "Data models:" in r.text, f"the text: {r.text[-300:]}")
        S["models_text"] = r.text[-400:]
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_send_input_target_click(ed: Editor):
    await ensure_scene(ed)
    await play_scene(ed)
    await ed.call("play", action="start", scene=S["play"])
    try:
        await ed.call("wait", frames=10)
        await ed.console_since(None, limit=1)
        box = (await ed.call("inspect_ui", entity=S["clicks"], selector="#ok", depth=0))["elements"][0]["box"]
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": "#ok"}], "frames_after": 5})
        ed.check(not r.is_error, r.text)
        ed.check("tools: ok" in await console_lines(ed), "the Lua listener ran (through the transparent overlay)")
        target = r.structured["targets"][0]
        ed.check_eq((target["event"], target["target"], target["document"]["html"]), (0, "#ok", "assets/ui/clicks.html"), "target")
        ed.check(abs(target["point"][0] - (box[0] + box[2] / 2)) <= 1 and abs(target["point"][1] - (box[1] + box[3] / 2)) <= 1,
                 f"the resolved point {target['point']} is #ok's center ({box})")
        ed.check('Target "#ok" (event 0) at canvas' in r.text, r.text)

        # the topmost document wins; document picks one
        r = await ed.call("send_input", events=[{"type": "mouse", "action": "move", "target": "#badge"}])
        ed.check_eq(r["targets"][0]["document"]["html"], "assets/ui/overlay.html", "the overlay's badge")
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": "#ok", "document": S["overlay"]}]})
        ed.check(r.is_error and "Overlay" in r.text, f"#ok is not in the overlay: {r.text}")

        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": "#nope"}]})
        ed.check(r.is_error and "UI Documents searched" in r.text and "Clicks (assets/ui/clicks.html)" in r.text and
                 "Overlay (assets/ui/overlay.html)" in r.text, f"unknown selector: {r.text}")
        ed.check(r.text.index("Overlay") < r.text.index("Clicks (assets"), "topmost first")
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": "div[["}]})
        ed.check(r.is_error and "not valid" in r.text, f"invalid selector: {r.text}")
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": ""}]})
        ed.check(r.is_error and "CSS selector" in r.text, f"empty target: {r.text}")
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": "#panel .close"}]})
        ed.check(r.is_error and "none shown" in r.text, f"a hidden element: {r.text}")
        # an opacity 0 element (the invisible checkbox over a styled switch) still takes the click
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": "#ghost"}], "frames_after": 5})
        ed.check(not r.is_error and "tools: ghost" in await console_lines(ed), f"a transparent target: {r.text}")
        ed.check_eq(r.structured["targets"][0]["document"]["ref"], "#ghost", "the element's ref")
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": "#ok", "to_target": "#open"}]})
        ed.check(r.is_error and "to_target is for mouse drag" in r.text, f"to_target on a click: {r.text}")

        # inspect_ui while playing: the running document, Lua-made state included
        hit = await ed.call("inspect_ui", entity=S["clicks"], at=[box[0] + 5, box[1] + 5])
        ed.check_eq(hit["hit"][0]["ref"], "#ok", "hit test while playing")
    finally:
        await ed.call("play", action="stop")


async def test_send_input_target_late_resolution(ed: Editor):
    await ensure_scene(ed)
    await play_scene(ed)
    await ed.call("play", action="start", scene=S["play"])
    try:
        await ed.call("wait", frames=10)
        await ed.console_since(None, limit=1)
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": "#open"},
                                                        {"type": "mouse", "action": "click", "target": "#panel .close"}],
                                             "frames_after": 5})
        ed.check(not r.is_error, f"the second target resolves after the first click opened the panel: {r.text}")
        ed.check("tools: close" in await console_lines(ed), "the close listener ran")
        ed.check_eq([t["target"] for t in r.structured["targets"]], ["#open", "#panel .close"], "both resolved")
        # play vs edit: the panel was shown by Lua in play
        r = await ed.call("send_input", events=[{"type": "mouse", "action": "click", "target": "#open"}], frames_after=3)
        shown = await ed.call("inspect_ui", entity=S["clicks"], selector="#panel", depth=0)
        ed.check("flags" not in shown["elements"][0] or "hidden" not in shown["elements"][0]["flags"], f"shown in play: {shown}")
    finally:
        await ed.call("play", action="stop")


async def test_gamepad_focus_in_inspect(ed: Editor):
    await ensure_scene(ed)
    await play_scene(ed)
    # A fixed frame delta and auto-pause: send_input's press holds dpad_down for 2 frames, and a
    # held direction repeats after 0.4 s of UI clock (Engine::updateGamepadNavigation). With the
    # measured delta, two stalled frames on a loaded host (each counted up to maxDeltatime, 0.25 s)
    # made the 2-frame press a held one: the repeat moved the focus on past #open to #ghost.
    await ed.call("play", action="start", scene=S["play"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=5)
        await lua(ed, f"UIDocument(scene, entity({S['clicks']})):getElementById('ok'):focus()")
        await ed.call("send_input", events=[{"type": "gamepad", "button": "dpad_down", "action": "press"}], frames_after=3)
        r = await ed.call_raw("inspect_ui", {"entity": S["clicks"], "selector": "button", "depth": 0})
        pseudo = {e["id"]: e.get("pseudo", []) for e in r.structured["elements"] if e.get("id")}
        ed.check("focus" in pseudo["open"] and "focus" not in pseudo["ok"], f"the d-pad moved the focus: {pseudo}")
        ed.check(re.search(r"button#open .*:focus", r.text), f"the outline shows it: {r.text}")
        styles = await ed.call("inspect_ui", entity=S["clicks"], selector="#open", styles=["background-color"])
        ed.check_eq(styles["elements"][0]["styles"]["background-color"], "rgb(64, 96, 192)", "the :focus style applies")

        # the failure on a loaded host, reproduced: frames of 0.5 s make the same 2-frame press a held
        # one, whose repeat moves the focus a second time
        await ed.call("play", action="set", fixed_fps=2)
        await lua(ed, f"UIDocument(scene, entity({S['clicks']})):getElementById('ok'):focus()")
        await ed.call("send_input", events=[{"type": "gamepad", "button": "dpad_down", "action": "press"}], frames_after=3)
        r = await ed.call_raw("inspect_ui", {"entity": S["clicks"], "selector": "button", "depth": 0})
        pseudo = {e["id"]: e.get("pseudo", []) for e in r.structured["elements"] if e.get("id")}
        ed.check("focus" in pseudo["ghost"], f"a press held across 1 s of UI clock repeats: {pseudo}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_send_input_drag_to_target(ed: Editor):
    await ensure_scene(ed)
    made = await ed.call("create_scene", name="HtmlUiInventory", type="UI")
    scene = made["scene"]["id"]
    inv = await ed.call("create_ui_document", scene=scene, html="assets/ui/bag.html", template="inventory")
    ed.check_eq(inv["script"]["path"], "scripts/BagUI.lua", "renamed Lua")
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=90)
        model = f"UIDocument(scene, entity({inv['entity']['id']})):getDataModel('inventory')"
        first, sixth = await lua(ed, f"local m = {model}; return m.items[1].id, m.items[6].id")
        ed.check(first != sixth, f"two different entries: {first!r}, {sixth!r}")
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "drag", "target": ".slot:nth-child(1)",
                                                         "to_target": ".slot:nth-child(6)", "steps": 8}], "frames_after": 5})
        ed.check(not r.is_error and len(r.structured["targets"]) == 2, r.text)
        ed.check_eq(await lua(ed, f"local m = {model}; return m.items[1].id, m.items[6].id"), [sixth, first], "swapped")
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "drag", "target": ".slot:nth-child(1)"}]})
        ed.check(r.is_error and "to_target" in r.text, f"a drag without an end: {r.text}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_send_input_touch_target(ed: Editor):
    await ensure_scene(ed)
    await play_scene(ed)
    await ed.call("configure_game_view", simulate_touch=True)
    await ed.call("play", action="start", scene=S["play"])
    try:
        await ed.call("wait", frames=10)
        await ed.console_since(None, limit=1)
        r = await ed.call_raw("send_input", {"events": [{"type": "touch", "action": "tap", "target": "#ok"}], "frames_after": 5})
        ed.check(not r.is_error and "tools: ok" in await console_lines(ed), f"touch tap target: {r.text}")
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": "#ok"}], "frames_after": 5})
        ed.check(not r.is_error and "tools: ok" in await console_lines(ed), f"a mouse click as a finger: {r.text}")
        r = await ed.call("send_input", events=[{"type": "touch", "action": "swipe", "target": "#ok", "to_target": "#open"}])
        ed.check_eq([t["target"] for t in r["targets"]], ["#ok", "#open"], "swipe target / to_target")
        r = await ed.call_raw("send_input", {"events": [{"type": "touch", "action": "start"}]})
        ed.check(r.is_error and "or target" in r.text, f"a touch without a point: {r.text}")
    finally:
        await ed.call("play", action="stop")
        await ed.call("configure_game_view", simulate_touch=False)


async def test_texture_document_not_targeted(ed: Editor):
    """send_input never aims at a texture-mode document's elements (not at canvas points)."""
    await ensure_scene(ed)
    await ensure_fixture(ed, "texture")
    await ed.call("play", action="start", scene=S["scene"])
    try:
        await ed.call("wait", frames=5)
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": "h1", "document": S["texture"]}]})
        ed.check(r.is_error and "renders to a texture" in r.text, f"a texture document: {r.text}")
    finally:
        await ed.call("play", action="stop")


# ---------------------------------------------------------------------------- import_files

async def test_import_fonts_icons_urls_mirror(ed: Editor):
    await ensure_scene(ed)
    r = await ed.call_raw("import_files", {"fonts": ["Karla:400"], "icons": ["lucide:play"], "sources": [ANIM_URL, MISSING_URL],
                                           "to_dir": "assets/ui/imported"})
    ed.check(not r.is_error, f"a partial failure is not an error: {r.text}")
    data = r.structured
    files = [f for item in data["remote"] for f in item["files"]]
    ed.check(any(f.endswith(".ttf") for f in files) and data.get("font_face_css", "").endswith(".css"), f"fonts: {data}")
    ed.check((ed.project / data["font_face_css"]).is_file(), "the @font-face stylesheet exists")
    css = (ed.project / data["font_face_css"]).read_text()
    ed.check("@font-face" in css and "Karla" in css and "https://" not in css, f"local @font-face: {css}")
    ed.check("assets/ui/imported/play.svg" in files and "currentColor" in (ed.project / "assets/ui/imported/play.svg").read_text(), "icon")
    anim = next(item for item in data["remote"] if item["source"] == ANIM_URL)
    ed.check(json.loads((ed.project / anim["files"][0]).read_text())["fr"] == 30, "the Lottie json")
    ed.check(any(f["source"] == MISSING_URL and "not found" in f["error"] for f in data["failed"]), f"the missing url: {data['failed']}")
    r = await ed.call_raw("import_files", {"sources": [MISSING_URL]})
    ed.check(r.is_error and "not found" in r.text, f"nothing imported is an error: {r.text}")
    r = await ed.call_raw("import_files", {"icons": ["lucide:no-such-icon"]})
    ed.check(r.is_error and "no Lucide icon" in r.text, f"an unknown icon: {r.text}")
    r = await ed.call_raw("import_files", {"icons": ["play"], "to_dir": "scripts/icons"})
    ed.check(r.is_error and "assets directory" in r.text, f"remote imports go under the assets: {r.text}")


async def test_import_from_document_rewrites(ed: Editor):
    await ensure_scene(ed)
    await ensure_fixture(ed, "other_scene")
    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Remote</title>
  <link href="{DOC_FONT_URL.replace('&', '&amp;')}" rel="stylesheet">
  <style>body {{ margin: 0; font-family: 'Karla', sans-serif; color: #ffffff; }}</style>
</head>
<body>
  <img id="sword" src="{SWORD_URL}" alt="Sword">
</body>
</html>
"""
    disk_write(ed, "assets/ui/remote.html", html)
    with scratch_project(ed.project) as copy:
        _, before = run_json(copy, "--ui", "--files", "assets/ui/remote.html")
    ed.check_eq([f["code"] for f in before["findings"] if f["code"] == "HTMLUI-IMP-0001"], ["HTMLUI-IMP-0001"] * 2,
                f"two remote URLs: {before['findings']}")
    r = await ed.call_raw("import_files", {"from_document": "assets/ui/remote.html"})
    ed.check(not r.is_error, r.text)
    rewritten = r.structured["rewritten"]
    ed.check_eq(rewritten["file"], "assets/ui/remote.html", "the file")
    ed.check_eq(sorted(x["from"] for x in rewritten["replacements"]), sorted([DOC_FONT_URL, SWORD_URL]), "both URLs")
    text = (ed.project / "assets/ui/remote.html").read_text()
    ed.check("https://" not in text and 'href="imported/' in text and 'src="imported/' in text, f"local paths: {text}")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / "assets/ui/remote.html").read_text(), html, "undo scope project restores the remote URLs")
    r = await ed.call("import_files", from_document="assets/ui/remote.html", rewrite=False)
    ed.check("rewritten" not in r and (ed.project / "assets/ui/remote.html").read_text() == html, "rewrite: false")
    r = await ed.call("import_files", from_document="assets/ui/remote.html")
    ed.check(len(r["rewritten"]["replacements"]) == 2, "already imported: rewritten again without downloading")
    made = await ed.call("create_ui_document", scene=S["other_scene"], html="assets/ui/remote.html")
    counts = made["document"]["diagnostics_count"]
    ed.check_eq((counts["error"], counts["warning"]), (0, 0), f"the document is clean: {made['diagnostics']}")
    with scratch_project(ed.project) as copy:
        _, after = run_json(copy, "--ui", "--files", "assets/ui/remote.html")
    ed.check_eq(after["findings"], [], "check --ui is clean")
    font = await ed.call("inspect_ui", scene=S["other_scene"], entity=made["entity"]["id"], selector="#sword", styles=["font-family"])
    family = font["elements"][0]["styles"]["font-family"]
    ed.check("karla" in family.lower(), f"the font family: {family}")


async def test_import_requires_something(ed: Editor):
    await ensure_scene(ed)
    r = await ed.call_raw("import_files", {})
    ed.check(r.is_error and "pass sources, fonts, icons or from_document." in r.text, f"nothing to import: {r.text}")
    local = Path(tempfile.mkdtemp(prefix="jasper-import-local-"))
    try:
        (local / "a.txt").write_text("a")
        r = await ed.call_raw("import_files", {"sources": [str(local / "a.txt")]})
        ed.check(r.is_error and "to_dir is required" in r.text, f"local without to_dir: {r.text}")
        r = await ed.call("import_files", sources=[str(local / "a.txt")], to_dir="assets/local")
        ed.check_eq(r["imported"], ["assets/local/a.txt"], "local sources still work")
        disk_write(ed, "assets/ui/not-ui.lua", "return {}\n")
        r = await ed.call_raw("import_files", {"from_document": "assets/ui/not-ui.lua"})
        ed.check(r.is_error and ".html" in r.text, f"from_document must be html/css: {r.text}")
    finally:
        shutil.rmtree(local, ignore_errors=True)


# ---------------------------------------------------------------------------- resources

async def test_examples_resources(ed: Editor):
    await ensure_scene(ed)
    got = await ed.session.read_resource("jasper://examples/html-ui")
    index = got.contents[0].text
    ed.check("jasper://examples/html-ui/" in index, "the example index links its files")
    mimes = {".html": "text/html", ".css": "text/css", ".lua": "text/x-lua", ".svg": "image/svg+xml", ".json": "application/json",
             ".webm": "video/webm", ".woff2": "font/woff2"}
    files = [LIBRARY / rel for rel in (
        "main_menu/main_menu.html", "main_menu/main_menu.css", "main_menu/MainMenuUI.lua",
        "main_menu/assets/logo.svg", "examples.json", "video_menu/assets/menu-loop.webm", "LICENSE-lucide.txt",
        "motion_graphics/assets/motion-roboto.woff2", "motion_graphics/assets/LICENSE-Roboto.txt")]
    for path in files:
        rel = path.relative_to(LIBRARY).as_posix()
        got = await ed.session.read_resource(f"jasper://examples/html-ui/{rel}")
        content = got.contents[0]
        ed.check_eq(content.mime_type, mimes.get(path.suffix, "text/plain"), f"{rel} mime")
        if content.mime_type.startswith(("video/", "font/")):
            # Binary examples are transported as blobs, including embedded NUL bytes.
            ed.check(base64.b64decode(content.blob) == path.read_bytes(), f"{rel} content (blob)")
        else:
            ed.check(content.text == path.read_text(encoding="utf-8"), f"{rel} content")
    try:
        await ed.session.read_resource("jasper://examples/html-ui/main_menu/nope.css")
    except MCPError as e:
        ed.check("No example file" in str(e) and "main_menu" in str(e), f"unknown path: {e}")
    else:
        ed.fail("an unknown example file should fail")
    listed = {str(r.uri) for r in (await ed.session.list_resources()).resources}
    ed.check("jasper://examples/html-ui" in listed, "the index is listed")
    templates = await ed.session.list_resource_templates()
    ed.check(any(t.uri_template == "jasper://examples/html-ui/{+path}" for t in templates.resource_templates), "the template")
    docs = (await ed.session.read_resource("jasper://docs")).contents[0].text
    ed.check("jasper://examples/html-ui" in docs, "jasper://docs lists the examples")
