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
import ui_input

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Shadows UI"}
TIMEOUT = 900

SHOT_DIR = harness.REPO / "build" / "test-output" / "shadows-ui"


class Ui(ui_input.Ui):
    """The shared frame-paced input (tools/mcp/ui_input.py) aimed where find_ui_item says a widget
    is drawn now (by its ImGui label); points here are in window coordinates."""

    async def at(self, label: str, index: int = 0, window: str = "Inspector") -> tuple[int, int]:
        """the centre of the widget drawn with this label (window coordinates), as drawn now; a
        widget drawn below its window's visible area is scrolled into view with the wheel"""
        for _ in range(30):
            drawn = (await self.ed.call("find_ui_item", label=label, window=window))["items"]
            items = [item for item in drawn if item["visible"]]
            if len(items) > index:
                rect = items[index]["rect"]
                return (int(rect["x"] + rect["width"] / 2), int(rect["y"] + rect["height"] / 2))
            if drawn:
                area = drawn[0]["window_rect"]
                await self.wheel(self.centre(area), 1)
            else:
                await self.frames(5)
        raise harness.TestFailure(f"find_ui_item: no visible {label!r} in {window!r}")

    def screen(self, point: tuple[int, int]) -> tuple[int, int]:
        return (self.origin[0] + point[0], self.origin[1] + point[1])

    async def move(self, point: tuple[int, int], frames: int = ui_input.SETTLE_FRAMES):
        """to a point in window coordinates"""
        await super().move(self.screen(point), frames)

    async def click(self, point: tuple[int, int] | None = None, button: str = "1"):
        """at a point in window coordinates"""
        await super().click(self.screen(point) if point is not None else None, button)


async def editor_shot(ed: Editor, name: str):
    r = await ed.call_raw("screenshot", {"view": "editor", "max_width": 1600})
    ed.check(not r.is_error and r.images, f"no editor screenshot: {r.text[:300]}")
    SHOT_DIR.mkdir(parents=True, exist_ok=True)
    (SHOT_DIR / f"{name}.png").write_bytes(pngdiff.encode(pngdiff.decode(r.images[0][0])))


# The ImGui labels of the light's Shadows rows (editor/window/InspectorShadows.cpp, the float rows
# InspectorWindow::propertyRow's), found where they are drawn: rows other sections add above them
# (the merge of upstream's lighting work did) move them
SHADOW_TYPE = "##shadow_type"
RESOLUTION = "##shadow_resolution"
BIAS = "##input_float_shadowBias"
NORMAL_BIAS = "##input_float_shadowNormalBias"
SHADOW_DISTANCE = "##input_float_shadowDistance"
CASCADE_COUNT = "##shadow_cascades"
SPLIT_HANDLE = "##split_handle"
# a mesh's Static Shadow Caster checkbox
STATIC_SHADOW_CASTER = "##checkbox_staticShadowCaster"

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
    await ui.key("Escape")
    await ed.call("undo")
    await ed.call("wait", frames=2)
    reverted = await read()
    ed.check(reverted == before, f"{name}: undo reverts it ({after} -> {reverted}, was {before})")
    print(f"     [ui] {name}: {before} -> {after} (\"{label}\"), undone")


async def type_into(ui: Ui, label: str, text: str):
    """a drag field's text input (double click), its text replaced"""
    await ui.move(await ui.at(label))
    await ui.double_click()
    await ui.key("ctrl+a")
    await ui.type(text)
    await ui.key("Return")


async def pick(ed: Editor, ui: Ui, label: str, item: str, name: str):
    """opens the combo and clicks the item with this text in its popup (above or below the combo,
    wherever ImGui opens it)"""
    await ui.click(await ui.at(label))
    await ed.call("wait", frames=3)
    await editor_shot(ed, f"{name}_popup")
    await ui.click(await ui.at(item, window="##Combo"))


async def _click_label(ui: Ui, label: str):
    await ui.click(await ui.at(label))


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
                        lambda: pick(ed, ui, SHADOW_TYPE, "Hard Shadows", "shadow_type"), differs)
    # Resolution: 512 (the combo's third entry after Use Quality Settings and 256)
    await edit_and_undo(ed, sid, "resolution", read("mapResolution"),
                        lambda: pick(ed, ui, RESOLUTION, "512", "resolution"), differs)
    await edit_and_undo(ed, sid, "bias", read("shadowBias"), lambda: type_into(ui, BIAS, "2.5"), differs)
    await edit_and_undo(ed, sid, "normal_bias", read("shadowNormalBias"), lambda: type_into(ui, NORMAL_BIAS, "0.25"), differs)
    await edit_and_undo(ed, sid, "shadow_distance", read("shadowDistance"), lambda: type_into(ui, SHADOW_DISTANCE, "42"), differs)
    # Cascade Count: 2 (after Use Quality Settings and 1)
    await edit_and_undo(ed, sid, "cascade_count", read("numShadowCascades"),
                        lambda: pick(ed, ui, CASCADE_COUNT, "2", "cascade_count"), differs)

    # the cascade split bar (manual splits, 4 cascades)
    await ed.call("set_properties", scene=sid, entity=sun, values={"light": {"cascadeSplitMode": "manual", "numShadowCascades": 4}})
    await ed.call("wait", frames=4)
    await editor_shot(ed, "split_bar")
    # a handle of the bar (the first find_ui_item reports visible), dragged right: one step
    handle = await ui.at(SPLIT_HANDLE)
    y = handle[1]

    async def drag():
        start = ui.screen(handle)
        await ui.drag(start, (start[0] + 16, ui.screen((0, y))[1]), steps=8)
    await edit_and_undo(ed, sid, "split_bar", read("cascadeSplits"), drag, differs)

    # the mesh's Static Shadow Caster
    await ed.call("select_entities", scene=sid, entities=[cube])
    await ed.call("wait", frames=4)
    await editor_shot(ed, "mesh")

    async def static_flag():
        got = await ed.call("get_entity", scene=sid, entity=cube, components=["mesh"])
        return harness.find_key(got, "staticShadowCaster")
    await edit_and_undo(ed, sid, "static_shadow_caster", static_flag, lambda: _click_label(ui, STATIC_SHADOW_CASTER), differs)
