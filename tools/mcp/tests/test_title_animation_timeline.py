"""S85 R41-R46, R48: the Animation panel driven like a person, with real mouse and key input in the
harness's own Xvfb: scrubbing, dragging keys (with Esc and undo), box selection, adding and deleting
keys, Record, Curves and Add Preset's hover preview. Screenshots land in build/shots.
"""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
from pathlib import Path

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Animation Timeline", "xvfb": "harness"}
TIMEOUT = 600

SHOTS = Path(__file__).resolve().parents[3] / "build" / "shots"
S: dict = {}


class Ui:
    def __init__(self, ed: Editor):
        self.ed = ed
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)

    def xdo(self, *args: str) -> None:
        subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20)

    async def settle(self, frames: int = 4) -> None:
        await asyncio.sleep(0.15)
        await self.ed.call("wait", frames=frames)

    async def move(self, x: float, y: float) -> None:
        self.xdo("mousemove", str(int(x)), str(int(y)))
        await self.settle(2)

    async def press(self, x: float, y: float, button: str = "1") -> None:
        await self.move(x, y)
        self.xdo("mousedown", button)
        await self.settle(2)

    async def release(self, button: str = "1") -> None:
        self.xdo("mouseup", button)
        await self.settle()

    async def click(self, x: float, y: float, button: str = "1", repeat: int = 1) -> None:
        await self.move(x, y)
        self.xdo("click", "--repeat", str(repeat), "--delay", "60", button)
        await self.settle()

    async def drag(self, a, b, steps: int = 6) -> None:
        await self.press(*a)
        for i in range(1, steps + 1):
            await self.move(a[0] + (b[0] - a[0]) * i / steps, a[1] + (b[1] - a[1]) * i / steps)
        await self.release()

    def key(self, name: str) -> None:
        self.xdo("key", name)

    async def item(self, label: str) -> tuple[float, float]:
        await self.settle(2)
        items = (await self.ed.call("find_ui_item", label=label, window="Title Designer")).get("items", [])
        visible = [i for i in items if i.get("visible")]
        self.ed.check(visible, f"{label} is drawn: {items[:3]}")
        r = visible[0]["rect"]
        return r["x"] + r["width"] / 2.0, r["y"] + r["height"] / 2.0


async def shot(ed: Editor, name: str) -> None:
    await ed.call("wait", frames=8)
    SHOTS.mkdir(parents=True, exist_ok=True)
    (SHOTS / name).write_bytes(await ed.screenshot("title_designer", max_width=0))


async def designer(ed: Editor) -> dict:
    """get_title's designer block: reads the window without taking the keyboard focus."""
    await ed.call("wait", frames=3)
    return (await ed.call("get_title", path=S["path"]))["designer"]


async def panel(ed: Editor) -> dict:
    return (await designer(ed))["animation_panel"]


async def tracks(ed: Editor, path: str) -> list[dict]:
    got = await ed.call("get_title", path=path, animation="Intro")
    return got["animation"]["tracks"]


def key_times(track: dict) -> list[float]:
    return [k["time"] if isinstance(k, dict) else k[0] for k in track["keys"]]


def key_xy(p: dict, row: int, time: float) -> tuple[float, float]:
    t = p["timeline"]
    return t["x"] + (time - t["view_start"]) / t["seconds_per_pixel"], t["rows_y"] + t["row_h"] * (row + 0.5)


async def fresh(ed: Editor, name: str) -> tuple[str, Ui]:
    if S.get("ed") is not ed:
        S.clear()
        S["ed"] = ed
        S["ui"] = Ui(ed)
        await ed.call("tool_groups", enable=["title_art", "editor_tools", "editor_ux"])
        await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    path = f"assets/{name}.titleart"
    await ed.call("edit_title", path=path, create={"text": "WIN", "overwrite": True}, bake="never", ops=[
        {"op": "add_animation", "name": "Intro", "duration": 1.0},
        {"op": "set_track", "animation": "Intro", "field": "opacity", "keys": [[0, 0], [0.5, 1]]},
        {"op": "set_track", "animation": "Intro", "field": "transform.rotation", "keys": [[0, 0], [0.5, 10]]},
    ])
    await ed.call("focus_window", window="title_designer", title_designer={"path": path})
    await S["ui"].settle(12)
    S["path"] = path
    return path, S["ui"]


async def test_scrub_shows_the_pose_and_stop_ends_it(ed: Editor):
    path, ui = await fresh(ed, "scrub")
    p = await panel(ed)
    t = p["timeline"]
    await ui.click(t["x"] + 0.25 / t["seconds_per_pixel"], t["ruler_y"] + t["ruler_h"] / 2)
    p = await panel(ed)
    ed.check(p["previewing"], str(p))
    ed.check(abs(p["time"] - 0.25) < 1.0 / 30 + 1e-3, f"the ruler sets the time: {p['time']}")
    await shot(ed, "timeline-scrub.png")
    await ui.click(*await stop_button(ui))
    ed.check((await panel(ed))["previewing"] is False, "Stop ends the preview")


async def stop_button(ui: Ui) -> tuple[float, float]:
    return await ui.item("")


async def test_dragging_a_key_is_one_undo_step_and_esc_cancels(ed: Editor):
    path, ui = await fresh(ed, "drag")
    p = await panel(ed)
    a = key_xy(p, 0, 0.5)
    spp = p["timeline"]["seconds_per_pixel"]
    pixels = 0.2 / spp
    await ui.drag(a, (a[0] + pixels, a[1]))
    await shot(ed, "timeline-dragged.png")
    moved = key_times((await tracks(ed, path))[0])
    ed.check(abs(moved[1] - 0.7) <= 1.0 / 30 + 1e-3, f"the key followed the drag: {moved}")
    ed.check(abs(moved[1] * 30 - round(moved[1] * 30)) < 1e-3, f"snapped to a frame: {moved}")
    await ed.call("undo", scope="project")
    ed.check_eq(key_times((await tracks(ed, path))[0]), [0.0, 0.5], "one undo step restores it")

    # Esc during the drag leaves the document as it was
    p = await panel(ed)
    a = key_xy(p, 0, 0.5)
    await ui.press(*a)
    for i in range(1, 5):
        await ui.move(a[0] + 15 * i, a[1])
    ui.key("Escape")
    await ui.settle()
    await ui.release()
    ed.check_eq(key_times((await tracks(ed, path))[0]), [0.0, 0.5], "Esc reverts the drag")


async def test_box_select_double_click_and_delete(ed: Editor):
    path, ui = await fresh(ed, "box")
    p = await panel(ed)
    a = key_xy(p, 0, 0.4)
    b = key_xy(p, 1, 0.6)
    await ui.drag((a[0], a[1] - 6), (b[0], b[1] + 6))
    p = await panel(ed)
    ed.check_eq(p["selected_keys"], 2, "the box takes the 0.5 s key of both rows")

    # a double click on an empty stretch of a row adds a key with the sampled value
    at = key_xy(p, 0, 0.8)
    await ui.click(*at, repeat=2)
    times = key_times((await tracks(ed, path))[0])
    ed.check_eq(len(times), 3, f"a double click adds a key: {times}")

    # Delete removes the selection: click one key, press Delete
    p = await panel(ed)
    await ui.click(*key_xy(p, 0, times[2]))
    ed.check_eq((await panel(ed))["selected_keys"], 1, "the click selected it")
    ui.key("Delete")
    await ui.settle(6)
    ed.check_eq(len(key_times((await tracks(ed, path))[0])), 2, "Delete removed the key")


async def test_mute_button_takes_clicks_in_the_track_column(ed: Editor):
    path, ui = await fresh(ed, "mute")
    p = await panel(ed)
    t = p["timeline"]
    await ui.click(t["x"] - 300 + 14, t["rows_y"] + t["row_h"] / 2)
    ed.check((await tracks(ed, path))[0].get("mute") is True, "the row's mute button works")


async def test_record_keys_an_edit_as_one_step(ed: Editor):
    path, ui = await fresh(ed, "record")
    await ed.call("focus_window", window="title_designer", title_designer={"select": "Title", "animation": {"name": "Intro", "time": 0.5}})
    await ui.settle(8)
    await ui.click(*(await ui.item("Record")))
    ed.check((await panel(ed))["record"], "Record is on")
    # the Move tool's drag on the canvas, at the layer's origin
    origin = (await designer(ed))["canvas_origin"]
    # the Designer's keys need its focus, not the panel's
    await ed.call("focus_window", window="title_designer", title_designer={})
    await ui.settle(4)
    ui.key("w")
    await ui.settle(4)
    await shot(ed, "timeline-record.png")
    ed.check((await panel(ed))["record"], f"Record is still on before the drag: {await panel(ed)}")
    await ui.drag((origin[0], origin[1]), (origin[0] + 40, origin[1]))
    got = await tracks(ed, path)
    layer_tracks = [t for t in got if t.get("layer") and t["field"] == "transform.position"]
    ed.check(layer_tracks, f"Record made a track on the layer: {[(t.get('layer'), t['field']) for t in got]}")
    ed.check_eq(key_times(layer_tracks[0]), [0.0, 0.5], "the rest at 0 and the edit at the playhead")
    layer = next(l for l in json.loads((Path(ed.project) / path).read_text())["layers"] if l["name"] == "Title")
    ed.check_eq(layer.get("transform", {}).get("position", [0, 0]), [0, 0], "the rest value did not change")
    await shot(ed, "timeline-recorded.png")
    undone = await ed.call("undo", scope="project")
    ed.check_in("Record Key", str(undone), "the step is named for the record")
    await ui.click(*(await ui.item("Record")))


async def test_curves_edit_keys(ed: Editor):
    path, ui = await fresh(ed, "curves")
    await ui.click(*(await ui.item("Curves")))
    await ui.settle(10)
    p = await panel(ed)
    ed.check(p["curves"] and "curve_view" in p, str(p)[:300])
    await shot(ed, "timeline-curves.png")
    v = p["curve_view"]
    to_x = lambda t: v["x0"] + (t - v["t0"]) / (v["t1"] - v["t0"]) * (v["x1"] - v["x0"])
    to_y = lambda value: v["y1"] - (value - v["v0"]) / (v["v1"] - v["v0"]) * (v["y1"] - v["y0"])
    a = (to_x(0.5), to_y(10.0))
    await ui.drag(a, (a[0], a[1] + 40))
    got = await tracks(ed, path)
    rotation = next(t for t in got if t["field"] == "transform.rotation")
    value = rotation["keys"][1]["value"] if isinstance(rotation["keys"][1], dict) else rotation["keys"][1][1]
    ed.check(value < 9.0, f"the drag lowered the key: {rotation['keys']}")
    await ed.call("undo", scope="project")
    rotation = next(t for t in await tracks(ed, path) if t["field"] == "transform.rotation")
    value = rotation["keys"][1]["value"] if isinstance(rotation["keys"][1], dict) else rotation["keys"][1][1]
    ed.check(abs(value - 10.0) < 1e-3, f"one undo step: {rotation['keys']}")
    await ui.click(*(await ui.item("Curves")))


async def test_preset_hover_previews_without_a_step(ed: Editor):
    path, ui = await fresh(ed, "hover")
    before = await ed.call("get_title", path=path, animation="Intro")
    await ui.click(*(await ui.item("Add Preset")))
    await ui.settle(6)
    in_menu = await ui.item("emphasis")
    await ui.move(*in_menu)
    await ui.settle(8)
    item = await ui.item("Shake")
    await ui.move(*item)
    await ui.settle(10)
    # get_title reads the state without taking the focus (focus closes the menu)
    designer = (await ed.call("get_title", path=path))["designer"]
    ed.check(designer["preview_active"], f"hovering a preset previews it: {designer}")
    # the whole editor: a shot of the window would take the focus and close the menu
    SHOTS.mkdir(parents=True, exist_ok=True)
    (SHOTS / "timeline-hover.png").write_bytes(await ed.screenshot("editor", max_width=0))
    ui.key("Escape")
    await ui.settle(4)
    ui.key("Escape")
    await ui.settle(6)
    ed.check((await panel(ed))["preview_active"] is False, "closing the menu ends the preview")
    after = await ed.call("get_title", path=path, animation="Intro")
    ed.check_eq(after["animation"], before["animation"], "a hover adds no step")
