"""The Fluid Simulation inspector (editor/window/InspectorFluid.cpp), its texture-slot link button, the
edit-mode preview and the Scene view's Paint Dye brush (editor/util/FluidEditorTools.cpp), driven like
a human with xdotool in the harness's Xvfb, on the real GPU simulator:

- a slider drag is one undo step, the reset arrow puts the default back;
- the Bloom group checkbox turns bloom off (and back with undo);
- the Preset combo applies a preset's fields in one undo step and leaves the others;
- a texture slot's droplet button links a scene fluid ("fluid|<entity>"), undone in one step;
- the preview animates the fluid in the Scene view, Pause holds it, a preset changes its look;
- Paint Dye: [ and ] scale splatRadius as one undo step a run,
  an armed click beside the Sprite keeps the selection, a stroke paints where it was drawn (the
  dye lands in the quadrant painted: orientation) without moving or deselecting the Sprite, and
  after Esc a drag moves the Sprite again.

Widgets are found where the layout puts them through the editor's test-only widget locator
(Editor(ui_probe=True), editor/util/UIProbe.h): the inspector is scrolled until the widget is
drawn whole, then clicked at its centre. Pixels are read from editor screenshots inside the
fluid Sprite's rectangle on the Scene view (the locator's "scene.fluidSprite").

Runs in a new empty project (EDITOR create).
"""

from __future__ import annotations

import asyncio
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

from harness import Editor
import ui_input

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from htmlui_pixels import decode_png  # noqa: E402

EDITOR = {"xvfb": "harness", "create": True, "name": "Fluid Inspector", "ui_probe": True}
CI_EXCLUDED = ("long-running (about 30 minutes: the Inspector's fluid preview drawn by the software renderer); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = 900

KEY = "fluidsimulation"
S: dict = {}

# the screenshots also go here when it exists (build/fluid-inspector-shots: the box's test runs
# leave nothing else behind)
SHOTS = Path(__file__).resolve().parents[3] / "build" / "fluid-inspector-shots"


class Ui(ui_input.Ui):
    """The shared frame-paced input (tools/mcp/ui_input.py) at the UIProbe's screen points: each
    action waits for editor frames, not a sleep (a drag whose press and moves land in one frame is
    never a drag)."""

    async def focus(self):
        super().focus()
        await self.frames()

    async def move(self, x: float, y: float):
        await super().move((x, y))

    async def click(self, x: float, y: float):
        await super().click((x, y))

    async def key(self, name: str):
        await super().key(name)

    async def drag(self, x: float, y: float, dx: float, dy: float = 0.0, steps: int = 8):
        """press, move by (dx, dy) a frame per step, release"""
        await super().drag((x, y), (x + dx, y + dy), steps=steps)

    async def wheel(self, at, down: bool, notches: int):
        await super().wheel(at, notches if down else -notches)

    async def frames(self, n: int = 3):
        await super().frames(n)

    async def reveal(self, name: str) -> list[float]:
        """The widget's rectangle, scrolling the inspector from its top until it is drawn whole."""
        await self.frames()
        rect = self.ed.ui_items().get(name)
        if rect:
            return rect
        panel = await self.ed.ui_rect("inspector")
        self.ed.check(panel, "the inspector is drawn")
        at = center(panel)
        await self.wheel(at, False, 60)
        for _ in range(80):
            await self.frames()
            rect = self.ed.ui_items().get(name)
            if rect:
                return rect
            await self.wheel(at, True, 2)
        self.ed.fail(f"{name} is never drawn in the inspector: {sorted(self.ed.ui_items())}")

    async def click_item(self, name: str):
        rect = await self.reveal(name)
        await self.click(*center(rect))

    async def click_popup_item(self, name: str):
        """an item of the popup or combo opened last (drawn at once, no scrolling)"""
        rect = await self.ed.ui_rect(name)
        self.ed.check(rect, f"{name} is drawn: {sorted(self.ed.ui_items())}")
        await self.click(*center(rect))


def center(rect) -> tuple[float, float]:
    return (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0


def close(a, b, tol=1e-3) -> bool:
    if isinstance(a, (list, tuple)):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return isinstance(a, (int, float)) and math.isclose(float(a), float(b), rel_tol=tol, abs_tol=tol)


async def values(ed: Editor, entity: int) -> dict:
    got = await ed.call("get_entity", scene=S["scene"], entity=entity, components=[KEY])
    return got["components"][KEY]


async def set_values(ed: Editor, entity: int, changed: dict):
    await ed.call("set_properties", scene=S["scene"], entity=entity, values={KEY: changed})


async def shot(ed: Editor, name: str):
    png = await ed.screenshot("editor", path=str(ed.scratch.root / f"{name}.png"), max_width=1600)
    if SHOTS.is_dir():
        (SHOTS / f"{name}.png").write_bytes(png)
    return decode_png(png)


def region(image, box, step=2):
    """the pixels of a box (x0, y0, x1, y1) of a decoded image, every step-th one"""
    w, h, ch, rows = image
    x0, y0, x1, y1 = (int(v) for v in box)
    out = []
    for y in range(max(0, y0), min(h, y1), step):
        row = rows[y]
        for x in range(max(0, x0), min(w, x1), step):
            out.append(tuple(row[x * ch:x * ch + 3]))
    return out


def brightness(pixels) -> float:
    return sum(0.299 * r + 0.587 * g + 0.114 * b for r, g, b in pixels) / max(1, len(pixels))


def difference(a, b) -> float:
    """mean absolute difference of two equal regions"""
    return sum(abs(p - q) for pa, pb in zip(a, b) for p, q in zip(pa, pb)) / max(1, 3 * len(a))


def inset(rect, margin=0.04):
    """the inner part of a rectangle (no outline or anti-aliased edge)"""
    x0, y0, x1, y1 = rect
    mx, my = (x1 - x0) * margin, (y1 - y0) * margin
    return x0 + mx, y0 + my, x1 - mx, y1 - my


def quadrants(rect):
    """upper-left, upper-right, lower-left, lower-right of a screen rectangle (y down)"""
    x0, y0, x1, y1 = inset(rect)
    xm, ym = (x0 + x1) / 2, (y0 + y1) / 2
    return {"ul": (x0, y0, xm, ym), "ur": (xm, y0, x1, ym), "ll": (x0, ym, xm, y1), "lr": (xm, ym, x1, y1)}


async def select(ed: Editor, *entities: int, frames: int = 10):
    await ed.call("select_entities", scene=S["scene"], entities=list(entities))
    await ed.call("wait", frames=frames)


async def position(ed: Editor, entity: int):
    got = await ed.call("get_entity", scene=S["scene"], entity=entity, components=["transform"], properties=["position"])
    return got["components"]["transform"]["position"]


async def selection(ed: Editor) -> list:
    got = await ed.call("get_selection", scene=S["scene"])
    return [e["id"] if isinstance(e, dict) else e for e in got.get("entities", [])]


async def sprite_rect(ed: Editor):
    rect = await ed.ui_rect("scene.fluidSprite")
    ed.check(rect, f"the fluid's Sprite is on the Scene view: {sorted(ed.ui_items())}")
    return rect


# ---------------------------------------------------------------------------- rows


async def test_slider_drag_is_one_undo_step(ed: Editor):
    ed.check(shutil.which("xdotool") and ed._xvfb and ed._xvfb.display, "needs xdotool and the harness's Xvfb")
    made = await ed.call("create_scene", name="Fluids", type="2D")
    S["scene"] = made["scene"]["id"]
    bare = await ed.call("create_entity", scene=S["scene"], type="empty", name="Bare")
    S["bare"] = bare["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["bare"], component="fluid")
    await ed.call("select_scene", scene=S["scene"])
    await select(ed, S["bare"], frames=30)
    S["ui"] = ui = Ui(ed)
    await ui.focus()

    x, y = center(await ui.reveal("fluid.value.curl"))
    await ui.drag(x, y, -60)
    curl = (await values(ed, S["bare"]))["curl"]
    ed.check(0.0 <= curl < 29.0, f"dragged left: curl {curl}")

    await ed.call("undo", scene=S["scene"])
    ed.check(close((await values(ed, S["bare"]))["curl"], 30.0), "one undo step reverts the whole drag")
    await ed.call("redo", scene=S["scene"])
    ed.check(close((await values(ed, S["bare"]))["curl"], curl), "redo")

    # the reset arrow next to the changed row's label
    await ui.click_item("fluid.default.curl")
    ed.check(close((await values(ed, S["bare"]))["curl"], 30.0), "the reset arrow puts Pavel's 30 back")
    await ed.call("undo", scene=S["scene"])
    ed.check(close((await values(ed, S["bare"]))["curl"], curl), "reset undone")
    await ed.call("undo", scene=S["scene"])


async def test_bloom_group_checkbox(ed: Editor):
    ui: Ui = S["ui"]
    await ui.click_item("fluid.group.bloom")
    ed.check(not (await values(ed, S["bare"]))["bloom"], "the group checkbox turns bloom off")
    await ui.frames()
    await ed.call("undo", scene=S["scene"])
    ed.check((await values(ed, S["bare"]))["bloom"], "undo turns bloom back on")


async def test_preset_combo(ed: Editor):
    ui: Ui = S["ui"]
    await set_values(ed, S["bare"], {"interactive": False, "seed": 42})
    before = await values(ed, S["bare"])
    await ui.click_item("fluid.preset")
    await ui.click_popup_item("fluid.preset.smoke")
    v = await values(ed, S["bare"])
    # Smoke (FluidPresets.cpp)
    for name, want in {"dyeResolution": 512, "densityDissipation": 0.4, "velocityDissipation": 0.1, "curl": 20.0,
                       "splatRadius": 0.5, "splatForce": 4000.0, "colorful": False, "bloom": False,
                       "sunraysWeight": 0.6}.items():
        ed.check(close(v[name], want) if not isinstance(want, bool) else v[name] == want, f"Smoke {name}: {v[name]}")
    # what presets never touch
    ed.check_eq(v["interactive"], False, "Interactive stays")
    ed.check_eq(v["seed"], 42, "the seed stays")

    await ed.call("undo", scene=S["scene"])
    after = await values(ed, S["bare"])
    for name, value in before.items():
        ed.check(close(after[name], value) if isinstance(value, (int, float)) and not isinstance(value, bool)
                 else after[name] == value, f"one undo step restores {name}: {after[name]!r} != {value!r}")
    await ed.call("redo", scene=S["scene"])
    ed.check_eq((await values(ed, S["bare"]))["dyeResolution"], 512, "redo")
    await ed.call("undo", scene=S["scene"])


# ---------------------------------------------------------------------------- texture slot link


async def test_texture_slot_fluid_button(ed: Editor):
    ui: Ui = S["ui"]
    panel = await ed.call("create_entity", scene=S["scene"], type="image", name="Panel")
    S["image"] = panel["entity_id"]
    await select(ed, S["image"])
    await ui.click_item("texture.fluid.texture")
    await ui.click_popup_item(f"texture.fluid.item.{S['bare']}")
    got = await ed.call("get_entity", scene=S["scene"], entity=S["image"], components=["ui"], properties=["texture"])
    tex = got["components"]["ui"]["texture"]
    ed.check(isinstance(tex, dict) and "fluid" in tex, f"the popup linked a fluid: {tex!r}")
    ed.check_eq(tex["fluid"]["id"], S["bare"], "linked to the fluid picked")
    await shot(ed, "fluid-linked")    # the slot names it

    await ed.call("undo", scene=S["scene"])
    got = await ed.call("get_entity", scene=S["scene"], entity=S["image"], components=["ui"], properties=["texture"])
    ed.check(got["components"]["ui"]["texture"] is None, f"one undo step unlinks: {got['components']['ui']['texture']}")
    await ed.check_alive()


# ---------------------------------------------------------------------------- the real fluid


async def test_preview_animates_and_presets_change_the_look(ed: Editor):
    ui: Ui = S["ui"]
    made = await ed.call("create_entity", scene=S["scene"], type="fluid_simulation")
    S["fluid"] = fluid = made["entity_id"]
    await set_values(ed, fluid, {"seed": 7})
    await select(ed, fluid)
    await ed.call("frame_entity", scene=S["scene"], entity=fluid)
    await ui.frames(10)
    rect = inset(await sprite_rect(ed))

    # Play: the start burst moves
    await ui.click_item("fluid.preview.play")
    await ui.settle(1.0, frames=4)
    a = region(await shot(ed, "preview-a"), rect)
    await ui.settle(1.0, frames=4)
    b = region(await shot(ed, "preview-b"), rect)
    ed.check(brightness(a) > 8, f"the fluid shows dye on its Sprite (brightness {brightness(a):.1f})")
    ed.check(difference(a, b) > 1.0, f"the preview animates (difference {difference(a, b):.2f})")

    # Pause holds it still
    await ui.click_item("fluid.preview.pause")
    await ui.settle(0.5, frames=4)
    c = region(await shot(ed, "paused-c"), rect)
    await ui.settle(1.0, frames=4)
    d = region(await shot(ed, "paused-d"), rect)
    ed.check(difference(c, d) < 0.5, f"paused, it holds still (difference {difference(c, d):.2f})")

    # a preset changes the look of the same (paused) fluid: Mobile has no shading, bloom or sunrays
    await ui.click_item("fluid.preset")
    await ui.click_popup_item("fluid.preset.mobile")
    await ui.settle(0.5, frames=4)
    e = region(await shot(ed, "preset-mobile"), rect)
    v = await values(ed, fluid)
    ed.check(not v["shading"] and not v["bloom"] and not v["sunrays"], f"Mobile applied: {v}")
    ed.check(difference(d, e) > 1.0, f"the preset changes the look (difference {difference(d, e):.2f})")
    await ed.call("undo", scene=S["scene"])
    await ui.settle(0.5, frames=4)
    g = region(await shot(ed, "preset-undone"), rect)
    ed.check(difference(d, g) < difference(d, e) / 2,
             f"undo brings the look back ({difference(d, g):.2f} vs {difference(d, e):.2f})")

    # Stop restarts it
    await ui.click_item("fluid.preview.stop")
    await ed.check_alive()


async def test_paint_dye_brush(ed: Editor):
    ui: Ui = S["ui"]
    fluid = S["fluid"]
    # a black fluid that keeps its dye where it is put (no push, no swirl, no glow): a stroke's
    # dye stays where it was painted
    await set_values(ed, fluid, {"splatsOnStart": False, "densityDissipation": 0.1, "splatForce": 0.0, "curl": 0.0,
                                 "bloom": False, "sunrays": False})
    await ui.click_item("fluid.restart")
    await ui.frames(10)
    sprite = await sprite_rect(ed)
    quads = quadrants(sprite)
    black = region(await shot(ed, "paint-black"), inset(sprite))
    ed.check(brightness(black) < 6, f"restarted without the start burst it is black ({brightness(black):.1f})")
    start = await position(ed, fluid)

    # Arm the brush before the input and selection checks.
    await ui.click_item("fluid.paint")
    sx, sy = center(sprite)
    await ui.move(sx, sy)
    await ui.move(sx + 5, sy + 5)
    await ui.frames(5)

    # a click beside the Sprite (the Scene view takes the keys after the inspector's clicks)
    # neither paints nor deselects
    view = await ed.ui_rect("scene.view")
    beside = ((view[0] + sprite[0]) / 2, (sprite[1] + sprite[3]) / 2)
    ed.check(view[0] + 10 < beside[0] < sprite[0] - 10, f"room beside the Sprite: view {view}, sprite {sprite}")
    await ui.click(*beside)
    ed.check_eq(await selection(ed), [fluid], "an armed click on empty space keeps the selection")

    # [ and ]: splatRadius, one undo step a run
    await ui.key("bracketright")
    await ui.key("bracketright")
    radius = (await values(ed, fluid))["splatRadius"]
    ed.check(close(radius, 0.25 * 1.15 * 1.15), f"] twice: {radius}")
    await ui.key("bracketleft")
    radius = (await values(ed, fluid))["splatRadius"]
    ed.check(close(radius, 0.25 * 1.15), f"[ once: {radius}")
    await ed.call("undo", scene=S["scene"])
    ed.check(close((await values(ed, fluid))["splatRadius"], 0.25), "one undo step for the run")

    # a stroke across the upper-left quadrant paints there (the view's up is the fluid's up)
    x0, y0, x1, y1 = quads["ul"]
    w, h = x1 - x0, y1 - y0
    await ui.drag(x0 + 0.25 * w, y0 + 0.3 * h, 0.5 * w, 0.3 * h, steps=10)
    await ui.move(view[0] + 5, view[1] + 5)       # the circle off the Sprite
    await ui.settle(0.5, frames=4)
    image = await shot(ed, "paint-stroke")
    lit = {name: brightness(region(image, box)) for name, box in quads.items()}
    ed.check(lit["ul"] > 4 and lit["ul"] > 3 * max(lit["ur"], lit["ll"], lit["lr"]),
             f"the dye is where it was painted, the upper-left quadrant: {lit}")
    ed.check(close(await position(ed, fluid), start), "the stroke did not move the Sprite")
    ed.check_eq(await selection(ed), [fluid], "nor change the selection")

    # Esc puts the brush down: ] does nothing and a drag moves the Sprite again
    await ui.click(*beside)
    await ui.key("Escape")
    await ui.key("bracketright")
    ed.check(close((await values(ed, fluid))["splatRadius"], 0.25), "] after Esc changes nothing")
    await shot(ed, "paint-disarmed")
    await ui.drag(sx - 40, sy, 60)
    ed.check(not close(await position(ed, fluid), start), "after Esc a drag moves the Sprite")
    await ed.call("undo", scene=S["scene"])
    ed.check(close(await position(ed, fluid), start), "move undone")
    await ed.check_alive()


# ---------------------------------------------------------------------------- a play-created fluid


async def test_preview_bar_on_a_play_created_fluid(ed: Editor):
    # a fluid the running game made is read only in the Inspector (Stop destroys it), but its
    # preview bar is no edit: Random Splats still bursts it
    ui = S["ui"]
    await ed.call("play", action="start", scene=S["scene"], timeout_seconds=300)
    try:
        got = await ed.call("eval_lua", code='local f = FluidSimulation(getScene()); f.name = "Spawned"; '
                                             'f.output = FluidOutput.TEXTURE; f.paused = true; f.splatsOnStart = false; '
                                             'f.randomSplatCount = 7; f.dyeResolution = 128; f.simResolution = 32; '
                                             'return f.entity')
        spawned = got["values"][0]
        live = await ed.call("get_runtime_entity", entity="Spawned", components=["fluid"], include_script_state=False)
        ed.check(live["created_at_runtime"], f"made by the game: {live}")
        await ed.call("select_entities", scene=S["scene"], entities=[spawned])
        await ed.call("wait", frames=10)
        await ui.focus()
        before = live.get("fluid_stats", {}).get("splats", 0)
        await ui.click_item("fluid.burst")
        after = before
        for _ in range(20):
            await ed.call("wait", frames=3)
            live = await ed.call("get_runtime_entity", entity="Spawned", components=["fluid"], include_script_state=False)
            after = live.get("fluid_stats", {}).get("splats", 0)
            if after >= before + 7:
                break
        ed.check(after >= before + 7, f"Random Splats bursts a read-only fluid: splats {before} -> {after}")
        # its property rows stay read only: a click on the Bloom checkbox changes nothing
        await ui.click_item("fluid.group.bloom")
        live = await ed.call("get_runtime_entity", entity="Spawned", components=["fluid"], include_script_state=False)
        ed.check(live["components"][KEY]["bloom"] is True, f"the rows stay disabled: {live['components'][KEY]['bloom']}")
    finally:
        await ed.call("play", action="stop")
    await ed.check_alive()
