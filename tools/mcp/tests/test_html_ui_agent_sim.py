"""The agent simulation (phase 4, spec §5.7): what a coding agent does with the HTML/CSS UI tools,
end to end, with the typical first attempt of a language model.

The "model" writes a shop screen the way it would for a browser (LLM_HTML / LLM_CSS of
test_html_ui_checks.py: a <script>, an onclick, a Google Fonts link, a remote image, a <canvas>,
CSS habits from the web, and a fixed-width button with a long label), then follows the loop the
guide and the instructions teach:

1. its hook (`check --hook`, as Claude Code runs it after every Write) and `check --ui` report the
   mistakes compiler-style, each with a code and a fix;
2. create_ui_document shows the page (the result's diagnostics are the CLI's), inspect_ui flags the
   overflowing button, assert_ui fails text_fits with the numbers and an outlined image;
3. it fixes the files as the suggestions say, in two rounds (the HTML and Lua first, the remaining
   CSS warnings after reading them): JavaScript moves into the Lua script create_ui_document wrote,
   import_files from_document imports the font and the image and rewrites the URLs, the label wraps;
   each round is reloaded before the next call, once, and reported;
4. check --ui is clean, assert_ui passes, the Game view shows the shop, and in play send_input
   clicks the buttons by selector and the Lua listeners log.

The importers read JASPERENGINE_IMPORT_MIRROR (no network). The mirror starts empty, as if the
network were down when the page first loads (the editor's automatic import then fails and leaves
the page as it is); the files appear before the agent calls import_files.
"""

from __future__ import annotations

import asyncio
import os
import re
import sys
import tempfile
import time
import urllib.parse
from pathlib import Path

import harness
from harness import Editor

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import pngdiff  # noqa: E402
from test_html_ui_checks import (LLM_CSS, LLM_HTML, LLM_TABLE_CONSTRUCTS, TEXT_LINE, check, check_eq, hook_input,  # noqa: E402
                                 line_of, normalized, run_check, run_json, scratch_project, support_row, touch_write,
                                 ui_support, write)

TIMEOUT = 900

S: dict = {}

HTML = "assets/ui/shop_llm.html"
CSS = "assets/ui/shop_llm.css"
LUA = "scripts/ShopLlmUI.lua"
FONT_URL = "https://fonts.googleapis.com/css2?family=Karla:wght@400;700&display=swap"
IMAGE_URL = "https://example.com/sword.png"
KARLA = harness.REPO / "libs" / "imgui" / "misc" / "fonts" / "Karla-Regular.ttf"
# what only a running RmlUi reports (fonts it has not loaded, its own messages): not in check --ui
RUNTIME_ONLY = {"HTMLUI-CSS-0023", "HTMLUI-GEN-0006"}

MIRROR = Path(tempfile.mkdtemp(prefix="jasper-agent-sim-mirror-"))
os.environ["JASPERENGINE_IMPORT_MIRROR"] = str(MIRROR)


def mirror_file(root: Path, url: str, content: bytes) -> None:
    """The file the importers read for `url` (spec §3.2.1: <dir>/<host>/<quote(path-and-query, safe="")>)."""
    parts = urllib.parse.urlsplit(url)
    path_and_query = parts.path + ("?" + parts.query if parts.query else "")
    target = root / parts.hostname / urllib.parse.quote(path_and_query, safe="")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)


def png(width: int, height: int, rgb: tuple) -> bytes:
    return pngdiff.encode(width, height, bytes(list(rgb) + [255]) * (width * height))


def fill_mirror() -> None:
    """The network comes back: the font stylesheet, its two files and the image."""
    regular = "https://fonts.gstatic.com/s/karla/v31/karla-400.ttf"
    bold = "https://fonts.gstatic.com/s/karla/v31/karla-700.ttf"
    css = "".join(f"@font-face {{\n  font-family: 'Karla';\n  font-style: normal;\n  font-weight: {w};\n"
                  f"  src: url({u}) format('truetype');\n}}\n" for w, u in ((400, regular), (700, bold)))
    mirror_file(MIRROR, FONT_URL, css.encode())
    mirror_file(MIRROR, regular, KARLA.read_bytes())
    mirror_file(MIRROR, bold, KARLA.read_bytes())
    mirror_file(MIRROR, IMAGE_URL, png(64, 64, (180, 190, 210)))


# ---------------------------------------------------------------------------- the model's fixes

# Round 1: what the errors say. No JavaScript (the click handlers go to the Lua script), no
# <canvas>, and the long label wraps in a wider button. The remote URLs stay: import_files rewrites them.
FIXED_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Shop</title>
  <link href="https://fonts.googleapis.com/css2?family=Karla:wght@400;700&display=swap" rel="stylesheet">
  <link rel="stylesheet" href="shop_llm.css">
</head>
<body>
  <div class="shop">
    <h1 class="title">Item Shop</h1>
    <div class="grid">
      <div class="card"><span class="sale">-20%</span>
        <img src="https://example.com/sword.png" alt="Sword">
        <h2 class="name">Sword</h2>
        <button class="buy" data-item="sword">Buy</button>
      </div>
      <div class="card">
        <button class="buy-legendary" data-item="legendary">Purchase Legendary Sword of Infinite Destiny</button>
      </div>
    </div>
  </div>
</body>
</html>
"""

FIXED_CSS_1 = LLM_CSS.replace(".buy-legendary { width: 120px; white-space: nowrap; }",
                              ".buy-legendary { width: 240px; white-space: normal; }")

# Round 2: the warnings that remain, replaced by what their suggestions say
FIXED_CSS_2 = (FIXED_CSS_1
               .replace(".card { mix-blend-mode: screen; }", ".card { opacity: 0.95; }")
               .replace("@container (min-width: 400px)", "@media (min-width: 400px)"))

# The click handlers as the guide's "A complete UI script" writes them (bind on load: survives reloads)
FIXED_LUA = """---@class ShopLlmUI : ScriptInstance
local ShopLlmUI = { properties = {} }

function ShopLlmUI:init()
    self.doc = UIDocument(self.scene, self.entity)
    -- created once: bind() adds the same function after every reload (a second add is ignored)
    ---@param event UIEvent
    self.onBuy = function(event)
        local button = event.currentTarget
        if button then
            Log.print("shop_llm: buy " .. button.dataset.item)
        end
    end
    RegisterEvent(self, self.doc.onLoad, "bind")
    self:bind()
end

function ShopLlmUI:bind()
    for _, button in ipairs(self.doc:querySelectorAll(".buy, .buy-legendary")) do
        button:addEventListener("click", self.onBuy)
    end
end

return ShopLlmUI
"""


# ---------------------------------------------------------------------------- helpers

def disk_write(ed: Editor, rel: str, text: str) -> None:
    """The model's own Write tool (not MCP), stamped surely after the previous write."""
    time.sleep(0.02)
    path = ed.project / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def cli_findings(project: Path, *files: str) -> list[dict]:
    with scratch_project(project) as copy:
        _, data = run_json(copy, "--ui", "--files", *files)
    return data["findings"]


def live(diagnostics: list[dict]) -> set[tuple]:
    return {x for x in normalized(diagnostics) if x[4] not in RUNTIME_ONLY}


def reloaded(ext: dict | None, html: str) -> bool:
    """The document was reloaded for this call: by the pick-up (ui_reloaded), or by the editor's poller
    a moment before it (ui_current), which shares the stamps: either way exactly once."""
    ext = ext or {}
    return any(item["html"] == html for item in ext.get("ui_reloaded", [])) or ext.get("ui_current", 0) >= 1


async def load_count(ed: Editor) -> int:
    return (await ed.call("inspect_ui", entity=S["entity"], depth=0, limit=1))["document"]["load_count"]


# ---------------------------------------------------------------------------- 1. the CLI (no editor)

async def test_cli_reports_llm_mistakes():
    with scratch_project() as project:
        write(project, HTML, LLM_HTML)
        write(project, CSS, LLM_CSS)
        code, data = run_json(project, "--ui", "--files", HTML, CSS)
        check_eq(code, 1, "errors exit 1")
        findings = data["findings"]
        html = [f for f in findings if f["path"] == HTML]
        css = [f for f in findings if f["path"] == CSS]
        for needle, severity, code in (("<script", "error", "HTMLUI-HTML-0006"), ("onclick", "error", "HTMLUI-HTML-0005"),
                                       ("fonts.googleapis", "error", "HTMLUI-IMP-0001"), ("example.com", "error", "HTMLUI-IMP-0001"),
                                       ("<canvas", "warning", "HTMLUI-HTML-0002")):
            at = [f for f in html if f["line"] == line_of(LLM_HTML, needle) and f["code"] == code]
            check(at and at[0]["severity"] == severity and at[0].get("suggestion"),
                  f"{needle}: a {severity} {code} with a fix at line {line_of(LLM_HTML, needle)}: {html}")
        remote = [f for f in html if f["code"] == "HTMLUI-IMP-0001"]
        check(all("import_files from_document" in f["suggestion"] for f in remote), f"the fix names import_files: {remote}")

        support = ui_support()
        for kind, name, prop, needle in LLM_TABLE_CONSTRUCTS:
            row = support_row(support, kind, name, prop)
            check(row is not None, f"ui-support has {kind} {name}")
            if row["support"] in ("unsupported", "fallback"):
                expected = True
            elif row["support"] == "approximated":
                expected = row.get("severity") == "warning"
            else:
                expected = False
            line = line_of(LLM_CSS, needle)
            reported = [f for f in css if f["line"] == line]
            check(bool(reported) == expected, f"{kind} {name} ({row['support']}): line {line} reported {reported}, "
                                              f"expected {'a warning' if expected else 'nothing'}")
        codes = {d["code"] for d in support["diagnostics"]}
        check(all(f["code"] in codes for f in findings), f"every code is in ui-support: {[f['code'] for f in findings]}")
        S["cli"] = findings


async def test_hook_feedback_like_claude_code():
    with scratch_project() as project:
        r = run_check(project, "--hook", stdin=hook_input(project, tool="Read"))
        check_eq(r.returncode, 0, f"the baseline run: {r.stderr}")
        touch_write(project / CSS, LLM_CSS)
        r = run_check(project, "--hook", stdin=hook_input(project, CSS))
        warned = [line for line in r.stderr.splitlines() if TEXT_LINE.match(line)]
        check(r.returncode == 2 and warned and all(": warning: " in w and w.startswith(CSS + ":") for w in warned),
              f"Write of the css: exit {r.returncode}, {r.stderr}")
        touch_write(project / HTML, LLM_HTML)
        r = run_check(project, "--hook", stdin=hook_input(project, HTML))
        errors = [line for line in r.stderr.splitlines() if TEXT_LINE.match(line) and ": error: " in line]
        check(r.returncode == 2 and len(errors) >= 4 and all(e.startswith(HTML + ":") for e in errors),
              f"Write of the html: exit {r.returncode}, {r.stderr}")
        check(any("[HTMLUI-HTML-0006]" in e for e in errors) and any("[HTMLUI-HTML-0005]" in e for e in errors), r.stderr)

        # again on the unchanged files: the errors every time, the warnings once
        r = run_check(project, "--hook", stdin=hook_input(project, HTML))
        lines = [line for line in r.stderr.splitlines() if TEXT_LINE.match(line)]
        check(r.returncode == 2 and any(": error: " in line for line in lines), f"the html errors again: {r.stderr}")
        check(not any(line.startswith(CSS + ":") for line in lines), f"the css warnings are not shown again: {r.stderr}")
        r = run_check(project, "--hook", stdin=hook_input(project, CSS))
        check_eq((r.returncode, r.stderr), (0, ""), "the css again: silent")


# ---------------------------------------------------------------------------- 2. the editor loop

async def test_editor_loop(ed: Editor):
    made = await ed.call("create_scene", name="Shop", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("save_scene", scene=S["scene"], path="scenes/Shop")
    # the model's first attempt, written with its own file tools
    disk_write(ed, CSS, LLM_CSS)
    disk_write(ed, HTML, LLM_HTML)
    cli = cli_findings(ed.project, HTML, CSS)
    check(cli, "the CLI reports the mistakes")

    # the page on screen, with the Lua script of the blank template to fill in
    r = await ed.call_raw("create_ui_document", {"scene": S["scene"], "html": HTML, "script": True})
    check(not r.is_error, r.text)
    data = r.structured
    S["entity"] = data["entity"]["id"]
    check_eq(data["files_kept"], [HTML], "the existing html is used")
    check_eq(data["files_written"], [LUA], "only the script is written")
    check_eq(data["script"], {"path": LUA, "attached": True}, "attached")
    check_eq(live(data["diagnostics"]), normalized(cli), "create_ui_document's diagnostics == check --ui (parity)")
    check("[HTMLUI-HTML-0006]" in r.text and "(fix: " in r.text, f"the text lists them compiler-style: {r.text[:600]}")
    S["load_count"] = data["document"]["load_count"]

    # the default tree stops at the cards (depth 3): the whole tree
    r = await ed.call_raw("inspect_ui", {"entity": S["entity"], "depth": 8})
    check(re.search(r"^\s*button\.buy-legendary \"Purchase Legendary Sword of Infinite Destiny\" \[[^\]]+\].*!overflow-x", r.text, re.M),
          f"the outline flags the overflowing button: {r.text}")
    button = next(e for e in r.structured["elements"] if "buy-legendary" in e.get("class", []))
    check("overflow-x" in button["flags"], f"flags: {button}")

    await ed.call("select_scene", scene=S["scene"])
    r = await ed.call_raw("assert_ui", {"entity": S["entity"], "checks": [{"type": "text_fits", "selector": ".buy-legendary"}],
                                        "max_width": 0})
    check(not r.is_error and not r.structured["passed"], f"text_fits fails: {r.text}")
    failure = r.structured["checks"][0]["failures"][0]
    over = re.search(r"\((\d+(?:\.\d+)?)px over\)", failure["message"])
    check(failure["ref"] and over and float(over.group(1)) > 50, f"the ref and the overflow: {failure}")
    check(r.images, "an image of the failure")
    img = pngdiff.decode(r.images[0][0])
    w, h, pixels = img
    reds = [(x, y) for y in range(h) for x in range(w)
            if pixels[(y * w + x) * 4] > 220 and 30 < pixels[(y * w + x) * 4 + 1] < 90 and 20 < pixels[(y * w + x) * 4 + 2] < 80]
    check(reds, "red outline pixels")
    cx, cy, cw, _ = r.structured["image"]["canvas_rect"]
    scale = w / cw
    rx, ry, rw, rh = failure["rect"]
    xs, ys = [p[0] for p in reds], [p[1] for p in reds]
    check(abs(min(xs) - (rx - cx) * scale) <= 3 and abs(max(xs) - (rx + rw - cx) * scale) <= 3 and
          abs(min(ys) - (ry - cy) * scale) <= 3 and abs(max(ys) - (ry + rh - cy) * scale) <= 3,
          f"the red rectangle {min(xs), min(ys), max(xs), max(ys)} covers the button {failure['rect']} (image of {r.structured['image']})")
    (ed.scratch.root / "shop_llm_text_fits.png").write_bytes(r.images[0][0])

    # the editor's automatic import of the two URLs failed (no network yet): it is not retried this session
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        log = " ".join(e["message"] for e in (await ed.call("read_console", types=["error"], limit=500))["entries"])
        if "HTMLUI-IMP-0002" in log and IMAGE_URL in log and "fonts.googleapis.com" in log:
            break
        await asyncio.sleep(0.25)
    else:
        check(False, f"the automatic import did not report its failure: {log[-1500:]}")


async def test_fix_and_reload(ed: Editor):
    # round 1: the errors (JavaScript, canvas) and the layout bug; the click handlers in the Lua script.
    # The three files are written back to back, as one edit: the editor's poller (0.25 s) would reload
    # twice if it ran between them (each reload is still counted once, but the round would be two).
    fill_mirror()
    time.sleep(0.02)
    for rel, text in ((LUA, FIXED_LUA), (CSS, FIXED_CSS_1), (HTML, FIXED_HTML)):
        (ed.project / rel).write_text(text, encoding="utf-8")
    r = await ed.call_raw("import_files", {"from_document": HTML})
    check(not r.is_error, r.text)
    ext = r.structured.get("external_changes")
    check(reloaded(ext, HTML), f"the edits were picked up before import_files ran: {ext}")
    problems = {(p["path"], p.get("code")) for p in ext["problems"]}
    check({(CSS, "HTMLUI-CSS-0002"), (CSS, "HTMLUI-AT-0001")} <= problems, f"the remaining css warnings are reported: {problems}")
    check(not any(code in ("HTMLUI-HTML-0006", "HTMLUI-HTML-0005") for _, code in problems), f"no JavaScript left: {problems}")
    check_eq(sorted(x["from"] for x in r.structured["rewritten"]["replacements"]), sorted([FONT_URL, IMAGE_URL]),
             "the font link and the image are imported and rewritten")
    text = (ed.project / HTML).read_text()
    check("https://" not in text, f"no remote URL left: {text}")
    check((ed.project / r.structured["font_face_css"]).is_file(), f"the @font-face stylesheet: {r.structured}")
    rounds = 2   # the external edit, then import_files' rewrite (an MCP write: reloaded once, not external)

    r = await ed.call_raw("inspect_ui", {"entity": S["entity"]})
    check(r.structured.get("external_changes") is None, f"the rewrite is not reported as external: {r.structured.get('external_changes')}")
    remaining = {(d["path"], d["code"]) for d in r.structured["diagnostics"]}
    check_eq(remaining, {(CSS, "HTMLUI-CSS-0002"), (CSS, "HTMLUI-AT-0001")}, "only the two css warnings remain")
    check(not re.search(r"button\.buy-legendary .*!overflow", r.text), f"the label wraps now: {r.text}")

    # round 2: the warnings, replaced by what their suggestions say
    disk_write(ed, CSS, FIXED_CSS_2)
    r = await ed.call_raw("inspect_ui", {"entity": S["entity"]})
    ext = r.structured.get("external_changes")
    check(reloaded(ext, HTML) and ext["problems"] == [], f"reloaded, no problem left: {ext}")
    rounds += 1
    check_eq(r.structured["document"]["load_count"], S["load_count"] + rounds, "one reload per round")
    check_eq(r.structured["diagnostics"], [], "no diagnostics")
    check_eq(cli_findings(ed.project, HTML, CSS), [], "check --ui is clean")

    r = await ed.call("assert_ui", entity=S["entity"], checks=[
        {"type": "text_fits", "selector": ".buy-legendary"},
        {"type": "within_viewport", "selector": ".card"},
        {"type": "no_overlap", "selector": ".card"},
        {"type": "visible", "selector": ".buy, .buy-legendary", "unobstructed": True}])
    check(r["passed"], f"the fixed layout passes: {r['checks']}")
    font = await ed.call("inspect_ui", entity=S["entity"], selector=".title", styles=["font-family"])
    check("karla" in font["elements"][0]["styles"]["font-family"].lower(), f"the imported font: {font['elements']}")


async def test_screenshot_and_play(ed: Editor):
    await ed.call("select_scene", scene=S["scene"])
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
    check(not shot.is_error and shot.images, shot.text)
    img = pngdiff.decode(shot.images[0][0])
    check(not pngdiff.flat(img), "the Game view is not flat")
    (ed.scratch.root / "shop_llm.png").write_bytes(shot.images[0][0])
    rect = shot.structured["canvas_rect_in_image"]
    canvas = shot.structured["canvas"]
    scale = rect["width"] / canvas["width"]
    cards = (await ed.call("inspect_ui", entity=S["entity"], selector=".card", depth=0))["elements"]
    w, _, pixels = img
    for card in cards:
        x0, y0 = int(rect["x"] + card["box"][0] * scale), int(rect["y"] + card["box"][1] * scale)
        x1, y1 = int(rect["x"] + (card["box"][0] + card["box"][2]) * scale), int(rect["y"] + (card["box"][1] + card["box"][3]) * scale)
        values = [sum(pixels[(y * w + x) * 4:(y * w + x) * 4 + 3]) for y in range(y0, y1) for x in range(x0, x1)]
        mean = sum(values) / len(values)
        variance = sum((v - mean) ** 2 for v in values) / len(values)
        check(variance > 100, f"the card {card['box']} shows something (variance {variance:.0f})")

    await ed.call("play", action="start", scene=S["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=10)
        cursor = (await ed.call("read_console", limit=1))["next_cursor"]
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "click", "target": ".buy-legendary"},
                                                        {"type": "mouse", "action": "click", "target": ".buy"}], "frames_after": 5})
        check(not r.is_error, r.text)
        log = [e["message"] for e in (await ed.call("read_console", since=cursor, limit=200))["entries"]]
        check("shop_llm: buy legendary" in log and "shop_llm: buy sword" in log, f"the Lua listeners ran: {log}")
        problems = (await ed.call("read_console", since=cursor, types=["error", "warning"], limit=200))["entries"]
        check(problems == [], f"no error or warning while playing: {problems}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_cleanup():
    import shutil
    shutil.rmtree(MIRROR, ignore_errors=True)
