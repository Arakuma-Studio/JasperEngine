"""The Title Designer with scenes (docs/specs/S82-title-designer.md R1, R30, R61, AC16, AC28; S81 R6):
dragging the window's document chip into the Hierarchy creates one GameObject with a Title Art
Renderer showing the file (one step "Create Title Art (<name>)"), and a Title Art Renderer's Open in
Title Designer opens the title In Context, its scene behind it and its screen rectangle found.
"""

import asyncio
from pathlib import Path

import harness
from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Title Designer Scene", "xvfb": "harness"}
TIMEOUT = 600

S: dict = {}


def shots_dir() -> Path:
    """Where a failing step leaves its screenshot (copied back with the test results)."""
    d = Path(__file__).resolve().parents[3] / "build" / "test-results" / "w2"
    d.mkdir(parents=True, exist_ok=True)
    return d


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art", "editor_tools", "editor_ux"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    return S


async def focus(ed: Editor, **keys) -> dict:
    return await ed.call("focus_window", window="title_designer", title_designer=keys)


async def top_step(ed: Editor) -> str:
    history = await ed.call("get_undo_history", scope="project", limit=1)
    steps = history.get("steps") or history.get("entries") or []
    if not steps:
        history = await ed.call("get_undo_history", limit=1)
        steps = history.get("steps") or history.get("entries") or []
    return steps[0].get("name", "") if steps else ""


async def test_document_chip_drag(ed: Editor):
    """AC28 (R61): the document chip dragged into the Hierarchy places the shown title."""
    await setup(ed)
    ui = Ui(ed)
    path = "assets/scene/chip.titleart"
    await ed.call("edit_title", path=path, create={"text": "CHIP"}, bake="always")
    await focus(ed, path=path, zoom="fit")
    await ui.frames(4)
    hierarchy = (await ed.call("focus_window", window="hierarchy")).get("rect")
    await ui.frames(2)
    if not hierarchy:
        rows = (await ed.call("find_ui_item", label="Directional Light")).get("items", [])
        ed.check(len(rows) > 0, "the Hierarchy shows the scene")
        r = rows[0]["window_rect"]
        hierarchy = [r["x"], r["y"], r["width"], r["height"]]
    before = harness.find_key(await ed.call("find_entities", component="titleart"), "entities") or []
    # the empty area at the bottom of the Hierarchy (a row would parent it)
    if isinstance(hierarchy, dict):
        hierarchy = [hierarchy["x"], hierarchy["y"], hierarchy["width"], hierarchy["height"]]
    target = ui.centre([hierarchy[0], hierarchy[1] + hierarchy[3] - 60, hierarchy[2], 60])
    await focus(ed)
    await ui.frames(2)
    chip = await ui.find("###docchip")
    ed.check(chip is not None, "the document chip is drawn")
    if chip is None:
        return
    await ui.drag(ui.centre(chip["rect"]), target, steps=16)
    await ui.frames(6)
    after = harness.find_key(await ed.call("find_entities", component="titleart"), "entities") or []
    if len(after) != len(before) + 1:
        (shots_dir() / "chip-drag.png").write_bytes(await ed.screenshot("editor", max_width=0))
    ed.check_eq(len(after), len(before) + 1, f"one GameObject shows the title: {after}")
    ed.check(await top_step(ed) in ("Create Title Art (chip)", "mcp: Create Title Art (chip)"), f"one step: {await top_step(ed)}")


async def test_open_in_context(ed: Editor):
    """AC16 (R1, R30): the renderer's Open in Title Designer opens it In Context: the backdrop is its
    scene without it, and the title sits at the rectangle where the renderer drew it."""
    await setup(ed)
    ui = Ui(ed)
    path = "assets/scene/context.titleart"
    await ed.call("edit_title", path=path, create={"text": "HUD"}, bake="always",
                  ops=[{"op": "set", "layer": "Title", "values": {"style.face.fill.color": "#ff2020"}}])
    made = await ed.call("create_scene", name="ContextHud", type="UI", path="scenes/ContextHud")
    scene = made["scene"]["id"]
    await ed.call("create_ui", scene=scene, root={"type": "image", "name": "Logo", "layout": {"anchor": "center", "size": [480, 270]}})
    await ed.call("add_component", scene=scene, entity="Logo", component="titleart")
    await ed.call("set_properties", scene=scene, entity="Logo", values={"titleart": {"title": path}})
    await ed.call("select_scene", scene=scene)
    await ed.call("select_entities", scene=scene, entities=["Logo"])
    await ed.call("focus_window", window="inspector")
    await ui.frames(6)
    if await ui.find("Open in Title Designer", "Inspector") is None:
        (shots_dir() / "in-context.png").write_bytes(await ed.screenshot("editor", max_width=0))
    await ui.click_revealed("Open in Title Designer", "Inspector")
    state = {}
    for _ in range(60):
        await ui.frames(4)
        state = (await focus(ed))["state"]
        if state.get("in_context", {}).get("snapshot"):
            break
        await asyncio.sleep(0.5)
    ed.check_eq(state.get("path"), path, "the renderer's document is open")
    ed.check_eq(state.get("background"), "in_context", "In Context")
    context = state.get("in_context", {})
    ed.check(context.get("snapshot"), f"the scene behind it was rendered: {context}")
    ed.check(context.get("located"), f"the instance's rectangle was found: {context}")
    x, y, w, h = context.get("rect", [0, 0, 0, 0])
    sw, sh = context.get("screen", [0, 0])
    ed.check(w > 0 and h > 0 and x >= 0 and y >= 0 and x + w <= sw and y + h <= sh, f"a rectangle on its screen: {context}")
    # the centred 480 x 270 image: its rectangle is around the screen's centre
    ed.check(abs((x + w / 2) - sw / 2) < sw * 0.15 and abs((y + h / 2) - sh / 2) < sh * 0.15, f"at the instance's place: {context}")
