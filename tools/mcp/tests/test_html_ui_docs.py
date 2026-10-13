"""The HTML/CSS UI agent docs (phase 4, p4-guide, spec §3.8 and §5.5): the guide
docs/mcp/guide/html-ui.md (its 16 sections, served split by DocsTools), every fenced html/css/lua
block of it checked (compat-clean through `check --ui`, Lua syntax through check_script, the
complete scripts through lua-language-server, and the behaviour the snippets show run in play:
the menu's clicks, keys and gamepad, data binding, anime, pointer pass-through, Lottie, fonts,
texture mode), the Lua names it uses present in the LuaLS stubs, the stubs themselves
(UIEventType, typed lua_CFunctions, BAD_UI_LUA codes), search_engine_api over the Lua UI API and
the docs, and the AGENTS.md section of a new project.

Snippet convention (the guide's, so it cannot rot): every ```html / ```css / ```lua block is a
good example; a block preceded by a line "`<project path>`:" is a complete file written at that
path, the others are fragments. Bad examples live in tables, never in those fences.

Wave-2 tool checks (create_ui_document, inspect_ui, assert_ui names) are guarded by
ed.require_tools: WAVE-2 marks each place p4-mcp-tools turns into a real check.
"""

from __future__ import annotations

import asyncio
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

import harness
from harness import REPO, Editor, skip

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_html_ui_basic import Shot, near  # noqa: E402
from test_html_ui_checks import check, check_eq, fail, run_json, scratch_project, tiny_png, write  # noqa: E402
from test_stubs import clangd_check, find_tool, lua_workspace, luals_check, stub_files, wait_tooling  # noqa: E402

TIMEOUT = 900

S: dict = {}

GUIDE = REPO / "docs" / "mcp" / "guide" / "html-ui.md"
EXAMPLES = REPO / "docs" / "mcp" / "guide" / "html-ui-examples"
FIXTURES = REPO / "engine" / "tests" / "htmlui" / "fixtures"

# The top-level sections of the guide, in order (phase-4 spec §3.8.1). Phase 5 appends "Effects".
SECTIONS = [
    "HTML/CSS UI in JasperEngine",
    "Quick start",
    "Files",
    "HTML",
    "CSS",
    "Lua API",
    "Data binding",
    "Animation",
    "Images, SVG, Lottie, fonts, icons",
    "Input and focus",
    "Responsive layout",
    "The loop in detail",
    "Habits from the web that need a change",
    "Examples",
    "Performance",
    "C++",
]

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
    elif ext in (".ttf", ".otf"):
        shutil.copy(REPO / "libs" / "imgui" / "misc" / "fonts" / "Karla-Regular.ttf", path)
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


async def center(ed: Editor, entity: str, selector: str) -> tuple[float, float]:
    values = await lua(ed, f"""
        local r = assert(object('{entity}', 'UIDocument'):querySelector('{selector}'), 'no {selector}'):getBoundingClientRect()
        return r.x + r.width / 2, r.y + r.height / 2""")
    return values[0], values[1]


async def click(ed: Editor, entity: str, selector: str) -> None:
    # WAVE-2: send_input {target: selector} once p4-mcp-tools has it; canvas coordinates work today
    x, y = await center(ed, entity, selector)
    await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": x, "y": y}], space="canvas",
                  frames_after=3)


async def text_of(ed: Editor, entity: str, selector: str) -> str:
    return (await lua(ed, f"return assert(object('{entity}', 'UIDocument'):querySelector('{selector}')).textContent"))[0]


async def focused_id(ed: Editor) -> str:
    return (await lua(ed, "local f = object('Menu', 'UIDocument'):querySelector(':focus'); return f and f.id or ''"))[0]


async def errors_since(ed: Editor) -> list[str]:
    entries = await ed.console_since(types=["error", "warning"], limit=1000)
    return [e.get("message", "") for e in entries]


# ---------------------------------------------------------------------------- 1. structure (no editor)

async def test_guide_structure():
    text = guide_text()
    titles = [line[3:].strip() for line in text.split("\n") if line.startswith("## ")]
    check_eq(titles[:len(SECTIONS)], SECTIONS, "the guide's top-level sections, in order")
    check(text.startswith("# HTML/CSS UI"), "the guide's title")
    langs = {b["lang"] for b in guide_blocks(text)}
    check(langs <= {"html", "css", "lua", "text", "cpp", "json"}, f"unexpected block languages: {langs}")
    # the rules the guide must state, so an edit cannot drop them
    for phrase in ("jasper://docs/html-ui-support",                            # links the matrix
                   "a transparent page lets the pointer through unless you listen", # pointer pass-through
                   "renderMode", "getTargetTexture",                           # texture mode
                   "Google Fonts", "import_files", "data-lucide",              # fonts and icons
                   "Chromium parity", "Parity stops at",                       # where parity stops
                   "Gamepads do not reach documents",
                   ".jasperengine/engine-api/html-ui-examples/", "check --ui"):
        check(phrase.lower() in text.lower(), f"the guide does not say {phrase!r}")
    named = [b["path"] for b in guide_blocks(text) if b["path"]]
    check(len(named) == len(set(named)), f"a file is given twice: {named}")
    for path in ("assets/ui/menu.html", "assets/ui/menu.css", "scripts/MenuUI.lua", "assets/ui/hud.html",
                 "assets/ui/hud.css", "scripts/HudUI.lua", "scripts/Terminal.lua"):
        check(path in named, f"the guide's complete file {path} is missing")


async def test_guide_examples_table():
    """The examples table of the guide follows examples.json: one row per example and template, in
    the manifest's order, each naming the checks its manifest entry runs; what the table says an
    example shows is in it (the icons are the built-in Lucide icons, not inline SVG)."""
    text = guide_text()
    manifest = json.loads((EXAMPLES / "examples.json").read_text(encoding="utf-8"))["examples"]
    section = text.split("\n## Examples\n", 1)[1].split("\n## ", 1)[0]
    rows = [line for line in section.split("\n") if line.startswith("| `") and not line.startswith("| `template`")]
    names = [re.match(r"\| `([a-z_]+)` \|", row).group(1) for row in rows]
    check_eq(names, [entry["name"] for entry in manifest], "the table's rows, in the manifest's order")
    for row, entry in zip(rows, manifest):
        cells = [cell.strip() for cell in row.strip("|").split("|")]
        checks = {word for word in re.findall(r"[a-z_]+", cells[2]) if word in ASSERT_CHECK_TYPES}
        check_eq(checks, {c["type"] for c in entry["checks"]}, f"{entry['name']}: the checks the table names")
    main = rows[names.index("main_menu")]
    check("inline SVG" not in main and "data-lucide" in main, f"main_menu's icons are the built-in Lucide ones: {main}")
    check("data-lucide" in (EXAMPLES / "main_menu" / "main_menu.html").read_text(encoding="utf-8"), "main_menu uses data-lucide")


# assert_ui's check types (HtmlUIAssertions' AssertKind, phase-4 spec 3.6.2)
ASSERT_CHECK_TYPES = {"exists", "count", "visible", "text_fits", "no_overlap", "within_parent", "within_viewport", "contrast",
                      "min_size"}


# ---------------------------------------------------------------------------- 2. html/css blocks (CLI)

async def test_guide_css_html_snippets_clean():
    with scratch_project() as project:
        shutil.rmtree(project / "assets" / "ui", ignore_errors=True)
        written = write_guide_snippets(project)
        check(len(written) >= 8, f"only {len(written)} html/css blocks")
        # fragments name images, animations and fonts the reader provides: provide real ones
        code, data = run_json(project, "--ui")
        for f in data["findings"]:
            if f.get("code") == "HTMLUI-GEN-0001" and f["path"].startswith("assets/ui/guide_snippet_"):
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
    ed.check(len(sections) >= len(SECTIONS), f"jasper://docs/html-ui is split into sections: {index[:600]}")
    ed.check(len(index) < 12000, f"the index stays small: {len(index)} chars")
    served = {uri: await read_text(ed, uri) for uri in sections}
    for title in SECTIONS:
        ed.check(any(f"\n## {title}\n" in "\n" + text for text in served.values()),
                 f"section {title!r} is not served in any part of jasper://docs/html-ui")
    for uri, text in served.items():
        ed.check(len(text) < 16 * 1024, f"{uri} is {len(text)} chars: split it with ### headings")
    listed = {str(r.uri) for r in (await ed.session.list_resources()).resources}
    ed.check(all(u in listed for u in sections), "the sections are listed resources")
    support = await read_text(ed, "jasper://docs/html-ui-support")
    ed.check("# HTML/CSS support matrix" in support or "jasper://docs/html-ui-support/" in support,
             f"jasper://docs/html-ui-support: {support[:300]}")
    docs = await read_text(ed, "jasper://docs")
    for name in ("jasper://docs/html-ui`", "jasper://docs/html-ui-support`"):
        ed.check(name in docs, f"jasper://docs lists {name}: {docs}")


# ---------------------------------------------------------------------------- 4. names

def backticked_words(text: str) -> set[str]:
    words = set()
    for span in re.findall(r"`([^`\n]+)`", text):
        words.update(re.findall(r"(?<![\w./-])[a-z][a-z0-9]*(?:_[a-z0-9]+)+(?![\w(]|\.\w)", span))
    return words


def lua_call_names(text: str) -> set[tuple[str, str]]:
    """(kind, name) of the guide's doc:x(, el:x(, anime.x( / anime.utils.x / anime.eases.x calls."""
    names = set()
    for m in re.finditer(r"\b(doc|el|document):(\w+)\(", text):
        names.add(("member", m.group(2)))
    for m in re.finditer(r"\banime\.(utils\.|eases\.)?(\w+)", text):
        if m.group(2) not in ("js", "lua"):   # Anime.js, the stub file anime.lua
            names.add(("anime" + ("." + m.group(1)[:-1] if m.group(1) else ""), m.group(2)))
    return names


async def test_guide_lua_names_in_stubs(ed: Editor):
    await wait_tooling(ed)
    stubs = stub_files(ed.project)
    ui = "\n".join(text for name, text in stubs.items() if name.startswith(("UI", "Anime", "anime", "HtmlUI")))
    members = set(re.findall(r"^function \w+[:.](\w+)\(", ui, re.M)) | set(re.findall(r"^---@field (\w+) ", ui, re.M))
    utils = set(re.findall(r"^---@field (\w+) fun\(", stubs["anime.lua"].split("---@class AnimeUtils", 1)[1].split("---@class", 1)[0], re.M))
    eases = set(re.findall(r"^---@field (\w+) AnimeEase", stubs["anime.lua"], re.M))
    anime = set(re.findall(r"^function anime\.(\w+)\(", stubs["anime.lua"], re.M)) | {"utils", "eases"}
    missing = []
    names = lua_call_names(guide_text())
    ed.check(len(names) > 20, f"the guide's Lua calls were not found: {sorted(names)}")
    for kind, name in sorted(names):
        known = {"member": members, "anime": anime, "anime.utils": utils, "anime.eases": eases}[kind]
        if name not in known:
            missing.append(f"{kind}:{name}")
    ed.check(not missing, f"Lua names of the guide missing from the stubs: {missing}")


async def test_guide_names_exist(ed: Editor):
    # WAVE-2: the guide names the phase-4 tools (create_ui_document, inspect_ui, assert_ui) and
    # parameters p4-mcp-tools adds (send_input target, import_files fonts/icons/from_document)
    ed.require_tools("create_ui_document", "inspect_ui", "assert_ui")
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from test_autosync import schema_words  # noqa: E402
    words: set = set()
    for t in ed.tool_list:
        schema_words(t.input_schema, words)
    # project setting values the guide names (graphics.scaling_mode values)
    settings = await ed.call("get_project_settings")

    def collect(value):
        if isinstance(value, dict):
            for k, v in value.items():
                words.add(k)
                collect(v)
        elif isinstance(value, list):
            for v in value:
                collect(v)
        elif isinstance(value, str):
            words.add(value)
    collect(settings)
    # result fields and enum values the guide explains (outputs, not inputs)
    outputs = {"external_changes", "ui_reloaded", "stylesheet_only", "render_target", "load_count", "sort_order",
               "fit_width", "fit_height", "zero_size"}
    unknown = sorted(w for w in backticked_words(guide_text())
                     if w not in ed.tool_names and w not in words and w not in outputs)
    ed.check(not unknown, f"the guide names unknown tools/parameters: {unknown}")
    for tool, params in {"create_ui_document": ["html", "template"], "inspect_ui": ["selector", "at", "viewport", "models", "file"],
                         "assert_ui": ["checks", "viewports"], "import_files": ["fonts", "icons", "from_document"],
                         "send_input": ["events"]}.items():
        have: set = set()
        schema_words(ed.tool(tool).input_schema, have)
        ed.check(all(p in have for p in params), f"{tool} lacks {[p for p in params if p not in have]}")


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
    await ed.call("save_scene", scene=S["scene"], path="scenes/GuideUI")

    # edit mode: the documents load without Lua, with no warning or error
    for name in ("Menu", "HUD", "Lottie", "Fonts", "Script", "Terminal"):
        diags = await lua(ed, f"""local out = {{}}
            for _, d in ipairs(object('{name}', 'UIDocument'):getDiagnostics()) do
                if d.severity ~= 'info' then out[#out + 1] = d.text end
            end
            return table.concat(out, '\\n'), object('{name}', 'UIDocument').loaded""", allow_edit_mode=True)
        ed.check_eq(diags, ["", True], f"{name}: loads clean in edit mode")

    started = await ed.call("play", action="start", scene=S["scene"], fixed_fps=60)
    ed.check(started, "play started")
    try:
        await play_checks(ed)
    finally:
        await ed.call("play", action="stop")


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

    # gamepad: the script's onGamepadButtonDown
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
               #doc:querySelectorAll('#log li') - 1, doc:querySelectorAll('#log li')[1].textContent""")
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

    # the edit-mode load of the HUD (no model yet) logs the one line the guide explains under "Data binding"
    explained = "Syntax error parsing inline property declaration 'width: %;'"
    errors = [m for m in await errors_since(ed) if ("[HTML UI]" in m or "Lua" in m or "rror" in m) and explained not in m]
    ed.check(not errors, f"errors or warnings while playing the guide's snippets: {errors[:10]}")


# ---------------------------------------------------------------------------- 7. API search

async def test_search_engine_api_ui(ed: Editor):
    r = await ed.call("search_engine_api", query="querySelector")
    details = [x["detail"] for x in r["results"]]
    for want in ("UIDocument:querySelector(", "UIElement:querySelector("):
        ed.check(any(d.startswith(want) for d in details[:6]), f"{want} in the first results: {details[:8]}")

    r = await ed.call("search_engine_api", query="anime animate")
    top = r["results"][0]["detail"]
    ed.check(top.startswith("anime.animate(targets, params)") and "duration" in top and "ease" in top,
             f"anime.animate with its parameters first: {r['results'][:3]}")

    r = await ed.call("search_engine_api", query="addEventListener")
    details = [x["detail"] for x in r["results"]]
    ed.check(any(d.startswith("UIElement:addEventListener(type, function(event)") for d in details[:4]),
             f"UIElement:addEventListener with its Lua signature: {details[:6]}")

    r = await ed.call("search_engine_api", query="mix-blend-mode")
    ed.check(any(h["doc"].startswith("jasper://docs/html-ui-support") for h in r["docs"]),
             f"a support matrix row: {r['docs']}")

    r = await ed.call("search_engine_api", query="UIDataModel onChange")
    ed.check(any(x["detail"].startswith("UIDataModel:onChange(name") for x in r["results"][:3]), f"{r['results'][:3]}")


# ---------------------------------------------------------------------------- 8. stubs

async def test_stubs_unresolved_empty(ed: Editor):
    await wait_tooling(ed)
    stubs = stub_files(ed.project)
    ui = {n: t for n, t in stubs.items() if n.startswith(("UI", "Anime", "anime", "HtmlUI"))}
    for name in ("UIDocument.lua", "UIElement.lua", "UIEvent.lua", "HtmlUITypes.lua", "anime.lua", "HtmlUI.lua"):
        ed.check(name in ui, f"{name} missing from the stubs")
    for name, text in ui.items():
        ed.check("-- unresolved:" not in text, f"{name}: " + "; ".join(l for l in text.splitlines() if "-- unresolved:" in l))
    types = ui["HtmlUITypes.lua"]
    for event in ("click", "keydown", "input", "change", "wheel", "transitionend", "dragdrop", "htmlnavigate", "complete"):
        ed.check(f'---| "{event}"' in types, f"UIEventType lacks {event}")
    for never in ("mouseenter", "contextmenu", "pointerdown", "touchstart"):
        ed.check(f'---| "{never}"' not in types, f"UIEventType must not list {never} (documents never get it)")
    ed.check("---@class UIDiagnostic" in types, "UIDiagnostic")
    ed.check("---@param type UIEventType" in ui["UIElement.lua"] and "---@param type UIEventType" in ui["UIDocument.lua"],
             "addEventListener takes a UIEventType")
    ed.check("---@return UIDiagnostic[]" in ui["UIDocument.lua"], "getDiagnostics returns UIDiagnostic[]")
    for name in ("UIDocument.lua", "UIElement.lua", "UIEvent.lua", "anime.lua"):
        ed.check("jasper://docs/html-ui" in ui[name], f"{name} links the guide")


async def test_luals_ui_stubs(ed: Editor):
    await wait_tooling(ed)
    scripts = {Path(b["path"]).name: b["code"] for b in guide_blocks() if b["lang"] == "lua" and b["path"]}
    dirs = []
    if EXAMPLES.is_dir():
        # the examples' scripts (p4-examples), as they are copied into a project's Lua directory
        tmp = Path(tempfile.mkdtemp(prefix="jasper-ui-examples-"))
        for lua_file in EXAMPLES.glob("*/*.lua"):
            shutil.copy(lua_file, tmp / lua_file.name)
        dirs.append(tmp)
    else:
        print("     no docs/mcp/guide/html-ui-examples yet (p4-examples): only the guide's scripts are checked")
    scripts["GoodUI.lua"] = GOOD_UI_LUA
    ws = lua_workspace(ed, scripts, dirs)
    try:
        # the guide's scripts and the examples' each in a workspace of their own: a guide snippet may
        # name a class as an example does (HudUI), which would be a duplicate definition in one
        for name in list(ws.glob("src*")):
            shutil.move(str(name), str(ws.parent / name.name))
        diags = luals_check(ws)
        for f in ws.glob("*.lua"):
            f.unlink()
        for d in dirs:
            shutil.copytree(d, ws / "src0")
            diags += luals_check(ws)
            shutil.rmtree(ws / "src0", ignore_errors=True)
        ed.check(not diags, "lua-language-server warnings on the guide's / examples' scripts:\n" +
                 "\n".join(f"{d['file']}:{d['line']}: {d['code']}: {d['message']}" for d in diags))
        for f in ws.glob("*.lua"):
            f.unlink()
        shutil.rmtree(ws / "src0", ignore_errors=True)
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
        for d in dirs:
            shutil.rmtree(d, ignore_errors=True)


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


# ---------------------------------------------------------------------------- 9. AGENTS.md (last: switches the project)

async def test_agents_md_section(ed: Editor):
    new_dir = Path(tempfile.mkdtemp(prefix="jasper-agents-")) / "Fresh"
    await ed.call_raw("play", {"action": "stop"})   # an earlier failure may have left play running
    try:
        await ed.call("create_project", path=str(new_dir), name="Fresh UI", unsaved="discard")
        # a new project gets its AGENTS.md when it is first read (or played, or built)
        agents = await read_text(ed, "jasper://project/AGENTS.md")
        ed.check_eq((new_dir / "AGENTS.md").read_text(encoding="utf-8"), agents, "AGENTS.md written")
        ed.check("## HTML/CSS UI" in agents, f"AGENTS.md has the HTML/CSS UI section: {agents[:2000]}")
        section = agents.split("## HTML/CSS UI", 1)[1].split("\n## ", 1)[0]
        for phrase in ("check --ui", ".jasperengine/engine-api/html-ui-examples/", "jasper://docs/html-ui",
                       "jasper://docs/html-ui-support", "create_ui_document", "addEventListener", "assets/ui/"):
            ed.check(phrase in section, f"the AGENTS.md section mentions {phrase}: {section}")
        ed.check("HTML/CSS UI, C++ build" in agents, "the check bullet names the UI check")
    finally:
        try:
            await ed.call("open_project", path=str(ed.project), unsaved="discard")
        finally:
            shutil.rmtree(new_dir.parent, ignore_errors=True)
