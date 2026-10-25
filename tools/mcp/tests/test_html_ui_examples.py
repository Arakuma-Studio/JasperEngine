"""The HTML/CSS UI example library (phase 4, p4-examples, spec §3.7, §3.9, §5.4).

docs/mcp/guide/html-ui-examples holds thirteen examples (phase 6 added video_menu; S17 server_browser, chat_log and
quest_log) and the blank template (examples.json is the manifest). The editor embeds them and writes them into every project's
.jasperengine/engine-api/html-ui-examples/. Per example <n> (generated from the manifest):

  test_<n>_files_in_project   the project copy has the example's files, byte for byte
  test_<n>_compat_clean       `check --ui --files` on its files: zero findings
  test_<n>_edit_mode          built in a 2D scene UITest_<n> from the copied files: it loads with no
                              warning or error and the Game view is not flat
  test_<n>_play_golden        played (fixed_fps 60, auto_pause) for 90 frames, the example's
                              interaction steps (behaviour checked through eval_lua and the Console),
                              captured twice from fresh starts (identical), compared with
                              engine/tests/htmlui/golden/examples/<n>.png (JASPER_HTMLUI_UPDATE_GOLDEN=1
                              rewrites it: look at every image you write)
  test_<n>_asserts            the manifest's assert_ui checks at its viewports

plus pngdiff, the manifest and authoring rules, the project copy (kept current when the editor's
examples change, unicode project paths, an unwritable target) and lua-language-server on the scripts.

The scenes are built with create_ui_document {template}; elements are clicked, hovered and dragged
with send_input {target}; inspect_ui, assert_ui and the jasper://examples/html-ui resources are
checked too (p4-mcp-tools).

Runs on a scratch copy of the fixture project (canvas 1280 x 720).
"""

from __future__ import annotations

import asyncio
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

import harness
from harness import Editor, TestFailure, skip

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import pngdiff  # noqa: E402  (tools/mcp/pngdiff.py)

TIMEOUT = 900

LIBRARY = harness.REPO / "docs" / "mcp" / "guide" / "html-ui-examples"
MANIFEST = json.loads((LIBRARY / "examples.json").read_text(encoding="utf-8"))
ENTRIES = {entry["name"]: entry for entry in MANIFEST["examples"]}
NAMES = [entry["name"] for entry in MANIFEST["examples"]]
GOLDEN_DIR = harness.REPO / "engine" / "tests" / "htmlui" / "golden" / "examples"
UPDATE_GOLDEN = os.environ.get("JASPER_HTMLUI_UPDATE_GOLDEN") == "1"
PROJECT_COPY = Path(".jasperengine") / "engine-api" / "html-ui-examples"
LUA_DIR = "scripts"                      # the fixture's luaDir
BACKGROUND = [0.09, 0.1, 0.13, 1.0]      # the test scenes' clear colour: the goldens' backdrop
GOLDEN_SIZE = (1280, 720)
COMPARE_SIZE = (640, 360)
CHANNEL_TOLERANCE = 24
MAX_FRACTION = 0.01
# Two runs from fresh starts: the same image (rendering is deterministic: p4-gaps fixed the shadow
# textures that kept the pixel size of the view they were first drawn in, fork P4-G7)
RUNS_MAX_FRACTION = 0.0

# Per example: the public surface the spec (§3.7.2), the tests and the docs name
SURFACE = {
    "main_menu": ["h1.logo", "nav.menu", "button.menu-button#play", "button.menu-button#settings", "button.menu-button#credits",
                  "button.menu-button#quit", "footer.version", "aside#settings-panel", "#settings-back"],
    "pause_menu": ["div#pause.pause.open", ".pause-card", "h2", "button.pause-button#resume", "button.pause-button#options",
                   "button.pause-button#restart", "button.pause-button#to-menu"],
    "hud": [".hud-panel", "#health-fill", "#shield-fill", "#score", "#timer", ".minimap", "#ammo", "#reserve", ".ability",
            "#damage-vignette", ".hud-label"],
    "inventory": ["div.inventory", "#gold", "#close", ".equipment", "div.grid", "div.slot", "#tooltip", ".tooltip-title", "#weight-fill"],
    "settings": ["div.settings", "button.tab#tab-graphics", "button.tab#tab-audio", "button.tab#tab-controls", "button.tab#tab-gameplay",
                 "section.panel", ".row", "input#music", "input#vsync", ".keybind", "#apply", "#reset", "#back"],
    "dialogue": ["section.dialogue", ".portrait", "#speaker", "#line.dialogue-text", ".continue", "#choices"],
    "shop": ["div.shop", "#gold", "button.chip", "div.grid", "article.card", ".name", ".rarity", ".stats", ".price", "button.buy",
             "#confirm", "#qty-minus", "#qty-plus", "#confirm-buy", "#confirm-cancel"],
    "toasts": ["div#toasts.toast-stack", "div.toast.toast-info", ".message", ".toast-progress"],
    "loading": ["div.loading", "lottie#spinner", "#progress-track", "#progress-fill", "#percent", ".tip", "#press-key", ".loading-card"],
    "video_menu": ["video.backdrop-video#backdrop", ".scrim", "h1.title", "nav.menu", "button.menu-button#play", "button.menu-button#continue",
                   "button.menu-button#options", "button.menu-button#quit", "button.motion#motion", "#motion-label", "footer.footer"],
    "server_browser": ["div.browser", "table-view#servers.servers", "button.chip#region-eu", "#count", "button#refresh", "#detail-name",
                       "button#join.join", ".ping-head", "template#ping-help"],
    "chat_log": ["section.chat", "list-view#messages.messages", "input#draft.draft", "button#send.send", "#online"],
    "quest_log": ["section.log", "tree-view#tree.tree", "markdown#journal", "#progress", "article.journal"],
    "blank": ["main.screen", "h1"],
}

# Examples whose CSS animations run from the document's load (infinite keyframes) and that make
# no data model (createDataModel reloads a document): play keeps the document the editor loaded,
# so their phase at play start depends on the wall time spent in edit mode. The test rebuilds them (a
# patch reload keeps running animations, S17)
# once play is held, which starts them on the fixed UI clock (video_menu: its autoplay video too).
RESTART_CSS = {"main_menu", "video_menu"}

S: dict = {}   # per example: {"scene", "entity"}, and "editor" (the Editor the scenes were made in)


# ---------------------------------------------------------------------------------------- helpers

def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def pascal(stem: str) -> str:
    """HtmlUiExamples::pascalName: words of ASCII letters and digits, capitalized and joined."""
    words = re.split(r"[^A-Za-z0-9]+", stem)
    name = "".join(w[:1].upper() + w[1:] for w in words if w)
    if not name:
        return "Document"
    return "Ui" + name if name[0].isdigit() else name


def example_files(name: str) -> list[str]:
    """Every file of an example, relative to the library root."""
    root = LIBRARY / name
    return sorted(p.relative_to(LIBRARY).as_posix() for p in root.rglob("*") if p.is_file())


def html_rel(name: str) -> str:
    return f"assets/ui/{name}/{ENTRIES[name]['html']}"


def doc_lua(name: str) -> str:
    """Lua for the example's UIDocument in the played (or selected) scene."""
    return f"UIDocument(scene, entity('{name}'))"


async def lua(ed: Editor, code: str, **args) -> list:
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def click(ed: Editor, name: str, selector: str, frames_after: int = 10) -> None:
    """A click at the centre of the first visible element the selector matches in the example's document."""
    await ed.call("send_input", events=[{"type": "mouse", "action": "click", "target": selector, "document": name}],
                  frames_after=frames_after)


async def hover(ed: Editor, name: str, selector: str, frames_after: int = 10) -> None:
    await ed.call("send_input", events=[{"type": "mouse", "action": "move", "target": selector, "document": name}],
                  frames_after=frames_after)


async def console_messages(ed: Editor, **args) -> list[str]:
    """The Console lines since the previous console_since / console_messages call."""
    return [e.get("message", "") for e in await ed.console_since(limit=1000, **args)]


async def pad(ed: Editor, *buttons: str, frames_after: int = 6) -> None:
    """Gamepad presses (p4-input: the focused document gets them as keys)."""
    await ed.call("send_input", events=[{"type": "gamepad", "button": b, "action": "press"} for b in buttons],
                  frames_after=frames_after)


async def focused(ed: Editor, name: str) -> str:
    """The id of the example's focused element ('' for none)."""
    return (await lua(ed, f"local f = {doc_lua(name)}:querySelector(':focus'); return f and f.id or ''"))[0]


async def walk(ed: Editor, name: str, button: str, limit: int = 12) -> list[str]:
    """Presses `button` until the focus stops moving; the ids it went through."""
    seen = []
    for _ in range(limit):
        await pad(ed, button)
        current = await focused(ed, name)
        if seen and current == seen[-1]:
            break
        seen.append(current)
    return seen


async def console_cursor(ed: Editor) -> int:
    """A read_console cursor at the newest line (the steps move console_since's own cursor)."""
    return (await ed.call("read_console", limit=1))["next_cursor"]


async def errors_since(ed: Editor, cursor: int) -> list[str]:
    """Errors, and the warnings of the HTML UI and of scripts, since the cursor."""
    result = await ed.call("read_console", since=cursor, types=["error", "warning"], limit=1000)
    return [e.get("message", "") for e in result["entries"]
            if e.get("type") == "error" or "[HTML UI]" in e.get("message", "") or "Lua" in e.get("message", "")]


def copy_example(project: Path, name: str, source: Path) -> None:
    """The example's html, css and assets into assets/ui/<name>/, its Lua into the Lua dir."""
    entry = ENTRIES[name]
    destination = project / "assets" / "ui" / name
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(source / name, destination, ignore=shutil.ignore_patterns("*.lua"))
    (project / LUA_DIR).mkdir(exist_ok=True)
    shutil.copy(source / name / entry["lua"], project / LUA_DIR / entry["lua"])


async def ensure_scene(ed: Editor, name: str) -> dict:
    """The 2D scene UITest_<name> with a GameObject <name> showing the example (built once per editor):
    create_ui_document {html: assets/ui/<name>/<name>.html, template: name} in one call."""
    state = S.get(name)
    if state and state.get("editor") is ed:
        return state
    entry = ENTRIES[name]
    made = await ed.call("create_scene", name=f"UITest_{name}", type="2D")
    scene = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=scene, settings={"background_color": BACKGROUND})
    # script: blank's Lua too (the examples' default), as every example has it attached; overwrite: the
    # LuaLS test copies the library's scripts (not renamed for <name>_assets/) into the Lua dir first
    created = await ed.call("create_ui_document", scene=scene, name=name, html=html_rel(name), template=name, script=True,
                            overwrite=True)
    entity = created["entity"]["id"]
    check(created["script"] == {"path": f"{LUA_DIR}/{entry['lua']}", "attached": True}, f"{name}: the template's script: {created['script']}")
    counts = created["document"]["diagnostics_count"]
    check((counts["error"], counts["warning"]) == (0, 0), f"{name}: the first load is clean: {created['diagnostics']}")
    await ed.call("save_scene", scene=scene, path=f"scenes/UITest_{name}")
    S[name] = {"editor": ed, "scene": scene, "entity": entity}
    return S[name]


async def capture(ed: Editor, label: str):
    """The Game view's canvas rectangle as a pngdiff image (full size)."""
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
    check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"{label}.png").write_bytes(png)
    rect = shot.value["canvas_rect_in_image"]
    return pngdiff.crop(pngdiff.decode(png), round(rect["x"]), round(rect["y"]), round(rect["width"]), round(rect["height"]))


async def wait_project_copy(ed: Editor, project: Path, timeout: float = 240) -> Path:
    """The editor writes the copy with the other tool files when it opens a project."""
    copy = project / PROJECT_COPY
    deadline = time.monotonic() + timeout
    await ed.call("get_project_summary")
    while time.monotonic() < deadline:
        if (copy / "examples.json").exists() and all((copy / f).exists() for n in NAMES for f in example_files(n)):
            return copy
        await asyncio.sleep(0.5)
    fail(f"the example library was not written into {copy} in {timeout:.0f}s")
    return copy


# ---------------------------------------------------------------------------------------- pngdiff

def _png(width: int, height: int, channels: int, rows: list[bytes], filters: list[int]) -> bytes:
    """A PNG whose rows use the given filters (the encoder side of each filter)."""
    stride = width * channels
    raw = b""
    prev = bytes(stride)
    for row, f in zip(rows, filters):
        out = bytearray(stride)
        for x in range(stride):
            a = row[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if f == 0:
                predictor = 0
            elif f == 1:
                predictor = a
            elif f == 2:
                predictor = b
            elif f == 3:
                predictor = (a + b) // 2
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
            out[x] = (row[x] - predictor) & 255
        raw += bytes([f]) + bytes(out)
        prev = row

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    color_type = {3: 2, 4: 6}[channels]
    return (pngdiff.PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


async def test_pngdiff():
    """decode (RGB and RGBA, the five filters), encode, crop, downscale, compare, flat."""
    rng = random.Random(4)
    for channels in (3, 4):
        width, height = 7, 10
        rows = [bytes(rng.randrange(256) for _ in range(width * channels)) for _ in range(height)]
        data = _png(width, height, channels, rows, [y % 5 for y in range(height)])
        w, h, rgba = pngdiff.decode(data)
        check((w, h) == (width, height), f"size {w}x{h}")
        for y in range(height):
            for x in range(width):
                pixel = rgba[(y * width + x) * 4:(y * width + x + 1) * 4]
                expected = rows[y][x * channels:(x + 1) * channels] + (b"\xff" if channels == 3 else b"")
                check(bytes(pixel) == expected, f"pixel {x},{y} of a {channels}-channel PNG: {bytes(pixel)!r} != {expected!r}")

    img = (4, 2, bytearray(range(32)))
    check(pngdiff.decode(pngdiff.encode(img)) == img, "encode then decode")
    check(pngdiff.encode(*img) == pngdiff.encode(img), "encode(w, h, rgba) == encode(img)")
    check(pngdiff.crop(img, 1, 1, 2, 5) == (2, 1, bytearray(range(20, 28))), "crop clamps to the image")

    # box filter: integer ratio (2x2 means) and a fractional one (area weights)
    big = (4, 2, bytearray([0, 0, 0, 255, 100, 100, 100, 255, 10, 20, 30, 255, 30, 40, 50, 255] * 2))
    check(pngdiff.downscale(big, 2, 1) == (2, 1, bytearray([50, 50, 50, 255, 20, 30, 40, 255])), "2:1 downscale")
    w, h, third = pngdiff.downscale((3, 1, bytearray([0, 0, 0, 0, 90, 90, 90, 90, 180, 180, 180, 180])), 2, 1)
    check((w, h) == (2, 1) and list(third[:4]) == [30, 30, 30, 30] and list(third[4:]) == [150, 150, 150, 150],
          f"3:2 downscale weights the areas: {list(third)}")

    a = (10, 10, bytearray([40, 40, 40, 255] * 100))
    b = (10, 10, bytearray(a[2]))
    b[2][0:4] = bytes([60, 40, 40, 255])          # within the tolerance
    ok, fraction, diff = pngdiff.compare(a, b)
    check(ok and fraction == 0.0 and diff.startswith(pngdiff.PNG_SIGNATURE), f"within tolerance: {ok} {fraction}")
    b[2][4:8] = bytes([200, 40, 40, 255])         # one pixel of 100 beyond it
    ok, fraction, diff = pngdiff.compare(a, b)
    check(ok and fraction == 0.01, f"1% differs: {ok} {fraction}")
    b[2][8:12] = bytes([40, 40, 40, 0])           # alpha counts
    ok, fraction, diff = pngdiff.compare(a, b)
    check(not ok and fraction == 0.02, f"2% differs: {ok} {fraction}")
    dw, dh, drgba = pngdiff.decode(diff)
    check((dw, dh) == (10, 10) and bytes(drgba[4:8]) == bytes([255, 0, 0, 255]), "the diff marks a differing pixel red")
    check(pngdiff.compare(a, (5, 5, bytearray(100))) == (False, 1.0, None), "different sizes never match")

    check(pngdiff.flat(a) and not pngdiff.flat(b), "flat")
    check(pngdiff.flat((0, 0, bytearray())), "an empty image is flat")


# ---------------------------------------------------------------------------------------- library rules

async def test_manifest_and_authoring_rules():
    """examples.json lists the thirteen examples then blank; each example follows the library's rules
    (spec §3.7.1): one html linking one css, assets under assets/, a purpose comment at the top, a
    `<Pascal>UI` Lua class with byId and the onLoad binding, generic font families, nothing remote,
    the size limits, and the public ids and classes."""
    check(MANIFEST.get("version") == 1, "manifest version 1")
    check(NAMES == ["main_menu", "pause_menu", "hud", "inventory", "settings", "dialogue", "shop", "toasts", "loading", "video_menu",
                    "server_browser", "chat_log", "quest_log", "blank"],
          f"manifest order: {NAMES}")
    on_disk = sorted(p.name for p in LIBRARY.iterdir() if p.is_dir())
    check(on_disk == sorted(NAMES), f"folders {on_disk}")
    check((LIBRARY / "LICENSE-lucide.txt").is_file() and "ISC License" in (LIBRARY / "LICENSE-lucide.txt").read_text(),
          "LICENSE-lucide.txt with the ISC license")
    keys = {"name", "title", "example", "description", "demonstrates", "html", "css", "lua", "assets", "viewports", "checks"}
    check_types = {"exists", "count", "visible", "text_fits", "no_overlap", "within_parent", "within_viewport", "contrast", "min_size",
                   "virtual_window", "tooltip", "selected"}
    for name, entry in ENTRIES.items():
        where = f"examples.json {name}"
        check(set(entry) == keys, f"{where}: keys {sorted(entry)}")
        check(entry["example"] == (name != "blank"), f"{where}: example flag")
        check(entry["html"] == f"{name}.html" and entry["css"] == f"{name}.css", f"{where}: file names")
        check(entry["lua"] == f"{pascal(name)}UI.lua", f"{where}: Lua file {entry['lua']}")
        check(entry["viewports"] and all(len(v) == 2 for v in entry["viewports"]), f"{where}: viewports")
        check(entry["checks"] and all(c["type"] in check_types and c.get("selector") for c in entry["checks"]), f"{where}: checks")
        listed = {f"{name}/{entry['html']}", f"{name}/{entry['css']}", f"{name}/{entry['lua']}"} | {f"{name}/{a}" for a in entry["assets"]}
        check(set(example_files(name)) == listed, f"{where}: the folder holds {example_files(name)}, the manifest lists {sorted(listed)}")

        html = (LIBRARY / name / entry["html"]).read_text(encoding="utf-8")
        css = (LIBRARY / name / entry["css"]).read_text(encoding="utf-8")
        code = (LIBRARY / name / entry["lua"]).read_text(encoding="utf-8")
        for text, limit, label in ((html, 200, "html"), (css, 450, "css"), (code, 250, "Lua")):
            lines = text.count("\n")
            check(lines <= limit, f"{name}: {label} has {lines} lines (at most {limit})")
        links = re.findall(r'<link[^>]*rel="stylesheet"[^>]*href="([^"]+)"', html)
        check(links == [entry["css"]], f"{name}: links {links}, one stylesheet expected")
        check(not re.search(r"https?://", html + css), f"{name}: a remote URL")
        check("<script" not in html, f"{name}: a <script> (logic goes in {entry['lua']})")
        for ref in re.findall(r'(?<![\w-])(?:src|href)="([^"#]+)"', html) + re.findall(r"url\(['\"]?([^'\")]+)", css):
            if ref != entry["css"]:
                check(ref.startswith("assets/") and (LIBRARY / name / ref).is_file(), f"{name}: reference {ref}")
        for family in re.findall(r"font-family:\s*([^;]+);", css):
            check(family.strip() in ("system-ui, sans-serif", "inherit", "monospace"), f"{name}: font-family {family}")
        if name != "blank":
            check(re.match(r"<!DOCTYPE html>\n<!--\n  [^\n]+\n  Shows: ", html), f"{name}: the html starts with its purpose and features")
            check(":root {" in css and "var(--" in css, f"{name}: design tokens in :root")
            check("local function byId(doc, id)" in code and "assert(element" in code, f"{name}: byId that asserts")
            check("RegisterEvent(self, self.doc.onLoad, \"onDocumentLoad\")" in code and "self:onDocumentLoad()" in code,
                  f"{name}: listeners bound on doc.onLoad and from init")
        cls = entry["lua"][:-4]
        check(f"---@class {cls} : ScriptInstance" in code and f"local {cls} = {{" in code and code.rstrip().endswith(f"return {cls}"),
              f"{name}: the Lua class {cls}")
        check(f"UIDocument(self.scene, self.entity)" in code, f"{name}: the document from UIDocument(self.scene, self.entity)")
        for selector in SURFACE[name]:
            tag, rest = re.match(r"([a-z0-9]*)(.*)", selector).groups()
            ids = re.findall(r"#([\w-]+)", rest)
            classes = re.findall(r"\.([\w-]+)", rest)
            found = False
            for m in re.finditer(r"<([a-z0-9]+)([^>]*)>", html):
                attrs = m.group(2)
                if tag and m.group(1) != tag:
                    continue
                element_id = re.search(r'\bid="([^"]+)"', attrs)
                element_classes = (re.search(r'\bclass="([^"]+)"', attrs) or [None, ""])[1].split()
                if all(element_id and element_id.group(1) == i for i in ids) and all(c in element_classes for c in classes):
                    found = True
                    break
            check(found, f"{name}: no element {selector} in {entry['html']}")


async def test_ui_check_on_the_library():
    """`check --ui` on the whole library copied into a project: zero errors and warnings."""
    with tempfile.TemporaryDirectory(prefix="jasper-examples-check-") as tmp:
        project = Path(tmp) / "project"
        project.mkdir()
        shutil.copy(harness.default_fixture() / "project.yaml", project / "project.yaml")
        shutil.copytree(LIBRARY, project / "assets" / "html-ui-examples")
        r = subprocess.run(harness.editor_command() + ["check", "--project", str(project), "--ui", "--format", "json"],
                           cwd=project, capture_output=True, text=True, timeout=120)
        data = json.loads(r.stdout)
        check(r.returncode == 0 and data["errors"] == 0 and data["warnings"] == 0 and data["findings"] == [],
              f"check --ui on the library: exit {r.returncode}, {data}")
        check(data["checked"] == {"ui": len(NAMES)}, f"documents checked: {data['checked']}")


# ---------------------------------------------------------------------------------------- the project copy

async def test_project_copy_is_the_library(ed: Editor):
    """The editor wrote the library into .jasperengine/engine-api/html-ui-examples: every file, byte
    for byte, nothing else."""
    copy = await wait_project_copy(ed, ed.project)
    library = sorted(p.relative_to(LIBRARY).as_posix() for p in LIBRARY.rglob("*") if p.is_file())
    written = sorted(p.relative_to(copy).as_posix() for p in copy.rglob("*") if p.is_file())
    check(written == library, f"the copy holds {written}")
    for rel in library:
        check((copy / rel).read_bytes() == (LIBRARY / rel).read_bytes(), f"{rel} differs from the library")
    check(json.loads((copy / "examples.json").read_text())["examples"][0]["name"] == "main_menu", "the copy's manifest parses")


async def test_project_copy_follows_the_editor(ed: Editor):
    """Opening a project brings its copy to the editor's examples (an edited file restored, a file and
    a folder the editor no longer has removed); a project at a unicode path gets one; a copy that
    cannot be written is reported in the Console and the project still opens."""
    root = Path(tempfile.mkdtemp(prefix="jasper-examples-copy-"))
    try:
        proj = root / "Projekt ☃ ünïcode"
        shutil.copytree(ed.project, proj, ignore=shutil.ignore_patterns(".jasperengine"))
        await ed.call("open_project", path=str(proj), unsaved="discard")
        copy = await wait_project_copy(ed, proj)
        check((copy / "hud" / "hud.css").read_bytes() == (LIBRARY / "hud" / "hud.css").read_bytes(), "unicode path: hud.css")

        (copy / "hud" / "hud.css").write_text("/* edited */")
        (copy / "retired_example").mkdir()
        (copy / "retired_example" / "retired.html").write_text("<p>retired</p>")
        (copy / "shop" / "assets" / "items" / "retired.svg").write_text("<svg/>")
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        await ed.call("open_project", path=str(proj), unsaved="discard")
        await wait_project_copy(ed, proj)
        check((copy / "hud" / "hud.css").read_bytes() == (LIBRARY / "hud" / "hud.css").read_bytes(), "the edited file is restored")
        check(not (copy / "retired_example").exists(), "a folder the editor no longer has is removed")
        check(not (copy / "shop" / "assets" / "items" / "retired.svg").exists(), "a file the editor no longer has is removed")

        # a file where the copy's folder has to be: nothing can be written there
        shutil.rmtree(copy)
        copy.write_text("in the way")
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        await ed.console_since(None, limit=1)
        await ed.call("open_project", path=str(proj), unsaved="discard")
        await ed.call("get_project_summary")
        errors = [m for m in await console_messages(ed, types=["error"]) if "HTML UI examples" in m]
        check(errors and "html-ui-examples" in errors[0], f"the failed copy is reported: {errors}")
        check(copy.is_file() and copy.read_text() == "in the way", "the file in the way is left alone")
    finally:
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        shutil.rmtree(root, ignore_errors=True)


def _find_tool(name: str) -> str | None:
    found = shutil.which(name)
    if found:
        return found
    local = Path("/var/lib/orquester/.local/bin") / name
    return str(local) if local.exists() else None


async def test_lua_scripts_have_no_luals_diagnostics(ed: Editor):
    """The examples' scripts in a project's Lua directory, checked by lua-language-server with the
    editor's stubs and .luarc.json: no diagnostic of Warning or above."""
    exe = _find_tool("lua-language-server")
    if not exe:
        skip("lua-language-server is not installed (https://github.com/LuaLS/lua-language-server/releases)")
    await wait_project_copy(ed, ed.project)
    for name in NAMES:
        shutil.copy(LIBRARY / name / ENTRIES[name]["lua"], ed.project / LUA_DIR / ENTRIES[name]["lua"])
    await ed.call("sync_files")
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "check.json"
        proc = subprocess.run([exe, f"--check={ed.project}", "--checklevel=Warning", "--check_format=json",
                               f"--check_out_path={out}"], capture_output=True, text=True, timeout=600)
        check(out.exists(), f"lua-language-server wrote no report: {proc.stdout[-2000:]} {proc.stderr[-2000:]}")
        data = json.loads(out.read_text() or "{}") or {}
    scripts = {ENTRIES[n]["lua"] for n in NAMES}
    problems = [f"{uri.rsplit('/', 1)[-1]}:{d['range']['start']['line'] + 1}: {d.get('code')}: {d['message'][:160]}"
                for uri, items in data.items() for d in items if uri.rsplit("/", 1)[-1] in scripts]
    check(not problems, "lua-language-server diagnostics in the examples:\n" + "\n".join(problems))


# ---------------------------------------------------------------------------------------- interaction steps
# Each example: `before` runs from play start to the golden capture (every run), `after` once after
# it (the rest of the behaviour). Both check the spec's §5.4 table through eval_lua and the Console.

async def main_menu_before(ed: Editor, n: str, first: bool) -> None:
    await hover(ed, n, "#settings")
    hovered = (await ed.call("inspect_ui", entity=n, selector="#settings", depth=0))["elements"][0]
    check("hover" in hovered.get("pseudo", []), f"inspect_ui shows #settings hovered: {hovered}")
    await click(ed, n, "#settings", frames_after=30)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('settings-panel').classList:contains('open')") == [True],
          "#settings opens the panel")
    await click(ed, n, "#settings-back", frames_after=30)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('settings-panel').classList:contains('open')") == [False],
          "#settings-back closes it")
    await ed.console_since(None, limit=1)
    await click(ed, n, "#play", frames_after=30)
    check("main_menu: play" in await console_messages(ed), "#play logs main_menu: play")


async def main_menu_after(ed: Editor, n: str) -> None:
    await ed.call("send_input", events=[{"type": "key", "key": "down"}, {"type": "key", "key": "enter"}], frames_after=10)
    check("main_menu: settings" in await console_messages(ed), "ArrowDown then Enter presses #settings")

    # the gamepad (p4-input): B closes the panel, the d-pad reaches every button, A presses
    panel = f"return {doc_lua(n)}:getElementById('settings-panel').classList:contains('open')"
    check(await lua(ed, panel) == [True], "the panel is open")
    await pad(ed, "dpad_down")
    check(await focused(ed, n) == "settings-back", "the d-pad stays on the open panel")
    await pad(ed, "b", frames_after=30)
    check(await lua(ed, panel) == [False] and await focused(ed, n) == "settings", "B closes the panel")
    check(await walk(ed, n, "dpad_down", 3) == ["credits", "quit", "play"], "the d-pad goes through the buttons")
    await pad(ed, "dpad_down", "dpad_down")
    await ed.console_since(None, limit=1)
    await pad(ed, "a", frames_after=10)
    check("main_menu: credits" in await console_messages(ed), "A presses the focused #credits")
    await pad(ed, "dpad_up", "a", frames_after=30)
    check(await lua(ed, panel) == [True] and await focused(ed, n) == "settings-back", "A on #settings opens the panel")
    await pad(ed, "a", frames_after=30)
    check(await lua(ed, panel) == [False], "A on Back closes it")


async def pause_menu_before(ed: Editor, n: str, first: bool) -> None:
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('pause').classList:contains('open')") == [False],
          "the game starts unpaused")
    await ed.call("send_input", events=[{"type": "key", "key": "escape"}], frames_after=20)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('pause').classList:contains('open'), Engine.timeScale") == [True, 0],
          "Escape opens the menu and freezes the game")


async def pause_menu_after(ed: Editor, n: str) -> None:
    await ed.console_since(None, limit=1)
    await click(ed, n, "#options")
    check("pause_menu: options" in await console_messages(ed), "#options logs")
    await ed.call("send_input", events=[{"type": "key", "key": "escape"}], frames_after=20)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('pause').classList:contains('open'), Engine.timeScale") == [False, 1],
          "Escape again closes it and the game runs")
    await ed.call("send_input", events=[{"type": "key", "key": "escape"}], frames_after=5)
    await click(ed, n, "#resume")
    check(await lua(ed, "return Engine.timeScale") == [1], "#resume closes the menu")

    # the gamepad (p4-input): Start opens, the d-pad reaches every button, A presses, B closes
    is_open = f"return {doc_lua(n)}:getElementById('pause').classList:contains('open'), Engine.timeScale"
    await pad(ed, "start", frames_after=20)
    check(await lua(ed, is_open) == [True, 0] and await focused(ed, n) == "resume", "Start opens the menu on #resume")
    check(await walk(ed, n, "dpad_down") == ["options", "restart", "to-menu"], "the d-pad goes through the buttons")
    await pad(ed, "dpad_up")
    await ed.console_since(None, limit=1)
    await pad(ed, "a", frames_after=10)
    check("pause_menu: restart" in await console_messages(ed), "A presses #restart")
    await pad(ed, "b", frames_after=20)
    check(await lua(ed, is_open) == [False, 1], "B closes the menu and the game runs")
    await pad(ed, "start", frames_after=20)
    await pad(ed, "a", frames_after=20)
    check(await lua(ed, is_open) == [False, 1], "A on #resume closes it")


async def hud_before(ed: Editor, n: str, first: bool) -> None:
    # the shield takes damage first: 80 empties the 50 of shield and leaves 70 health
    # (the spec's damage(30) assumed no shield)
    check(await lua(ed, f"local hud = script('{n}'); hud:damage(80); return hud.model.health, hud.model.shield") == [70, 0], "damage(80)")
    await ed.call("wait", frames=30)
    fill, track = await lua(ed, f"""
        local d = {doc_lua(n)}
        local fill = d:getElementById('health-fill')
        return fill:getBoundingClientRect().width, fill.parentElement:getBoundingClientRect().width""")
    check(abs(fill - 0.7 * track) <= 1.0, f"#health-fill is 70% of its track: {fill} of {track}")
    before = int((await lua(ed, f"return {doc_lua(n)}:getElementById('ammo').textContent"))[0])
    await lua(ed, f"local hud = script('{n}'); hud:fire(); hud:fire(); hud:fire()")
    await ed.call("wait", frames=2)
    after = int((await lua(ed, f"return {doc_lua(n)}:getElementById('ammo').textContent"))[0])
    check(after == before - 3, f"three shots: #ammo {before} -> {after}")


async def hud_after(ed: Editor, n: str) -> None:
    await lua(ed, f"script('{n}'):reload()")
    await ed.call("wait", frames=2)
    check(await lua(ed, f"local d = {doc_lua(n)}; return d:getElementById('ammo').textContent, d:getElementById('reserve').textContent")
          == ["30", "/ 87"], "reload refills the magazine from the reserve")
    timer = (await lua(ed, f"return {doc_lua(n)}:getElementById('timer').textContent"))[0]
    check(re.fullmatch(r"\d\d:\d\d", timer.strip()) is not None, f"#timer shows mm:ss: {timer!r}")


async def inventory_before(ed: Editor, n: str, first: bool) -> None:
    check(await lua(ed, f"return #{doc_lua(n)}:querySelectorAll('.grid > .slot')") == [24], "24 slots (the data-for template is no element)")
    await ed.call("send_input", events=[{"type": "mouse", "action": "drag", "target": ".slot:nth-child(1)",
                                         "to_target": ".slot:nth-child(6)", "document": n, "steps": 8}], frames_after=5)
    check(await lua(ed, f"local m = {doc_lua(n)}:getDataModel('inventory'); return m.items[1].id, m.items[6].id") == ["", "sword"],
          "dragging slot 1 onto slot 6 swaps the entries")
    # the drag carried its slot in event.dataTransfer: no drag state is left in the script or the document
    check(await lua(ed, f"local d = {doc_lua(n)}; return #d:querySelectorAll('.htmlui-drag-ghost'), UIDrag.isActive()") == [0, False],
          "the ghost went with the drag")
    await hover(ed, n, ".slot:nth-child(3)", frames_after=30)
    check(await lua(ed, f"local t = {doc_lua(n)}.tooltip; return t.visible, t.text:find('Health Potion') ~= nil") == [True, True],
          "hovering a slot shows its tooltip (el:setTooltip with a function)")


async def inventory_after(ed: Editor, n: str) -> None:
    await hover(ed, n, ".slot:nth-child(7)", frames_after=30)
    check(await lua(ed, f"return {doc_lua(n)}.tooltip.visible") == [False], "an empty slot has no tooltip")
    # the gamepad (p4-input): the first key focuses the first slot, the d-pad moves through the grid
    # (the tooltip follows), A picks an item up and puts it down, B cancels, the close button is reachable
    slot = (f"local d = {doc_lua(n)}; local s = d:querySelectorAll('.grid > .slot'); "
            f"for i, e in ipairs(s) do if e:matches(':focus') then return i end end; return 0")
    await lua(ed, f"local f = {doc_lua(n)}:querySelector(':focus'); if f then f:blur() end")
    await pad(ed, "dpad_right")
    check(await lua(ed, slot) == [1], "the first gamepad key focuses the first slot")
    await pad(ed, "dpad_right", "dpad_down")
    check(await lua(ed, slot) == [8], "right then down: slot 8 (six columns)")
    await ed.call("wait", frames=30)
    check(await lua(ed, f"local t = {doc_lua(n)}.tooltip; return t.visible, t.text:find('Vault Key') ~= nil") == [True, True],
          "the tooltip follows the focus (after its delay)")
    model = f"{doc_lua(n)}:getDataModel('inventory')"
    await pad(ed, "a")
    check(await lua(ed, f"return {model}.picked") == [8], "A picks the key up")
    await pad(ed, "dpad_left", "a")
    check(await lua(ed, f"local m = {model}; return m.picked, m.items[7].id, m.items[8].id") == [0, "key", ""],
          "A on slot 7 puts it down there")
    await pad(ed, "a", "b")
    check(await lua(ed, f"return {model}.picked, {doc_lua(n)}.visible") == [0, True], "B cancels a pick")
    rows = [await lua(ed, slot)]
    for button in ("dpad_up", "dpad_right", "dpad_right", "dpad_right", "dpad_right", "dpad_right", "dpad_up"):
        await pad(ed, button)
        rows.append((await lua(ed, slot))[0])
    check(await focused(ed, n) == "close", f"the close button above the grid is reachable: {rows}")

    await click(ed, n, "#close")
    check(await lua(ed, f"return {doc_lua(n)}.visible") == [False], "#close hides the document")
    await pad(ed, "back", frames_after=10)
    check(await lua(ed, f"return {doc_lua(n)}.visible") == [True], "View shows it again")
    await pad(ed, "b", frames_after=10)
    check(await lua(ed, f"return {doc_lua(n)}.visible") == [False], "B closes it")


async def settings_before(ed: Editor, n: str, first: bool) -> None:
    model = f"{doc_lua(n)}:getDataModel('settings')"
    check(await lua(ed, f"local m = {model}; return m.music, m.vsync, m.quality, {doc_lua(n)}:getElementById('quality').value")
          == [60, True, "high", "high"], "the controls show the model's values")
    await click(ed, n, "#vsync")
    check(await lua(ed, f"return {model}.vsync") == [False], "the V-Sync switch toggles the model")
    await click(ed, n, "#tab-audio")
    check(await lua(ed, f"local d = {doc_lua(n)}; return d:getElementById('tab-audio').classList:contains('active'), "
                        f"d:getElementById('tab-graphics').classList:contains('active'), "
                        f"d:getElementById('panel-audio').classList:contains('active')") == [True, False, True], "the Audio tab")
    x, y, w, h = await lua(ed, f"local r = {doc_lua(n)}:getElementById('music'):getBoundingClientRect(); return r.x, r.y, r.width, r.height")
    thumb = 18.0
    start = x + thumb / 2 + (w - thumb) * 0.60
    end = x + thumb / 2 + (w - thumb) * 0.25
    await ed.call("send_input", events=[{"type": "mouse", "action": "drag", "x": start, "y": y + h / 2, "to": [end, y + h / 2],
                                         "steps": 8}], frames_after=20)
    music = (await lua(ed, f"return {model}.music"))[0]
    check(abs(music - 25) <= 3, f"the music slider dragged to its 25% point: {music}")


async def settings_after(ed: Editor, n: str) -> None:
    await click(ed, n, "#tab-controls")
    await click(ed, n, "#bind-jump")
    check("Press a key" in (await lua(ed, f"return {doc_lua(n)}:getElementById('bind-jump').textContent"))[0], "the binding waits for a key")
    await ed.call("send_input", events=[{"type": "key", "key": "j"}], frames_after=5)
    check(await lua(ed, f"return {doc_lua(n)}:getDataModel('settings').keys.jump") == ["J"], "the next key is the binding")
    await ed.console_since(None, limit=1)
    await click(ed, n, "#apply")
    applied = [m for m in await console_messages(ed) if m.startswith("settings: apply")]
    check(applied and "music=" in applied[0] and "jump=J" in applied[0] and "vsync=false" in applied[0], f"#apply logs the values: {applied}")
    await click(ed, n, "#reset")
    check(await lua(ed, f"local m = {doc_lua(n)}:getDataModel('settings'); return m.music, m.vsync, m.keys.jump") == [60, True, "Space"],
          "#reset restores the defaults")

    # the gamepad (p4-input): every control of a tab is reachable, ranges adjust, checkboxes toggle,
    # selects open and choose, B is Back
    model = f"{doc_lua(n)}:getDataModel('settings')"
    await lua(ed, f"{doc_lua(n)}:getElementById('tab-graphics'):focus()")
    await pad(ed, "a", frames_after=10)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('panel-graphics').classList:contains('active')") == [True],
          "A selects the Graphics tab")
    check(await walk(ed, n, "dpad_right") == ["tab-audio", "tab-controls", "tab-gameplay"], "the d-pad goes along the tabs")
    down = await walk(ed, n, "dpad_down")
    for want in ("resolution", "quality", "fov", "vsync", "bloom"):
        check(want in down, f"the d-pad reaches #{want}: {down}")
    check(any(i in down for i in ("back", "reset", "apply")), f"and the footer: {down}")
    footer = await walk(ed, n, "dpad_left") + await walk(ed, n, "dpad_right")
    for want in ("back", "reset", "apply"):
        check(want in footer, f"the footer's #{want} is reachable: {footer}")
    up = await walk(ed, n, "dpad_up")
    check(up and up[-1].startswith("tab-"), f"up reaches the tabs again: {up}")

    await lua(ed, f"{doc_lua(n)}:getElementById('fov'):focus()")
    fov = (await lua(ed, f"return {model}.fov"))[0]
    await pad(ed, "dpad_right", "dpad_right", frames_after=10)
    check(await lua(ed, f"return {model}.fov") == [fov + 2], "the d-pad adjusts the focused range")
    await lua(ed, f"{doc_lua(n)}:getElementById('bloom'):focus()")
    await pad(ed, "a", frames_after=10)
    check(await lua(ed, f"return {model}.bloom") == [False], "A toggles the focused switch")
    await lua(ed, f"{doc_lua(n)}:getElementById('quality'):focus()")
    await pad(ed, "a", "dpad_down", "a", frames_after=10)
    check(await lua(ed, f"return {model}.quality") == ["ultra"], "A opens the select, the d-pad moves, A chooses")
    await ed.console_since(None, limit=1)
    await pad(ed, "b", frames_after=10)
    check("settings: back" in await console_messages(ed), "B is Back")


async def dialogue_before(ed: Editor, n: str, first: bool) -> None:
    await ed.call("wait", frames=240)
    check(await lua(ed, f"return #{doc_lua(n)}:querySelectorAll('.choice')") == [3], "the line is typed and its choices shown")


async def dialogue_after(ed: Editor, n: str) -> None:
    before = (await lua(ed, f"return {doc_lua(n)}:getElementById('line').textContent"))[0]
    await ed.console_since(None, limit=1)
    await click(ed, n, ".choice:nth-child(2)")
    check("dialogue: choice 2" in await console_messages(ed), "the second choice logs")
    after = (await lua(ed, f"return {doc_lua(n)}:getElementById('line').textContent"))[0]
    check(after != before, f"#line changed: {after!r}")
    # Space completes the new line, a click goes on to the next one
    await ed.call("send_input", events=[{"type": "key", "key": "space"}], frames_after=2)
    full = (await lua(ed, f"return {doc_lua(n)}:getElementById('line').textContent"))[0]
    check(full.startswith("Good eye.") and full.endswith("light low."), f"Space completes the line: {full!r}")


async def shop_before(ed: Editor, n: str, first: bool) -> None:
    # the descriptions are clamped to two lines (-webkit-line-clamp, p6-line-clamp): the Ember Blade's runs on past them,
    # the Iron Helm's is one line
    long, scroll, short = await lua(ed, f"local d = {doc_lua(n)}; local a = d:querySelector('.card:nth-child(1) .stats'); "
                                        f"return a.offsetHeight, a.scrollHeight, d:querySelector('.card:nth-child(5) .stats').offsetHeight")
    check(abs(long - 2 * short) <= 1 and scroll > long + short / 2,
          f"a long description shows two lines of more ({long}px of {scroll}px), a short one one ({short}px)")
    check(await lua(ed, f"local d = {doc_lua(n)}; return d:querySelector('.card:nth-child(3) .buy'):hasAttribute('disabled'), "
                        f"d:querySelector('.card:nth-child(2) .buy'):hasAttribute('disabled')") == [True, False],
          "a card the purse cannot pay for has its button disabled")
    await click(ed, n, ".card:nth-child(2) .buy", frames_after=20)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('confirm').classList:contains('open')") == [True], "Buy opens #confirm")


async def shop_after(ed: Editor, n: str) -> None:
    await click(ed, n, "#qty-plus")
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('qty').textContent") == ["2"], "#qty-plus")
    await click(ed, n, "#qty-minus")
    await ed.console_since(None, limit=1)
    await click(ed, n, "#confirm-buy", frames_after=60)
    check("shop: bought axe x1" in await console_messages(ed), "the purchase logs")
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('gold').textContent") == ["520"], "#gold counted down by the price")
    # (read through the computed display: a card data-if hides keeps its last offsetHeight in the engine)
    await click(ed, n, "#chip-potions", frames_after=20)
    shown = await lua(ed, f"local names = {{}}; for _, c in ipairs({doc_lua(n)}:querySelectorAll('.card')) do "
                          f"if c.style.display ~= 'none' then names[#names + 1] = c:querySelector('.name').textContent end end; "
                          f"return table.concat(names, ', ')")
    check(shown == ["Health Potion, Golden Apple"], f"the Potions chip shows the potions: {shown}")

    # the gamepad (p4-input): the dialog keeps the d-pad on its buttons, A presses, B cancels
    await click(ed, n, ".card:nth-child(7) .buy", frames_after=20)
    is_open = f"return {doc_lua(n)}:getElementById('confirm').classList:contains('open')"
    check(await lua(ed, is_open) == [True] and await focused(ed, n) == "confirm-buy", "Buy opens the dialog on its Buy")
    await pad(ed, "dpad_right")
    check(await focused(ed, n) == "qty-plus", "right wraps to the stepper (the disabled minus is skipped)")
    await pad(ed, "a", frames_after=10)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('qty').textContent") == ["2"], "A on + adds one")
    await pad(ed, "dpad_down", "dpad_down", "dpad_down")
    check(await focused(ed, n) in ("qty-minus", "qty-plus", "confirm-cancel", "confirm-buy"), "the d-pad stays in the dialog")
    await pad(ed, "b", frames_after=20)
    check(await lua(ed, is_open) == [False], "B cancels the dialog")
    check(await lua(ed, f"return {doc_lua(n)}:querySelectorAll('.card')[7]:querySelector('.buy'):matches(':focus')") == [True],
          "the focus goes back to the card's Buy")


async def toasts_before(ed: Editor, n: str, first: bool) -> None:
    await ed.call("wait", frames=30)
    check(await lua(ed, f"return #{doc_lua(n)}:querySelectorAll('.toast')") == [3], "three toasts (the samples removed)")


async def toasts_after(ed: Editor, n: str) -> None:
    await ed.call("wait", game_seconds=4)
    check(await lua(ed, f"return #{doc_lua(n)}:querySelectorAll('.toast')") == [0], "they go after 3 seconds")
    await lua(ed, f"local t = script('{n}'); for i = 1, 7 do t:show('Toast ' .. i, 'error', 3) end")
    await ed.call("wait", frames=30)
    check(await lua(ed, f"return #{doc_lua(n)}:querySelectorAll('.toast')") == [5], "at most five")


async def loading_before(ed: Editor, n: str, first: bool) -> None:
    percent = int((await lua(ed, f"return {doc_lua(n)}:getElementById('percent').textContent"))[0].rstrip("%"))
    check(45 <= percent <= 55, f"#percent after 1.5 s of 3: {percent}")


async def loading_after(ed: Editor, n: str) -> None:
    await ed.console_since(None, limit=1)
    await ed.call("wait", game_seconds=2)
    check("loading: done" in await console_messages(ed), "loading: done")
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('press-key').offsetHeight > 0") == [True], "#press-key shows")
    await ed.call("send_input", events=[{"type": "key", "key": "space"}], frames_after=2)
    check("loading: continue" in await console_messages(ed), "a key continues")


async def video_state(ed: Editor, n: str) -> dict:
    tree = await ed.call("inspect_ui", entity=n, selector="#backdrop", depth=0)
    return tree["elements"][0]["media"]


async def video_menu_before(ed: Editor, n: str, first: bool) -> None:
    m = await video_state(ed, n)
    check(not m["paused"] and m["loop"] and m["muted"] and m["readyState"] == 4, f"the background video plays: {m}")
    check((m["videoWidth"], m["videoHeight"]) == (640, 360) and abs(m["duration"] - 4.0) < 1e-3, f"menu-loop.webm: {m}")
    # reloaded when play is held, then 90 frames of 1/60 s: 1.5 s, frame 45 (frame-exact)
    check(abs(m["currentTime"] - 1.5) < 1e-9 and m["frame"] == 45, f"frame-exact on the UI clock: {m}")
    await hover(ed, n, "#play", frames_after=20)


async def video_menu_after(ed: Editor, n: str) -> None:
    await console_messages(ed)
    await click(ed, n, "#motion", frames_after=5)
    m = await video_state(ed, n)
    check(m["paused"], f"the motion toggle pauses the video: {m}")
    label, pressed = await lua(ed, f"local d = {doc_lua(n)}; return d:getElementById('motion-label').textContent, "
                                   f"d:getElementById('motion'):getAttribute('aria-pressed')")
    check((label, pressed) == ("Play background", "true"), f"the toggle follows the pause event: {label!r} {pressed!r}")
    check("video_menu: background paused" in await console_messages(ed), "logged")
    await ed.call("wait", frames=30)
    check((await video_state(ed, n))["currentTime"] == m["currentTime"], "paused: the time stays")
    await click(ed, n, "#motion", frames_after=5)
    t0 = (await video_state(ed, n))["currentTime"]
    label = (await lua(ed, f"return {doc_lua(n)}:getElementById('motion-label').textContent"))[0]
    check(label == "Pause background", f"playing again: {label!r}")
    # 3 s on: past the 4 s end, the loop wraps (no ended, no pause)
    await ed.call("wait", frames=180)
    m = await video_state(ed, n)
    check(not m["paused"] and not m["ended"] and abs(m["currentTime"] - (t0 + 3.0) % m["duration"]) < 1e-6,
          f"looped: {t0} + 3 s -> {m['currentTime']}")
    await ed.call("send_input", events=[{"type": "key", "key": "down"}, {"type": "key", "key": "enter"}], frames_after=10)
    check(await focused(ed, n) == "play" and "video_menu: play" in await console_messages(ed), "Down focuses Play, Enter presses it")
    await pad(ed, "dpad_down", "a", frames_after=10)
    check(await focused(ed, n) == "continue" and "video_menu: continue" in await console_messages(ed), "the d-pad and A do the same")


def view_lua(n: str, element_id: str) -> str:
    return f"{doc_lua(n)}:getElementById('{element_id}').itemsView"


async def server_browser_before(ed: Editor, n: str, first: bool) -> None:
    view = view_lua(n, "servers")
    check(await lua(ed, f"local v = {view}; return v.count, v:getState().pool <= v:getState().poolBound") == [10000, True],
          "ten thousand servers, a pool of rows")
    await click(ed, n, ".htmlui-header-cell[data-column=name]", frames_after=10)
    check(await lua(ed, f"local v = {view}; local s = v:getState(); return s.sortKey, s.sortDirection") == ["name", "asc"],
          "a header click sorts by its column")
    await click(ed, n, '.htmlui-row[aria-rowindex="2"]', frames_after=10)
    name = await lua(ed, f"return {doc_lua(n)}:getDataModel('browser').selected.name")
    check(name[0] != "" and await lua(ed, f"return {doc_lua(n)}:getElementById('detail-name').textContent") == name,
          f"the selected server fills the details: {name}")


async def server_browser_after(ed: Editor, n: str) -> None:
    view = view_lua(n, "servers")
    await click(ed, n, "#region-eu", frames_after=10)
    regions = await lua(ed, f"local m = {doc_lua(n)}:getDataModel('browser'); return m.servers[1].region, m.servers[#m.servers].region, m.shown")
    check(regions[:2] == ["EU", "EU"] and 0 < regions[2] < 10000, f"the Europe chip filters: {regions}")
    await ed.console_since(None, limit=1)
    # (the chip has the focus: a row click gives it back to the table)
    await click(ed, n, '.htmlui-row[aria-rowindex="2"]', frames_after=10)
    await ed.call("send_input", events=[{"type": "key", "key": "down"}, {"type": "key", "key": "enter"}], frames_after=5)
    check(await lua(ed, f"return {view}.activeIndex") == [3], "Down moves the selection")
    check(any(m.startswith("servers: joining ") for m in await console_messages(ed)), "Enter joins the selected server")
    await hover(ed, n, ".ping-head", frames_after=40)
    check(await lua(ed, f"local t = {doc_lua(n)}.tooltip; return t.visible, t.text:find('round trip') ~= nil") == [True, True],
          "the Ping header's rich tooltip (tooltip=\"#ping-help\")")
    await hover(ed, n, '.htmlui-row[aria-rowindex="4"] .name', frames_after=40)
    check(await lua(ed, f"local t = {doc_lua(n)}.tooltip; return t.visible, t.text:find('players') ~= nil") == [True, True],
          "a row's title tooltip")


async def chat_log_before(ed: Editor, n: str, first: bool) -> None:
    view = view_lua(n, "messages")
    check(await lua(ed, f"local v = {view}; local first, last = v:getVisibleRange(); return v.count, last") == [120, 120],
          "120 messages, the view at the end (anchor=\"end\")")
    await lua(ed, f"{doc_lua(n)}:getElementById('draft').value = 'gg **all**'")
    await ed.console_since(None, limit=1)
    await click(ed, n, "#send", frames_after=10)
    check("chat: sent gg **all**" in await console_messages(ed), "Send posts the draft")
    check(await lua(ed, f"local v = {view}; local first, last = v:getVisibleRange(); return v.count, last") == [121, 121],
          "the new message shows at the end")
    check(await lua(ed, f"return {view}:getRow(121):querySelector('strong').textContent") == ["all"], "its Markdown is rendered")


async def chat_log_after(ed: Editor, n: str) -> None:
    view = view_lua(n, "messages")
    await lua(ed, f"{doc_lua(n)}:getElementById('messages').scrollTop = 0")
    await ed.call("wait", game_seconds=4.5)
    values = await lua(ed, f"local v = {view}; local first, last = v:getVisibleRange(); return v.count, first, last")
    check(values[0] == 122 and values[1] == 1 and values[2] < 122, f"scrolled up, a new message leaves the view where it is: {values}")


async def quest_log_before(ed: Editor, n: str, first: bool) -> None:
    view = view_lua(n, "tree")
    check(await lua(ed, f"return {view}.count") == [8], "the two first chapters open: 3 chapters and 5 quests")
    await click(ed, n, '.htmlui-row[aria-rowindex="3"]', frames_after=10)
    check((await lua(ed, f"return {doc_lua(n)}:getElementById('journal').textContent:find('vault key') ~= nil"))[0],
          "the selected quest's notes in the journal")


async def quest_log_after(ed: Editor, n: str) -> None:
    view = view_lua(n, "tree")
    await click(ed, n, '.htmlui-row[aria-rowindex="1"]', frames_after=10)
    check((await lua(ed, f"return {doc_lua(n)}:getElementById('journal').textContent:find('Journal') ~= nil"))[0],
          "a chapter shows the journal's first page (<markdown src>)")
    await ed.call("send_input", events=[{"type": "key", "key": "left"}], frames_after=5)
    check(await lua(ed, f"return {view}.count, {view}:isExpanded('main')") == [5, False], "Left closes the chapter")
    await ed.call("send_input", events=[{"type": "key", "key": "right"}, {"type": "key", "key": "right"}], frames_after=5)
    check(await lua(ed, f"return {view}.activeIndex, {view}:getNode({view}.activeIndex).key") == [2, "map"],
          "Right opens it, then goes to its first quest")
    await ed.console_since(None, limit=1)
    await pad(ed, "a", frames_after=10)
    check("quests: finished The old map" in await console_messages(ed), "A finishes the quest")
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('progress').textContent") == ["2 of 6 done"], "the progress counts it")


async def blank_before(ed: Editor, n: str, first: bool) -> None:
    check(await lua(ed, f"return {doc_lua(n)}:querySelector('h1').textContent") == ["Blank"], "the heading")


async def blank_after(ed: Editor, n: str) -> None:
    pass


STEPS = {name: (globals()[f"{name}_before"], globals()[f"{name}_after"]) for name in NAMES}


# ---------------------------------------------------------------------------------------- per example

def _make_tests(name: str) -> None:
    entry = ENTRIES[name]

    async def files_in_project(ed: Editor):
        copy = await wait_project_copy(ed, ed.project)
        for rel in example_files(name):
            check((copy / rel).is_file(), f"{rel} is not in the project copy")
            check((copy / rel).read_bytes() == (LIBRARY / rel).read_bytes(), f"{rel} differs")
        check(json.loads((copy / "examples.json").read_text())["examples"][NAMES.index(name)]["name"] == name, "the copy's manifest")
        # the resource jasper://examples/html-ui/<name>/<name>.html is the same file
        r = await ed.session.read_resource(f"jasper://examples/html-ui/{name}/{entry['html']}")
        check(r.contents[0].text == (LIBRARY / name / entry["html"]).read_text(encoding="utf-8"), "the resource")

    async def compat_clean():
        with tempfile.TemporaryDirectory(prefix="jasper-example-check-") as tmp:
            project = Path(tmp) / "project"
            project.mkdir()
            shutil.copy(harness.default_fixture() / "project.yaml", project / "project.yaml")
            copy_example(project, name, LIBRARY)
            files = [html_rel(name), f"assets/ui/{name}/{entry['css']}"]
            r = subprocess.run(harness.editor_command() + ["check", "--project", str(project), "--ui", "--files", *files,
                                                           "--format", "json"], cwd=project, capture_output=True, text=True, timeout=120)
            data = json.loads(r.stdout)
            check(r.returncode == 0 and data["findings"] == [], f"check --ui --files: exit {r.returncode}, {data['findings']}")
            check(data["checked"].get("ui", 0) >= 1, f"checked {data['checked']}")

    async def edit_mode(ed: Editor):
        cursor = await console_cursor(ed)
        state = await ensure_scene(ed, name)
        values = await lua(ed, f"""
            local doc = UIDocument(scene, entity('{name}'))
            local errors, warnings, lines = 0, 0, {{}}
            for _, d in ipairs(doc:getDiagnostics()) do
                if d.severity == 'error' then errors = errors + 1 end
                if d.severity == 'warning' then warnings = warnings + 1 end
                if d.severity ~= 'info' then lines[#lines + 1] = d.text end
            end
            return doc.loaded, errors, warnings, table.concat(lines, '\\n')""", allow_edit_mode=True, scene=state["scene"])
        check(values[:3] == [True, 0, 0], f"{name} in edit mode: loaded {values[0]}, {values[1]} errors, {values[2]} warnings\n{values[3]}")
        shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 0, "scene": state["scene"]})
        check(not shot.is_error and shot.images, f"edit-mode screenshot: {shot.text[:300]}")
        (ed.scratch.root / f"{name}.edit.png").write_bytes(shot.images[0][0])
        check(not pngdiff.flat(pngdiff.decode(shot.images[0][0])), f"{name}: the edit-mode Game view is flat")
        problems = await errors_since(ed, cursor)
        check(not problems, f"{name}: Console problems in edit mode: {problems[:5]}")

    async def play_golden(ed: Editor):
        state = await ensure_scene(ed, name)
        before, after = STEPS[name]
        await ed.call("configure_game_view", aspect="project", scale=1, maximize_on_play=True)
        captures = []
        try:
            for run in range(2):
                cursor = await console_cursor(ed)
                await ed.call("play", action="start", scene=state["scene"], fixed_fps=60, auto_pause=True)
                if name in RESTART_CSS:
                    await lua(ed, f"{doc_lua(name)}:reload{{mode = \"rebuild\"}}")
                await ed.call("wait", frames=90)
                await before(ed, name, run == 0)
                captures.append(await capture(ed, f"{name}.play{run}"))
                if run == 0:
                    await after(ed, name)
                errors = await errors_since(ed, cursor)
                check(not errors, f"{name}: Console problems while playing: {errors[:5]}")
                await ed.call("play", action="stop")
        finally:
            await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)

        first, second = captures
        same, fraction, diff = pngdiff.compare(first, second, CHANNEL_TOLERANCE, RUNS_MAX_FRACTION)
        if not same:
            (ed.scratch.root / f"{name}.runs.diff.png").write_bytes(diff or b"")
            ed.keep = True
        check(same, f"{name}: two runs differ in {fraction * 100:.2f}% of the pixels ({ed.scratch.root / (name + '.runs.diff.png')})")

        width, height = first[0], first[1]
        check(width >= GOLDEN_SIZE[0] and height >= GOLDEN_SIZE[1],
              f"the Game view shows the canvas at {width}x{height}, smaller than {GOLDEN_SIZE[0]}x{GOLDEN_SIZE[1]}")
        golden_path = GOLDEN_DIR / f"{name}.png"
        if UPDATE_GOLDEN:
            GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
            golden_path.write_bytes(pngdiff.encode(pngdiff.downscale(first, *GOLDEN_SIZE)))
            print(f"     [golden] wrote {golden_path.relative_to(harness.REPO)}: look at it")
            return
        check(golden_path.is_file(), f"no golden {golden_path} (JASPER_HTMLUI_UPDATE_GOLDEN=1 writes it)")
        golden = pngdiff.load(golden_path)
        ok, fraction, diff = pngdiff.compare(pngdiff.downscale(first, *COMPARE_SIZE), pngdiff.downscale(golden, *COMPARE_SIZE),
                                             CHANNEL_TOLERANCE, MAX_FRACTION)
        if not ok:
            actual = ed.scratch.root / f"{name}.actual.png"
            actual.write_bytes(pngdiff.encode(pngdiff.downscale(first, *GOLDEN_SIZE)))
            (ed.scratch.root / f"{name}.diff.png").write_bytes(diff or b"")
            ed.keep = True
            fail(f"{name}: {fraction * 100:.2f}% of the pixels differ from the golden (at most {MAX_FRACTION * 100:.0f}%): "
                 f"{actual}, {ed.scratch.root / (name + '.diff.png')}")

    async def asserts(ed: Editor):
        state = await ensure_scene(ed, name)
        before, _ = STEPS[name]
        try:
            await ed.call("play", action="start", scene=state["scene"], fixed_fps=60, auto_pause=True)
            if name in RESTART_CSS:
                await lua(ed, f"{doc_lua(name)}:reload{{mode = \"rebuild\"}}")
            await ed.call("wait", frames=90)
            await before(ed, name, True)
            r = await ed.call("assert_ui", entity=name, checks=entry["checks"], viewports=entry["viewports"])
            check(r["passed"], f"{name}: assert_ui failed: {json.dumps([c for c in r['checks'] if not c['passed']])[:3000]}")
            # one deliberate failure: a label 20px wide does not fit (blank has no text_fits check: its h1)
            selector = next((c["selector"] for c in entry["checks"] if c["type"] == "text_fits"),
                            entry["checks"][0]["selector"]).split(",")[0].strip()
            # the first match on screen (a hidden one, in an inactive tab, is not checked); width does not apply
            # to an inline box: it becomes an inline-block
            shown = next(e for e in (await ed.call("inspect_ui", entity=name, selector=selector, depth=0,
                                                   styles=["display"]))["elements"] if "hidden" not in e.get("flags", []))
            selector = shown["ref"]
            shrink = "e.style.width = '20px'" + ("; e.style.display = 'inline-block'" if shown["styles"]["display"] == "inline" else "")
            await lua(ed, f"local e = {doc_lua(name)}:querySelector({json.dumps(selector)}); {shrink}")
            await ed.call("wait", frames=2)
            raw = await ed.call_raw("assert_ui", {"entity": name, "checks": [{"type": "text_fits", "selector": selector}],
                                                  "screenshot": "on_failure", "max_width": 0})
            result = raw.value
            check(not raw.is_error and not result["passed"], f"{name}: the shrunk label fails text_fits")
            failures = result["checks"][0]["failures"]
            check(failures[0]["ref"], "the failure names the element")
            check(raw.images, "a failed assertion returns an image")
            img = pngdiff.decode(raw.images[0][0])
            # the image is the Game view cropped around the failures: image.canvas_rect is the part of
            # the canvas it shows; each failing element (the label and others its width squeezes) is outlined
            cx, cy, cw, _ = result["image"]["canvas_rect"]
            scale = img[0] / cw
            rects = [((f["rect"][0] - cx) * scale, (f["rect"][1] - cy) * scale, f["rect"][2] * scale, f["rect"][3] * scale)
                     for f in failures]
            reds = [(x, y) for y in range(img[1]) for x in range(img[0])
                    if img[2][(y * img[0] + x) * 4] > 200 and img[2][(y * img[0] + x) * 4 + 1] < 60 and img[2][(y * img[0] + x) * 4 + 2] < 60]
            check(reds, "red outline pixels in the image")
            for x, y in reds:
                check(any(rx - 4 <= x <= rx + rw + 4 and ry - 4 <= y <= ry + rh + 4 for rx, ry, rw, rh in rects),
                      f"a red pixel at {x},{y} outside the reported rects {[f['rect'] for f in failures]} (image {result['image']})")
        finally:
            await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)

    for suffix, fn in (("files_in_project", files_in_project), ("compat_clean", compat_clean), ("edit_mode", edit_mode),
                       ("play_golden", play_golden), ("asserts", asserts)):
        fn.__name__ = fn.__qualname__ = f"test_{name}_{suffix}"
        fn.__doc__ = f"{name}: {suffix.replace('_', ' ')} (spec §5.4)"
        globals()[fn.__name__] = fn


for _name in NAMES:
    _make_tests(_name)
