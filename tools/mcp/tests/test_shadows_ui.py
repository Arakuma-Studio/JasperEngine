"""3D shadows in the Inspector (spec S10 AC17), scripted with real mouse input (xdotool in the
harness's Xvfb, 1600x900, default layout) and checked through MCP: the shadow rows of the AC's list
(Shadow Type, Resolution, Bias, Normal Bias, Shadow Distance, Cascade Count, the split bar's drag,
Static Shadow Caster) each edit the light or mesh as one named undo step, and undo puts the value
back.

The rows' positions are window coordinates measured in the default layout; every action is
verified through MCP, and the screenshots of each step go to build/test-output/shadows-ui, so a
layout change fails with the pictures to look at."""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess

import harness
import pngdiff
from harness import Editor, skip

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Shadows UI"}
TIMEOUT = 900

SHOT_DIR = harness.REPO / "build" / "test-output" / "shadows-ui"


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

    async def move(self, point: tuple[int, int]):
        """to a point in window coordinates"""
        self.xdo("mousemove", str(self.origin[0] + point[0]), str(self.origin[1] + point[1]))
        await asyncio.sleep(0.25)

    async def click(self, point: tuple[int, int]):
        await self.move(point)
        self.xdo("click", "1")
        await asyncio.sleep(0.6)


async def editor_shot(ed: Editor, name: str):
    r = await ed.call_raw("screenshot", {"view": "editor", "max_width": 1600})
    ed.check(not r.is_error and r.images, f"no editor screenshot: {r.text[:300]}")
    SHOT_DIR.mkdir(parents=True, exist_ok=True)
    (SHOT_DIR / f"{name}.png").write_bytes(pngdiff.encode(pngdiff.decode(r.images[0][0])))


# Window coordinates of the light's Shadows rows (default layout: the Inspector at x 1248..1600,
# its Shadows section in view without scrolling, a directional light selected)
SHADOW_TYPE = (1487, 592)
RESOLUTION = (1470, 642)
BIAS = (1487, 667)
NORMAL_BIAS = (1487, 692)
SHADOW_DISTANCE = (1487, 742)
CASCADE_COUNT = (1470, 767)
SPLIT_BAR = (1395, 1580, 817)     # x from, x to, y
# a combo's popup: its first item this far under the combo's centre, the next ones a row apart
POPUP_FIRST = 29
POPUP_ROW = 21
# a mesh's Static Shadow Caster checkbox (the Inspector scrolled to its top)
STATIC_SHADOW_CASTER = (1404, 452)

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        S["ui"].xdo("mouseup", "1")
        S["ui"].xdo("key", "Escape")
        return S
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness's Xvfb")
    S.clear()
    S["ed"] = ed
    S["ui"] = Ui(ed)
    return S


async def light_value(ed: Editor, sid: int, entity: int, prop: str):
    got = await ed.call("get_entity", scene=sid, entity=entity, components=["light"])
    return harness.find_key(got, prop)


async def undo_label(ed: Editor) -> str:
    return (await ed.call("editor_status"))["undo"]["undo_label"]


async def edit_and_undo(ed: Editor, sid: int, name: str, read, act, changed):
    """act() edits through the Inspector; the value read() changes (changed(before, after)), one
    undo step names it, and undo puts the value back."""
    ui = S["ui"]
    before = await read()
    await act()
    await ed.call("wait", frames=4)
    await editor_shot(ed, f"{name}_edited")
    after = await read()
    ed.check(changed(before, after), f"{name}: the Inspector edited it ({before} -> {after})")
    label = await undo_label(ed)
    ed.check(label and label != "(none)", f"{name}: a named undo step ({label!r})")
    ui.xdo("key", "Escape")
    await ed.call("undo")
    await ed.call("wait", frames=2)
    reverted = await read()
    ed.check(reverted == before, f"{name}: undo reverts it ({after} -> {reverted}, was {before})")
    print(f"     [ui] {name}: {before} -> {after} (\"{label}\"), undone")


async def type_into(ui: Ui, point: tuple[int, int], text: str):
    """a drag field's text input (double click), its text replaced"""
    await ui.move(point)
    ui.xdo("click", "--repeat", "2", "--delay", "80", "1")
    await asyncio.sleep(0.5)
    ui.xdo("key", "ctrl+a")
    await asyncio.sleep(0.2)
    ui.xdo("type", "--delay", "40", text)
    await asyncio.sleep(0.3)
    ui.xdo("key", "Return")
    await asyncio.sleep(0.6)


async def pick(ed: Editor, ui: Ui, combo: tuple[int, int], index: int, name: str):
    await ui.click(combo)
    await ed.call("wait", frames=3)
    await editor_shot(ed, f"{name}_popup")
    await ui.click((combo[0], combo[1] + POPUP_FIRST + POPUP_ROW * index))


async def test_inspector_shadow_rows_undo(ed: Editor):
    """AC17: each shadow row of the Inspector edits as one named undo step that undo reverts."""
    s = await setup(ed)
    ui = s["ui"]
    made = await ed.call("create_scene", name="InspectorUndo", type="3D", path="scenes/InspectorUndo")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    found = await ed.call("find_entities", scene=sid, component="light")
    sun = harness.entity_id_of(harness.find_entities(found)[0])
    cube = (await ed.call("create_entity", scene=sid, type="cube", name="Crate", select=False))["entity_id"]
    await ed.call("select_entities", scene=sid, entities=[sun])
    await ed.call("focus_window", window="inspector")
    await ed.call("wait", frames=4)
    await editor_shot(ed, "light")

    def read(prop):
        return lambda: light_value(ed, sid, sun, prop)

    def differs(a, b):
        return a != b

    # Shadow Type: Hard Shadows (shadows and softShadows together, one step)
    await edit_and_undo(ed, sid, "shadow_type", read("softShadows"),
                        lambda: pick(ed, ui, SHADOW_TYPE, 1, "shadow_type"), differs)
    # Resolution: 512 (the combo's third entry after Use Quality Settings and 256)
    await edit_and_undo(ed, sid, "resolution", read("mapResolution"),
                        lambda: pick(ed, ui, RESOLUTION, 2, "resolution"), differs)
    await edit_and_undo(ed, sid, "bias", read("shadowBias"), lambda: type_into(ui, BIAS, "2.5"), differs)
    await edit_and_undo(ed, sid, "normal_bias", read("shadowNormalBias"), lambda: type_into(ui, NORMAL_BIAS, "0.25"), differs)
    await edit_and_undo(ed, sid, "shadow_distance", read("shadowDistance"), lambda: type_into(ui, SHADOW_DISTANCE, "42"), differs)
    # Cascade Count: 2 (after Use Quality Settings and 1)
    await edit_and_undo(ed, sid, "cascade_count", read("numShadowCascades"),
                        lambda: pick(ed, ui, CASCADE_COUNT, 2, "cascade_count"), differs)

    # the split bar's handle between the first two cascades, dragged right: one step
    await ed.call("set_properties", scene=sid, entity=sun, values={"light": {"cascadeSplitMode": "manual", "numShadowCascades": 4}})
    await ed.call("wait", frames=4)
    await editor_shot(ed, "split_bar")
    x0, x1, y = SPLIT_BAR
    splits = await light_value(ed, sid, sun, "cascadeSplits")
    handle = (int(x0 + (x1 - x0) * splits[0]), y)

    async def drag():
        await ui.move(handle)
        ui.xdo("mousedown", "1")
        await asyncio.sleep(0.3)
        for k in range(1, 9):
            ui.xdo("mousemove", str(ui.origin[0] + handle[0] + 2 * k), str(ui.origin[1] + y))
            await asyncio.sleep(0.05)
        await asyncio.sleep(0.3)
        ui.xdo("mouseup", "1")
        await asyncio.sleep(0.6)
    await edit_and_undo(ed, sid, "split_bar", read("cascadeSplits"), drag, differs)

    # the mesh's Static Shadow Caster
    await ed.call("select_entities", scene=sid, entities=[cube])
    await ed.call("wait", frames=4)
    await editor_shot(ed, "mesh")

    async def static_flag():
        got = await ed.call("get_entity", scene=sid, entity=cube, components=["mesh"])
        return harness.find_key(got, "staticShadowCaster")
    await edit_and_undo(ed, sid, "static_shadow_caster", static_flag, lambda: ui.click(STATIC_SHADOW_CASTER), differs)
