"""S46 AC18 (docs/specs/S46-pixel-art-cleanup.md R42, R43): the Pixel Art Cleanup window, driven like a
person (xdotool in the harness's Xvfb, aimed where find_ui_item and editor_status.ui.pixel_art_cleanup
say things are drawn now):

1. The window opens on an AI sprite (a run_command payload) and its badge reads Fixable.
2. With Lines on, dragging grid line x:3 by 2 source pixels writes recipe.moved, shown by Copy Recipe.
3. Hovering a Lost Colors row draws the canvas highlight item; a lint row centres the view on its spot.
4. Ctrl+Z inside the window undoes the drag; outside it does nothing to the window.
5. Write is one project-history step.
6. Screenshots (the Compare view, the Frames tab with onion skin) go to $JE_EVIDENCE_DIR (else the
   scratch directory).
"""

from __future__ import annotations

import asyncio
import json
import os
import random
from pathlib import Path

import pngdiff
import ui_input
from harness import Editor

CI = True
TIMEOUT = 900
EDITOR = {"create": True, "name": "PixelArtCleanupWindow", "xvfb": "harness", "ui_probe": True}
WINDOW = "Pixel Art Cleanup"
ART = "assets/art"
HERO = f"{ART}/hero.png"
ACCENT = f"{ART}/accent.png"
SHEET = f"{ART}/sheet.png"
STATE: dict = {}

COLORS = [(26, 28, 44, 255), (239, 125, 87, 255), (255, 205, 117, 255), (41, 54, 111, 255), (37, 200, 80, 255)]


def sprite(size: int = 24, accent: bool = False) -> list[list[tuple[int, int, int, int]]]:
    rows: list[list[tuple[int, int, int, int]]] = []
    for y in range(size):
        row = []
        for x in range(size):
            edge = x in (3, size - 4) or y in (3, size - 4)
            inside = 3 < x < size - 4 and 3 < y < size - 4
            if edge and 3 <= x <= size - 4 and 3 <= y <= size - 4:
                row.append(COLORS[0])
            elif inside:
                row.append(COLORS[1] if (x * 3 + y) % 7 else COLORS[2])
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    # a lone stray pixel: the orphans lint has a spot
    rows[size // 2][size // 2] = COLORS[3]
    if accent:
        for y in range(5, 9):
            for x in range(5, 9):
                rows[y][x] = COLORS[4]
    return rows


def upscale(rows, scale: int = 8, noise: int = 3, seed: int = 11) -> bytes:
    rng = random.Random(seed)
    height = len(rows)
    width = len(rows[0])
    rgba = bytearray()
    for y in range(height * scale):
        for x in range(width * scale):
            r, g, b, a = rows[y // scale][x // scale]
            if a == 0:
                rgba += bytes((0, 0, 0, 0))
            else:
                rgba += bytes((min(255, max(0, r + rng.randint(-noise, noise))),
                               min(255, max(0, g + rng.randint(-noise, noise))),
                               min(255, max(0, b + rng.randint(-noise, noise))), 255))
    return pngdiff.encode(width * scale, height * scale, bytes(rgba))


def sheet_png(frames: int = 4, size: int = 14, scale: int = 6, gap: int = 24) -> bytes:
    """A row of frames on transparency: the sheet detector boxes each one."""
    rows = [sprite(size) for _ in range(frames)]
    frame_px = size * scale
    width = frames * frame_px + (frames + 1) * gap
    height = frame_px + 2 * gap
    rgba = bytearray(width * height * 4)
    for f, row_set in enumerate(rows):
        left = gap + f * (frame_px + gap)
        for y in range(frame_px):
            for x in range(frame_px):
                r, g, b, a = row_set[y // scale][(x + f * 2) // scale] if (x + f * 2) // scale < size else (0, 0, 0, 0)
                at = (gap + y) * width * 4 + (left + x) * 4
                rgba[at:at + 4] = bytes((r, g, b, a))
    return pngdiff.encode(width, height, bytes(rgba))


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root) / "pixel-art-cleanup"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def shot(ed: Editor, name: str) -> None:
    await ed.screenshot("editor", evidence(ed) / f"{name}.png", focus=False, max_width=0)


def ui_of(ed: Editor) -> ui_input.Ui:
    if "ui" not in STATE:
        STATE["ui"] = ui_input.for_editor(ed)
    return STATE["ui"]


async def window_state(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]["pixel_art_cleanup"]


async def until_state(ed: Editor, predicate, what: str, tries: int = 300) -> dict:
    state: dict = {}
    for _ in range(tries):
        state = await window_state(ed)
        if predicate(state):
            return state
        await ui_input.frames(ed, 2)
    ed.fail(f"{what}: {state}")


def badge(item: dict) -> str:
    return item["label"].split("##", 1)[0]


async def open_on(ed: Editor, path: str, verdict: str | None = None) -> dict:
    await ed.call("run_command", id="window.pixelArtCleanup", payload=path)
    ui = ui_of(ed)
    # the badge tells a run is in; without one the verdict is whatever came
    for _ in range(600):
        item = await ui.find("###cleanup_verdict", WINDOW, frames=4)
        if item and not badge(item).startswith("...") and (verdict is None or badge(item).startswith(verdict)):
            return item
        await ui_input.frames(ed, 2)
    ed.fail(f"the verdict badge for {path}: {item}")


async def close_window(ed: Editor, ui: ui_input.Ui) -> None:
    """The close button of its tab, as a person closes a docked window: the #CLOSE item on the tab bar
    at the top of the window's rect"""
    focused = await ed.call("focus_window", window="pixel_art_cleanup")
    rect = focused["rect"]
    for _ in range(60):
        closes = [item for item in (await ed.call("find_ui_item", label="#CLOSE")).get("items", [])
                  if item.get("visible", True) and rect["x"] <= item["rect"]["x"] <= rect["x"] + rect["width"]
                  and rect["y"] - 48 <= item["rect"]["y"] <= rect["y"] + 40]
        if closes:
            await ui.click(ui.centre(closes[0]["rect"]))
            return
        await ui_input.frames(ed, 2)
    ed.fail(f"no close button on the window's tab above {rect}")


async def test_badge_reads_fixable(ed: Editor):
    (ed.project / ART).mkdir(parents=True, exist_ok=True)
    (ed.project / HERO).write_bytes(upscale(sprite()))
    await ed.call("tool_groups", enable=["pixel_art", "editor_ux"])
    await ed.call("sync_files", paths=[HERO], full=False)
    item = await open_on(ed, HERO, "Fixable")
    ed.check(badge(item).startswith("Fixable"), f"badge: {item['label']}")
    await shot(ed, "1-opened")


async def source_view(ed: Editor) -> None:
    await ui_of(ed).click_revealed("Source", WINDOW)
    await until_state(ed, lambda s: s["view"] == "source", "the Source view is shown")


async def test_drag_grid_line_moves_it(ed: Editor):
    ui = ui_of(ed)
    await source_view(ed)
    await ui.click_revealed("Lines", WINDOW)
    state = await until_state(ed, lambda s: s["zoom"] > 0 and len(s["xs"]) > 4, "the grid is drawn")
    STATE["moved_from"] = state["xs"][3]
    x = state["image_min"][0] + state["xs"][3] * state["zoom"]
    canvas = await ui.find("##cleanup_canvas", WINDOW)
    rect = canvas["rect"]
    y = rect["y"] + rect["height"] / 2
    drag_px = max(4.0, 2.0 * state["zoom"])
    await ui.drag(ui.at({"x": x, "y": y}), ui.at({"x": x + drag_px, "y": y}), steps=10)
    state = await until_state(ed, lambda s: s["recipe_moved"] == 1, "the drag wrote recipe.moved")
    await ui.click_revealed("Copy Recipe", WINDOW)
    state = await until_state(ed, lambda s: s["copied_recipe"], "Copy Recipe copied")
    recipe = json.loads(state["copied_recipe"])
    moved = {entry[0]: entry[1] for entry in recipe["moved"]}
    ed.check("x:3" in moved, f"recipe.moved has x:3: {recipe['moved']}")
    ed.check(abs(moved["x:3"] - (STATE["moved_from"] + drag_px / state["zoom"])) <= 0.75,
             f"x:3 went where it was dragged: {moved['x:3']} from {STATE['moved_from']} by {drag_px / state['zoom']}")


async def test_undo_inside_the_window_only(ed: Editor):
    ui = ui_of(ed)
    # the mouse is over the canvas and the window focused (the drag just ended there)
    await ui.key("ctrl+z")
    await until_state(ed, lambda s: s["recipe_moved"] == 0, "Ctrl+Z inside the window undoes the drag")
    # the same undo pressed elsewhere leaves the window's stack alone
    canvas = await ui.find("##cleanup_canvas", WINDOW)
    x = canvas["rect"]["x"] + canvas["rect"]["width"] * 0.25
    y = canvas["rect"]["y"] + canvas["rect"]["height"] * 0.5
    state = await window_state(ed)
    start_x = state["image_min"][0] + state["xs"][3] * state["zoom"]
    await ui.drag(ui.at({"x": start_x, "y": y}), ui.at({"x": start_x + 16, "y": y}), steps=10)
    await until_state(ed, lambda s: s["recipe_moved"] == 1, "the line is dragged again")
    await ed.call("focus_window", window="hierarchy")
    await ui_input.frames(ed, 4)
    await ui.key("ctrl+z")
    await ui_input.frames(ed, 6)
    state = await window_state(ed)
    ed.check_eq(state["recipe_moved"], 1, "Ctrl+Z outside the window leaves its edit")
    # closed while it had the focus: its edits are out of Ctrl+Z's reach too (and it says unfocused)
    await ed.call("focus_window", window="pixel_art_cleanup")
    await until_state(ed, lambda s: s["focused"], "the window has the focus")
    await close_window(ed, ui)
    state = await until_state(ed, lambda s: not s["open"], "the window closes")
    ed.check(not state["focused"], f"a closed window is not focused: {state}")
    await ui_input.frames(ed, 4)
    await ui.key("ctrl+z")
    await ui_input.frames(ed, 6)
    state = await window_state(ed)
    ed.check_eq(state["recipe_moved"], 1, "Ctrl+Z after closing the window leaves its edit")
    await ed.call("run_command", id="window.pixelArtCleanup")
    await until_state(ed, lambda s: s["open"] and s["zoom"] > 0, "the window opens again")
    await ui.move(ui.at({"x": x, "y": y}))
    await ed.call("focus_window", window="pixel_art_cleanup")
    await ui_input.frames(ed, 4)


async def test_lost_colors_highlight_and_lint_click(ed: Editor):
    ui = ui_of(ed)
    # a palette without the green accent: its cells are lost
    (ed.project / ACCENT).write_bytes(upscale(sprite(accent=True), seed=4))
    await ed.call("sync_files", paths=[ACCENT], full=False)
    palette = ["#%02x%02x%02x" % c[:3] for c in COLORS[:4]]
    written = await ed.call_raw("fix_pixel_art", {"source": ACCENT, "palette": palette, "output": True, "force": True})
    ed.check(not written.is_error, written.text)
    output = f"{ART}/accent_clean.png"
    await ed.call("sync_files", paths=[output], full=False)
    await open_on(ed, output)
    await ui.click_revealed("Lost Colors", WINDOW)
    row = await ui.find("###cleanup_lost_0", WINDOW, frames=120)
    ed.check(row is not None, "the Lost Colors tab lists the accent")
    await ui.move(ui.centre(row["rect"]))
    await ui_input.frames(ed, 3)
    highlight = await ui.find("##cleanup_highlight", WINDOW, frames=30)
    ed.check(highlight is not None, "hovering the row draws the canvas highlight")
    state = await window_state(ed)
    ed.check(state["highlight"].startswith("lost:"), f"highlight: {state['highlight']}")
    await shot(ed, "2-lost-highlight")
    # Lints: a row that has spots pans the view to its first spot
    await ui.click_revealed("Lints", WINDOW)
    state = await until_state(ed, lambda s: s["spots"], "a lint has spots")
    spot = state["spots"][0]
    await ui.click_revealed(f"###cleanup_lint_{spot['lint']}", WINDOW)
    state = await until_state(ed, lambda s: abs(s["center"][0] - spot["x"]) <= 1.0 and abs(s["center"][1] - spot["y"]) <= 1.0,
                              f"the view centres on spot ({spot['x']}, {spot['y']})")
    ed.check(state["highlight"].startswith("spot:"), f"the spot flashes: {state['highlight']}")


async def test_write_is_one_project_step(ed: Editor):
    ui = ui_of(ed)
    await open_on(ed, HERO, "Fixable")
    before = await ed.call("get_undo_history", scope="project", limit=200)
    await ui.click_revealed("Write", WINDOW)
    for _ in range(600):
        if (ed.project / f"{ART}/hero_clean.png").exists():
            break
        await ui_input.frames(ed, 2)
    ed.check((ed.project / f"{ART}/hero_clean.png").exists(), "Write wrote the output")
    await shot(ed, "3-written")
    history = await ed.call("get_undo_history", scope="project", limit=200)
    known = {entry["id"] for entry in before["entries"]}
    added = [entry for entry in history["entries"] if entry["id"] not in known]
    ed.check_eq(len(added), 1, f"Write is one project-history step: {[entry['name'] for entry in added]}")
    ed.check("Pixel Art Cleanup (hero_clean.png)" in added[0]["name"], f"its name: {added[0]['name']}")


async def test_compare_and_frames_screenshots(ed: Editor):
    ui = ui_of(ed)
    await ui.click_revealed("Compare", WINDOW)
    await until_state(ed, lambda s: s["view"] == "compare", "the Compare view is shown")
    await shot(ed, "4-compare")
    # a sheet opens on its Frames tab (boxes, animations, the player with its onion skin)
    (ed.project / SHEET).write_bytes(sheet_png())
    await ed.call("sync_files", paths=[SHEET], full=False)
    await open_on(ed, SHEET)
    await ui.click_revealed("Frames", WINDOW)
    await ui.find("###cleanup_play", WINDOW, frames=120)
    await shot(ed, "5-frames-onion-skin")
    ed.check((evidence(ed) / "5-frames-onion-skin.png").exists(), "the Frames screenshot was written")


async def project_state(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]["project_window"]


async def until_project(ed: Editor, predicate, what: str, tries: int = 300) -> dict:
    state: dict = {}
    for _ in range(tries):
        state = await project_state(ed)
        if predicate(state):
            return state
        await ui_input.frames(ed, 2)
    ed.fail(f"{what}: {state}")


def tree_node(state: dict, path: str) -> dict | None:
    return next((node for node in state["tree"] if node["path"] == path), None)


def entry(state: dict, name: str) -> list | None:
    return next((item["rect"] for item in state["entries"] if item["name"] == name), None)


def menu(state: dict, label: str) -> list | None:
    return next((item["rect"] for item in state["context_menu"] if item["label"] == label), None)


async def show_art_folder(ed: Editor, ui: ui_input.Ui) -> dict:
    await ed.call("focus_window", window="project")
    state = await until_project(ed, lambda s: s["drawn"] and tree_node(s, "assets"), "the tree shows assets")
    if not tree_node(state, "assets/art"):
        x, y = tree_node(state, "assets")["arrow"]
        await ui.click(ui.at((x, y)))
        state = await until_project(ed, lambda s: tree_node(s, "assets/art"), "assets opens in the tree")
    await ui.click(ui.centre(tree_node(state, "assets/art")["rect"]))
    return await until_project(ed, lambda s: entry(s, "hero_clean.png") and entry(s, "hero.png"), "the art folder lists hero and its output")


async def test_project_window_entries(ed: Editor):
    """R43, R32: an image offers Pixel Art Cleanup..., an output also Re-run; the output's Source changed badge."""
    ui = ui_of(ed)
    state = await show_art_folder(ed, ui)

    await ui.click(ui.centre(entry(state, "hero.png")), button="3")
    state = await until_project(ed, lambda s: s["context_menu"], "the context menu opens")
    ed.check(menu(state, "Pixel Art Cleanup...") is not None, f"an image offers Pixel Art Cleanup: {[i['label'] for i in state['context_menu']]}")
    ed.check(menu(state, "Re-run Pixel Art Cleanup") is None, "a source is not an output")
    await ui.key("Escape")
    await ui_input.frames(ed, 4)

    state = await project_state(ed)
    await ui.click(ui.centre(entry(state, "hero_clean.png")), button="3")
    state = await until_project(ed, lambda s: s["context_menu"], "the output's menu opens")
    rerun = menu(state, "Re-run Pixel Art Cleanup")
    ed.check(rerun is not None, f"an output offers Re-run: {[i['label'] for i in state['context_menu']]}")
    await ed.console_since("last")
    await ui.click(ui.centre(rerun))
    await asyncio.sleep(0)
    for _ in range(400):
        lines = await ed.console_since("last")
        if any("re-ran" in str(line) for line in lines):
            break
        await ui_input.frames(ed, 2)
    else:
        ed.fail("Re-run Pixel Art Cleanup reported on the Console")

    # an .aseprite output carries its recipe in its .meta: the same badge
    written = await ed.call_raw("fix_pixel_art", {"source": HERO, "format": "aseprite", "output": True})
    ed.check(not written.is_error, written.text)
    await ed.call("sync_files", paths=[f"{ART}/hero_clean.aseprite"], full=False)
    await show_art_folder(ed, ui)

    # the source changes: the outputs' badges go stale
    (ed.project / HERO).write_bytes(upscale(sprite(), seed=99))
    await ed.call("sync_files", paths=[HERO], full=False)
    for _ in range(200):
        if await ed.ui_rect("project.generatedStale.hero_clean.png", timeout=0.5):
            break
        await ui_input.frames(ed, 2)
    else:
        ed.fail("the output's Source changed badge shows")
    for _ in range(200):
        if await ed.ui_rect("project.generatedStale.hero_clean.aseprite", timeout=0.5):
            break
        await ui_input.frames(ed, 2)
    else:
        ed.fail("the .aseprite output's Source changed badge shows")
    await shot(ed, "6-source-changed-badge")


async def test_inspector_section(ed: Editor):
    """R43, R33: the Inspector's section of the source: Open in Cleanup Window, Enabled as one project step."""
    ui = ui_of(ed)
    state = await show_art_folder(ed, ui)
    await ui.key("Escape")
    state = await project_state(ed)
    await ui.click(ui.centre(entry(state, "hero.png")))
    item = await ui.reveal("Open in Cleanup Window", "Inspector", frames=600)
    await ui.click(ui.centre(item["rect"]))
    await until_state(ed, lambda s: s["source"] == HERO, "Open in Cleanup Window opens the source")
    # Enabled writes import.pixelCleanup as one project step
    meta = ed.project / f"{HERO}.meta"
    await ed.call("focus_window", window="inspector")
    await ui.frames(4)
    enabled = await ui.reveal("Enabled", "Inspector", frames=300)
    await ui.click(ui.centre(enabled["rect"]))
    for _ in range(200):
        if "pixelCleanup" in meta.read_text():
            break
        await ui_input.frames(ed, 2)
    ed.check("pixelCleanup" in meta.read_text(), "Enabled writes import.pixelCleanup")
    await ed.call("undo", scope="project")
    for _ in range(200):
        if "pixelCleanup" not in meta.read_text():
            break
        await ui_input.frames(ed, 2)
    ed.check("pixelCleanup" not in meta.read_text(), "one project undo takes it back")


async def recipe_where(ed: Editor, predicate, what: str, tries: int = 40) -> dict:
    """Copy Recipe until the copied recipe satisfies predicate"""
    ui = ui_of(ed)
    recipe: dict = {}
    for _ in range(tries):
        await ui.click_revealed("Copy Recipe", WINDOW)
        text = (await window_state(ed))["copied_recipe"]
        if text:
            recipe = json.loads(text)
            if predicate(recipe):
                return recipe
        await ui_input.frames(ed, 2)
    ed.fail(f"{what}: {recipe}")


async def test_palette_lock_toggle(ed: Editor):
    """R42 Palette: a swatch's lock toggle records the colour in recipe.palette.locked"""
    ui = ui_of(ed)
    await open_on(ed, HERO)
    await ui.click_revealed("Palette", WINDOW)
    button = await ui.reveal("###cleanup_lock_1", WINDOW, frames=120)
    await ui.click(ui.centre(button["rect"]))
    recipe = await recipe_where(ed, lambda r: 1 in r["palette"].get("locked", []), "the lock is in the recipe")
    ed.check(recipe["palette"]["colors"], f"a lock keeps concrete colours: {recipe['palette']}")


async def test_add_to_palette_from_lost_colors(ed: Editor):
    """R42 Lost Colors: Add to Palette appends the lost colour; its cells are no longer lost"""
    ui = ui_of(ed)
    output = f"{ART}/accent_clean.png"
    await open_on(ed, output)
    await ui.click_revealed("Lost Colors", WINDOW)
    await ui.find("###cleanup_lost_0", WINDOW, frames=120)
    before = await recipe_where(ed, lambda r: r["palette"]["colors"], "the output's palette")
    count = len(before["palette"]["colors"])
    await ui.click_revealed("Add to Palette", WINDOW)
    recipe = await recipe_where(ed, lambda r: len(r["palette"]["colors"]) == count + 1, "the colour joins the palette")
    ed.check_eq(len(recipe["palette"]["colors"]), count + 1, "one colour added")
    for _ in range(200):
        if await ui.find("###cleanup_lost_0", WINDOW, frames=2) is None:
            return
        await ui_input.frames(ed, 4)
    ed.fail("the added colour's cells stay lost")


async def test_save_as_import_step(ed: Editor):
    """R43, R33: Save as Import Step writes import.pixelCleanup as one project step"""
    ui = ui_of(ed)
    await open_on(ed, HERO)
    before = await ed.call("get_undo_history", scope="project", limit=200)
    known = {entry["id"] for entry in before["entries"]}
    await ui.click_revealed("Save as Import Step", WINDOW)
    meta = ed.project / f"{HERO}.meta"
    for _ in range(300):
        if "pixelCleanup" in meta.read_text():
            break
        await ui_input.frames(ed, 2)
    ed.check("pixelCleanup" in meta.read_text(), "the .meta has import.pixelCleanup")
    history = await ed.call("get_undo_history", scope="project", limit=200)
    added = [entry for entry in history["entries"] if entry["id"] not in known]
    ed.check_eq(len(added), 1, f"one project-history step: {[entry['name'] for entry in added]}")
    ed.check("Save as Import Step" in added[0]["name"], f"its name: {added[0]['name']}")
    await ed.call("undo", scope="project")
    for _ in range(300):
        if "pixelCleanup" not in meta.read_text():
            return
        await ui_input.frames(ed, 2)
    ed.fail("one undo takes the import step out")


async def test_needs_person_write_asks_first(ed: Editor):
    """R30, R42: Write on needs_person asks through the alert modal; No writes nothing, Yes writes"""
    ui = ui_of(ed)
    # most of the sprite in a colour its palette lacks: lost_share trips, the verdict needs a person
    wide = f"{ART}/lost.png"
    rows = sprite()
    for y in range(4, 20):
        for x in range(4, 20):
            rows[y][x] = COLORS[4]
    (ed.project / wide).write_bytes(upscale(rows, seed=21))
    await ed.call("sync_files", paths=[wide], full=False)
    palette = ["#%02x%02x%02x" % c[:3] for c in COLORS[:4]]
    written = await ed.call_raw("fix_pixel_art", {"source": wide, "palette": palette, "output": True, "force": True})
    ed.check(not written.is_error, written.text)
    output = f"{ART}/lost_clean.png"
    await ed.call("sync_files", paths=[output], full=False)
    await open_on(ed, output, "Needs a Person")
    (ed.project / output).unlink()
    await ed.call("sync_files", paths=[output], full=False)

    async def asked() -> dict:
        for _ in range(120):
            dialog = (await ed.call("editor_status")).get("dialog")
            if dialog and dialog.get("title") == "Write Result Anyway?":
                return dialog
            await ui_input.frames(ed, 2)
        ed.fail("Write on needs_person asks first")

    await ui.click_revealed("Write", WINDOW)
    dialog = await asked()
    ed.check_in("no", dialog.get("choices"), f"{dialog}")
    r = await ed.call_raw("respond_dialog", {"choice": "no"})
    ed.check(not r.is_error, r.text)
    await ui_input.frames(ed, 6)
    ed.check(not (ed.project / output).exists(), "No writes nothing")

    await ui.click_revealed("Write", WINDOW)
    await asked()
    r = await ed.call_raw("respond_dialog", {"choice": "yes"})
    ed.check(not r.is_error, r.text)
    for _ in range(300):
        if (ed.project / output).exists():
            return
        await ui_input.frames(ed, 2)
    ed.fail("Yes writes the output")


async def test_arrow_keys_step_through_lint_spots(ed: Editor):
    """R42 Lints: the arrow keys step through the spots, the view centred on each"""
    ui = ui_of(ed)
    await open_on(ed, HERO)
    await source_view(ed)
    await ui.click_revealed("Lints", WINDOW)
    state = await until_state(ed, lambda s: len(s["spots"]) >= 2 and s["spots"][0]["lint"] == s["spots"][1]["lint"], "a lint with two spots")
    first, second = state["spots"][0], state["spots"][1]

    def at(spot: dict):
        return lambda s: abs(s["center"][0] - spot["x"]) <= 1.0 and abs(s["center"][1] - spot["y"]) <= 1.0

    await ui.click_revealed(f"###cleanup_lint_{first['lint']}", WINDOW)
    await until_state(ed, at(first), "a click centres the first spot")
    await ui.key("Right")
    await until_state(ed, at(second), "Right steps to the next spot")
    await ui.key("Left")
    await until_state(ed, at(first), "Left steps back")


async def boxes_of(ed: Editor) -> list[dict]:
    recipe = await recipe_where(ed, lambda r: isinstance(r.get("sheet"), dict) and r["sheet"].get("frames"), "the sheet's boxes")
    return recipe["sheet"]["frames"]


def box_screen(state: dict, box: dict, fx: float, fy: float) -> dict:
    """window coordinates of a point of a frame box (fx, fy: 0-1 across it) in the Source view"""
    b = box["box"]
    return {"x": state["image_min"][0] + (b["x"] + b["width"] * fx) * state["zoom"],
            "y": state["image_min"][1] + (b["y"] + b["height"] * fy) * state["zoom"]}


async def open_sheet_frames(ed: Editor) -> ui_input.Ui:
    ui = ui_of(ed)
    await open_on(ed, SHEET)
    await ui.click_revealed("Frames", WINDOW)
    await source_view(ed)
    await ui.find("###cleanup_play", WINDOW, frames=120)
    return ui


async def test_frames_boxes_move_resize_add_exclude(ed: Editor):
    """R42 Frames: a box moves and resizes by dragging, a drag on empty space adds one, a double-click excludes"""
    ui = await open_sheet_frames(ed)
    boxes = await boxes_of(ed)
    ed.check_eq(len(boxes), 4, f"four boxes: {boxes}")
    state = await window_state(ed)
    zoom = state["zoom"]

    start = dict(boxes[0]["box"])
    p = box_screen(state, boxes[0], 0.5, 0.5)
    await ui.drag(ui.at(p), ui.at({"x": p["x"] + 6 * zoom, "y": p["y"] + 3 * zoom}), steps=10)
    moved = (await recipe_where(ed, lambda r: r["sheet"]["frames"][0]["box"]["x"] != start["x"], "the box moves"))["sheet"]["frames"][0]["box"]
    ed.check_eq((moved["x"], moved["y"], moved["width"], moved["height"]), (start["x"] + 6, start["y"] + 3, start["width"], start["height"]),
                "moved as a whole")

    last = dict(boxes[3]["box"])
    p = box_screen(state, boxes[3], 1.0, 1.0)
    await ui.drag(ui.at(p), ui.at({"x": p["x"] + 4 * zoom, "y": p["y"] + 2 * zoom}), steps=10)
    resized = (await recipe_where(ed, lambda r: r["sheet"]["frames"][3]["box"]["width"] != last["width"], "the box resizes"))["sheet"]["frames"][3]["box"]
    ed.check_eq((resized["x"], resized["y"], resized["width"], resized["height"]), (last["x"], last["y"], last["width"] + 4, last["height"] + 2),
                "the corner resizes it")

    top = min(b["box"]["y"] for b in boxes)
    ed.check(top * zoom >= 16, f"an empty strip above the boxes: {boxes}")
    a = {"x": state["image_min"][0] + 6 * zoom, "y": state["image_min"][1] + 1 * zoom}
    z = {"x": a["x"] + 24 * zoom, "y": a["y"] + (top - 3) * zoom}
    await ui.drag(ui.at(a), ui.at(z), steps=10)
    added = await recipe_where(ed, lambda r: len(r["sheet"]["frames"]) == 5, "a drag on empty space adds a box")
    box = added["sheet"]["frames"][4]["box"]
    ed.check(abs(box["x"] - 6) <= 1 and abs(box["y"] - 1) <= 1 and abs(box["width"] - 24) <= 2, f"where it was drawn: {box}")
    ed.check(4 in added["sheet"]["animations"][0]["frames"], f"it joins the selected animation: {added['sheet']['animations']}")

    state = await window_state(ed)
    current = added["sheet"]["frames"]
    await ui.double_click(ui.at(box_screen(state, current[1], 0.5, 0.5)))
    excluded = await recipe_where(ed, lambda r: not r["sheet"]["frames"][1]["include"], "a double-click excludes the box")
    ed.check(excluded["sheet"]["frames"][0]["include"], "only that box")
    await shot(ed, "7-frames-edited")


async def test_split_here_and_join_with_next(ed: Editor):
    """R42 Frames: Split Here cuts the animation at the player's frame; Join With Next puts it back"""
    ui = await open_sheet_frames(ed)
    start = (await recipe_where(ed, lambda r: isinstance(r.get("sheet"), dict) and r["sheet"].get("animations"), "the animations"))["sheet"]["animations"]
    ed.check_eq(len(start), 1, f"{start}")
    frames = start[0]["frames"]
    slider = await ui.find("##cleanup_frame", WINDOW, frames=120)
    rect = slider["rect"]
    await ui.click(ui.at(rect, (rect["width"] - 3, rect["height"] / 2)))
    await ui_input.frames(ed, 4)
    await ui.click_revealed("Split Here", WINDOW)
    split = await recipe_where(ed, lambda r: len(r["sheet"]["animations"]) == 2, "Split Here makes two animations")
    # the player's frame belongs to the selected animation: another row cannot split there
    other = await ui.find("Split Here", WINDOW, index=1, frames=60)
    ed.check(other is not None and other.get("enabled") is False, f"Split Here on an unselected row is greyed out: {other}")
    ed.check_eq(split["sheet"]["animations"][0]["frames"], frames[:-1], "the head keeps the frames before")
    ed.check_eq(split["sheet"]["animations"][1]["frames"], frames[-1:], "the tail starts at the player's frame")
    await ui.click_revealed("Join With Next", WINDOW)
    joined = await recipe_where(ed, lambda r: len(r["sheet"]["animations"]) == 1, "Join With Next merges them")
    ed.check_eq(joined["sheet"]["animations"][0]["frames"], frames, "back in order")


async def test_nudge_with_alt_arrows(ed: Editor):
    """R42 Frames: Alt+Arrows nudge the player's frame by one pixel"""
    ui = await open_sheet_frames(ed)
    await until_state(ed, lambda s: s["tab"] == "frames", "the Frames tab is on")
    await ui.click(ui.centre((await ui.find("###cleanup_play", WINDOW, frames=60))["rect"]))
    await ui.click(ui.centre((await ui.find("###cleanup_play", WINDOW, frames=60))["rect"]))
    await ui.key("alt+Right")
    await ui.key("alt+Down")
    recipe = await recipe_where(ed, lambda r: any(f.get("nudge", {}).get("x") == 1 and f.get("nudge", {}).get("y") == 1 for f in r["sheet"]["frames"]),
                                "the nudge is in the recipe")
    nudged = [f for f in recipe["sheet"]["frames"] if f["nudge"]["x"] or f["nudge"]["y"]]
    ed.check_eq(len(nudged), 1, f"one frame moved: {nudged}")


async def test_drag_and_drop_from_the_project_window(ed: Editor):
    """R42: an image dropped from the Project window on the window becomes its source"""
    ui = ui_of(ed)
    await open_on(ed, HERO)
    state = await show_art_folder(ed, ui)
    canvas = await ui.find("##cleanup_canvas", WINDOW)
    ed.check(canvas is not None, "the canvas is drawn")
    await ui.drag(ui.centre(entry(state, "accent.png")), ui.centre(canvas["rect"]), steps=16)
    await until_state(ed, lambda s: s["source"] == ACCENT, "the dropped image becomes the source")
