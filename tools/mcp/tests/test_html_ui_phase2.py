"""Phase-2 integration (p2-integrate): what crosses the phase-2 work packages, in the real editor.

An agent's-eye run and the checks no single package owned:
- a menu written the way an LLM writes one for a browser, with the mistakes the diagnostics
  exist for (a Tailwind CDN script and stylesheet, a JavaScript <script>, onclick=,
  position: sticky, a property name one letter off, a remote image nobody imported): every
  one is in the Console with file:line:column, its code and a suggestion, once, and in the
  document's diagnostics; the page still draws;
- the file fixed (Lua instead of JavaScript, flex instead of grid, a Google Font and an image
  the import mirror serves): the editor imports both, the document reloads to "0 errors" and
  draws with the imported font and image;
- the editor setting HTML UI > Verbose Diagnostics lists the notes one by one (a second editor
  started with the setting).

No network: the importers read JASPERENGINE_IMPORT_MIRROR, built here before the editor starts.
Runs on a scratch copy of the fixture project (canvas 1280 x 720).
"""

import asyncio
import json
import os
import tempfile
import time
import urllib.parse
import struct
import zlib
from pathlib import Path

import yaml

import harness
from harness import Editor
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

# (file, line, column, severity, code): diagnostic wording is not part of this contract.
EXPECTED = [
    ('ui/p2/menu.html', 7, 3, 'warning', 'HTMLUI-HTML-0007'),
    ('ui/p2/menu.html', 8, 15, 'error', 'HTMLUI-IMP-0001'),
    ('ui/p2/menu.html', 14, 28, 'error', 'HTMLUI-IMP-0001'),
    ('ui/p2/menu.html', 16, 27, 'error', 'HTMLUI-HTML-0005'),
    ('ui/p2/menu.html', 20, 3, 'error', 'HTMLUI-HTML-0006'),
    ('ui/p2/menu.css', 17, 3, 'warning', 'HTMLUI-CSS-0001'),
]
# (the menu's 'display: grid' is no finding since phase 3: the layout engine has CSS Grid, ForkFeature::Grid)
# (nor its 'position: sticky': the layout engine sticks it, ForkFeature::Sticky)

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
_mirror = tempfile.TemporaryDirectory(prefix="jasper-p2-mirror-")
MIRROR = Path(_mirror.name)
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


async def _setup_menu(ed: Editor):
    if "scene" in S:
        return
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

    await wait_messages(ed, "[HTMLUI-IMP-0002]", 60.0, count=2)


async def test_mistakes_are_reported(ed: Editor):
    await _setup_menu(ed)
    # the editor tried to import the two remote files and could not (the mirror has neither)
    await wait_messages(ed, "[HTMLUI-IMP-0002]", 60.0, count=2)
    lines = await messages(ed)

    for file, line, column, severity, code in EXPECTED:
        head = f"[HTML UI] {file}:{line}:{column}: {severity}: "
        found = [m for m in lines if m.startswith(head) and m.rstrip().endswith(f"[{code}]")]
        ed.check(found, f"no Console line '{head}... [{code}]'; [HTML UI] lines:\n" +
                 "\n".join(m for m in lines if "[HTML UI]" in m)[-4000:])
        ed.check("(fix: " in found[0], f"the diagnostic supplies a fix: {found[0]}")
    # notes are counted, not listed (Verbose Diagnostics is off)
    ed.check(not [m for m in lines if "[HTML UI]" in m and ": info: " in m and "ui/p2/menu" in m],
             "info lines listed without the verbose setting")
    summary = summaries(lines, "ui/p2/menu.html")
    ed.check(summary, "no summary line for ui/p2/menu.html")
    ed.check("4 errors, 2 warnings" in summary[-1], f"the summary: {summary[-1]}")

    # the same findings through the document, with the notes (edit mode: no script has run)
    found = await lua(ed, """
        local out = {}
        for _, d in ipairs(object('Menu', 'UIDocument'):getDiagnostics()) do
            out[#out + 1] = { d.file, d.line, d.column, d.severity, d.code, d.message, d.suggestion }
        end
        return out
    """, edit=True)
    rows = found[0]
    for file, line, column, severity, code in EXPECTED:
        match = [r for r in rows if r[0] == file and r[1] == line and r[2] == column and r[3] == severity and r[4] == code]
        ed.check(match, f"getDiagnostics() lacks {file}:{line}:{column} {severity} {code}: {rows}")
        ed.check(match[0][6], f"getDiagnostics() supplies a suggestion for {code}: {match[0]}")
    ed.check([r for r in rows if r[3] == "info"], "getDiagnostics() lists the notes too")

    # the two failed downloads: an error each, at the reference, and the page still draws
    failed = [m for m in lines if "[HTMLUI-IMP-0002]" in m]
    ed.check_eq(len(failed), 2, f"download errors: {failed}")
    ed.check(all("ui/p2/menu.html:" in m for m in failed), f"the download errors point at the document: {failed}")
    loads = len(summaries(lines, "ui/p2/menu.html"))
    ed.check(1 <= loads <= 2, f"the document loaded {loads} times (once, and at most once more after the import batch)")
    for file, line, column, severity, code in EXPECTED:
        head = f"[HTML UI] {file}:{line}:{column}: {severity}: "
        count = len([m for m in lines if m.startswith(head) and m.rstrip().endswith(f"[{code}]")])
        ed.check_eq(count, loads, f"'{head}[{code}]' lines for {loads} load(s)")
    shot = await game_shot(ed, "mistakes")
    ed.check(shot.px(20, 20)[2] > shot.px(20, 20)[1] + 10, f"the gradient background: {shot.px(20, 20)}")


async def test_fixed_file_loads_clean(ed: Editor):
    await _setup_menu(ed)
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
        logo, button = await lua(ed, """
            local d = object('Menu', 'UIDocument')
            local r = d:getElementById('logo'):getBoundingClientRect()
            local p = d:getElementById('play'):getBoundingClientRect()
            return { r.x + r.width / 2, r.y + r.height / 2 }, { p.x + p.width / 2, p.y + p.height / 2 }
        """)
        shot = await game_shot(ed, "fixed")
        ed.check(near(shot.px(*logo), ORANGE, 12), f"the imported image: {shot.px(*logo)}")

        # the document's own Lua: one listener, one count per click
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": button[0], "y": button[1]}],
                      space="canvas", frames_after=2)
        ed.check_eq(await lua(ed, "return p2MenuClicks"), [1], "the <script type=\"text/lua\"> listener")
    finally:
        await ed.call("play", action="stop")


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
            ed.check(any("font family 'Segoe UI'" in m and "[HTMLUI-CSS-0023]" in m for m in notes), f"the font note: {notes}")
            summary = summaries(lines, "ui/p2/menu.html")[-1]
            count = int(summary.split(" warnings, ")[1].split(" note")[0])
            ed.check_eq(len(notes) % count if count else 1, 0, f"as many info lines as the summary counts notes: {summary} / {len(notes)}")
            # auto-import is off in this editor: nothing was downloaded
            ed.check(not [m for m in lines if "[HTML UI] importing " in m], "imports ran with Import Remote Resources off")
    finally:
        harness.Scratch.make = original
