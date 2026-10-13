"""Hot reload of HTML UI documents (phase 1, p1-editor, spec §3.8.6), through MCP.

The editor's HtmlUIWatcher hands changed files to UIDocumentSystem::reloadChangedFiles every
0.25 s, in edit and play mode. write_file changes them here, like an external editor would:
- edit mode: a CSS colour change shows in the Game view and the Scene view within 2 s;
- play mode: a CSS change shows within 2 s and keeps the DOM (an element handle stored in a
  Lua global stays valid); an HTML change reloads the document keeping the input's value,
  the scroll position and the focus, and a listener on an element with an id still fires;
  onLoad runs once more.
The Lua checks need p1-lua's element API (UIDocument in Lua); without it they are skipped.

Runs on a scratch copy of the fixture project (canvas 1280 x 720).
"""

import asyncio
import json
import re
import shutil
import time
from pathlib import Path

from harness import REPO, Editor, skip
from htmlui_pixels import Shot, near, count_near

TIMEOUT = 900

S: dict = {}

DOC = "assets/ui/reload.html"
CSS_PATH = "assets/ui/reload.css"

HTML = """<html>
<head>
    <title>reload</title>
    <link type="text/css" href="reload.css"/>
</head>
<body>
    <div id="box"></div>
    <input id="name" type="text" value=""/>
    <div id="list"><p>1</p><p>2</p><p>3</p><p>4</p><p>5</p><p>6</p></div>
    <div id="go">Go</div>
</body>
</html>
"""

CSS = """body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0;
       font-family: sans-serif; font-size: 16px; color: #ffffff; }
div, p, input { display: block; }
#box { position: absolute; left: 40px; top: 40px; width: 300px; height: 200px; background-color: #ff0000; }
#name { position: absolute; left: 400px; top: 40px; width: 200px; height: 30px; background-color: #333333; }
#list { position: absolute; left: 400px; top: 100px; width: 200px; height: 80px; overflow: auto; }
#list p { height: 40px; }
#go { position: absolute; left: 40px; top: 300px; width: 120px; height: 40px; background-color: #808080; }
"""

PROBE = """---@class ReloadProbe : ScriptInstance
local ReloadProbe = {
    properties = {
        { name = "loads", displayName = "Loads", type = "int", default = 0 },
        { name = "clicks", displayName = "Clicks", type = "int", default = 0 },
    }
}

function ReloadProbe:init()
    self.doc = UIDocument(self.scene, self.entity)
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    local probe = self
    self.doc:getElementById("go"):addEventListener("click", function(event) probe.clicks = probe.clicks + 1 end)
    _G.reloadBox = self.doc:getElementById("box")
end

function ReloadProbe:onDocumentLoad()
    self.loads = self.loads + 1
end

return ReloadProbe
"""

RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
YELLOW = (255, 255, 0)
P_BOX = (190, 140)


async def game_shot(ed: Editor, label: str) -> Shot:
    shot = await ed.call_raw("screenshot", {"view": "game", "scene": S["scene"], "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    (ed.scratch.root / f"html-ui-reload-{label}.png").write_bytes(shot.images[0][0])
    return Shot(shot.images[0][0], shot.value)


async def wait_for_color(ed: Editor, label: str, point, color, seconds: float = 2.0) -> Shot:
    """Game view screenshots until `point` shows `color`; fails after `seconds` and at least
    three screenshots (a screenshot itself takes a while on a loaded host)."""
    deadline = time.monotonic() + seconds
    attempts = 0
    while True:
        shot = await game_shot(ed, label)
        attempts += 1
        if near(shot.px(*point), color, 12):
            return shot
        if time.monotonic() > deadline and attempts >= 3:
            ed.fail(f"{label}: pixel at canvas {point} is {shot.px(*point)} after {seconds} s, expected {color}")
        await asyncio.sleep(0.1)


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def has_element_api(ed: Editor) -> bool:
    values = (await ed.call("eval_lua", code="return UIDocument ~= nil",
                            allow_edit_mode=True))["values"]
    return values == [True]


async def test_setup(ed: Editor):
    await ed.console_since(None, limit=1)
    await ed.call("write_file", path=DOC, content=HTML)
    await ed.call("write_file", path=CSS_PATH, content=CSS)
    made = await ed.call("create_scene", name="HtmlUiReload", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=S["scene"], settings={"background_color": [0.118, 0.118, 0.118, 1.0]})
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Menu")
    S["menu"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": DOC}})
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiReload")
    await wait_for_color(ed, "setup", P_BOX, RED)


async def test_edit_mode_css_change(ed: Editor):
    await ed.call("write_file", path=CSS_PATH, content=CSS.replace("#ff0000", "#00ff00"))
    await wait_for_color(ed, "edit-css-game", P_BOX, GREEN)

    # the Scene view of the UI scene redraws with it (p1-render draws documents there)
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("set_scene_camera", scene=S["scene"], center=[640, 360], zoom=2.0)
    deadline = time.monotonic() + 2.0
    while True:
        shot = await ed.call_raw("screenshot", {"view": "scene", "scene": S["scene"], "max_width": 800})
        ed.check(not shot.is_error and shot.images, f"scene view: {shot.text[:300]}")
        png = shot.images[0][0]
        (ed.scratch.root / "html-ui-reload-edit-css-scene.png").write_bytes(png)
        greens, reds = count_near(png, GREEN), count_near(png, RED)
        if greens > 50 and reds < 10:
            break
        if time.monotonic() > deadline:
            ed.fail(f"the Scene view shows no restyled document: {greens} green, {reds} red samples")
        await asyncio.sleep(0.1)

    # a hot reload is not an edit: nothing to save
    info = await ed.call("list_scenes")
    for scene in info.get("scenes", []):
        if scene.get("id") == S["scene"]:
            ed.check(not scene.get("modified", False), f"scene marked modified by a reload: {scene}")


async def test_play_mode_css_change_keeps_dom(ed: Editor):
    S["playing_lua"] = await has_element_api(ed)
    if S["playing_lua"]:
        await ed.call("create_script", kind="lua", name="ReloadProbe", content=PROBE, scene=S["scene"], entity=S["menu"])
    await ed.call("play", action="start", scene=S["scene"])
    try:
        loads = (await lua(ed, "return script('Menu', 'ReloadProbe').loads"))[0] if S["playing_lua"] else None
        await ed.call("write_file", path=CSS_PATH, content=CSS.replace("#ff0000", "#0000ff"))
        await wait_for_color(ed, "play-css", P_BOX, BLUE)
        if S["playing_lua"]:
            ed.check_eq(await lua(ed, "return reloadBox:isValid()"), [True], "the element handle survived a stylesheet reload")
            ed.check_eq((await lua(ed, "return script('Menu', 'ReloadProbe').loads"))[0], loads, "a stylesheet reload is not a load")
    except BaseException:
        await ed.call("play", action="stop")
        raise


async def test_play_mode_html_change_keeps_state(ed: Editor):
    try:
        if not S.get("playing_lua"):
            # without the element API: the document still reloads (a new element shows)
            await ed.call("write_file", path=DOC, content=HTML.replace(
                '<div id="go">Go</div>', '<div id="go">Go</div>\n    <div id="added" style="position: absolute; left: 700px; top: 300px; '
                'width: 100px; height: 100px; background-color: #ffff00;"></div>'))
            await wait_for_color(ed, "play-html", (750, 350), YELLOW)
            skip("needs p1-lua's element API for the state and listener checks")

        await lua(ed, "local d = script('Menu', 'ReloadProbe').doc; d:getElementById('name').value = 'kept'; "
                      "d:getElementById('list').scrollTop = 40; d:getElementById('name'):focus()")
        loads_before = (await lua(ed, "return script('Menu', 'ReloadProbe').loads"))[0]

        await ed.call("write_file", path=DOC, content=HTML.replace(
            '<div id="go">Go</div>', '<div id="go">Go</div>\n    <div id="added" style="position: absolute; left: 700px; top: 300px; '
            'width: 100px; height: 100px; background-color: #ffff00;"></div>'))
        await wait_for_color(ed, "play-html", (750, 350), YELLOW)

        values = await lua(ed, "local d = script('Menu', 'ReloadProbe').doc; local n = d:getElementById('name'); "
                               "return n.value, d:getElementById('list').scrollTop, n:matches(':focus')")
        ed.check_eq(values[0], "kept", "the input's value survived the reload")
        ed.check(abs(values[1] - 40) < 1, f"scrollTop after the reload: {values[1]}")
        ed.check_eq(values[2], True, "the input kept the focus")
        ed.check_eq((await lua(ed, "return script('Menu', 'ReloadProbe').loads"))[0], loads_before + 1, "onLoad ran again")

        # the listener added in init on #go moved to the new #go
        await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": 100, "y": 320}], space="canvas", frames_after=2)
        ed.check_eq(await lua(ed, "return script('Menu', 'ReloadProbe').clicks"), [1], "the listener on #go fires after the reload")
    finally:
        await ed.call("play", action="stop")
        await ed.call("write_file", path=DOC, content=HTML)


async def test_no_errors(ed: Editor):
    entries = await ed.console_since(None, types=["error"], limit=1000)
    errors = [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "") or "Lua" in e.get("message", "")]
    ed.check(not errors, f"errors: {errors}")


# =============================================================================================
# Phase 4 (p4-checks, spec §3.5 and §5.3): the automatic pick-up (McpAutoSync) reloads documents
# before the next MCP call, in edit mode and during play, once (the editor's poller shares the
# stamps), and reports `ui_reloaded`, `ui_current` and the diagnostics as `problems`.
#
# The reloads are counted from the Console lines both reloaders write ("[HTML UI] Restyled
# ui/pick/panel.html in scene ..."): one line per reload of a document, whoever does it, and
# through inspect_ui's document.load_count (p4-mcp-tools).
# =============================================================================================

P = {}   # phase-4 state: scene id, entity ids

PANEL = "assets/ui/pick/panel.html"
PANEL_CSS = "assets/ui/pick/panel.css"
ICON = "assets/ui/pick/icon.png"

# GOOD_HTML of test_html_ui_checks.py (a compat-clean panel) with an image
PANEL_HTML = """<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Panel</title>
  <link rel="stylesheet" href="panel.css">
</head>
<body>
  <div class="panel">
    <h1 class="title">Settings</h1>
    <img class="icon" src="icon.png">
    <div class="actions">
      <button id="ok" class="btn primary">OK</button>
      <button id="cancel" class="btn">Cancel</button>
    </div>
    <ul class="list">
      <li>Audio</li>
      <li>Video</li>
    </ul>
  </div>
</body>
</html>
"""

PANEL_STYLE = """:root { --accent: #3fa9f5; --panel: #1e2430; }
body { margin: 0; font-family: sans-serif; color: #ffffff; }
.panel { position: absolute; left: 40px; top: 40px; width: 420px; padding: clamp(12px, 2vw, 24px);
         background-color: var(--panel); display: flex; flex-direction: column; gap: 12px; }
.title { margin: 0; font-size: 28px; color: #ff0000; }
.title::before { content: "> "; }
.icon { width: 32px; height: 32px; }
.actions { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.btn { padding: 8px 12px; background-color: #39414f; color: #ffffff; text-align: center; }
.btn.primary { background-color: var(--accent); }
"""

PANEL_PROBE = """---@class PanelProbe : ScriptInstance
local PanelProbe = {
    properties = {
        { name = "clicks", displayName = "Clicks", type = "int", default = 0 },
        { name = "loads", displayName = "Loads", type = "int", default = 0 },
    }
}

function PanelProbe:init()
    self.doc = UIDocument(self.scene, self.entity)
    RegisterEvent(self, self.doc.onLoad, "onDocumentLoad")
    self:bind()
end

-- (re)binds #ok once per document: after an HTML reload the new #ok has no data-bound
function PanelProbe:bind()
    local ok = self.doc:getElementById("ok")
    if ok and not ok:hasAttribute("data-bound") then
        ok:setAttribute("data-bound", "1")
        local probe = self
        ok:addEventListener("click", function(event)
            probe.clicks = probe.clicks + 1
            print("panel: ok clicked " .. probe.clicks)
        end)
    end
end

function PanelProbe:onDocumentLoad()
    self.loads = self.loads + 1
    self:bind()
end

return PanelProbe
"""

RELOAD_LINE = re.compile(r"\[HTML UI\] (Reloaded|Restyled) (\S+) in scene")


def tiny_png(rgba=(255, 0, 0, 255)) -> bytes:
    import struct
    import zlib

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(rgba) * 4 for _ in range(4))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


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


def ext_of(r) -> dict | None:
    s = r.structured if isinstance(r.structured, dict) else {}
    return s.get("external_changes")


async def status(ed: Editor):
    r = await ed.call_raw("editor_status")
    ed.check(not r.is_error, r.text[:300])
    return r


class Reloads:
    """Counts the reloads of each document from the Console (both reloaders log one line each)."""

    def __init__(self):
        self.cursor = None
        self.counts: dict[str, int] = {}

    async def update(self, ed: Editor) -> dict[str, int]:
        args = {"limit": 1000, "types": ["info"]}
        if self.cursor is not None:
            args["since"] = self.cursor
        result = await ed.call("read_console", args)
        self.cursor = result["next_cursor"]
        for entry in result["entries"]:
            m = RELOAD_LINE.search(entry.get("message", ""))
            if m:
                self.counts[m.group(2)] = self.counts.get(m.group(2), 0) + 1
        return self.counts

    async def of(self, ed: Editor, asset_path: str) -> int:
        return (await self.update(ed)).get(asset_path, 0)


RELOADS = Reloads()


async def load_count(ed: Editor, entity: str) -> int:
    """inspect_ui's document.load_count."""
    r = await ed.call("inspect_ui", scene=P["scene"], entity=entity, depth=0, limit=1)
    return r["document"]["load_count"]


async def title_color(ed: Editor, entity: str = "Panel", play: bool = False):
    """The computed color of .title of the entity's live document, through Lua."""
    code = (f"local d = UIDocument(scene, entity('{entity}')); local t = d:querySelector('.title'); "
            "return t and t.style:getPropertyValue('color') or 'none'")
    args = {"code": code} if play else {"code": code, "allow_edit_mode": True, "scene": P["scene"]}
    r = await ed.call_raw("eval_lua", args)
    ed.check(not r.is_error, f"eval_lua: {r.text[:500]}")
    return r, r.structured["values"][0]


def rgb(value: str) -> tuple:
    """(r, g, b) of a computed color: rgb()/rgba() with any spacing, or #rrggbb."""
    value = (value or "").strip()
    if value.startswith("#") and len(value) >= 7:
        return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))
    numbers = re.findall(r"[\d.]+", value)
    return tuple(int(round(float(n))) for n in numbers[:3]) if len(numbers) >= 3 else ()


def reloaded_htmls(ext: dict | None) -> list[str]:
    return [item["html"] for item in (ext or {}).get("ui_reloaded", [])]


async def test_p4_setup(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "set_properties", "eval_lua", "read_console", "write_file")
    disk_write(ed, PANEL, PANEL_HTML)
    disk_write(ed, PANEL_CSS, PANEL_STYLE)
    disk_write(ed, ICON, tiny_png())
    made = await ed.call("create_scene", name="HtmlUiPick", type="2D")
    P["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=P["scene"], type="ui_document", name="Panel")
    P["panel"] = ent["entity_id"]
    await ed.call("set_properties", scene=P["scene"], entity=P["panel"], values={"uidocument": {"filename": PANEL}})
    await ed.call("create_script", kind="lua", name="PanelProbe", content=PANEL_PROBE, scene=P["scene"], entity=P["panel"])
    await ed.call("save_scene", scene=P["scene"], path="scenes/HtmlUiPick")
    _, color = await title_color(ed)
    ed.check_eq(rgb(color), (255, 0, 0), f"the panel loaded with a red title ({color})")
    await status(ed)
    await RELOADS.update(ed)


async def test_tracked_extensions(ed: Editor):
    font = (REPO / "libs/imgui/misc/fonts/Karla-Regular.ttf")
    files = {
        "assets/ui/pick/extra/new.html": "<html><body><p>new</p></body></html>\n",
        "assets/ui/pick/extra/new.css": ".x { color: red; }\n",
        "assets/ui/pick/extra/font.ttf": font.read_bytes() if font.exists() else b"\x00\x01\x00\x00",
        "assets/ui/pick/extra/data.json": '{"a": 1}\n',
        "assets/ui/pick/extra/anim.lottie": b"PK\x03\x04lottie",
        "assets/ui/pick/extra/icon.svg": '<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><rect width="8" height="8"/></svg>\n',
        "assets/ui/pick/extra/page.htm": "<html><body></body></html>\n",
    }
    for rel, content in files.items():
        disk_write(ed, rel, content)
    disk_write(ed, "assets/build/skipped.css", ".x { zoom: 2 }\n")
    disk_write(ed, "build/skipped.css", ".x { zoom: 2 }\n")
    r = await status(ed)
    ext = ext_of(r)
    ed.check(ext is not None, f"the new files are reported: {r.text[-400:]}")
    for rel in files:
        ed.check_in(rel, ext["changed"], "tracked")
    ed.check(not any("skipped.css" in p for p in ext["changed"]), f"a .css under build/ is not tracked: {ext['changed']}")
    # modifications too, one type at a time
    for rel, content in files.items():
        disk_write(ed, rel, content + (b"\n" if isinstance(content, bytes) else "\n"))
        ext = ext_of(await status(ed))
        ed.check(ext is not None and ext["changed"] == [rel], f"a modified {Path(rel).suffix}: {ext}")
    shutil.rmtree(ed.project / "assets/ui/pick/extra")
    ext = ext_of(await status(ed))
    ed.check(ext is not None and sorted(ext.get("removed", [])) == sorted(files), f"removed: {ext}")


async def test_css_edit_reloads_before_call(ed: Editor):
    before = await RELOADS.of(ed, "ui/pick/panel.html")
    count_before = await load_count(ed, "Panel")
    # the editor's poller may see a write first (its 0.25 s tick): then the document is current,
    # not reloaded by this call. Retry a few times to see the auto-sync do it.
    for attempt, color in enumerate(("#00ff00", "#00ff01", "#00ff02")):
        disk_write(ed, PANEL_CSS, PANEL_STYLE.replace("#ff0000", color))
        r, value = await title_color(ed)
        ext = ext_of(r)
        ed.check(ext is not None and PANEL_CSS in ext["changed"], f"the css edit is reported: {r.text[-400:]}")
        ed.check_eq(rgb(value), rgb(color), f"the call already sees the new color ({value})")
        if ext.get("ui_reloaded"):
            break
        ed.check_eq(ext.get("ui_current"), 1, f"not reloaded by the call: the poller was first: {ext}")
    ed.check_eq(reloaded_htmls(ext), [PANEL], f"ui_reloaded lists the document: {ext}")
    item = ext["ui_reloaded"][0]
    ed.check_eq((item["stylesheet_only"], item["scene"], item["entity"]["name"]), (True, P["scene"], "Panel"), f"{item}")
    ed.check(f'Reloaded UI: {PANEL} in "Panel" (styles only)' in r.text, f"the text line: {r.text[-400:]}")
    ed.check_eq(await RELOADS.of(ed, "ui/pick/panel.html"), before + attempt + 1, "one reload per edit")
    ed.check_eq(await load_count(ed, "Panel"), count_before + attempt + 1, "load_count")
    P["last_color"] = color


async def test_no_double_reload_autosync_first(ed: Editor):
    before = await RELOADS.of(ed, "ui/pick/panel.html")
    count_before = await load_count(ed, "Panel")
    await asyncio.sleep(2.5)   # longer than the poller's interval
    r = await status(ed)
    ed.check(ext_of(r) is None, f"nothing new: {ext_of(r)}")
    ed.check_eq(await RELOADS.of(ed, "ui/pick/panel.html"), before, "the poller did not reload it again")
    ed.check_eq(await load_count(ed, "Panel"), count_before, "load_count unchanged")


async def test_no_double_reload_poller_first(ed: Editor):
    before = await RELOADS.of(ed, "ui/pick/panel.html")
    count_before = await load_count(ed, "Panel")
    disk_write(ed, PANEL_CSS, PANEL_STYLE.replace("#ff0000", "#0000ff"))
    await asyncio.sleep(2.5)   # the poller reloads it
    r, value = await title_color(ed)
    ext = ext_of(r)
    ed.check(ext is not None and PANEL_CSS in ext["changed"], f"the edit is still reported: {r.text[-300:]}")
    ed.check_eq(ext.get("ui_reloaded", []), [], "not reloaded again")
    ed.check_eq(ext.get("ui_current"), 1, "one document was already current")
    ed.check("1 document(s) were already current" in r.text, f"the text says so: {r.text[-300:]}")
    ed.check_eq(rgb(value), (0, 0, 255), f"the new color: {value}")
    ed.check_eq(await RELOADS.of(ed, "ui/pick/panel.html"), before + 1, "reloaded once in total")
    ed.check_eq(await load_count(ed, "Panel"), count_before + 1, "load_count +1 in total")


BROKEN_PANEL = """<html>
<head>
  <link rel="stylesheet" href="panel.css">
</head>
<body>
  <div class="panel"><p class="title">Settings
  <script>document.querySelector('.title').textContent = 'x';</script>
  <button id="ok" class="btn" onclick="ok()">OK</button>
</body>
"""


async def test_html_error_reported(ed: Editor):
    disk_write(ed, PANEL, BROKEN_PANEL)
    r = await status(ed)
    ext = ext_of(r)
    ed.check(ext is not None and reloaded_htmls(ext) == [PANEL], f"the html change reloads the document: {ext}")
    problems = [p for p in ext["problems"] if p["path"] == PANEL]
    script = [p for p in problems if p.get("code") == "HTMLUI-HTML-0006"]
    ed.check(script, f"the <script> is a problem: {ext['problems']}")
    p = script[0]
    ed.check_eq(set(p), {"path", "line", "col", "severity", "code", "message", "suggestion"}, f"problem keys: {p}")
    ed.check_eq((p["line"], p["col"], p["severity"]), (7, 3, "error"), f"where: {p}")
    ed.check(p["suggestion"], "with a fix")
    ed.check(any(q.get("code") == "HTMLUI-HTML-0005" for q in problems), f"onclick too: {problems}")
    ed.check(f"{PANEL}:7: " in r.text, f"the text line names file:line: {r.text[-600:]}")
    # a linked stylesheet that does not exist: an error at its <link>
    disk_write(ed, PANEL, BROKEN_PANEL.replace("panel.css", "nowhere.css").replace("<script>", "<!--").replace("</script>", "-->"))
    ext = ext_of(await status(ed))
    missing = [p for p in ext["problems"] if p.get("code") == "HTMLUI-GEN-0001"]
    ed.check(missing and missing[0]["line"] == 3 and missing[0]["severity"] == "error", f"missing css: {ext['problems']}")
    # fixed: no problems
    disk_write(ed, PANEL, PANEL_HTML)
    ext = ext_of(await status(ed))
    ed.check_eq((reloaded_htmls(ext), ext["problems"]), ([PANEL], []), "fixed")


async def test_orphan_css_checked(ed: Editor):
    disk_write(ed, "assets/ui/pick/orphan.css", ".lonely { color: red; }\n.lonely { mix-blend-mode: multiply; }\n")
    r = await status(ed)
    ext = ext_of(r)
    ed.check(ext is not None, "reported")
    ed.check_eq(ext.get("ui_reloaded", []), [], "no document uses it")
    ed.check_eq([(p["path"], p["line"], p["severity"], p["code"]) for p in ext["problems"]],
                [("assets/ui/pick/orphan.css", 2, "warning", "HTMLUI-CSS-0002")], "checked on its own")
    ed.check("assets/ui/pick/orphan.css:2: warning: 'mix-blend-mode'" in r.text and "[HTMLUI-CSS-0002]" in r.text,
             f"a warning prints as path:line: warning: message [code]: {r.text[-400:]}")
    (ed.project / "assets/ui/pick/orphan.css").unlink()
    await status(ed)


async def ok_center(ed: Editor) -> tuple[float, float]:
    values = (await ed.call("eval_lua", code=(
        "local r = UIDocument(scene, entity('Panel')):getElementById('ok'):getBoundingClientRect() "
        "return r.x + r.width / 2, r.y + r.height / 2")))["values"]
    return values[0], values[1]


async def clicks(ed: Editor) -> int:
    return (await ed.call("eval_lua", code="return script('Panel', 'PanelProbe').clicks"))["values"][0]


async def click_ok(ed: Editor) -> int:
    before = await clicks(ed)
    x, y = await ok_center(ed)
    await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": x, "y": y}], space="canvas", frames_after=2)
    return (await clicks(ed)) - before


async def test_reload_during_play(ed: Editor):
    ed.require_tools("play", "send_input")
    before = await RELOADS.of(ed, "ui/pick/panel.html")
    await ed.call("play", action="start", scene=P["scene"])
    try:
        ed.check_eq(await click_ok(ed), 1, "the listener works before any reload")
        disk_write(ed, PANEL_CSS, PANEL_STYLE.replace("#ff0000", "#ffff00"))
        r, value = await title_color(ed, play=True)
        ext = ext_of(r)
        ed.check(ext is not None and PANEL_CSS in ext["changed"], f"reported during play: {r.text[-300:]}")
        ed.check("deferred" not in ext, f"a UI file is not deferred: {ext}")
        ed.check(reloaded_htmls(ext) == [PANEL] or ext.get("ui_current") == 1, f"reloaded during play: {ext}")
        ed.check_eq(rgb(value), (255, 255, 0), f"the running document restyled: {value}")
        ed.check_eq(await click_ok(ed), 1, "the Lua listener still works after a css-only reload")

        loads = (await ed.call("eval_lua", code="return script('Panel', 'PanelProbe').loads"))["values"][0]
        disk_write(ed, PANEL, PANEL_HTML.replace("<li>Video</li>", "<li>Video</li>\n      <li>Controls</li>"))
        r = await ed.call_raw("eval_lua", {"code": "return #UIDocument(scene, entity('Panel')):querySelectorAll('li')"})
        ext = ext_of(r)
        ed.check(ext is not None and "deferred" not in ext, f"html during play: {ext}")
        ed.check_eq(r.structured["values"][0], 3, "the running document has the new markup")
        ed.check((await ed.call("eval_lua", code="return script('Panel', 'PanelProbe').loads"))["values"][0] > loads,
                 "onLoad ran after the html reload")
        ed.check(await click_ok(ed) >= 1, "a click on the reloaded #ok reaches the Lua listener")
        logged = await ed.console_since(None, limit=1000)
        ed.check(any("panel: ok clicked" in e.get("message", "") for e in logged), "the listener printed")
        during = await RELOADS.of(ed, "ui/pick/panel.html")
        ed.check_eq(during, before + 2, "two reloads during play (css, html)")
    finally:
        await ed.call("play", action="stop")
    await asyncio.sleep(0.6)
    r = await status(ed)
    ed.check(ext_of(r) is None or not ext_of(r).get("ui_reloaded"), f"stop: no second reload for the same changes: {ext_of(r)}")
    ed.check_eq(await RELOADS.of(ed, "ui/pick/panel.html"), during, "the edit-time document is not reloaded again")
    _, value = await title_color(ed)
    ed.check_eq(rgb(value), (255, 255, 0), f"the edit-time document shows the edit: {value}")


async def test_mcp_write_not_external(ed: Editor):
    before = await RELOADS.of(ed, "ui/pick/panel.html")
    count_before = await load_count(ed, "Panel")
    r = await ed.call_raw("write_file", {"path": PANEL_CSS, "content": PANEL_STYLE.replace("#ff0000", "#ff00ff")})
    ed.check(not r.is_error and ext_of(r) is None, f"write_file: {r.text[:300]}")
    r, value = await title_color(ed)
    ed.check(ext_of(r) is None, f"an MCP write is not external: {ext_of(r)}")
    ed.check_eq(rgb(value), (255, 0, 255), f"reloaded by write_file: {value}")
    await asyncio.sleep(0.6)
    ed.check_eq(await RELOADS.of(ed, "ui/pick/panel.html"), before + 1, "reloaded once")
    ed.check_eq(await load_count(ed, "Panel"), count_before + 1, "load_count +1")


async def test_image_dependency_reload(ed: Editor):
    ed.require_tools("sync_files")
    before = await RELOADS.of(ed, "ui/pick/panel.html")
    disk_write(ed, ICON, tiny_png((0, 255, 0, 255)))
    r = await status(ed)
    ext = ext_of(r)
    ed.check(ext is not None and ICON in ext["changed"], f"the png is reported: {ext}")
    ed.check(reloaded_htmls(ext) == [PANEL] or ext.get("ui_current") == 1, f"the document using it reloads: {ext}")
    ed.check_eq(await RELOADS.of(ed, "ui/pick/panel.html"), before + 1, "once")
    # a scene texture using the same png still reloads (sync_files counts it)
    sprite = await ed.call("create_entity", scene=P["scene"], type="image", name="IconImage")
    await ed.call("set_properties", scene=P["scene"], entity=sprite["entity_id"], values={"ui": {"texture": ICON}})
    await ed.call("screenshot", view="scene", scene=P["scene"], max_width=256)
    disk_write(ed, ICON, tiny_png((0, 0, 255, 255)))
    r = await ed.call("sync_files", paths=[ICON])
    ed.check(r.get("textures_reloaded", 0) >= 1, f"the scene texture reloads as before: {r}")
    ed.check(reloaded_htmls({"ui_reloaded": r.get("ui_reloaded", [])}) == [PANEL] or r.get("ui_current") == 1,
             f"and the document: {r}")
    await ed.call("delete_entities", scene=P["scene"], entities=[sprite["entity_id"]])


async def test_sync_files_reports_ui(ed: Editor):
    disk_write(ed, PANEL_CSS, PANEL_STYLE + ".panel { mix-blend-mode: screen; }\n")
    r = await ed.call("sync_files", paths=[PANEL_CSS])
    ed.check(reloaded_htmls({"ui_reloaded": r.get("ui_reloaded", [])}) == [PANEL] or r.get("ui_current") == 1,
             f"sync_files reloads the document: {r}")
    problems = [p for p in r["problems"] if p.get("code") == "HTMLUI-CSS-0002"]
    ed.check(problems and problems[0]["path"] == PANEL_CSS and problems[0]["severity"] == "warning", f"and reports: {r['problems']}")
    r = await status(ed)
    ed.check(ext_of(r) is None, f"sync_files' pick-up is not reported again: {ext_of(r)}")


async def test_read_write_file_html(ed: Editor):
    r = await ed.call_raw("read_file", {"path": PANEL})
    ed.check(not r.is_error and "<html" in r.text, f"read_file reads .html: {r.text[:200]}")
    r = await ed.call_raw("read_file", {"path": PANEL_CSS})
    ed.check(not r.is_error and ".panel" in r.text, f"read_file reads .css: {r.text[:200]}")
    await ed.call("write_file", path="assets/ui/pick/written.htm", content="<html><body><p>x</p></body></html>\n")
    ed.check((ed.project / "assets/ui/pick/written.htm").exists(), "write_file writes .htm")
    await ed.call("write_file", path=PANEL_CSS, content=PANEL_STYLE)


# ---- edge cases

async def test_many_documents_share_a_stylesheet(ed: Editor):
    disk_write(ed, "assets/ui/pick/many.css", ".title { color: #808080; }\n")
    names = []
    for i in range(10):
        rel = f"assets/ui/pick/many{i}.html"
        disk_write(ed, rel, f'<html><head><link rel="stylesheet" href="many.css"></head><body><p class="title">{i}</p></body></html>\n')
        ent = await ed.call("create_entity", scene=P["scene"], type="ui_document", name=f"Many{i}")
        await ed.call("set_properties", scene=P["scene"], entity=ent["entity_id"], values={"uidocument": {"filename": rel, "sortOrder": i}})
        names.append(rel)
    await status(ed)
    for i in range(10):
        await title_color(ed, f"Many{i}")
    before = dict(await RELOADS.update(ed))
    disk_write(ed, "assets/ui/pick/many.css", ".title { color: #ff8000; }\n")
    r = await status(ed)
    ext = ext_of(r)
    reloaded = sorted(reloaded_htmls(ext))
    ed.check_eq(len(reloaded) + ext.get("ui_current", 0), 10, f"all ten documents: {ext}")
    ed.check("and 6 more" in r.text or ext.get("ui_current"), f"the text lists four and counts the rest: {r.text[-500:]}")
    for i in range(10):
        _, value = await title_color(ed, f"Many{i}")
        ed.check_eq(rgb(value), (255, 128, 0), f"Many{i}: {value}")
    await asyncio.sleep(0.6)
    after = await RELOADS.update(ed)
    for i in range(10):
        key = f"ui/pick/many{i}.html"
        ed.check_eq(after.get(key, 0) - before.get(key, 0), 1, f"{key} reloaded once")


async def test_unicode_paths(ed: Editor):
    html, css = "assets/ui/pick/ñandú dir/menú.html", "assets/ui/pick/ñandú dir/estilo é.css"
    disk_write(ed, css, ".title { color: #102030; }\n")
    disk_write(ed, html, '<html><head><link rel="stylesheet" href="estilo é.css"></head><body><p class="title">ü</p></body></html>\n')
    ent = await ed.call("create_entity", scene=P["scene"], type="ui_document", name="Unicode")
    await ed.call("set_properties", scene=P["scene"], entity=ent["entity_id"], values={"uidocument": {"filename": html}})
    await status(ed)
    _, value = await title_color(ed, "Unicode")
    ed.check_eq(rgb(value), (16, 32, 48), f"loaded: {value}")
    disk_write(ed, css, ".title { color: #405060; }\n.title { zoom: 2; }\n")
    r = await status(ed)
    ext = ext_of(r)
    ed.check(css in ext["changed"], f"the unicode css is tracked: {ext}")
    ed.check(html in reloaded_htmls(ext) or ext.get("ui_current"), f"its document reloads: {ext}")
    ed.check_eq([(p["path"], p["line"], p["code"]) for p in ext["problems"]], [(css, 2, "HTMLUI-CSS-0002")], "located in the unicode file")
    _, value = await title_color(ed, "Unicode")
    ed.check_eq(rgb(value), (64, 80, 96), f"restyled: {value}")


async def test_texture_mode_document(ed: Editor):
    html = "assets/ui/pick/screen3d.html"
    disk_write(ed, html, '<html><head><link rel="stylesheet" href="panel.css"></head><body><p class="title">3D</p></body></html>\n')
    ent = await ed.call("create_entity", scene=P["scene"], type="ui_document", name="Screen3D")
    await ed.call("set_properties", scene=P["scene"], entity=ent["entity_id"],
                  values={"uidocument": {"filename": html, "renderMode": "texture", "textureWidth": 256, "textureHeight": 128}})
    await status(ed)
    _, value = await title_color(ed, "Screen3D")
    ed.check(value != "none", f"the texture-mode document loaded: {value}")
    disk_write(ed, PANEL_CSS, PANEL_STYLE.replace("#ff0000", "#123456"))
    r = await status(ed)
    ext = ext_of(r)
    both = set(reloaded_htmls(ext))
    ed.check(html in both or ext.get("ui_current"), f"the texture-mode document reloads with the screen one: {ext}")
    _, value = await title_color(ed, "Screen3D")
    ed.check_eq(rgb(value), (18, 52, 86), f"the texture document restyled: {value}")


async def test_rapid_saves_during_play(ed: Editor):
    ed.require_tools("play")
    console = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", scene=P["scene"])
    try:
        # a save before each call: each call sees its own save
        for i in range(6):
            color = f"#{i * 40:02x}ff{i * 40:02x}"
            disk_write(ed, PANEL_CSS, PANEL_STYLE.replace("#ff0000", color))
            _, value = await title_color(ed, play=True)
            ed.check_eq(rgb(value), (i * 40, 255, i * 40), f"save {i} ({value})")
        # a burst of saves between two calls: the last one wins, the document reloads once for it
        before = await RELOADS.of(ed, "ui/pick/panel.html")
        for i in range(8):
            path = ed.project / PANEL_CSS
            path.write_text(PANEL_STYLE.replace("#ff0000", f"#0{i}0{i}0{i}"), encoding="utf-8")
        time.sleep(0.02)
        r, value = await title_color(ed, play=True)
        ed.check_eq(rgb(value), (7, 7, 7), f"the last save: {value}")
        await asyncio.sleep(0.6)
        after = await RELOADS.of(ed, "ui/pick/panel.html")
        ed.check(after - before <= 2, f"a burst reloads at most once per reloader tick, not per save: {after - before}")
        r = await status(ed)
        ed.check(ext_of(r) is None, f"nothing left: {ext_of(r)}")
        logged = (await ed.call("read_console", since=console, types=["error"], limit=1000))["entries"]
        errors = [e["message"] for e in logged if "panel" in e.get("message", "")]
        ed.check(not errors, f"no errors: {errors}")
    finally:
        await ed.call("play", action="stop")


async def test_delete_document_file_during_play(ed: Editor):
    ed.require_tools("play")
    html = "assets/ui/pick/gone.html"
    disk_write(ed, html, '<html><body><p class="title">here</p></body></html>\n')
    ent = await ed.call("create_entity", scene=P["scene"], type="ui_document", name="Gone")
    await ed.call("set_properties", scene=P["scene"], entity=ent["entity_id"], values={"uidocument": {"filename": html}})
    await status(ed)
    await ed.call("play", action="start", scene=P["scene"])
    try:
        # (an edit within a few ms of a writing call's end is taken as that call's: the settle scan)
        time.sleep(0.05)
        (ed.project / html).unlink()
        r = await status(ed)
        ext = ext_of(r)
        ed.check(ext is not None and html in ext.get("removed", []), f"the deletion is reported: {ext}")
        missing = [p for p in ext["problems"] if p["path"] == html]
        ed.check(missing and missing[0]["severity"] == "error" and missing[0]["code"] == "HTMLUI-GEN-0001",
                 f"the document's missing file is a problem: {ext['problems']}")
        await ed.check_alive()
        values = (await ed.call("eval_lua", code="return UIDocument(scene, entity('Gone')).loaded"))["values"]
        ed.check_eq(values, [False], "the document is unloaded")
        disk_write(ed, html, '<html><body><p class="title">back</p></body></html>\n')
        r = await status(ed)
        ext = ext_of(r)
        ed.check(ext is not None and html in ext["changed"], f"restored: {ext}")
        ed.check_eq([p for p in ext["problems"] if p["path"] == html], [], "no problem once it is back")
        values = (await ed.call("eval_lua", code=(
            "local d = UIDocument(scene, entity('Gone')); local t = d:querySelector('.title'); return d.loaded, t and t.textContent")))["values"]
        ed.check_eq(values, [True, "back"], "the document loads again")
    finally:
        await ed.call("play", action="stop")


async def test_p4_no_crash(ed: Editor):
    await ed.check_alive()
    r = await status(ed)
    ed.check(not r.is_error, "the editor still answers")
