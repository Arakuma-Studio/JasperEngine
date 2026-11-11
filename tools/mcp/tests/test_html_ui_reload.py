"""External HTML UI edits through the MCP autosync boundary.

The documented contract in docs/mcp/DESIGN.md and docs/mcp/guide/html-ui.md requires a call
following a saved UI file to see the latest document, report located diagnostics, and share
reload stamps with the editor poller. Core DOM, form-state, font and registered-property
reload behavior is owned by the engine's direct UIDocumentSystem tests. An HTML edit patches the
live document (S17): its onReload runs instead of onLoad (the probe rebinds from either).
"""

import asyncio
import os
import random
import re
import shutil
import time
from pathlib import Path

from harness import REPO, Editor
from htmlui_pixels import Shot, check_color

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}
CI = True

TIMEOUT = 900

# =============================================================================================
# Phase 4 (p4-checks, spec §3.5 and §5.3): the automatic pick-up (McpAutoSync) reloads documents
# before the next MCP call, in edit mode and during play, once (the editor's poller shares the
# stamps), and reports `ui_reloaded`, `ui_current` and the diagnostics as `problems`.
#
# Reload counts use the public inspect_ui document.load_count and Lua onLoad callbacks;
# diagnostic log wording is not a behavioral contract.
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
    self.onClick = function(event) self.clicks = self.clicks + 1 end
    -- S17: an HTML edit patches the live document: onReload instead of onLoad
    self.doc.onReload = function(report) self:onDocumentLoad() end
    self:bind()
end

-- Id-ed listeners move to the new element on reload; adding the same function is ignored.
-- Reuse the function, as the public reload guide requires; attributes are reset by a reload.
function PanelProbe:bind()
    local ok = self.doc:getElementById("ok")
    if ok then ok:addEventListener("click", self.onClick) end
end

function PanelProbe:onDocumentLoad()
    self.loads = self.loads + 1
    self:bind()
end

return PanelProbe
"""



def tiny_png(rgba=(255, 0, 0, 255)) -> bytes:
    import struct
    import zlib

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(rgba) * 4 for _ in range(4))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def disk_write(ed: Editor, rel: str, text) -> Path:
    """An external edit (the agent's own Write tool), outside the previous call's stamp window."""
    # McpAutoSync::afterCall allows 15 ms of coarse-clock slack (end + 3 ms). An mtime
    # increment alone can still be attributed to the previous writing call instead of picked up.
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


def reloaded_for_call(ext: dict | None, html: str) -> bool:
    """The edit reloaded the document before the call: the pick-up did (ui_reloaded), or the editor's
    poller (0.25 s tick) saw the write first and the pick-up found it current (ui_current, 5.3 case 4)."""
    return reloaded_htmls(ext) == [html] or (ext is not None and not ext.get("ui_reloaded") and ext.get("ui_current") == 1)


async def _setup_panel(ed: Editor):
    if "scene" in P:
        return
    ed.require_tools("create_scene", "create_entity", "set_properties", "eval_lua", "write_file")
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


async def test_tracked_extensions(ed: Editor):
    await _setup_panel(ed)
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


def _webfont_bytes(extension: str) -> bytes:
    return (REPO / "engine/tests/htmlui/fixtures/fonts/webfonts" / f"webfont.{extension}").read_bytes()


def _webfont_version_edit(extension: str, source: bytes) -> bytes:
    edited = bytearray(source)
    # Change the container's majorVersion, preserving all tables and checksums.
    edited[21 if extension == "woff" else 25] ^= 1
    return bytes(edited)


async def test_webfont_edits_are_tracked(ed: Editor):
    await status(ed)
    files = {f"assets/ui/webfont-tracking/font.{extension}": _webfont_bytes(extension) for extension in ("woff", "woff2")}
    for rel, source in files.items():
        disk_write(ed, rel, source)
    ext = ext_of(await status(ed))
    ed.check(ext is not None and all(rel in ext.get("changed", []) for rel in files), f"webfont additions are tracked: {ext}")
    for extension in ("woff", "woff2"):
        rel = f"assets/ui/webfont-tracking/font.{extension}"
        disk_write(ed, rel, _webfont_version_edit(extension, files[rel]))
        ext = ext_of(await status(ed))
        ed.check(ext is not None and rel in ext.get("changed", []), f"a {extension} save is tracked: {ext}")


async def test_webfont_sync_files_reloads_documents(ed: Editor):
    for extension in ("woff", "woff2"):
        font = f"assets/ui/webfont-reload/font.{extension}"
        html = f"assets/ui/webfont-reload/{extension}.html"
        source = _webfont_bytes(extension)
        disk_write(ed, font, source)
        disk_write(ed, html, "<html><head><style>"
                   f"@font-face {{font-family: WebReload{extension}; src: url(font.{extension}) format('{extension}');}}"
                   f"body {{font-family: WebReload{extension};}}"
                   "</style></head><body><p>Webfont reload</p></body></html>")
        made = await ed.call("create_ui_document", html=html, name=f"WebReload{extension}")
        ed.check(not [d for d in made["diagnostics"] if d.get("severity") == "error"], f"valid {extension} fixture loads: {made}")
        entity = made["entity"]["id"]
        before = (await ed.call("inspect_ui", entity=entity, depth=0, limit=1))["document"]["load_count"]
        await status(ed)
        disk_write(ed, font, _webfont_version_edit(extension, source))
        result = await ed.call("sync_files", paths=[font], full=False)
        ed.check(any(r.get("html") == html for r in result.get("ui_reloaded", [])) or result.get("ui_current", 0) >= 1,
                 f"sync_files dispatches the changed {extension} dependency to HTML UI: {result}")
        after = (await ed.call("inspect_ui", entity=entity, depth=0, limit=1))["document"]["load_count"]
        ed.check_eq(after, before + 1, f"the document reloads once after the {extension} save")


async def test_css_edit_reloads_before_call(ed: Editor):
    await _setup_panel(ed)
    count_before = await load_count(ed, "Panel")
    disk_write(ed, PANEL_CSS, PANEL_STYLE.replace("#ff0000", "#00ff00"))
    r, value = await title_color(ed)
    ext = ext_of(r)
    ed.check(ext is not None and PANEL_CSS in ext["changed"], f"the css edit is reported: {r.text[-400:]}")
    ed.check_eq(rgb(value), (0, 255, 0), f"the call already sees the new color ({value})")
    # The poller and MCP share stamps. Either may win; both must expose the latest file once.
    ed.check(reloaded_for_call(ext, PANEL), f"the document is current before the call: {ext}")
    if ext.get("ui_reloaded"):
        item = ext["ui_reloaded"][0]
        ed.check_eq((item["stylesheet_only"], item["scene"], item["entity"]["name"]),
                    (True, P["scene"], "Panel"), f"{item}")
    ed.check_eq(await load_count(ed, "Panel"), count_before + 1, "load_count")


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
    await _setup_panel(ed)
    reloads = await load_count(ed, "Panel")
    disk_write(ed, PANEL, BROKEN_PANEL)
    r = await status(ed)
    ext = ext_of(r)
    ed.check(reloaded_for_call(ext, PANEL), f"the html change reloads the document: {ext}")
    ed.check_eq(await load_count(ed, "Panel"), reloads + 1, "reloaded once")
    problems = [p for p in ext["problems"] if p["path"] == PANEL]
    script = [p for p in problems if p.get("code") == "HTMLUI-HTML-0006"]
    ed.check(script, f"the <script> is a problem: {ext['problems']}")
    p = script[0]
    ed.check({"path", "line", "col", "severity", "code", "message", "suggestion"} <= set(p), f"problem keys: {p}")
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
    reloads = await load_count(ed, "Panel")
    disk_write(ed, PANEL, PANEL_HTML)
    ext = ext_of(await status(ed))
    ed.check(reloaded_for_call(ext, PANEL), f"the fix reloads the document: {ext}")
    ed.check_eq(ext["problems"], [], "fixed")
    ed.check_eq(await load_count(ed, "Panel"), reloads + 1, "reloaded once")


async def test_orphan_css_checked(ed: Editor):
    await _setup_panel(ed)
    disk_write(ed, "assets/ui/pick/orphan.css", ".lonely { color: red; }\n.lonely { mix-blend-mode: multiply; }\n")
    r = await status(ed)
    ext = ext_of(r)
    ed.check(ext is not None, "reported")
    ed.check_eq(ext.get("ui_reloaded", []), [], "no document uses it")
    ed.check_eq([(p["path"], p["line"], p["severity"], p["code"]) for p in ext["problems"]],
                [("assets/ui/pick/orphan.css", 2, "warning", "HTMLUI-CSS-0002")], "checked on its own")
    ed.check("assets/ui/pick/orphan.css:2: warning:" in r.text and "[HTMLUI-CSS-0002]" in r.text,
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
    await _setup_panel(ed)
    ed.require_tools("play", "send_input")
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
        ed.check_eq((await ed.call("eval_lua", code="return script('Panel', 'PanelProbe').loads"))["values"][0], loads + 1,
                    "one load callback for the HTML reload (onReload: a patch, S17)")
        ed.check_eq(await click_ok(ed), 1, "one click reaches the reloaded Lua listener once")
    finally:
        await ed.call("play", action="stop")
    await asyncio.sleep(0.6)
    r = await status(ed)
    ed.check(ext_of(r) is None or not ext_of(r).get("ui_reloaded"), f"stop: no second reload for the same changes: {ext_of(r)}")
    _, value = await title_color(ed)
    ed.check_eq(rgb(value), (255, 255, 0), f"the edit-time document shows the edit: {value}")


async def test_mcp_write_not_external(ed: Editor):
    await _setup_panel(ed)
    count_before = await load_count(ed, "Panel")
    r = await ed.call_raw("write_file", {"path": PANEL_CSS, "content": PANEL_STYLE.replace("#ff0000", "#ff00ff")})
    ed.check(not r.is_error and ext_of(r) is None, f"write_file: {r.text[:300]}")
    r, value = await title_color(ed)
    ed.check(ext_of(r) is None, f"an MCP write is not external: {ext_of(r)}")
    ed.check_eq(rgb(value), (255, 0, 255), f"reloaded by write_file: {value}")
    await asyncio.sleep(0.6)
    ed.check_eq(await load_count(ed, "Panel"), count_before + 1, "load_count +1")


async def test_image_dependency_reload(ed: Editor):
    await _setup_panel(ed)
    ed.require_tools("sync_files")
    async def check_icon(want):
        values = (await ed.call("eval_lua", allow_edit_mode=True, scene=P["scene"], code=(
            "local r = UIDocument(scene, entity('Panel')):querySelector('.icon'):getBoundingClientRect() "
            "return r.x + r.width / 2, r.y + r.height / 2")))["values"]
        shot = await ed.call_raw("screenshot", {"view": "game", "scene": P["scene"], "max_width": 1280})
        ed.check(not shot.is_error and shot.images, "the document's image is rendered")
        check_color(ed, Shot(shot.images[0][0], shot.value), values, want, "image dependency pixels")

    await check_icon((255, 0, 0))
    disk_write(ed, ICON, tiny_png((0, 255, 0, 255)))
    r = await status(ed)
    ext = ext_of(r)
    ed.check(ext is not None and ICON in ext["changed"], f"the png is reported: {ext}")
    ed.check(reloaded_htmls(ext) == [PANEL] or ext.get("ui_current") == 1, f"the document using it reloads: {ext}")
    await check_icon((0, 255, 0))
    # a scene texture using the same png still reloads (sync_files counts it)
    sprite = await ed.call("create_entity", scene=P["scene"], type="image", name="IconImage")
    await ed.call("set_properties", scene=P["scene"], entity=sprite["entity_id"], values={"ui": {"texture": ICON}})
    await ed.call("screenshot", view="scene", scene=P["scene"], max_width=256)
    disk_write(ed, ICON, tiny_png((0, 0, 255, 255)))
    r = await ed.call("sync_files", paths=[ICON])
    ed.check(r.get("textures_reloaded", 0) >= 1, f"the scene texture reloads as before: {r}")
    ed.check(reloaded_htmls({"ui_reloaded": r.get("ui_reloaded", [])}) == [PANEL] or r.get("ui_current") == 1,
             f"and the document: {r}")
    await check_icon((0, 0, 255))
    await ed.call("delete_entities", scene=P["scene"], entities=[sprite["entity_id"]])


async def test_sync_files_reports_ui(ed: Editor):
    await _setup_panel(ed)
    disk_write(ed, PANEL_CSS, PANEL_STYLE + ".panel { mix-blend-mode: screen; }\n")
    r = await ed.call("sync_files", paths=[PANEL_CSS])
    ed.check(reloaded_htmls({"ui_reloaded": r.get("ui_reloaded", [])}) == [PANEL] or r.get("ui_current") == 1,
             f"sync_files reloads the document: {r}")
    problems = [p for p in r["problems"] if p.get("code") == "HTMLUI-CSS-0002"]
    ed.check(problems and problems[0]["path"] == PANEL_CSS and problems[0]["severity"] == "warning", f"and reports: {r['problems']}")
    r = await status(ed)
    ed.check(ext_of(r) is None, f"sync_files' pick-up is not reported again: {ext_of(r)}")


async def test_read_write_file_html(ed: Editor):
    await _setup_panel(ed)
    r = await ed.call_raw("read_file", {"path": PANEL})
    ed.check(not r.is_error and "<html" in r.text, f"read_file reads .html: {r.text[:200]}")
    r = await ed.call_raw("read_file", {"path": PANEL_CSS})
    ed.check(not r.is_error and ".panel" in r.text, f"read_file reads .css: {r.text[:200]}")
    await ed.call("write_file", path="assets/ui/pick/written.htm", content="<html><body><p>x</p></body></html>\n")
    ed.check((ed.project / "assets/ui/pick/written.htm").exists(), "write_file writes .htm")
    await ed.call("write_file", path=PANEL_CSS, content=PANEL_STYLE)


# ---- edge cases

async def test_many_documents_share_a_stylesheet(ed: Editor):
    await _setup_panel(ed)
    disk_write(ed, "assets/ui/pick/many.css", ".title { color: #808080; }\n")
    for i in range(2):
        rel = f"assets/ui/pick/many{i}.html"
        disk_write(ed, rel, f'<html><head><link rel="stylesheet" href="many.css"></head><body><p class="title">{i}</p></body></html>\n')
        ent = await ed.call("create_entity", scene=P["scene"], type="ui_document", name=f"Many{i}")
        await ed.call("set_properties", scene=P["scene"], entity=ent["entity_id"], values={"uidocument": {"filename": rel, "sortOrder": i}})
    await status(ed)
    for i in range(2):
        await title_color(ed, f"Many{i}")
    before = {f"Many{i}": await load_count(ed, f"Many{i}") for i in range(2)}
    disk_write(ed, "assets/ui/pick/many.css", ".title { color: #ff8000; }\n")
    r = await status(ed)
    ext = ext_of(r)
    reloaded = sorted(reloaded_htmls(ext))
    ed.check_eq(len(reloaded) + ext.get("ui_current", 0), 2, f"both documents: {ext}")
    for i in range(2):
        _, value = await title_color(ed, f"Many{i}")
        ed.check_eq(rgb(value), (255, 128, 0), f"Many{i}: {value}")
    for name, count in before.items():
        ed.check_eq(await load_count(ed, name), count + 1, f"{name} refreshed once")


async def test_unicode_paths(ed: Editor):
    await _setup_panel(ed)
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
    await _setup_panel(ed)
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


async def test_burst_after_writing_call_during_play(ed: Editor):
    """S05: the race behind test_rapid_saves_during_play's rare failure, run many times. A save
    right after a writing call is held by the paused reloader (and picked up by the next call);
    the held batch then reloads in the middle of the next burst of saves, reading a half-written
    file. The reload must not take the burst's last save as reloaded: it stamps what the file was
    before it read it, so the burst's own change reloads it. JASPER_BURST_ROUNDS sets the rounds."""
    await _setup_panel(ed)
    ed.require_tools("play")
    rounds = int(os.environ.get("JASPER_BURST_ROUNDS", "40"))
    await ed.call("play", action="start", scene=P["scene"])
    wrong = []
    try:
        for n in range(rounds):
            disk_write(ed, PANEL_CSS, PANEL_STYLE.replace("#ff0000", "#40ff40"))
            await title_color(ed, play=True)
            await ed.call("read_console", limit=1)
            # the held batch is applied by the frame after the call: land the burst around it
            time.sleep(random.random() * 0.25)
            last = n % 9
            for i in range(8):
                (ed.project / PANEL_CSS).write_text(PANEL_STYLE.replace("#ff0000", f"#0{i}0{i}0{last}"), encoding="utf-8")
            time.sleep(0.02)
            _, value = await title_color(ed, play=True)
            if rgb(value) != (7, 7, last):
                wrong.append(f"round {n}: {value}, not #07070{last}")
    finally:
        await ed.call("play", action="stop")
    print(f"     [burst] {len(wrong)} of {rounds} rounds showed an older save")
    ed.check_eq(wrong, [], "every burst's last save is what the document shows")

async def test_delete_document_file_during_play(ed: Editor):
    await _setup_panel(ed)
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
