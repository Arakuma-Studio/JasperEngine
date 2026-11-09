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

import json
import os
import random
from pathlib import Path

import pngdiff
import ui_input
from harness import Editor

CI = True
TIMEOUT = 900
EDITOR = {"create": True, "name": "PixelArtCleanupWindow", "xvfb": "harness"}
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
