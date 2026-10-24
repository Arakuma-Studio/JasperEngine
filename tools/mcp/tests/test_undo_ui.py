"""S24 AC5, AC15 and AC16, scripted: real mouse and key input (xdotool in the harness's Xvfb,
1600x900, default layout) checked through MCP, with screenshots.

- AC5: Esc during a drag puts the values back and adds no step: the move gizmo in the Scene
  view, an Inspector float drag, a terrain sculpt stroke.
- AC15: the Undo History panel: a click jumps to a row, "(Start)" reverts everything, Forward
  redoes one, the filter and the Show Selection Changes toggle, the memory footer.
- AC16: the Play Mode Changes window opens on a Stop from the editor with the Inspector's
  play-time edits checked; Keep Selected applies the checked ones as one step; closing the
  window discards.

Positions come from the editor (focus_window's panel rect, the scene screenshot's
view_rect_in_window) plus offsets inside the panels measured in the default layout. Every
action is verified through MCP, so a layout change fails with the screenshots to look at.
Screenshots go to $JE_EVIDENCE_DIR (else the scratch directory).
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
from pathlib import Path

from harness import Editor, skip

EDITOR = {"xvfb": "harness", "create": True, "name": "Undo UI"}
TIMEOUT = 900

# Inside the Inspector (default layout, a box selected): the X field of Transform's Position, Scale
INSPECTOR_POSITION_X = (177, 119)
INSPECTOR_SCALE_X = (177, 169)
# Inside the Terrain window, a terrain selected: the Raise or Lower Terrain brush
TERRAIN_RAISE_BRUSH = (150, 190)
# Inside the bottom dock panel (Undo History): toolbar row, list rows
UNDO_BACK = (20, 35)
UNDO_FORWARD = (52, 35)
UNDO_FILTER = (297, 35)
UNDO_SHOW_SELECTION = (555, 35)
UNDO_FIRST_ROW_Y = 63      # "(Start)"
UNDO_ROW_STEP = 18.9
UNDO_ROW_X = 120
# Inside the bottom dock panel (Play Mode Changes): the first two property rows' checkboxes,
# the buttons, the dock's close button
PMC_ROW_CHECKBOX = [(45, 115), (45, 140)]
PMC_KEEP_SELECTED = (52, 243)
PMC_CLOSE = (1230, 10)
# The main toolbar's Play button (centred in the 1600-wide window)
TOOLBAR_PLAY = (764, 36)

S: dict = {}


class Ui:
    def __init__(self, ed: Editor):
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)
        windows = self.xdo("search", "--name", "JasperEngine").split() or self.xdo("search", "--name", ".").split()
        if not windows:
            skip("no editor window found")
        self.window = windows[-1]
        geometry = self.xdo("getwindowgeometry", self.window)
        x, y = 0, 0
        for line in geometry.splitlines():
            if "Position:" in line:
                x, y = (int(v) for v in line.split("Position:")[1].split("(")[0].strip().split(","))
        self.origin = (x, y)

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    def focus(self):
        self.xdo("windowactivate", "--sync", self.window)
        self.xdo("windowfocus", "--sync", self.window)

    def at(self, rect: dict, offset: tuple[float, float]) -> tuple[int, int]:
        return (int(self.origin[0] + rect["x"] + offset[0]), int(self.origin[1] + rect["y"] + offset[1]))

    async def move(self, point: tuple[int, int]):
        self.xdo("mousemove", str(point[0]), str(point[1]))
        await asyncio.sleep(0.25)

    async def click(self, point: tuple[int, int]):
        await self.move(point)
        self.xdo("click", "1")
        await asyncio.sleep(0.6)

    async def press(self, point: tuple[int, int]):
        await self.move(point)
        self.xdo("mousedown", "1")
        await asyncio.sleep(0.3)

    async def drag_to(self, start: tuple[int, int], dx: int, dy: int, steps: int = 12):
        for k in range(1, steps + 1):
            self.xdo("mousemove", str(int(start[0] + dx * k / steps)), str(int(start[1] + dy * k / steps)))
            await asyncio.sleep(0.05)
        await asyncio.sleep(0.4)

    async def release(self):
        self.xdo("mouseup", "1")
        await asyncio.sleep(0.6)

    async def key(self, *keys: str):
        for key in keys:
            # a chord's modifiers go down first and up last, frames apart: `xdotool key ctrl+z`
            # can land Z's press and Ctrl's release in one editor frame, where Ctrl reads up
            *mods, main = key.split("+")
            for mod in mods:
                self.xdo("keydown", mod)
            if mods:
                await asyncio.sleep(0.1)
            self.xdo("key", main)
            if mods:
                await asyncio.sleep(0.1)
            for mod in reversed(mods):
                self.xdo("keyup", mod)
            await asyncio.sleep(0.3)

    async def type(self, text: str):
        self.xdo("type", "--delay", "40", text)
        await asyncio.sleep(0.5)


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
    inspector = await rect(ed, "inspector")
    field = ui.at(inspector, INSPECTOR_POSITION_X)
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
    window = await rect(ed, "terrain")
    ui.focus()
    await ui.click(ui.at(window, TERRAIN_RAISE_BRUSH))
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
    await ui.click(ui.at(await rect(ed, "terrain"), TERRAIN_RAISE_BRUSH))


# -------------------------------------------------------------------------------------- AC15


def row_point(ui: Ui, panel: dict, row: int) -> tuple[int, int]:
    """row 0 = "(Start)", then the listed steps oldest first"""
    return ui.at(panel, (UNDO_ROW_X, UNDO_FIRST_ROW_Y + UNDO_ROW_STEP * row))


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
    panel = await rect(ed, "undo_history")
    ui.focus()
    await ed.screenshot("editor", path=str(s["out"] / "ac15-1-panel.png"))

    # a click on the 2nd row: the create and the rename stay applied, the moves are undone
    await ui.click(row_point(ui, panel, 2))
    ed.check_eq(await newest_applied(ed, sid), listed[1]["id"], "jumped to the 2nd row")
    ed.check_eq(len([e for e in await history(ed, sid) if e["applied"]]), 2, "two applied rows")
    ed.check_eq(await position(ed, sid, cube), [0.0, 0.0, 0.0], "the scene matches: the moves are undone")
    await ed.screenshot("editor", path=str(s["out"] / "ac15-2-jumped.png"))

    # "(Start)" reverts everything, Forward redoes one
    await ui.click(row_point(ui, panel, 0))
    ed.check(not [e for e in await history(ed, sid, include_selection=True) if e["applied"]], "(Start) undid everything")
    ed.check(not (await ed.call("find_entities", scene=sid, name="Crate"))["entities"], "the cube is gone")
    await ui.click(ui.at(panel, UNDO_FORWARD))
    ed.check_eq(await newest_applied(ed, sid), listed[0]["id"], "Forward redid one step")
    await ui.click(ui.at(panel, UNDO_BACK))
    ed.check_eq(await newest_applied(ed, sid), None, "Back undid it")

    # the filter: only the rename row; a click on it jumps there
    await ui.click(ui.at(panel, UNDO_FILTER))
    await ui.type("Rename")
    await ed.screenshot("editor", path=str(s["out"] / "ac15-3-filter.png"))
    await ui.click(row_point(ui, panel, 1))
    ed.check_eq(await newest_applied(ed, sid), listed[1]["id"], "the filtered row was the rename")
    await ui.click(ui.at(panel, UNDO_FILTER))
    await ui.key("ctrl+a", "BackSpace", "Return")

    # Show Selection Changes: the selection step is listed (row 2) and can be jumped to
    await ui.click(ui.at(panel, UNDO_SHOW_SELECTION))
    await ed.screenshot("editor", path=str(s["out"] / "ac15-4-selection-shown.png"))
    await ui.click(row_point(ui, panel, 2))
    ed.check_eq(await newest_applied(ed, sid, include_selection=True), selection[0]["id"],
                "row 2 is the selection step when they are shown")
    await ui.click(ui.at(panel, UNDO_SHOW_SELECTION))

    # the memory footer: deleting 100 GameObjects is a big step
    made = await ed.batch([("create_entity", {"scene": sid, "type": "box", "name": f"Bulk{k}"}) for k in range(100)])
    ids = [r["structured"]["entity_id"] for r in made["results"]]
    await ed.call("delete_entities", scene=sid, entities=ids)
    memory = (await ed.call("get_undo_history", scene=sid, limit=1))["memory"]
    await ed.screenshot("editor", path=str(s["out"] / "ac15-5-memory.png"))
    ed.check(memory["used"] > 100 * 1024, f"the footer's memory is non-zero after deleting 100 objects: {memory}")


# -------------------------------------------------------------------------------------- AC16


async def play_and_drag(ed: Editor, ui: Ui, sid: int, cube: int, fields: list[tuple[int, int]]):
    """Play started, the Inspector's fields dragged, Stop from the editor's toolbar"""
    started = await ed.call("play", action="start", scene=sid)
    ed.check_eq(started["state"], "playing", "play")
    await ed.call("select_entities", scene=sid, entities=[cube])
    inspector = await rect(ed, "inspector")
    ui.focus()
    for offset in fields:
        point = ui.at(inspector, offset)
        await ui.press(point)
        await ui.drag_to(point, 70, 0)
        await ui.release()
    # Stop from the editor (the toolbar's Play button, else Ctrl+P), not through MCP
    for attempt in (lambda: ui.click(ui.at({"x": 0, "y": 0}, TOOLBAR_PLAY)), lambda: ui.key("ctrl+p")):
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
    await play_and_drag(ed, ui, sid, cube, [INSPECTOR_POSITION_X, INSPECTOR_SCALE_X])
    rows = (await ed.call("get_play_changes", scene=sid))["rows"]
    ed.check_eq(len(rows), 2, f"two rows: {rows}")
    ed.check(all(r["keepable"] for r in rows), "both keepable")
    await ed.screenshot("editor", path=str(s["out"] / "ac16-1-window-opened.png"))
    panel = await rect(ed, "play_mode_changes")

    # uncheck the second row, Keep Selected: one step "Apply Play Mode Changes (1)"
    await ui.click(ui.at(panel, PMC_ROW_CHECKBOX[1]))
    await ed.screenshot("editor", path=str(s["out"] / "ac16-2-unchecked.png"))
    await ui.click(ui.at(panel, PMC_KEEP_SELECTED))
    top = (await history(ed, sid))[-1]
    ed.check_eq(top["name"], "Apply Play Mode Changes (1)", "one step for the kept row")
    ed.check(await position(ed, sid, cube) != edit_position, "the kept Position has its play value")
    ed.check_eq(await position(ed, sid, cube, "scale"), edit_scale, "the unchecked Scale keeps its edit value")
    ed.check_eq((await ed.call("get_play_state"))["pending_play_changes"], 0, "nothing pending")
    await ed.screenshot("editor", path=str(s["out"] / "ac16-3-kept.png"))

    # closing the window without choosing discards
    steps = [e["id"] for e in await history(ed, sid)]
    kept_position = await position(ed, sid, cube)
    await play_and_drag(ed, ui, sid, cube, [INSPECTOR_POSITION_X])
    ed.check_eq((await ed.call("get_play_state"))["pending_play_changes"], 1, "one row pending")
    panel = await rect(ed, "play_mode_changes")
    await ui.click(ui.at(panel, PMC_CLOSE))
    await ed.screenshot("editor", path=str(s["out"] / "ac16-4-closed.png"))
    ed.check_eq((await ed.call("get_play_state"))["pending_play_changes"], 0, "closing discarded the rows")
    ed.check_eq([e["id"] for e in await history(ed, sid)], steps, "no step")
    ed.check_eq(await position(ed, sid, cube), kept_position, "the edit value stays")
