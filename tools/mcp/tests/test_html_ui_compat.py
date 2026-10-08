"""Standard HTML/CSS through the compat layer (phase 2, p2-ua, spec 5.5), end to end.

The golden page main-menu.html (engine/tests/htmlui/fixtures/compat/pages: Google Fonts link,
:root variables, a glass panel, gradients, Lucide icons, `transition: all .3s ease`, keyframes)
is copied into the project as assets/ui/compat/ with its fonts and import manifest (the
manifest's paths rebased under ui/compat/), shown by a UI Document, and played:
- the document loads with no [HTML UI] error, and the Console has the per-document summary line
  `[HTML UI] ui/compat/main-menu.html: 0 errors, ...`;
- the screenshot shows the page: the dark gradient background, the violet "Continue" button;
- editing main-menu.css (write_file, like an external editor) reloads the document through its
  dependencies: the new background color shows in the next screenshots.

Runs on a scratch copy of the fixture project (canvas 1280 x 720).
"""

import asyncio
import json
import shutil
import time
from pathlib import Path

from harness import REPO, Editor
from htmlui_pixels import Shot, near

TIMEOUT = 900

S: dict = {}

PAGES = REPO / "engine" / "tests" / "htmlui" / "fixtures" / "compat" / "pages"
UI_DIR = "ui/compat"
DOC = f"assets/{UI_DIR}/main-menu.html"
CSS_PATH = f"assets/{UI_DIR}/main-menu.css"

# canvas points of the page (golden/compat/reference/main-menu.png)
P_CONTINUE = (700, 342)      # the "Continue" button: violet gradient (Chromium: 94, 66, 232)
P_CORNER = (20, 700)         # the body gradient, bottom left (Chromium: 38, 34, 82)
NEW_BACKGROUND = (185, 28, 28)


def copy_pages(project: Path) -> None:
    """The pages with their assets under assets/ui/compat/, the import manifest at the project's
    assets/htmlui-imports/manifest.json with its paths rebased (the runtime reads it there)."""
    target = project / "assets" / UI_DIR
    shutil.copytree(PAGES, target, dirs_exist_ok=True)
    manifest = json.loads((PAGES / "htmlui-imports" / "manifest.json").read_text())
    for entry in manifest["entries"]:
        for key in ("path", "license"):
            if entry.get(key):
                entry[key] = f"{UI_DIR}/{entry[key]}"
        entry["files"] = [f"{UI_DIR}/{f}" for f in entry.get("files", [])]
    imports = project / "assets" / "htmlui-imports"
    imports.mkdir(parents=True, exist_ok=True)
    (imports / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (target / "htmlui-imports" / "manifest.json").unlink()


async def game_shot(ed: Editor, label: str) -> Shot:
    shot = await ed.call_raw("screenshot", {"view": "game", "scene": S["scene"], "max_width": 1280})
    ed.check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    (ed.scratch.root / f"html-ui-compat-{label}.png").write_bytes(shot.images[0][0])
    return Shot(shot.images[0][0], shot.value)


async def wait_for(ed: Editor, label: str, predicate, what: str, seconds: float = 3.0) -> Shot:
    """Game view screenshots until predicate(shot) holds; fails after `seconds` and at least three
    screenshots."""
    deadline = time.monotonic() + seconds
    attempts = 0
    while True:
        shot = await game_shot(ed, label)
        attempts += 1
        if predicate(shot):
            return shot
        if time.monotonic() > deadline and attempts >= 3:
            ed.fail(f"{label}: {what} after {seconds} s (continue {shot.px(*P_CONTINUE)}, corner {shot.px(*P_CORNER)})")
        await asyncio.sleep(0.2)


def violet(p) -> bool:
    return p[2] > 180 and p[2] > p[0] + 80 and p[2] > p[1] + 100


def dark_background(p) -> bool:
    return max(p[:3]) < 110 and p[2] > p[0]


async def html_ui_messages(ed: Editor) -> list[str]:
    entries = await ed.console_since(None, limit=2000)
    return [e.get("message", "") for e in entries if "[HTML UI]" in e.get("message", "")]


async def test_page_loads_and_matches(ed: Editor):
    copy_pages(ed.project)
    await ed.console_since(None, limit=1)
    made = await ed.call("create_scene", name="HtmlUiCompat", type="UI")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="MainMenu")
    S["menu"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["menu"], values={"uidocument": {"filename": DOC}})
    await ed.call("save_scene", scene=S["scene"], path="scenes/HtmlUiCompat")

    await ed.call("play", action="start", scene=S["scene"])
    S["playing"] = True
    shot = await wait_for(ed, "play", lambda s: violet(s.px(*P_CONTINUE)), "no violet Continue button")
    ed.check(dark_background(shot.px(*P_CORNER)), f"the page background at {P_CORNER}: {shot.px(*P_CORNER)}")

    messages = await html_ui_messages(ed)
    errors = [m for m in messages if "error:" in m or "[HTMLUI-GEN-0006]" in m]
    ed.check(not errors, "HTML UI errors:\n" + "\n".join(errors[:20]))
    summary = [m for m in messages if f"{UI_DIR}/main-menu.html: 0 errors" in m]
    ed.check(summary, "no '[HTML UI] ui/compat/main-menu.html: 0 errors' summary line; [HTML UI] lines:\n" +
             "\n".join(messages[:40]))


async def test_css_edit_reloads_through_dependencies(ed: Editor):
    css = (ed.project / "assets" / UI_DIR / "main-menu.css").read_text()
    old = "background: linear-gradient(135deg, #0f0c29 0%, #302b63 50%, #24243e 100%);"
    ed.check(old in css, "main-menu.css has the body gradient")
    await ed.call("write_file", path=CSS_PATH, content=css.replace(old, "background: #b91c1c;"))
    shot = await wait_for(ed, "edited", lambda s: near(s.px(*P_CORNER), NEW_BACKGROUND, 16),
                          f"the background is not {NEW_BACKGROUND}")
    ed.check(violet(shot.px(*P_CONTINUE)), f"the Continue button after the reload: {shot.px(*P_CONTINUE)}")
    messages = await html_ui_messages(ed)
    ed.check(not [m for m in messages if "error:" in m], "HTML UI errors after the reload")


async def test_stop(ed: Editor):
    if S.get("playing"):
        await ed.call("play", action="stop")
