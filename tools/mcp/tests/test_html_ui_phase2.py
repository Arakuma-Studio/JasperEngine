"""Phase-2 integration (p2-integrate): what crosses the phase-2 work packages, in the real editor.

An agent's-eye run and the checks no single package owned:
- a menu written the way an LLM writes one for a browser, with the mistakes the diagnostics
  exist for (a Tailwind CDN script and stylesheet, a JavaScript <script>, onclick=, display:
  grid, position: sticky, a property name one letter off, a remote image nobody imported): every
  one is in the Console with file:line:column, its code and a suggestion, once, and in the
  document's diagnostics; the page still draws;
- the file fixed (Lua instead of JavaScript, flex instead of grid, a Google Font and an image
  the import mirror serves): the editor imports both, the document reloads to "0 errors" and
  draws with the imported font and image;
- hot reload on the compat path, in edit and play mode: the .html, its linked .css, an
  @imported .css, an image and a font replaced on disk; scroll, form value and focus kept; the
  document's <script type="text/lua"> runs again and its listener fires once per click; one
  summary line per load;
- the import-ui command line on a copy of the project, and an unreachable URL (error, exit 1);
- texture-mode and child-scene documents and documents loaded from Lua (UIDocument:load,
  innerHTML, insertAdjacentHTML) go through the compat layer;
- an .rml document still loads through RmlUi's own loader, unchanged;
- play stop leaves no Lua reference;
- the editor setting HTML UI > Verbose Diagnostics lists the notes one by one (a second editor
  started with the setting).

No network: the importers read JASPERENGINE_IMPORT_MIRROR, built here before the editor starts.
Runs on a scratch copy of the fixture project (canvas 1280 x 720).
"""

import asyncio
import json
import os
import shutil
import subprocess
import tempfile
import time
import urllib.parse
import struct
import zlib
from pathlib import Path

import yaml

import harness
from harness import Editor, skip
from htmlui_pixels import Shot, near

TIMEOUT = 900

REPO = Path(__file__).resolve().parents[3]
FONTS = REPO / "engine/tests/htmlui/fixtures/compat/pages/htmlui-imports/fonts"
S: dict = {}

UI = "assets/ui/p2"
MENU = f"{UI}/menu.html"
MENU_CSS = f"{UI}/menu.css"
FONT_URL = "https://fonts.googleapis.com/css2?family=Orbitron:wght@700&display=swap"
FONT_FILE_URL = "https://fonts.gstatic.com/s/orbitron/v31/orbitron-700.ttf"
LOGO_URL = "https://images.example.net/logo.png"
TAILWIND_CSS = "https://cdn.jsdelivr.net/npm/tailwindcss@2.2.19/dist/tailwind.min.css"

BAD_HTML = f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Star Forge</title>
  <script src="https://cdn.tailwindcss.com"></script>
  <link href="{TAILWIND_CSS}" rel="stylesheet">
  <link rel="stylesheet" href="menu.css">
</head>
<body>
  <div class="menu">
    <h1 class="title">Star Forge</h1>
    <img class="logo" src="https://images.example.net/missing-logo.png" alt="logo">
    <div class="buttons">
      <button class="btn" onclick="startGame()">Play</button>
      <button class="btn" id="quit">Quit</button>
    </div>
  </div>
  <script>
    document.getElementById('quit').addEventListener('click', () => window.close());
  </script>
</body>
</html>
"""

BAD_CSS = """:root { --accent: #7c3aed; }
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: 'Segoe UI', sans-serif;
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(135deg, #0f0c29, #302b63);
  color: #fff;
}
.menu {
  position: sticky;
  top: 0;
  width: 420px;
  padding: 32px;
  backgroud-color: rgba(255, 255, 255, 0.08);
  border: 1px solid rgba(255, 255, 255, 0.18);
  border-radius: 16px;
}
.buttons {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
}
.btn {
  padding: 12px 20px;
  border: 1px solid var(--accent);
  border-radius: 10px;
  background: rgba(255, 255, 255, 0.06);
  color: #fff;
  font: inherit;
  transition: all 200ms ease;
}
.btn:hover { background: var(--accent); }
"""

# (file, line, column, severity, code, part of the message, part of the suggestion)
EXPECTED = [
    ("ui/p2/menu.html", 7, 3, "warning", "HTMLUI-HTML-0007", "Tailwind Play CDN script not run", "run the Tailwind CLI"),
    ("ui/p2/menu.html", 8, 15, "error", "HTMLUI-IMP-0001", f"remote stylesheet '{TAILWIND_CSS}' is not imported", "jasperengine-editor import-ui"),
    ("ui/p2/menu.html", 14, 28, "error", "HTMLUI-IMP-0001", "remote image 'https://images.example.net/missing-logo.png' is not imported", "load the document in the editor"),
    ("ui/p2/menu.html", 16, 27, "error", "HTMLUI-HTML-0005", "'onclick' (JavaScript) is not run", "addEventListener('click', fn) in Lua"),
    ("ui/p2/menu.html", 20, 3, "error", "HTMLUI-HTML-0006", "JavaScript is not run", '<script type="text/lua">'),
    ("ui/p2/menu.css", 13, 3, "warning", "HTMLUI-CSS-0006", "'position: sticky' is drawn as 'relative'", "position: absolute"),
    ("ui/p2/menu.css", 17, 3, "warning", "HTMLUI-CSS-0001", "'backgroud-color' is not a CSS property", "did you mean 'background-color'?"),
    ("ui/p2/menu.css", 22, 3, "warning", "HTMLUI-CSS-0007", "'display: grid' is drawn as a wrapping flex container", "lay the items out with flex"),
]

GOOD_HTML = f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <title>Star Forge</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="{FONT_URL}" rel="stylesheet">
  <link rel="stylesheet" href="menu.css">
</head>
<body>
  <div class="menu">
    <h1 class="title"><span id="title">Star Forge</span><span id="plain">Star Forge</span></h1>
    <img class="logo" id="logo" src="{LOGO_URL}" alt="logo">
    <div class="buttons">
      <button class="btn" id="play"><i data-lucide="play"></i> Play</button>
      <button class="btn" id="quit">Quit</button>
    </div>
  </div>
  <script type="text/lua">
    document:getElementById("play"):addEventListener("click", function() _G.p2MenuClicks = (_G.p2MenuClicks or 0) + 1 end)
  </script>
</body>
</html>
"""

GOOD_CSS = (BAD_CSS.replace("  position: sticky;\n  top: 0;\n", "")
            .replace("backgroud-color", "background-color")
            .replace("  display: grid;\n  grid-template-columns: 1fr 1fr;\n", "  display: flex;\n")
            .replace("font-family: 'Segoe UI', sans-serif;", "font-family: sans-serif;")
            + ".title { font-family: 'Orbitron', sans-serif; font-size: 40px; margin-bottom: 16px; }\n"
              "#plain { position: absolute; visibility: hidden; font-family: sans-serif; }\n"
              ".logo { display: block; width: 64px; height: 64px; margin-bottom: 16px; }\n"
              ".btn { flex: 1; }\n")


def png(width: int, height: int, rgb: tuple) -> bytes:
    raw = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def mirror_file(root: Path, url: str, content: bytes) -> None:
    parts = urllib.parse.urlsplit(url)
    path_and_query = parts.path + ("?" + parts.query if parts.query else "")
    target = root / parts.hostname / urllib.parse.quote(path_and_query, safe="")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)


# the import mirror, set before the Editor starts (the harness copies os.environ)
MIRROR = Path(tempfile.mkdtemp(prefix="jasper-p2-mirror-"))
mirror_file(MIRROR, FONT_URL, (
    "@font-face {\n  font-family: 'Orbitron';\n  font-style: normal;\n  font-weight: 700;\n  font-display: swap;\n"
    f"  src: url({FONT_FILE_URL}) format('truetype');\n}}\n").encode())
mirror_file(MIRROR, FONT_FILE_URL, (FONTS / "orbitron/Orbitron-700.ttf").read_bytes())
mirror_file(MIRROR, LOGO_URL, png(8, 8, (255, 128, 0)))
os.environ["JASPERENGINE_IMPORT_MIRROR"] = str(MIRROR)

ORANGE = (255, 128, 0)


async def messages(ed: Editor) -> list:
    return [e.get("message", "") for e in await ed.console_since(None, limit=5000)]


async def wait_messages(ed: Editor, needle: str, seconds: float = 30.0, count: int = 1) -> list:
    deadline = time.monotonic() + seconds
    while True:
        found = [m for m in await messages(ed) if needle in m]
        if len(found) >= count:
            return found
        if time.monotonic() > deadline:
            ed.fail(f"{len(found)} Console line(s) with {needle!r} after {seconds} s, expected {count}; "
                    "[HTML UI] lines:\n" + "\n".join(m for m in await messages(ed) if "[HTML UI]" in m)[-3000:])
        await asyncio.sleep(0.25)


async def game_shot(ed: Editor, label: str, scene=None) -> Shot:
    shot = await ed.call_raw("screenshot", {"view": "game", "scene": scene or S["scene"], "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    (ed.scratch.root / f"p2-{label}.png").write_bytes(shot.images[0][0])
    return Shot(shot.images[0][0], shot.value)


async def wait_for_color(ed: Editor, label: str, point, color, seconds: float = 4.0, scene=None) -> Shot:
    deadline = time.monotonic() + seconds
    attempts = 0
    while True:
        shot = await game_shot(ed, label, scene)
        attempts += 1
        if near(shot.px(*point), color, 12):
            return shot
        if time.monotonic() > deadline and attempts >= 3:
            ed.fail(f"{label}: pixel at canvas {point} is {shot.px(*point)} after {seconds} s, expected {color}")
        await asyncio.sleep(0.15)


async def lua(ed: Editor, code: str, edit: bool = False):
    args = {"code": code}
    if edit:
        args["allow_edit_mode"] = True
    return (await ed.call("eval_lua", **args))["values"]


def summaries(lines: list, document: str) -> list:
    """The per-load summary lines of a document ("[HTML UI] ui/menu.html: 0 errors, 1 warning, 7 notes")."""
    return [m for m in lines if m.startswith(f"[HTML UI] {document}: ") and (" errors, " in m or " error, " in m)]


# ---------------------------------------------------------------------------
# the agent's-eye run


async def test_mistakes_are_reported(ed: Editor):
    await ed.console_since(None, limit=1)
    await ed.call("write_file", path=MENU_CSS, content=BAD_CSS)
    await ed.call("write_file", path=MENU, content=BAD_HTML)
    made = await ed.call("create_scene", name="P2Menu", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["scene"], settings={"background_color": [0.0, 0.0, 0.0, 1.0]})
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Menu")
    S["menu"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": MENU}})
    await ed.call("save_scene", scene=S["scene"], path="scenes/P2Menu")

    # the editor tried to import the two remote files and could not (the mirror has neither)
    await wait_messages(ed, "[HTMLUI-IMP-0002]", 60.0, count=2)
    lines = await messages(ed)

    for file, line, column, severity, code, text, fix in EXPECTED:
        head = f"[HTML UI] {file}:{line}:{column}: {severity}: "
        found = [m for m in lines if m.startswith(head) and text in m and m.rstrip().endswith(f"[{code}]")]
        ed.check(found, f"no Console line '{head}...{text}... [{code}]'; [HTML UI] lines:\n" +
                 "\n".join(m for m in lines if "[HTML UI]" in m)[-4000:])
        ed.check(f"(fix: " in found[0] and fix in found[0], f"the suggestion of {code}: {found[0]}")
    # notes are counted, not listed (Verbose Diagnostics is off)
    ed.check(not [m for m in lines if "[HTML UI]" in m and ": info: " in m and "ui/p2/menu" in m],
             "info lines listed without the verbose setting")
    summary = summaries(lines, "ui/p2/menu.html")
    ed.check(summary, "no summary line for ui/p2/menu.html")
    ed.check("4 errors, 4 warnings" in summary[-1], f"the summary: {summary[-1]}")

    # the same findings through the document, with the notes (edit mode: no script has run)
    found = await lua(ed, """
        local out = {}
        for _, d in ipairs(object('Menu', 'UIDocument'):getDiagnostics()) do
            out[#out + 1] = { d.file, d.line, d.column, d.severity, d.code, d.message, d.suggestion }
        end
        return out
    """, edit=True)
    rows = found[0]
    for file, line, column, severity, code, text, fix in EXPECTED:
        match = [r for r in rows if r[0] == file and r[1] == line and r[2] == column and r[3] == severity and r[4] == code]
        ed.check(match, f"getDiagnostics() lacks {file}:{line}:{column} {severity} {code}: {rows}")
        ed.check(text in match[0][5] and fix in match[0][6], f"getDiagnostics() entry of {code}: {match[0]}")
    ed.check([r for r in rows if r[3] == "info"], "getDiagnostics() lists the notes too")

    # the two failed downloads: an error each, at the reference, and the page still draws
    failed = [m for m in lines if "[HTMLUI-IMP-0002]" in m]
    ed.check_eq(len(failed), 2, f"download errors: {failed}")
    ed.check(all("ui/p2/menu.html:" in m for m in failed), f"the download errors point at the document: {failed}")
    shot = await game_shot(ed, "mistakes")
    ed.check(shot.px(20, 20)[2] > shot.px(20, 20)[1] + 10, f"the gradient background: {shot.px(20, 20)}")


async def test_each_finding_is_logged_once(ed: Editor):
    """One load, one line per finding and one summary: no duplicates from the importers'
    discovery, the reload after the failed batch or the Inspector."""
    lines = await messages(ed)
    loads = len(summaries(lines, "ui/p2/menu.html"))
    ed.check(1 <= loads <= 2, f"the document loaded {loads} times (once, and at most once more after the import batch)")
    for file, line, column, severity, code, text, fix in EXPECTED:
        head = f"[HTML UI] {file}:{line}:{column}: {severity}: "
        count = len([m for m in lines if m.startswith(head) and text in m])
        ed.check_eq(count, loads, f"'{head}{text}' lines for {loads} load(s)")


async def test_fixed_file_loads_clean(ed: Editor):
    await ed.console_since(None, limit=1)
    await ed.call("clear_console")
    await ed.call("write_file", path=MENU_CSS, content=GOOD_CSS)
    await ed.call("write_file", path=MENU, content=GOOD_HTML)

    # the editor imports the Google Font and the image, then the document reloads clean
    await wait_messages(ed, "imported 2 remote resources", 60.0)
    lines = await messages(ed)
    for url in (FONT_URL, LOGO_URL):
        ed.check(any(f"[HTML UI] importing {url}" in m for m in lines), f"no 'importing {url}' line")
    ed.check(not any("importing https://fonts.googleapis.com\n" in m + "\n" for m in lines), "the preconnect hint was imported")

    manifest = json.loads((ed.project / "assets/htmlui-imports/manifest.json").read_text())
    entries = {e["url"]: e for e in manifest["entries"]}
    ed.check_eq(entries[FONT_URL]["kind"], "google-fonts-css", "the Google Fonts entry")
    ttf = [f for f in entries[FONT_URL]["files"] if f.endswith(".ttf")]
    ed.check(len(ttf) == 1 and (ed.project / "assets" / ttf[0]).is_file(), f"one static TTF: {entries[FONT_URL]}")
    ed.check((ed.project / "assets" / entries[LOGO_URL]["path"]).is_file(), "the image file")

    clean = await wait_messages(ed, "[HTML UI] ui/p2/menu.html: 0 errors, 0 warnings", 30.0)
    lines = await messages(ed)
    after = lines[len(lines) - 1 - lines[::-1].index(clean[-1]):]
    ed.check(not [m for m in after if "[HTML UI]" in m and ("error:" in m or "warning:" in m)], f"lines after the clean load: {after}")

    rows = (await lua(ed, """
        local out = {}
        for _, d in ipairs(object('Menu', 'UIDocument'):getDiagnostics()) do
            out[#out + 1] = { d.severity, d.code, d.message }
        end
        return out
    """, edit=True))[0]
    ed.check(not [r for r in rows if r[0] != "info"], f"getDiagnostics() after the fix: {rows}")
    ed.check(not [r for r in rows if r[1] == "HTMLUI-CSS-0023"], f"the imported font family is loaded: {rows}")

    # the page: the imported image (8x8 orange, drawn 64x64 above the buttons) and the panel
    await ed.call("play", action="start", scene=S["scene"])
    try:
        await ed.call("wait", frames=3)
        box = (await lua(ed, """
            local d = object('Menu', 'UIDocument')
            local r = d:getElementById('logo'):getBoundingClientRect()
            local t = d:getElementById('title')
            local p = d:getElementById('play'):getBoundingClientRect()
            local q = d:getElementById('quit'):getBoundingClientRect()
            return { r.x, r.y, r.width, r.height }, t.offsetWidth, { p.x, p.y, p.width, q.x, q.y, q.width },
                   d:getElementById('plain').offsetWidth
        """))
        logo, title_width, buttons, fallback_width = box
        ed.check_eq(logo[2:], [64, 64], "the logo's CSS size")
        shot = await game_shot(ed, "fixed")
        ed.check(near(shot.px(logo[0] + 32, logo[1] + 32), ORANGE, 12), f"the imported image: {shot.px(logo[0] + 32, logo[1] + 32)}")
        # flex: the two buttons side by side, equally wide, 12px apart
        ed.check(abs(buttons[1] - buttons[4]) < 1 and abs(buttons[2] - buttons[5]) < 1.5, f"buttons in a row: {buttons}")
        ed.check(abs(buttons[3] - (buttons[0] + buttons[2] + 12)) < 1.5, f"the gap: {buttons}")
        # the title is laid out with the imported Orbitron 700, much wider than the fallback face
        ed.check(title_width > fallback_width * 1.1, f"'Star Forge' in Orbitron: {title_width}px, in the fallback face: {fallback_width}px")

        # the document's own Lua: one listener, one count per click
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": buttons[0] + 20, "y": buttons[1] + 10}],
                      space="canvas", frames_after=2)
        ed.check_eq(await lua(ed, "return p2MenuClicks"), [1], "the <script type=\"text/lua\"> listener")
    finally:
        await ed.call("play", action="stop")
    ed.check_eq(await lua(ed, "return UIDocument.getLiveLuaRefCount()", edit=True), [0], "Lua references after play stop")


# ---------------------------------------------------------------------------
# hot reload on the compat path

HOT = f"{UI}/hot.html"
HOT_HTML = """<!DOCTYPE html>
<html>
<head>
  <link rel="stylesheet" href="hot.css">
</head>
<body>
  <div id="box"></div>
  <div id="imp"></div>
  <img id="pic" src="hot.png" alt="">
  <span id="txt">Hot reload text</span>
  <input id="name" type="text">
  <div id="list"><p>1</p><p>2</p><p>3</p><p>4</p><p>5</p><p>6</p></div>
  <button id="go">Go</button>
  <script type="text/lua">
    _G.hotRuns = (_G.hotRuns or 0) + 1
    document:getElementById("go"):addEventListener("click", function() _G.hotClicks = (_G.hotClicks or 0) + 1 end)
  </script>
</body>
</html>
"""
HOT_CSS = """@import url("hot-imported.css");
@font-face { font-family: "Hot"; src: url("hot.ttf") format("truetype"); }
* { margin: 0; padding: 0; box-sizing: border-box; }
body { color: #fff; font-family: sans-serif; }
#box { position: absolute; left: 40px; top: 40px; width: 200px; height: 100px; background: rgb(255, 0, 0); }
#pic { position: absolute; left: 40px; top: 300px; width: 100px; height: 100px; }
#txt { position: absolute; left: 40px; top: 450px; font-family: "Hot", sans-serif; font-size: 32px; }
#name { position: absolute; left: 400px; top: 40px; width: 200px; height: 30px; }
#list { position: absolute; left: 400px; top: 100px; width: 200px; height: 80px; overflow: auto; }
#list p { height: 40px; }
#go { position: absolute; left: 400px; top: 300px; width: 120px; height: 40px; }
"""
HOT_IMPORTED = "#imp { position: absolute; left: 40px; top: 160px; width: 200px; height: 100px; background: rgb(0, 0, 255); }\n"

RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255)
P_BOX, P_IMP, P_PIC, P_ADDED = (140, 90), (140, 210), (90, 350), (750, 350)


def write_bytes(ed: Editor, relative: str, content: bytes) -> None:
    """Like an external tool replacing a binary file in the project."""
    path = ed.project / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(content)
    os.replace(temporary, path)


async def test_hot_reload_edit_mode(ed: Editor):
    await ed.call("clear_console")
    write_bytes(ed, f"{UI}/hot.png", png(8, 8, MAGENTA))
    write_bytes(ed, f"{UI}/hot.ttf", (FONTS / "inter/Inter-400.ttf").read_bytes())
    await ed.call("write_file", path=f"{UI}/hot-imported.css", content=HOT_IMPORTED)
    await ed.call("write_file", path=f"{UI}/hot.css", content=HOT_CSS)
    await ed.call("write_file", path=HOT, content=HOT_HTML)
    made = await ed.call("create_scene", name="P2Hot", type="UI")
    S["hot_scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["hot_scene"], settings={"background_color": [0.0, 0.0, 0.0, 1.0]})
    ent = await ed.call("create_entity", scene=S["hot_scene"], type="ui_document", name="Hot")
    await ed.call("set_properties", scene=S["hot_scene"], entity=ent["entity_id"], values={"uidocument": {"filename": HOT}})
    await ed.call("save_scene", scene=S["hot_scene"], path="scenes/P2Hot")
    scene = S["hot_scene"]

    shot = await wait_for_color(ed, "hot-setup", P_BOX, RED, scene=scene)
    ed.check(near(shot.px(*P_IMP), BLUE, 12), f"the @imported sheet: {shot.px(*P_IMP)}")
    ed.check(near(shot.px(*P_PIC), MAGENTA, 12), f"the image: {shot.px(*P_PIC)}")
    lines = await messages(ed)
    ed.check_eq(len(summaries(lines, "ui/p2/hot.html")), 1, "one summary line for the first load")
    ed.check("0 errors, 0 warnings" in summaries(lines, "ui/p2/hot.html")[0], f"clean: {summaries(lines, 'ui/p2/hot.html')}")

    # the linked sheet, the @imported sheet, the image, the document: each change shows
    await ed.call("write_file", path=f"{UI}/hot.css", content=HOT_CSS.replace("rgb(255, 0, 0)", "rgb(0, 255, 0)"))
    await wait_for_color(ed, "hot-edit-css", P_BOX, GREEN, scene=scene)
    await ed.call("write_file", path=f"{UI}/hot-imported.css", content=HOT_IMPORTED.replace("rgb(0, 0, 255)", "rgb(255, 255, 0)"))
    shot = await wait_for_color(ed, "hot-edit-import", P_IMP, YELLOW, scene=scene)
    ed.check(near(shot.px(*P_BOX), GREEN, 12), f"the linked sheet's color stays: {shot.px(*P_BOX)}")
    write_bytes(ed, f"{UI}/hot.png", png(8, 8, CYAN))
    await wait_for_color(ed, "hot-edit-image", P_PIC, CYAN, scene=scene)
    await ed.call("write_file", path=HOT, content=HOT_HTML.replace(
        '<button id="go">Go</button>',
        '<button id="go">Go</button>\n  <div id="added" style="position: absolute; left: 700px; top: 300px; width: 100px; '
        'height: 100px; background: orange;"></div>'))
    shot = await wait_for_color(ed, "hot-edit-html", P_ADDED, (255, 165, 0), scene=scene)
    ed.check(near(shot.px(*P_BOX), GREEN, 12) and near(shot.px(*P_IMP), YELLOW, 12) and near(shot.px(*P_PIC), CYAN, 12),
             f"the other changes stay after the document reload: {shot.px(*P_BOX)} {shot.px(*P_IMP)} {shot.px(*P_PIC)}")

    # no error, no warning, nothing logged twice for one reload
    await asyncio.sleep(1.0)
    lines = await messages(ed)
    ed.check(not [m for m in lines if "[HTML UI]" in m and ("error:" in m or "warning:" in m)],
             "HTML UI problems: " + "\n".join(m for m in lines if "[HTML UI]" in m))
    loads = len(summaries(lines, "ui/p2/hot.html"))
    ed.check(loads <= 5, f"{loads} summary lines for one load and four changes (a change reloaded twice):\n" +
             "\n".join(summaries(lines, "ui/p2/hot.html")))
    info = await ed.call("list_scenes")
    for item in info.get("scenes", []):
        if item.get("id") == scene:
            ed.check(not item.get("modified", False), f"the scene is marked modified by a reload: {item}")


async def test_hot_reload_play_mode(ed: Editor):
    scene = S["hot_scene"]
    await ed.call("clear_console")
    await ed.call("write_file", path=f"{UI}/hot.css", content=HOT_CSS)
    await ed.call("write_file", path=f"{UI}/hot-imported.css", content=HOT_IMPORTED)
    await ed.call("write_file", path=HOT, content=HOT_HTML)
    await wait_for_color(ed, "hot-reset", P_BOX, RED, scene=scene)
    await ed.call("play", action="start", scene=scene)
    try:
        await ed.call("wait", frames=3)
        doc = "object('Hot', 'UIDocument')"
        ed.check_eq(await lua(ed, "return hotRuns"), [1], "the document script ran once at play start")
        await lua(ed, f"local d = {doc}; hotBox = d:getElementById('box'); d:getElementById('name').value = 'kept'; "
                      "d:getElementById('list').scrollTop = 40; d:getElementById('name'):focus()")
        width_inter = (await lua(ed, f"return {doc}:getElementById('txt').offsetWidth"))[0]

        # a linked sheet and an @imported one: the DOM stays (the handle is valid), no script runs again
        await ed.call("write_file", path=f"{UI}/hot.css", content=HOT_CSS.replace("rgb(255, 0, 0)", "rgb(0, 255, 0)"))
        await wait_for_color(ed, "hot-play-css", P_BOX, GREEN, scene=scene)
        await ed.call("write_file", path=f"{UI}/hot-imported.css", content=HOT_IMPORTED.replace("rgb(0, 0, 255)", "rgb(255, 255, 0)"))
        await wait_for_color(ed, "hot-play-import", P_IMP, YELLOW, scene=scene)
        values = await lua(ed, f"local d = {doc}; return hotBox:isValid(), hotRuns, d:getElementById('name').value, "
                               "d:getElementById('list').scrollTop")
        ed.check_eq(values[:3], [True, 1, "kept"], "a stylesheet reload keeps the DOM and runs no script")
        ed.check(abs(values[3] - 40) < 1, f"scrollTop after the stylesheet reloads: {values[3]}")

        # the image, replaced on disk
        write_bytes(ed, f"{UI}/hot.png", png(8, 8, MAGENTA))
        await wait_for_color(ed, "hot-play-image", P_PIC, MAGENTA, scene=scene)

        # the font, replaced on disk: the text is laid out with the new face (Orbitron is wider)
        write_bytes(ed, f"{UI}/hot.ttf", (FONTS / "orbitron/Orbitron-700.ttf").read_bytes())
        deadline = time.monotonic() + 6.0
        while True:
            width = (await lua(ed, f"return {doc}:getElementById('txt').offsetWidth"))[0]
            if width > width_inter * 1.15:
                break
            if time.monotonic() > deadline:
                ed.fail(f"the replaced font did not reload: the text is {width}px wide, {width_inter}px before")
            await asyncio.sleep(0.25)
        values = await lua(ed, f"local d = {doc}; local n = d:getElementById('name'); return n.value, "
                               "d:getElementById('list').scrollTop, n:matches(':focus')")
        ed.check_eq(values[0], "kept", "the input's value survived the font reload")
        ed.check(abs(values[1] - 40) < 1, f"scrollTop after the font reload: {values[1]}")
        ed.check_eq(values[2], True, "the focus survived the font reload")
        runs = (await lua(ed, "return hotRuns"))[0]

        # the document: state kept, the script runs again, its listener fires once per click
        await ed.call("write_file", path=HOT, content=HOT_HTML.replace(
            '<button id="go">Go</button>',
            '<button id="go">Go</button>\n  <div id="added" style="position: absolute; left: 700px; top: 300px; width: 100px; '
            'height: 100px; background: orange;"></div>'))
        await wait_for_color(ed, "hot-play-html", P_ADDED, (255, 165, 0), scene=scene)
        values = await lua(ed, f"local d = {doc}; local n = d:getElementById('name'); return n.value, "
                               "d:getElementById('list').scrollTop, n:matches(':focus'), hotRuns, hotBox:isValid()")
        ed.check_eq(values[0], "kept", "the input's value survived the document reload")
        ed.check(abs(values[1] - 40) < 1, f"scrollTop after the document reload: {values[1]}")
        ed.check_eq(values[2], True, "the focus survived the document reload")
        ed.check_eq(values[3], runs + 1, "the document script ran again, once")
        ed.check_eq(values[4], False, "a document reload replaces the elements")
        await lua(ed, "hotClicks = 0")
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": 460, "y": 320}], space="canvas", frames_after=2)
        ed.check_eq(await lua(ed, "return hotClicks"), [1], "one listener on #go after the reload: one count per click")

        lines = await messages(ed)
        ed.check(not [m for m in lines if "[HTML UI]" in m and ("error:" in m or "warning:" in m)],
                 "HTML UI problems: " + "\n".join(m for m in lines if "[HTML UI]" in m))
    finally:
        await ed.call("play", action="stop")
        write_bytes(ed, f"{UI}/hot.ttf", (FONTS / "inter/Inter-400.ttf").read_bytes())
    ed.check_eq(await lua(ed, "return UIDocument.getLiveLuaRefCount()", edit=True), [0], "Lua references after play stop")


# ---------------------------------------------------------------------------
# texture mode, child scenes, documents loaded from Lua, .rml

OTHER_HTML = """<!DOCTYPE html>
<html><head><style>
  body { margin: 0; }
  .tile { position: absolute; left: 600px; top: 500px; width: 100px; height: 100px; background: hsl(120 100% 50%); }
</style></head>
<body><div class="tile" id="tile"></div><p id="where">other</p></body></html>
"""
TEXTURE_HTML = """<!DOCTYPE html>
<html><head><style>
  body { margin: 0; background: rgb(0 0 255 / 100%); }
  #t { box-sizing: border-box; width: 50%; height: 128px; background: rgba(255, 0, 0, 0.5); border: 2px solid white; }
</style></head>
<body><div id="t">on a texture</div></body></html>
"""
HUD_HTML = """<!DOCTYPE html>
<html><head><style>
  body { margin: 0; }
  #hud { position: fixed; right: 20px; top: 20px; width: 120px; height: 60px; background: rgb(0, 255, 255); }
</style></head>
<body><div id="hud"></div></body></html>
"""
RML_DOC = """<rml>
<head>
    <style>
        body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; }
        div { display: block; }
        #r { position: absolute; left: 900px; top: 500px; width: 100px; height: 100px; background-color: rgba(255, 255, 0, 255);
             border: 4px #ff00ff; }
    </style>
</head>
<body>
    <div id="r"/>
    <div id="after">after</div>
</body>
</rml>
"""


async def test_lua_loaded_documents_and_fragments(ed: Editor):
    await ed.call("clear_console")
    await ed.call("write_file", path=f"{UI}/other.html", content=OTHER_HTML)
    await ed.call("write_file", path=f"{UI}/texture.html", content=TEXTURE_HTML)
    await ed.call("write_file", path=f"{UI}/plain.rml", content=RML_DOC)
    scene = S["hot_scene"]
    made = await ed.call("create_entity", scene=scene, type="ui_document", name="OnTexture")
    await ed.call("set_properties", scene=scene, entity=made["entity_id"],
                  values={"uidocument": {"filename": f"{UI}/texture.html", "renderMode": "texture", "textureWidth": 256,
                                         "textureHeight": 256}})
    made = await ed.call("create_entity", scene=scene, type="ui_document", name="Rml")
    await ed.call("set_properties", scene=scene, entity=made["entity_id"], values={"uidocument": {"filename": f"{UI}/plain.rml"}})
    await ed.call("save_scene", scene=scene)

    await ed.call("play", action="start", scene=scene)
    try:
        await ed.call("wait", frames=3)
        # a texture-mode document is compiled like a screen one, for its own viewport
        values = await lua(ed, """
            local d = object('OnTexture', 'UIDocument')
            local t = d:getElementById('t')
            return d.documentElement.tagName, d.body.tagName, t.offsetWidth, t.offsetHeight, t.style.backgroundColor,
                   #d:getDiagnostics(), d:getTargetTexture() ~= nil
        """)
        ed.check_eq(values[:2], ["HTML", "BODY"], "the texture document is an HTML document")
        ed.check_eq(values[2:4], [128, 128], "50% of the 256px texture page")
        ed.check("255, 0, 0" in str(values[4]) or "#ff0000" in str(values[4]).lower(), f"rgba(255, 0, 0, 0.5) rewritten: {values[4]}")
        ed.check_eq(values[6], True, "its target texture")

        # an .rml document: RmlUi's own loader (XML, RCSS, no user-agent sheet, lower-case tag names)
        values = await lua(ed, """
            local d = object('Rml', 'UIDocument')
            local r = d:getElementById('r')
            return d:getElementById('after').tagName, r.offsetWidth, #d:getDiagnostics(), d:getElementById('after').offsetTop
        """)
        ed.check_eq(values[:3], ["div", 108, 0], "the .rml document: tagName as RML, RCSS border (4px each side), no compat diagnostics")
        shot = await game_shot(ed, "rml", scene)
        ed.check(near(shot.px(950, 550), YELLOW, 12), f"the .rml document's RCSS color (alpha 0-255): {shot.px(950, 550)}")
        ed.check(near(shot.px(902, 550), MAGENTA, 12), f"its RCSS border: {shot.px(902, 550)}")

        # UIDocument:load of another .html, and HTML fragments
        values = await lua(ed, """
            local d = object('Hot', 'UIDocument')
            local loaded = d:load('ui/p2/other.html')
            local where = d:getElementById('where')
            where.innerHTML = '<b class=x>a&nbsp;b</b><br><img src=hot.png width=10 height=10><p style="color: rgb(0 128 0 / 50%)">c'
            where:insertAdjacentHTML('beforeend', '<span id=added-late>late</span>')
            local kids = {}
            for _, c in ipairs(where.children) do kids[#kids + 1] = c.tagName end
            return loaded, d.documentElement.tagName, kids, where:querySelector('b').textContent, d:getElementById('added-late') ~= nil,
                   where:querySelector('p').style.color
        """)
        ed.check_eq(values[0], True, "UIDocument:load of an .html")
        ed.check_eq(values[1], "HTML", "the loaded document went through the HTML stage")
        ed.check_eq(values[2], ["B", "BR", "IMG", "P", "SPAN"], "innerHTML parsed as HTML (unquoted attributes, void tags, an unclosed <p>)")
        ed.check_eq(values[3], "a b", "the &nbsp; entity")
        ed.check_eq(values[4], True, "insertAdjacentHTML")
        ed.check("0, 128, 0" in str(values[5]) or str(values[5]).lower().startswith("#008000"),
                 f"the fragment's style attribute was rewritten: {values[5]}")
        await wait_for_color(ed, "lua-loaded", (650, 550), GREEN, scene=scene)

        lines = await messages(ed)
        for document in ("ui/p2/other.html", "ui/p2/texture.html"):
            ed.check(summaries(lines, document), f"no compat summary line for {document}")
        ed.check(not summaries(lines, "ui/p2/plain.rml"), "an .rml document has no compat summary")
        ed.check(not [m for m in lines if "[HTML UI]" in m and ("error:" in m or "warning:" in m)],
                 "HTML UI problems: " + "\n".join(m for m in lines if "[HTML UI]" in m))
    finally:
        await ed.call("play", action="stop")
    ed.check_eq(await lua(ed, "return UIDocument.getLiveLuaRefCount()", edit=True), [0], "Lua references after play stop")


async def test_child_scene_document(ed: Editor):
    await ed.call("clear_console")
    await ed.call("write_file", path=f"{UI}/hud.html", content=HUD_HTML)
    made = await ed.call("create_scene", name="P2Hud", type="UI")
    hud = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=hud, type="ui_document", name="Hud")
    await ed.call("set_properties", scene=hud, entity=ent["entity_id"], values={"uidocument": {"filename": f"{UI}/hud.html"}})
    await ed.call("save_scene", scene=hud, path="scenes/P2Hud")
    await ed.call("set_child_scene", scene=S["scene"], child=hud, attached=True, start_active=True)
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("play", action="start", scene=S["scene"])
    try:
        await ed.call("wait", frames=3)
        # position: fixed in the child scene's document, over the parent's menu
        await wait_for_color(ed, "child-scene", (1200, 50), (0, 255, 255), scene=S["scene"])
        lines = await messages(ed)
        ed.check(summaries(lines, "ui/p2/hud.html"), "no compat summary line for the child scene's document")
        ed.check(not [m for m in lines if "[HTML UI]" in m and "error:" in m], "errors: " + "\n".join(m for m in lines if "error:" in m))
    finally:
        await ed.call("play", action="stop")
        await ed.call("set_child_scene", scene=S["scene"], child=hud, attached=False)


# ---------------------------------------------------------------------------
# the command line, and what is not reachable


async def test_import_ui_command_line(ed: Editor):
    """jasperengine-editor import-ui on a copy of the project: imports through the mirror, exit 0;
    a document with a URL the mirror lacks: an HTMLUI-IMP-0002 line and exit 1."""
    copy = ed.scratch.root / "cli-project"
    shutil.copytree(ed.project, copy, ignore=shutil.ignore_patterns(".jasperengine", "htmlui-imports"))
    (copy / UI / "cli.html").write_text(GOOD_HTML)
    (copy / UI / "offline.html").write_text(GOOD_HTML.replace(LOGO_URL, "https://images.example.net/unreachable.png"))
    command = harness.editor_command()
    env = dict(os.environ)

    def run(*documents):
        return subprocess.run(command + ["import-ui", "--project", str(copy), *documents], env=env, capture_output=True,
                              text=True, timeout=300)

    done = await asyncio.to_thread(run, f"{UI}/cli.html")
    ed.check_eq(done.returncode, 0, f"import-ui: {done.stdout[-1500:]}\n{done.stderr[-1500:]}")
    manifest = json.loads((copy / "assets/htmlui-imports/manifest.json").read_text())
    ed.check_eq(sorted(e["url"] for e in manifest["entries"]), sorted([FONT_URL, LOGO_URL]), "the manifest the command wrote")
    ed.check("HTMLUI-IMP-0004" in done.stdout and f"{UI}/cli.html" in done.stdout, f"compiler-style lines with project-relative paths: {done.stdout[-800:]}")

    failed = await asyncio.to_thread(run, f"{UI}/offline.html")
    ed.check_eq(failed.returncode, 1, f"exit code with a failed import: {failed.stdout[-800:]}")
    ed.check("HTMLUI-IMP-0002" in failed.stdout + failed.stderr and "unreachable.png" in failed.stdout + failed.stderr,
             f"the failed URL is reported: {failed.stdout[-800:]}{failed.stderr[-800:]}")
    after = json.loads((copy / "assets/htmlui-imports/manifest.json").read_text())
    ed.check(not [e for e in after["entries"] if "unreachable" in e["url"]], "nothing recorded for the failed URL")

    usage = await asyncio.to_thread(run)
    ed.check_eq(usage.returncode, 2, f"a usage error: {usage.stdout[-300:]}{usage.stderr[-300:]}")


async def test_inspector_panel_screenshot(ed: Editor):
    """The Inspector of the menu's UI Document (Remote resources, diagnostics): a screenshot to look at."""
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("select_entities", scene=S["scene"], entity=S["menu"])
    await ed.call("focus_window", window="inspector")
    await asyncio.sleep(1.0)
    shot = await ed.call_raw("screenshot", {"view": "editor", "max_width": 1600})
    if shot.is_error and "need the OpenGL renderer" in shot.text:
        skip("whole-editor screenshots need the OpenGL editor (this one renders with another backend)")
    ed.check(not shot.is_error and shot.images, f"editor screenshot: {shot.text[:300]}")
    out = ed.scratch.root / "p2-inspector.png"
    out.write_bytes(shot.images[0][0])
    print(f"inspector screenshot: {out}")
    keep = os.environ.get("P2_KEEP_SHOTS")
    if keep:
        Path(keep).mkdir(parents=True, exist_ok=True)
        for image in ed.scratch.root.glob("p2-*.png"):
            shutil.copy(image, Path(keep) / image.name)


async def test_verbose_setting_reaches_the_compat_layer():
    """A second editor started with html_ui.verbose_diagnostics: the notes are Console lines."""
    original = harness.Scratch.make

    def make(*args, **kwargs):
        scratch = original(*args, **kwargs)
        path = scratch.root / "settings.yaml"
        settings = yaml.safe_load(path.read_text())
        settings["html_ui"] = {"auto_import": False, "verbose_diagnostics": True}
        path.write_text(yaml.safe_dump(settings, sort_keys=False))
        return scratch

    harness.Scratch.make = make
    try:
        async with Editor() as ed:
            await ed.call("write_file", path=MENU_CSS, content=BAD_CSS)
            await ed.call("write_file", path=MENU, content=BAD_HTML)
            made = await ed.call("create_scene", name="P2Verbose", type="UI")
            ent = await ed.call("create_entity", scene=made["scene"]["id"], type="ui_document", name="Menu")
            await ed.call("set_properties", scene=made["scene"]["id"], entity=ent["entity_id"], values={"uidocument": {"filename": MENU}})
            await wait_messages(ed, "[HTML UI] ui/p2/menu.html: 4 errors", 30.0)
            lines = await messages(ed)
            notes = [m for m in lines if m.startswith("[HTML UI] ui/p2/menu.") and ": info: " in m]
            ed.check(notes, "no info lines with Verbose Diagnostics on; [HTML UI] lines:\n" +
                     "\n".join(m for m in lines if "[HTML UI]" in m))
            ed.check(any("grid-template-columns" in m and "[HTMLUI-CSS-0007]" in m for m in notes), f"the grid note: {notes}")
            summary = summaries(lines, "ui/p2/menu.html")[-1]
            count = int(summary.split(" warnings, ")[1].split(" note")[0])
            ed.check_eq(len(notes) % count if count else 1, 0, f"as many info lines as the summary counts notes: {summary} / {len(notes)}")
            # auto-import is off in this editor: nothing was downloaded
            ed.check(not [m for m in lines if "[HTML UI] importing " in m], "imports ran with Import Remote Resources off")
    finally:
        harness.Scratch.make = original


async def test_cleanup():
    shutil.rmtree(MIRROR, ignore_errors=True)
