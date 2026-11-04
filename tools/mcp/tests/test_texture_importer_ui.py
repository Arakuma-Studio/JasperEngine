"""The Inspector's Texture Import Settings with real input (docs/specs/S14-texture-compression.md
R22, AC14): clicking photo.png in the Project window shows its importer; its Mobile tab at Low
quality shows the 8x8 cook in the preview's Source / Cooked split and the per-platform table;
undo restores the settings and the table; the Game view's Simulate platform textures renders the
mobile cooks. xdotool clicks the rects editor_status and find_ui_item report; the screenshots of
each step (the AC14 record) go to build/test-results/texture-importer-ui/.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
from pathlib import Path

from harness import Editor, skip
import ui_input

REPO = Path(__file__).resolve().parents[3]
EDITOR = {"project": REPO / "tools" / "fixtures" / "textures", "xvfb": "harness"}
TIMEOUT = 900
CI = True
SHOTS = REPO / "build" / "test-results" / "texture-importer-ui"
PHOTO = "assets/textures/photo.png"
STATE: dict = {}


def xdo(ed: Editor, *args: str) -> str:
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout.strip()


def window(ed: Editor) -> str:
    if "window" not in STATE:
        ids = xdo(ed, "search", "--name", "JasperEngine").split()
        if not ids:
            ed.fail("no editor window on the harness display")
        STATE["window"] = ids[-1]
        xdo(ed, "windowactivate", "--sync", STATE["window"])
    return STATE["window"]


async def frames(ed: Editor, count: int = 2) -> None:
    await ui_input.frames(ed, count)


async def click_at(ed: Editor, x: float, y: float) -> None:
    """a click at a point in window coordinates (ui_input: frame-paced, never a double click)"""
    window(ed)
    ui = ui_input.for_editor(ed)
    await ui.click(ui.at((x, y)))


async def click(ed: Editor, rect: list) -> None:
    x, y, w, h = rect
    await click_at(ed, x + w / 2, y + h / 2)


async def ui(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]


async def wait_ui(ed: Editor, predicate, what: str, timeout: float = 20.0) -> dict:
    state = None
    for _ in range(int(timeout / 0.25)):
        state = await ui(ed)
        if predicate(state):
            return state
        await ui_input.frames(ed, 2)
    ed.fail(f"{what}; ui: {state}")


def tree_node(state: dict, path: str) -> dict | None:
    return next((node for node in state["project_window"]["tree"] if node["path"] == path), None)


def entry(state: dict, name: str) -> list | None:
    return next((item["rect"] for item in state["project_window"]["entries"] if item["name"] == name), None)


async def item(ed: Editor, label: str, window_name: str | None = None) -> list | None:
    args = {"label": label}
    if window_name:
        args["window"] = window_name
    # find_ui_item's rects are {x, y, width, height} in window coordinates (editor_status gives lists)
    items = [i for i in (await ed.call("find_ui_item", **args))["items"] if i.get("visible", True)]
    if not items:
        return None
    r = items[0]["rect"]
    return [r["x"], r["y"], r["width"], r["height"]]


async def wait_item(ed: Editor, label: str, window_name: str | None = None, timeout: float = 30.0) -> list:
    for _ in range(int(timeout / 0.25)):
        rect = await item(ed, label, window_name)
        if rect:
            return rect
        await ui_input.frames(ed, 2)
    ed.fail(f"no drawn UI item {label!r}")


async def shot(ed: Editor, name: str) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    await ed.screenshot("editor", SHOTS / f"{name}.png", focus=False, max_width=0)


async def cooked(ed: Editor, family: str, label_prefix: str, timeout: float = 300.0) -> dict:
    for _ in range(int(timeout / 0.5)):
        r = await ed.call("get_texture_cook_report", paths=[PHOTO], platforms=[family])
        state = r["textures"][0]["platforms"][family]
        if state["status"] == "cooked" and state.get("label", "").startswith(label_prefix):
            return state
        await asyncio.sleep(0.5)
    ed.fail(f"photo.png never cooked to {label_prefix} for {family}")


async def test_importer_from_the_project_window(ed: Editor):
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness Xvfb")
    await ed.call("tool_groups", enable=["assets_pipeline", "editor_ux"])
    await ed.call("focus_window", window="project")
    # A drawn tree may still be covered by the startup texture/shader progress modal.
    await shot(ed, "0-project-ready")
    state = await wait_ui(ed, lambda s: s["project_window"]["drawn"] and tree_node(s, "assets"), "the tree shows assets")
    if not tree_node(state, "assets/textures"):
        x, y = tree_node(state, "assets")["arrow"]
        await click_at(ed, x, y)
        state = await wait_ui(ed, lambda s: tree_node(s, "assets/textures"), "assets opened in the tree")
    await click(ed, tree_node(state, "assets/textures")["rect"])
    state = await wait_ui(ed, lambda s: entry(s, "photo.png"), "assets/textures shown")
    await click(ed, entry(state, "photo.png"))

    # the Inspector shows the importer of the selected file (its platform tabs), its desktop cook
    await wait_item(ed, "Mobile", "Inspector")
    await cooked(ed, "desktop", "BC1")
    await frames(ed, 6)
    await shot(ed, "1-importer-desktop")

    # the Mobile tab, Low quality: an 8x8 cook, the preview's split and the table follow
    await click(ed, await wait_item(ed, "Mobile", "Inspector"))
    await ed.call("set_texture_import_settings", paths=[PHOTO], settings={"quality": "low"}, platform="mobile")
    state = await cooked(ed, "mobile", "ASTC 8x8")
    await frames(ed, 10)
    await shot(ed, "2-importer-mobile-low")
    ed.check(state["vram_bytes"] > 0, f"{state}")

    # undo: the settings and the table are back (mobile at Normal: 6x6)
    await ed.call("undo", scope="project")
    restored = await ed.call("get_texture_import_settings", path=PHOTO, platform="mobile")
    ed.check(not (restored["stored"] or {}).get("platforms"), f"undo restored the settings: {restored['stored']}")
    await cooked(ed, "mobile", "ASTC 6x6")
    await frames(ed, 10)
    await shot(ed, "3-importer-undo")

    # the Game view renders with the mobile cooks
    r = await ed.call("configure_game_view", simulate_textures="mobile")
    ed.check_eq(r["simulate_textures"], "mobile")
    await ed.call("focus_window", window="game")
    await frames(ed, 20)
    await shot(ed, "4-game-view-simulate-mobile")
    r = await ed.call("configure_game_view", simulate_textures="desktop")
    ed.check_eq(r["simulate_textures"], "desktop")
