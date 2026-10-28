"""S24 AC5, AC15 and AC16, scripted: real mouse and key input (xdotool in the harness's Xvfb,
1600x900, default layout) checked through MCP, with screenshots.

- AC5: Esc during a drag puts the values back and adds no step: the move gizmo in the Scene
  view, an Inspector float drag, a terrain sculpt stroke.
- AC15: the Undo History panel: a click jumps to a row, "(Start)" reverts everything, Forward
  redoes one, the filter and the Show Selection Changes toggle, the memory footer.
- AC16: the Play Mode Changes window opens on a Stop from the editor with the Inspector's
  play-time edits checked; Keep Selected applies the checked ones as one step; closing the
  window discards.

Positions come from the editor: the controls where find_ui_item says they are drawn now, the
Scene view from the scene screenshot's view_rect_in_window. Every action is verified through MCP.
Screenshots go to $JE_EVIDENCE_DIR (else the scratch directory).
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
from pathlib import Path

from harness import Editor, skip
import ui_input

EDITOR = {"xvfb": "harness", "create": True, "name": "Undo UI"}
CI = True
TIMEOUT = 900

# find_ui_item labels: the Inspector's Transform X fields (a box selected)
POSITION_X = "##input_x_position"
SCALE_X = "##input_x_scale"
# the Terrain window's Raise or Lower Terrain brush
TERRAIN_RAISE_BRUSH = "##terrain_raise"
# the Undo History toolbar
UNDO_HISTORY = "Undo History"
UNDO_BACK = "##UndoHistoryBack"
UNDO_FORWARD = "##UndoHistoryForward"
UNDO_FILTER = "##UndoHistoryFilter"
UNDO_SHOW_SELECTION = "Show Selection Changes"
UNDO_START = "(Start)"
# the Play Mode Changes window: a row's checkbox is labelled with its property
PLAY_MODE_CHANGES = "Play Mode Changes"
# the main toolbar's Play button (ICON_FA_PLAY)
TOOLBAR_PLAY = "\uf04b##MainPlay"

S: dict = {}


Ui = ui_input.Ui   # frame-paced xdotool input (tools/mcp/ui_input.py)


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root)
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        # a test that failed mid-drag left the button down
        S["ui"].xdo("mouseup", "1")
        S["ui"].xdo("key", "Escape")
        return S
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness's Xvfb")
    S.clear()
    S["ed"] = ed
    S["ui"] = Ui(ed)
    S["out"] = evidence(ed)
    return S


async def scene(ed: Editor, name: str) -> int:
    made = await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    return sid


async def position(ed: Editor, sid: int, entity: int, prop: str = "position") -> list[float]:
    got = await ed.call("get_entity", scene=sid, entity=entity, components=["transform"], properties=[prop])
    return [round(float(v), 3) for v in got["components"]["transform"][prop]]


async def history(ed: Editor, sid: int, **args) -> list[dict]:
    """T(scene) oldest first"""
    got = await ed.call("get_undo_history", scene=sid, limit=500, **args)
    return list(reversed(got["entries"]))


async def undo_label(ed: Editor) -> str:
    return (await ed.call("editor_status"))["undo"]["undo_label"]


async def heights(ed: Editor, sid: int, terrain: int) -> list:
    """the terrain's heights on a 65 x 65 grid"""
    return (await ed.call("sample_terrain", scene=sid, entity=terrain, grid=65))["grid"]["world_heights"]


async def rect(ed: Editor, window: str, **args) -> dict:
    return (await ed.call("focus_window", window=window, **args))["rect"]


async def dock_close_button(ed: Editor, window: str) -> dict:
    """The rect of the close button on the tab bar of the dock panel showing this window (focused
    first): the "#CLOSE" item ImGui draws there, inside the top of the panel's rect."""
    panel = await rect(ed, window)
    await frames(ed)
    buttons = [i["rect"] for i in (await ed.call("find_ui_item", label="#CLOSE"))["items"] if i["visible"]]
    inside = [b for b in buttons
              if panel["x"] <= b["x"] + b["width"] / 2 <= panel["x"] + panel["width"]
              and panel["y"] <= b["y"] + b["height"] / 2 <= panel["y"] + 2 * b["height"] + 8]
    ed.check(inside, f"a close button on the {window} panel's tab bar {panel}: {buttons}")
    return min(inside, key=lambda b: b["y"])


async def frames(ed: Editor, count: int = 3):
    await ed.call("wait", frames=count)


# --------------------------------------------------------------------------------------- AC5


async def test_esc_cancels_a_gizmo_drag(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await scene(ed, "EscGizmo")
    cube = (await ed.call("create_entity", scene=sid, type="box", name="Cube"))["entity_id"]
    await ed.call("select_entities", scene=sid, entities=[cube])
    await ed.call("frame_entity", scene=sid, entity=cube)
    shot = await ed.call_raw("screenshot", {"view": "scene", "scene": sid})
    view = shot.structured["view_rect_in_window"]
    center = ui.at(view, (view["width"] / 2, view["height"] / 2))
    before = await position(ed, sid, cube)
    steps = [e["id"] for e in await history(ed, sid)]
    label = await undo_label(ed)

    ui.focus()
    await ui.press(center)
    await ui.drag_to(center, 160, 0)
    await frames(ed, 3)
    during = await position(ed, sid, cube)
    await ed.screenshot("editor", path=str(s["out"] / "ac5-1-gizmo-dragging.png"))
    ed.check(during != before, f"the drag moved the cube ({before} -> {during}): the press missed the gizmo's centre")
    await ui.key("Escape")
    await ui.release()
    await frames(ed)
    await ed.screenshot("editor", path=str(s["out"] / "ac5-2-gizmo-cancelled.png"))
    ed.check_eq(await position(ed, sid, cube), before, "Esc put the cube back")
    ed.check_eq([e["id"] for e in await history(ed, sid)], steps, "no new Undo History row")
    ed.check_eq(await undo_label(ed), label, "Edit > Undo shows the previous step")

    # the same drag released without Esc is one step
    await ui.press(center)
    await ui.drag_to(center, 120, 0)
    await ui.release()
    await frames(ed)
    after = await history(ed, sid)
    ed.check_eq(len(after), len(steps) + 1, f"one step for the drag: {[e['name'] for e in after]}")
    ed.check(after[-1]["name"] == "Move Cube", f"named: {after[-1]['name']}")
    S["gizmo_scene"] = sid


async def test_esc_cancels_an_inspector_drag(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await scene(ed, "EscInspector")
    cube = (await ed.call("create_entity", scene=sid, type="box", name="Cube"))["entity_id"]
    await ed.call("select_entities", scene=sid, entities=[cube])
    await ed.call("focus_window", window="inspector")
    field = ui.centre((await ui.reveal(POSITION_X, "Inspector"))["rect"])
    before = await position(ed, sid, cube)
    steps = [e["id"] for e in await history(ed, sid)]
    label = await undo_label(ed)

    ui.focus()
    await ui.press(field)
    await ui.drag_to(field, 90, 0)
    await frames(ed, 3)
    during = await position(ed, sid, cube)
    await ed.screenshot("editor", path=str(s["out"] / "ac5-3-inspector-dragging.png"))
    ed.check(during[0] != before[0], f"the drag changed Position X ({before} -> {during}): the field moved")
    await ui.key("Escape")
    await ui.release()
    await frames(ed)
    await ed.screenshot("editor", path=str(s["out"] / "ac5-4-inspector-cancelled.png"))
    ed.check_eq(await position(ed, sid, cube), before, "Esc put Position back")
    ed.check_eq([e["id"] for e in await history(ed, sid)], steps, "no new Undo History row")
    ed.check_eq(await undo_label(ed), label, "Edit > Undo shows the previous step")

    # released normally: one step for the whole drag
    await ui.press(field)
    await ui.drag_to(field, 60, 0)
    await ui.release()
    await frames(ed)
    after = await history(ed, sid)
    ed.check_eq(len(after), len(steps) + 1, f"one step for the drag: {[e['name'] for e in after]}")
    ed.check_eq(after[-1]["name"], "Set Position (Cube)", "named")


async def test_esc_cancels_a_terrain_stroke(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await scene(ed, "EscTerrain")
    terrain = (await ed.call("create_entity", scene=sid, type="terrain", name="Ground"))["entity_id"]
    # a small terrain with a height map to sculpt, seen from above
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {"terrainSize": 20}})
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, heightmap={"generate": "flat", "base_height": 0.2})
    await ed.call("select_entities", scene=sid, entities=[terrain])
    await ed.call("set_scene_camera", scene=sid, position=[0, 14, -16], target=[0, 0, 0])
    # the generated map loads asynchronously: a stroke needs its pixels
    for _ in range(100):
        info = (await ed.call("sample_terrain", scene=sid, entity=terrain, grid=2))["info"]
        if info.get("height_map_loaded"):
            break
        await asyncio.sleep(0.2)
    ed.check(info.get("height_map_loaded"), f"the height map loaded: {info}")
    await ed.call("focus_window", window="terrain")
    ui.focus()
    await ui.click_revealed(TERRAIN_RAISE_BRUSH, "Terrain")
    shot = await ed.call_raw("screenshot", {"view": "scene", "scene": sid})
    view = shot.structured["view_rect_in_window"]
    start = ui.at(view, (view["width"] / 2 + 120, view["height"] / 2 + 80))
    before = await heights(ed, sid, terrain)
    steps = [e["id"] for e in await history(ed, sid)]
    label = await undo_label(ed)

    await ui.press(start)
    await frames(ed, 3)
    await ui.drag_to(start, 120, 30, steps=20)
    await frames(ed, 3)
    during = await heights(ed, sid, terrain)
    # a loaded machine draws a few frames a second: keep the brush held (and moving) until the
    # stroke shows in the samples
    for k in range(20):
        if during != before:
            break
        ui.xdo("mousemove", str(start[0] + 120 - (k % 2) * 6), str(start[1] + 30))
        await asyncio.sleep(0.25)
        await frames(ed, 2)
        during = await heights(ed, sid, terrain)
    await ed.screenshot("editor", path=str(s["out"] / "ac5-5-terrain-stroke.png"))
    ed.check(during != before, "the stroke raised the terrain: the brush or the press missed")
    await ui.key("Escape")
    await ui.release()
    await frames(ed)
    after = await heights(ed, sid, terrain)
    await ed.screenshot("editor", path=str(s["out"] / "ac5-6-terrain-cancelled.png"))
    ed.check(after == before, "Esc put the heights back")
    ed.check_eq([e["id"] for e in await history(ed, sid)], steps, "no new Undo History row")
    ed.check_eq(await undo_label(ed), label, "Edit > Undo shows the previous step")

    # a stroke released normally is one step
    await ui.press(start)
    await ui.drag_to(start, 100, 0, steps=12)
    await ui.release()
    await frames(ed)
    done = await history(ed, sid)
    ed.check_eq(len(done), len(steps) + 1, f"one step: {[e['name'] for e in done]}")
    ed.check(done[-1]["name"].startswith("Sculpt Terrain"), f"named: {done[-1]['name']}")
    # leave the brush off for the next tests
    await ed.call("focus_window", window="terrain")
    await ui.click_revealed(TERRAIN_RAISE_BRUSH, "Terrain")


# -------------------------------------------------------------------------------------- AC15


async def click_row(ui: Ui, name: str):
    """the Undo History row of the step with this name (its icon left out), else "(Start)" """
    await ui.click_revealed(name, UNDO_HISTORY)


async def newest_applied(ed: Editor, sid: int, **args) -> int | None:
    applied = [e for e in await history(ed, sid, **args) if e["applied"]]
    return applied[-1]["id"] if applied else None


async def test_undo_history_panel(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await scene(ed, "HistoryPanel")
    cube = (await ed.call("create_entity", scene=sid, type="box", name="Cube"))["entity_id"]
    await ed.call("select_entities", scene=sid, entities=[])        # a selection step, hidden by default
    await ed.call("rename_entity", scene=sid, entity=cube, name="Crate")
    for k in range(1, 6):
        await ed.call("set_transform", scene=sid, entity=cube, position=[k, 0, 0])
    listed = await history(ed, sid)
    selection = [e for e in await history(ed, sid, include_selection=True) if e["type"] == "SelectionCmd"]
    ed.check_eq(len(listed), 7, f"create, rename and 5 moves listed: {[e['name'] for e in listed]}")
    ed.check_eq(len(selection), 1, "one selection step")
    await ed.call("focus_window", window="undo_history")
    ui.focus()
    await ed.screenshot("editor", path=str(s["out"] / "ac15-1-panel.png"))

    # a click on the 2nd row: the create and the rename stay applied, the moves are undone
    await click_row(ui, listed[1]["name"])
    ed.check_eq(await newest_applied(ed, sid), listed[1]["id"], "jumped to the 2nd row")
    ed.check_eq(len([e for e in await history(ed, sid) if e["applied"]]), 2, "two applied rows")
    ed.check_eq(await position(ed, sid, cube), [0.0, 0.0, 0.0], "the scene matches: the moves are undone")
    await ed.screenshot("editor", path=str(s["out"] / "ac15-2-jumped.png"))

    # "(Start)" reverts everything, Forward redoes one
    await click_row(ui, UNDO_START)
    ed.check(not [e for e in await history(ed, sid, include_selection=True) if e["applied"]], "(Start) undid everything")
    ed.check(not (await ed.call("find_entities", scene=sid, name="Crate"))["entities"], "the cube is gone")
    await ui.click_revealed(UNDO_FORWARD, UNDO_HISTORY)
    ed.check_eq(await newest_applied(ed, sid), listed[0]["id"], "Forward redid one step")
    await ui.click_revealed(UNDO_BACK, UNDO_HISTORY)
    ed.check_eq(await newest_applied(ed, sid), None, "Back undid it")

    # the filter: only the rename row; a click on it jumps there
    await ui.click_revealed(UNDO_FILTER, UNDO_HISTORY)
    await ui.type("Rename")
    await ed.screenshot("editor", path=str(s["out"] / "ac15-3-filter.png"))
    await click_row(ui, listed[1]["name"])
    ed.check_eq(await newest_applied(ed, sid), listed[1]["id"], "the filtered row was the rename")
    await ui.click_revealed(UNDO_FILTER, UNDO_HISTORY)
    await ui.key("ctrl+a", "BackSpace", "Return")

    # Show Selection Changes: the selection step is listed (row 2) and can be jumped to
    await ui.click_revealed(UNDO_SHOW_SELECTION, UNDO_HISTORY)
    await ed.screenshot("editor", path=str(s["out"] / "ac15-4-selection-shown.png"))
    await click_row(ui, selection[0]["name"])
    ed.check_eq(await newest_applied(ed, sid, include_selection=True), selection[0]["id"],
                "the selection step is listed and jumped to when they are shown")
    await ui.click_revealed(UNDO_SHOW_SELECTION, UNDO_HISTORY)

    # the memory footer: deleting 100 GameObjects is a big step
    made = await ed.batch([("create_entity", {"scene": sid, "type": "box", "name": f"Bulk{k}"}) for k in range(100)])
    ids = [r["structured"]["entity_id"] for r in made["results"]]
    await ed.call("delete_entities", scene=sid, entities=ids)
    memory = (await ed.call("get_undo_history", scene=sid, limit=1))["memory"]
    await ed.screenshot("editor", path=str(s["out"] / "ac15-5-memory.png"))
    ed.check(memory["used"] > 100 * 1024, f"the footer's memory is non-zero after deleting 100 objects: {memory}")


# -------------------------------------------------------------------------------------- AC16


async def play_and_drag(ed: Editor, ui: Ui, sid: int, cube: int, fields: list[str]):
    """Play started, the Inspector's fields (find_ui_item labels) dragged, Stop from the editor's toolbar"""
    started = await ed.call("play", action="start", scene=sid)
    ed.check_eq(started["state"], "playing", "play")
    await ed.call("select_entities", scene=sid, entities=[cube])
    await ed.call("focus_window", window="inspector")
    ui.focus()
    for label in fields:
        point = ui.centre((await ui.reveal(label, "Inspector"))["rect"])
        await ui.press(point)
        await ui.drag_to(point, 70, 0)
        await ui.release()
    # Stop from the editor (the toolbar's Play button, else Ctrl+P), not through MCP
    for attempt in (lambda: ui.click_revealed(TOOLBAR_PLAY), lambda: ui.key("ctrl+p")):
        await attempt()
        for _ in range(20):
            if (await ed.call("get_play_state"))["state"] == "stopped":
                break
            await asyncio.sleep(0.5)
        if (await ed.call("get_play_state"))["state"] == "stopped":
            break
    ed.check_eq((await ed.call("get_play_state"))["state"], "stopped", "the editor stopped play")
    await frames(ed, 5)


async def test_play_mode_changes_window(ed: Editor):
    s = await setup(ed)
    ui = s["ui"]
    sid = await scene(ed, "PlayReview")
    cube = (await ed.call("create_entity", scene=sid, type="box", name="Cube"))["entity_id"]
    await ed.call("save_scene", scene=sid)
    edit_position = await position(ed, sid, cube)
    edit_scale = await position(ed, sid, cube, "scale")

    # two Inspector edits while playing: Stop opens the window with two checked rows
    await play_and_drag(ed, ui, sid, cube, [POSITION_X, SCALE_X])
    rows = (await ed.call("get_play_changes", scene=sid))["rows"]
    ed.check_eq(len(rows), 2, f"two rows: {rows}")
    ed.check(all(r["keepable"] for r in rows), "both keepable")
    await ed.screenshot("editor", path=str(s["out"] / "ac16-1-window-opened.png"))

    # uncheck the Scale row, Keep Selected: one step "Apply Play Mode Changes (1)"
    await ed.call("focus_window", window="play_mode_changes")
    await ui.click_revealed("Scale", PLAY_MODE_CHANGES)
    await ed.screenshot("editor", path=str(s["out"] / "ac16-2-unchecked.png"))
    await ui.click_revealed("Keep Selected", PLAY_MODE_CHANGES)
    top = (await history(ed, sid))[-1]
    ed.check_eq(top["name"], "Apply Play Mode Changes (1)", "one step for the kept row")
    ed.check(await position(ed, sid, cube) != edit_position, "the kept Position has its play value")
    ed.check_eq(await position(ed, sid, cube, "scale"), edit_scale, "the unchecked Scale keeps its edit value")
    ed.check_eq((await ed.call("get_play_state"))["pending_play_changes"], 0, "nothing pending")
    await ed.screenshot("editor", path=str(s["out"] / "ac16-3-kept.png"))

    # closing the window without choosing discards
    steps = [e["id"] for e in await history(ed, sid)]
    kept_position = await position(ed, sid, cube)
    await play_and_drag(ed, ui, sid, cube, [POSITION_X])
    ed.check_eq((await ed.call("get_play_state"))["pending_play_changes"], 1, "one row pending")
    await ui.click(ui.centre(await dock_close_button(ed, "play_mode_changes")))
    await ed.screenshot("editor", path=str(s["out"] / "ac16-4-closed.png"))
    ed.check_eq((await ed.call("get_play_state"))["pending_play_changes"], 0, "closing discarded the rows")
    ed.check_eq([e["id"] for e in await history(ed, sid)], steps, "no step")
    ed.check_eq(await position(ed, sid, cube), kept_position, "the edit value stays")
