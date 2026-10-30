"""Executable HTML/CSS UI guide examples and their published tooling boundary.

The authored HTML/CSS/Lua/C++ snippets must check and run through the public APIs. Resource
reads must publish the guide, and LuaLS must accept valid API use and reject documented mistakes.
"""

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

from harness import REPO, Editor, skip

sys.path.insert(0, str(Path(__file__).resolve().parent))
from htmlui_pixels import Shot, near  # noqa: E402
from test_html_ui_checks import check, check_eq, fail, run_json, scratch_project, tiny_png, write  # noqa: E402
from test_stubs import clangd_check, find_tool, lua_workspace, luals_check, wait_tooling  # noqa: E402

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}
CI = True

TIMEOUT = 900

S: dict = {}

GUIDE = REPO / "docs" / "mcp" / "guide" / "html-ui.md"
FIXTURES = REPO / "engine" / "tests" / "htmlui" / "fixtures"

FENCE = re.compile(r"^```(\w*)\s*$")
PATH_LINE = re.compile(r"^`([\w./-]+\.(?:html|css|lua))`:$")

# Wrong Lua an LLM writes from web habits (spec §5.5 case 6), and what lua-language-server must say
BAD_UI_LUA = """---@class BadUI : ScriptInstance
local BadUI = { properties = {} }

function BadUI:init()
    local doc = UIDocument(self.scene, self.entity)
    local el = assert(doc:getElementById("x"))
    local f = function(event) print(event.type) end
    doc:getElementByID("x")
    doc:querySelector(42)
    el:addEventListener("onclick", f)
    anime.animate(el, 5)
    el.innerText = "x"
    document.querySelector("#x")
    el:addEventListener("mouseenter", f)
    el.classList.add("on")
    el.onclick = f
end

return BadUI
"""
BAD_UI_LUA_EXPECTED = {
    8: "undefined-field",       # getElementByID
    9: "param-type-mismatch",   # a number where a selector goes
    10: "param-type-mismatch",  # not a UIEventType
    11: "param-type-mismatch",  # a number where the params table goes
    12: "inject-field",         # innerText is not a UIElement field
    13: "undefined-global",     # document exists only in <script type="text/lua">
    14: "param-type-mismatch",  # mouseenter never reaches documents
    15: "param-type-mismatch",  # a dot call: the string goes where the UIClassList goes
    16: "inject-field",         # onclick is not a UIElement field
}

# The same API used right must check clean, a custom event name with its cast included
GOOD_UI_LUA = """---@class GoodUI : ScriptInstance
local GoodUI = { properties = {} }

function GoodUI:init()
    local doc = UIDocument(self.scene, self.entity)
    local el = assert(doc:getElementById("x"))
    local scored = "scored"
    ---@cast scored UIEventType
    el:addEventListener(scored, function(event) print(event.detail) end)
    el:dispatchEvent("scored", { points = 10 })
    el:addEventListener("transitionend", function(event) print(event.propertyName) end, { once = true })
    for _, d in ipairs(doc:getDiagnostics()) do print(d.code, d.line, d.suggestion) end
    local texture = doc:getTargetTexture()
    if texture then print(texture) end
    doc:setColorScheme("dark")
    el:insertAdjacentHTML("beforeend", "<b>new</b>")
    el.classList:toggle("open", true)
    local model = doc:createDataModel("m", { n = 1 })
    model.n = 2
    local tl = anime.createTimeline({ defaults = { duration = 200 } })
    tl:add(el, { opacity = { from = 0 } }, "+=50"):call(function() print("done") end)
end

return GoodUI
"""


# ---------------------------------------------------------------------------- helpers

def guide_text() -> str:
    return GUIDE.read_text(encoding="utf-8")


def guide_blocks(text: str | None = None) -> list[dict]:
    """Fenced blocks of the guide: {lang, line (of the fence), code, path (or None)}."""
    lines = (text if text is not None else guide_text()).split("\n")
    out = []
    i = 0
    while i < len(lines):
        m = FENCE.match(lines[i])
        if not m or not m.group(1):
            i += 1
            continue
        j = i + 1
        while j < len(lines) and not lines[j].startswith("```"):
            j += 1
        before = [line.strip() for line in lines[:i] if line.strip()]
        pm = PATH_LINE.match(before[-1]) if before else None
        out.append({"lang": m.group(1), "line": i + 1, "code": "\n".join(lines[i + 1:j]) + "\n",
                    "path": pm.group(1) if pm else None})
        i = j + 1
    return out


def named_block(path: str) -> str:
    for b in guide_blocks():
        if b["path"] == path:
            return b["code"]
    fail(f"the guide has no block for {path}")
    return ""


def fragment(lang: str, needle: str) -> str:
    """The guide's fragment block of `lang` that contains `needle`."""
    found = [b["code"] for b in guide_blocks() if b["lang"] == lang and needle in b["code"]]
    if len(found) != 1:
        fail(f"expected one {lang} block of the guide with {needle!r}, found {len(found)}")
    return found[0]


def placeholder_asset(path: Path) -> None:
    """A real file of the kind a fragment refers to (fragments name files the reader provides)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    ext = path.suffix.lower()
    if ext == ".png":
        path.write_bytes(tiny_png())
    elif ext == ".svg":
        path.write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10" '
                        'fill="currentColor"/></svg>\n')
    elif ext in (".json", ".lottie"):
        shutil.copy(FIXTURES / "p1" / "lottie" / "shapes.json", path)
    elif ext in (".webm", ".mkv"):
        # a <video> (phase 6): the AV1 test card
        shutil.copy(FIXTURES / "video" / "testcard.webm", path)
    elif ext in (".ttf", ".otf"):
        shutil.copy(REPO / "libs" / "imgui" / "misc" / "fonts" / "Karla-Regular.ttf", path)
    elif ext in (".md", ".markdown"):
        # a <markdown src> (S17)
        path.write_text("# Notes\n\nSome *text*.\n")
    elif ext == ".frag":
        # a project effect (phase 5): the guide's own text block of that effect
        found = [b["code"] for b in guide_blocks() if b["lang"] == "text" and f"// @effect     {path.stem}\n" in b["code"]]
        if len(found) != 1:
            fail(f"the guide has no effect file for {path.name}")
        path.write_text(found[0])
    else:
        fail(f"a guide snippet refers to {path.name}, a kind no placeholder exists for")


def write_guide_snippets(project: Path) -> list[str]:
    """Every html/css block into the project (named ones at their path); returns the written paths."""
    written = []
    for b in guide_blocks():
        if b["lang"] not in ("html", "css"):
            continue
        rel = b["path"] or f"assets/ui/guide_snippet_{b['line']}.{b['lang']}"
        write(project, rel, b["code"])
        written.append(rel)
    return written


async def lua(ed: Editor, code: str, **args) -> list:
    r = await ed.call("eval_lua", code=code, **args)
    return r["values"]


def prelude(entity: str) -> str:
    """`self` as a script on `entity` of the guide's scene sees it (the guide's fragments use self.scene/self.entity)."""
    return f"local self = {{ scene = getScene('{S['scene_name']}'), entity = entity('{entity}') }}\n"


async def click(ed: Editor, entity: str, selector: str) -> None:
    await ed.call("send_input", events=[{"type": "mouse", "action": "click", "target": selector, "document": entity}],
                  frames_after=3)


async def text_of(ed: Editor, entity: str, selector: str) -> str:
    return (await lua(ed, f"return assert(object('{entity}', 'UIDocument'):querySelector('{selector}')).textContent"))[0]


async def focused_id(ed: Editor) -> str:
    return (await lua(ed, "local f = object('Menu', 'UIDocument'):querySelector(':focus'); return f and f.id or ''"))[0]


async def errors_since(ed: Editor) -> list[str]:
    entries = await ed.console_since(types=["error", "warning"], limit=1000)
    return [e.get("message", "") for e in entries]


# ---------------------------------------------------------------------------- 1. structure (no editor)


# ---------------------------------------------------------------------------- 2. html/css blocks (CLI)

async def test_guide_css_html_snippets_clean():
    with scratch_project() as project:
        shutil.rmtree(project / "assets" / "ui", ignore_errors=True)
        written = write_guide_snippets(project)
        check(len(written) >= 8, f"only {len(written)} html/css blocks")
        # fragments name images, animations and fonts the reader provides: provide real ones
        code, data = run_json(project, "--ui")
        for f in data["findings"]:
            if f.get("code") in ("HTMLUI-GEN-0001", "HTMLUI-FX-0013") and f["path"].startswith("assets/ui/guide_snippet_"):
                m = re.search(r"'([^']+)' not found", f["message"])
                check(m, f"a missing-file finding without the file: {f}")
                placeholder_asset((project / f["path"]).parent / m.group(1))
        code, data = run_json(project, "--ui")
        bad = [f"{f['path']}:{f['line']}:{f['col']}: {f['severity']}: {f['message']} [{f.get('code')}]" for f in data["findings"]]
        check(not bad, "guide snippets with warnings or errors:\n" + "\n".join(bad))
        check_eq((data["errors"], data["warnings"]), (0, 0), "guide snippets are compat-clean")
        check(data["checked"].get("ui", 0) >= 6, f"documents and stylesheets checked: {data['checked']}")


# ---------------------------------------------------------------------------- 3. served

async def read_text(ed: Editor, uri: str) -> str:
    got = await ed.session.read_resource(uri)
    return "".join(getattr(c, "text", "") or "" for c in got.contents)


async def test_guide_served(ed: Editor):
    index = await read_text(ed, "jasper://docs/html-ui")
    sections = [line.split("`")[1] for line in index.splitlines() if line.startswith("- `jasper://docs/html-ui/")]
    ed.check(sections, "the published guide index links its sections")
    listed = {str(r.uri) for r in (await ed.session.list_resources()).resources}
    ed.check(all(uri in listed for uri in sections), "guide section URIs are discoverable")
    served = "\n".join([await read_text(ed, uri) for uri in sections])
    for line in guide_text().splitlines():
        if line.startswith("## "):
            ed.check(line in served, f"the published guide is missing {line}")
    for uri in ("jasper://docs/html-ui-support", "jasper://docs/html-ui-effects"):
        ed.check(await read_text(ed, uri), f"{uri} is readable")
    docs = await read_text(ed, "jasper://docs")
    ed.check("jasper://docs/html-ui" in docs, "the guide is discoverable from the docs index")


# ---------------------------------------------------------------------------- 4. names


# ---------------------------------------------------------------------------- 5. Lua blocks parse

async def test_guide_lua_snippets_parse(ed: Editor):
    blocks = [b for b in guide_blocks() if b["lang"] == "lua"]
    ed.check(len(blocks) >= 6, f"only {len(blocks)} lua blocks")
    for b in blocks:
        # check_script also runs the chunk's top level: a fragment (it uses `self`, `doc`) goes into a
        # function, a complete script is checked as it is
        code = b["code"] if b["path"] else f"local function snippet(self)\n{b['code']}end\nreturn snippet\n"
        r = await ed.call("check_script", code=code, language="lua")
        ed.check(r.get("ok") is True, f"guide line {b['line']}: the Lua block does not parse: {r}")
    # the Lua inside the html blocks (<script type="text/lua">) too
    for b in guide_blocks():
        for code in re.findall(r'<script type="text/lua">\n(.*?)</script>', b["code"], re.S):
            r = await ed.call("check_script", code=f"local function snippet(document)\n{code}end\nreturn snippet\n",
                              language="lua")
            ed.check(r.get("ok") is True, f"guide line {b['line']}: the <script type=\"text/lua\"> does not parse: {r}")


# ---------------------------------------------------------------------------- 6. the snippets in play

TERMINAL_HTML = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Terminal</title>
<style>body { margin: 0; background: #0b1020; color: #7CFC00; font: 24px monospace; }</style></head>
<body><p id="line">READY</p></body></html>
"""


def wrap_page(title: str, body: str, css_link: str = "") -> str:
    link = f'\n  <link rel="stylesheet" href="{css_link}">' if css_link else ""
    return (f'<!DOCTYPE html>\n<html lang="en">\n<head>\n  <meta charset="utf-8">\n  <title>{title}</title>{link}\n'
            f'</head>\n<body>\n{body}</body>\n</html>\n')


async def add_document(ed: Editor, name: str, html: str, sort_order: int, script: str | None = None) -> None:
    made = await ed.call("create_entity", scene=S["scene"], type="ui_document", name=name)
    entity = made["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=entity,
                  values={"uidocument": {"filename": html, "sortOrder": sort_order}})
    if script:
        await ed.call("attach_script", scene=S["scene"], entity=entity, path=script)


async def test_guide_snippets_play(ed: Editor):
    proj = ed.project
    # complete files at their paths; fragments wrapped into pages of their own
    for path in ("assets/ui/menu.html", "assets/ui/menu.css", "assets/ui/hud.html", "assets/ui/hud.css",
                 "scripts/MenuUI.lua", "scripts/HudUI.lua", "scripts/Terminal.lua"):
        write(proj, path, named_block(path))
    write(proj, "assets/ui/terminal.html", TERMINAL_HTML)
    write(proj, "assets/ui/lottie.html", wrap_page("Lottie", fragment("html", "<lottie")))
    placeholder_asset(proj / "assets/ui/anim/loading.json")
    write(proj, "assets/ui/fonts.css", fragment("css", "@font-face"))
    placeholder_asset(proj / "assets/ui/fonts/Title.ttf")
    write(proj, "assets/ui/effects.css", fragment("css", "font-effect"))
    write(proj, "assets/ui/fonts.html", wrap_page("Fonts", '<h1 class="title score ready">Title 123</h1>\n', "fonts.css")
          .replace('</title>', '</title>\n  <link rel="stylesheet" href="effects.css">'))
    write(proj, "assets/ui/script.html", wrap_page("Script", fragment("html", 'type="text/lua"')))
    # phase 6 (p6-accept): the Video fragment (the test card as its files) and the line-clamp box
    write(proj, "assets/ui/video.html", wrap_page("Video", fragment("html", '<video id="intro"')))
    for name in ("intro.webm", "trailer.webm", "trailer.png"):
        placeholder_asset(proj / "assets/ui/video" / name)
    write(proj, "assets/ui/clamp.css", fragment("css", ".item-desc"))
    write(proj, "assets/ui/clamp.html", wrap_page(
        "Clamp", '<p class="item-desc" id="desc" style="width: 160px; font-size: 14px; line-height: 20px">A long item description '
                 'that wraps to many more lines than the two the clamp leaves, the second ending with an ellipsis.</p>\n', "clamp.css"))

    await ed.console_since(None, limit=1)
    made = await ed.call("create_scene", name="GuideUI", type="UI")
    S["scene"] = made["scene"]["id"]
    S["scene_name"] = "GuideUI"
    await ed.call("sync_files")
    await add_document(ed, "Menu", "assets/ui/menu.html", 0, "scripts/MenuUI.lua")
    await add_document(ed, "HUD", "assets/ui/hud.html", 1, "scripts/HudUI.lua")
    await add_document(ed, "Lottie", "assets/ui/lottie.html", -1)
    await add_document(ed, "Fonts", "assets/ui/fonts.html", -2)
    await add_document(ed, "Script", "assets/ui/script.html", -3)
    await add_document(ed, "Terminal", "assets/ui/terminal.html", -4, "scripts/Terminal.lua")
    await add_document(ed, "Video", "assets/ui/video.html", -5)
    await add_document(ed, "Clamp", "assets/ui/clamp.html", -6)
    await ed.call("save_scene", scene=S["scene"], path="scenes/GuideUI")

    # edit mode: the documents load without Lua, with no warning or error
    for name in ("Menu", "HUD", "Lottie", "Fonts", "Script", "Terminal", "Video", "Clamp"):
        diags = await lua(ed, f"""local out = {{}}
            for _, d in ipairs(object('{name}', 'UIDocument'):getDiagnostics()) do
                if d.severity ~= 'info' then out[#out + 1] = d.text end
            end
            return table.concat(out, '\\n'), object('{name}', 'UIDocument').loaded""", allow_edit_mode=True)
        ed.check_eq(diags, ["", True], f"{name}: loads clean in edit mode")

    # fixed_fps + auto_pause: the game runs only inside wait / send_input, each frame exactly 1/60 s,
    # so every check sees the same frame however fast the machine is (the wall clock decides nothing)
    started = await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True)
    ed.check(started, "play started")
    try:
        await play_checks(ed)
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def play_checks(ed: Editor):
    await ed.call("wait", frames=60)

    # the menu: Lucide icons, the entrance animation done, the first button focused
    values = await lua(ed, """local doc = object('Menu', 'UIDocument')
        return #doc:querySelectorAll('.btn'), doc:querySelector('#play .lucide-play') ~= nil,
               doc:querySelector('#quit').style.opacity""")
    ed.check_eq(values, [3, True, "1"], "menu buttons, icons, entrance animation finished")
    ed.check_eq(await focused_id(ed), "play", "the first button has the focus")
    shot = Shot(*(lambda r: (r.images[0][0], r.value))(await ed.call_raw("screenshot", {"view": "game", "max_width": 1280})))
    (ed.scratch.root / "html-ui-docs-menu.png").write_bytes(shot.png)
    ed.check(not near(shot.px(640, 40), (0, 0, 0), 8), f"the menu's page background covers the canvas: {shot.px(640, 40)}")

    # keyboard: ArrowDown moves the focus, Enter clicks the focused button
    await ed.call("send_input", events=[{"type": "key", "key": "down"}], frames_after=3)
    ed.check_eq(await focused_id(ed), "settings", "ArrowDown")
    await ed.call("send_input", events=[{"type": "key", "key": "enter"}], frames_after=3)
    ed.check_eq(await text_of(ed, "Menu", "#status"), "Selected: settings", "Enter clicks the focused button")

    # gamepad: the d-pad and A reach the focused document as ArrowDown and Enter (the script's keydown
    # listener moves the focus, A clicks the focused button)
    await ed.call("send_input", events=[{"type": "gamepad", "button": "dpad_down", "action": "press"},
                                        {"type": "gamepad", "button": "a", "action": "press"}], frames_after=3)
    ed.check_eq(await focused_id(ed), "quit", "gamepad dpad_down")
    ed.check_eq(await text_of(ed, "Menu", "#status"), "Selected: quit", "gamepad A")

    # a click on #play reaches the menu through the HUD document above it (a transparent page)
    await click(ed, "Menu", "#play")
    ed.check_eq(await text_of(ed, "Menu", "#status"), "Selected: play", "click through the transparent HUD page")

    # data binding: {{ }}, data-style-width, data-class-low, data-for, data-event-click
    ed.check_eq(await text_of(ed, "HUD", "#health-label"), "100 / 100", "{{ health }}")
    await lua(ed, "local hud = object('HUD', 'UIDocument'):getDataModel('hud'); hud.health = 25; "
                  "table.insert(hud.log, '-75 HP'); hud.score = 1200")
    await ed.call("wait", frames=30)
    values = await lua(ed, """local doc = object('HUD', 'UIDocument')
        local fill = doc:getElementById('health-fill')
        return doc:getElementById('health-label').textContent, fill.classList:contains('low'),
               math.floor(fill:getBoundingClientRect().width + 0.5), doc:getElementById('score').textContent,
               #doc:querySelectorAll('#log li'), doc:querySelectorAll('#log li')[1].textContent""")
    ed.check_eq(values, ["25 / 100", True, 70, "1200", 1, "-75 HP"], "the HUD follows its model")
    await click(ed, "HUD", "#heal")
    ed.check_eq(await lua(ed, "return object('HUD', 'UIDocument'):getDataModel('hud').health"), [35],
                "data-event-click=\"heal(10)\"")

    # the Lua API fragment, as a script on the menu
    await lua(ed, prelude("Menu") + fragment("lua", 'play.textContent = "Continue"'))
    values = await lua(ed, """local p = object('Menu', 'UIDocument'):getElementById('play')
        return p.textContent, p.classList:contains('highlight'), p:getAttribute('data-level'), p:getAttribute('aria-label')""")
    ed.check_eq(values, ["Continue", True, "3", "Continue the game"], "the element API fragment")

    # the animation fragment: selectors, stagger, a timeline with a label and a call, a plain table
    # (eval_lua captures print while it runs: the callbacks' prints go to a global log instead)
    log = ("_G.guideLog = ''\nlocal print = function(...) "
           "_G.guideLog = _G.guideLog .. table.concat({...}, ' ') .. '\\n' end\n")
    await lua(ed, prelude("Menu") + log + fragment("lua", "anime.createTimeline"))
    await ed.call("wait", frames=150)
    printed = (await lua(ed, "return _G.guideLog"))[0]
    lines = printed.splitlines()
    ed.check(len(lines) == 2 and lines[0] == "menu shown" and re.fullmatch(r"hp 0(\.0)?", lines[1]),
             f"the timeline's call and the table animation's end: {lines}")

    # pointer pass-through: a pointer listener on the HUD document makes its page take the clicks
    status = await text_of(ed, "Menu", "#status")
    await lua(ed, prelude("HUD") + fragment("lua", "menu:contains(target)"))
    await click(ed, "Menu", "#settings")
    ed.check_eq(await text_of(ed, "Menu", "#status"), status, "the listening HUD page took the click")
    await click(ed, "HUD", "#heal")
    ed.check_eq(await lua(ed, "return object('HUD', 'UIDocument'):getDataModel('hud').health"), [45],
                "the HUD's own button still works")

    # Lottie, the <script type="text/lua"> fragment, fonts and text effects, texture mode
    values = await lua(ed, """local s = assert(object('Lottie', 'UIDocument'):getElementById('spinner'))
        return s:getTotalFrames() > 0, s:isPlaying()""")
    ed.check_eq(values, [True, True], "the Lottie fragment plays")
    values = await lua(ed, "return object('Script', 'UIDocument'):getElementById('hello') ~= nil")
    ed.check_eq(values, [True], "the script fragment's page")
    values = await lua(ed, """local doc = object('Fonts', 'UIDocument')
        local codes = {}
        for _, d in ipairs(doc:getDiagnostics()) do
            if d.code == 'HTMLUI-CSS-0023' or d.code == 'HTMLUI-CSS-0022' or d.severity ~= 'info' then codes[#codes + 1] = d.text end
        end
        return table.concat(codes, ' | '), doc:querySelector('.title').style.fontFamily""")
    ed.check_eq(values[0], "", "the @font-face fragment loads its font")
    ed.check(values[1].lower().startswith("title"), f"font-family: {values[1]}")
    values = await lua(ed, """local doc = object('Terminal', 'UIDocument')
        local c = doc:getUIDocumentComponent()
        return c.renderMode == UIDocumentRenderMode.TEXTURE, doc:getTargetTexture() ~= nil, doc.loaded,
               doc:getElementById('line').textContent""")
    ed.check_eq(values, [True, True, True, "READY"], "the texture-mode script")

    # the Video fragment: the muted loop plays on the UI clock, the trailer waits on its poster; the clamp box
    values = await lua(ed, """local doc = object('Video', 'UIDocument')
        local intro, trailer = doc:getElementById('intro'), doc:querySelector('video[controls]')
        return intro.paused, intro.muted, intro.loop, intro.currentTime > 0, intro.readyState >= UIElement.HAVE_CURRENT_DATA,
               trailer.paused, trailer.readyState >= UIElement.HAVE_METADATA, trailer.videoWidth""")
    ed.check_eq(values, [False, True, True, True, True, True, True, 320], "the Video fragment plays")
    values = await lua(ed, """local d = object('Clamp', 'UIDocument'):getElementById('desc')
        return d.offsetHeight, d.scrollHeight > d.offsetHeight""")
    ed.check_eq(values, [40, True], "the line-clamp fragment shows two lines")

    # nothing logged, also not by the edit-mode load of the HUD (its model did not exist yet); a host without an audio
    # device (the test machines) gets the engine's one "No audio output available" warning when the Video fragment's
    # audio track opens the mixer: not the UI's
    errors = [m for m in await errors_since(ed) if ("[HTML UI]" in m or "Lua" in m or "rror" in m)
              and not m.startswith("No audio output available")]
    ed.check(not errors, f"errors or warnings while playing the guide's snippets: {errors[:10]}")


# ---------------------------------------------------------------------------- 7. API search

async def test_search_engine_api_ui(ed: Editor):
    r = await ed.call("search_engine_api", query="querySelector")
    details = [x["detail"] for x in r["results"]]
    for want in ("UIDocument:querySelector(", "UIElement:querySelector("):
        ed.check(any(d.startswith(want) for d in details), f"search finds {want}: {details}")
    for query, signature in (("anime animate", "anime.animate(targets, params)"),
                             ("addEventListener", "UIElement:addEventListener(type, function(event)"),
                             ("UIDataModel onChange", "UIDataModel:onChange(name")):
        r = await ed.call("search_engine_api", query=query)
        ed.check(any(hit["detail"].startswith(signature) for hit in r["results"]),
                 f"search finds {signature}: {r['results']}")
    r = await ed.call("search_engine_api", query="mix-blend-mode")
    ed.check(any(h["doc"].startswith("jasper://docs/html-ui-support") for h in r["docs"]),
             f"search finds the published support matrix: {r['docs']}")


# ---------------------------------------------------------------------------- 8. stubs


async def test_luals_ui_stubs(ed: Editor):
    await wait_tooling(ed)
    scripts = {Path(b["path"]).name: b["code"] for b in guide_blocks() if b["lang"] == "lua" and b["path"]}
    scripts["GoodUI.lua"] = GOOD_UI_LUA
    ws = lua_workspace(ed, scripts)
    try:
        diags = luals_check(ws)
        for f in ws.glob("*.lua"):
            f.unlink()
        ed.check(not diags, "lua-language-server warnings on the guide's scripts:\n" +
                 "\n".join(f"{d['file']}:{d['line']}: {d['code']}: {d['message']}" for d in diags))
        for f in ws.glob("*.lua"):
            f.unlink()
        (ws / "BadUI.lua").write_text(BAD_UI_LUA, encoding="utf-8")
        diags = luals_check(ws)
        got: dict[int, set] = {}
        for d in diags:
            if d["file"].endswith("BadUI.lua"):
                got.setdefault(d["line"], set()).add(d["code"])
        for line, code in BAD_UI_LUA_EXPECTED.items():
            ed.check(code in got.get(line, set()), f"BadUI.lua:{line}: expected {code}, got {sorted(got.get(line, set()))}")
        stray = {line: codes for line, codes in got.items() if line not in BAD_UI_LUA_EXPECTED}
        ed.check(not stray, f"unexpected diagnostics on correct lines: {stray}")
    finally:
        shutil.rmtree(ws.parent, ignore_errors=True)


async def test_guide_cpp_snippet_parses(ed: Editor):
    """The guide's C++ block, in a function of a project script, against the project's API headers."""
    if not find_tool("clangd"):
        skip("clangd is not installed; the guide's C++ block is checked with it")
    await wait_tooling(ed)
    blocks = [b for b in guide_blocks() if b["lang"] == "cpp"]
    ed.check(blocks, "the guide has a C++ block")
    path = ed.project / "scripts" / "GuideCppCheck.cpp"
    try:
        for b in blocks:
            body = "\n".join("    " + line for line in b["code"].splitlines())
            path.write_text('#include "JasperEngine.h"\n\nusing namespace jasperengine;\n\n'
                            f"void guideCpp(Scene* scene, Entity entity) {{\n{body}\n}}\n", encoding="utf-8")
            n, errors = clangd_check(path)
            ed.check_eq(n, 0, f"guide line {b['line']}: clangd errors in the C++ block: {errors}")
    finally:
        path.unlink(missing_ok=True)
