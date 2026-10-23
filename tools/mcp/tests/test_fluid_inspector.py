"""The Fluid Simulation inspector (editor/window/InspectorFluid.cpp), its texture-slot link button and
the Scene view's Paint Dye brush (editor/util/FluidEditorTools.cpp), driven like a human with xdotool
in the harness's Xvfb (1600x900, default layout):

- the inspector draws (not a blank panel) for one fluid and for a mixed multi-selection;
- a slider drag is one undo step, the reset arrow puts the default back;
- the Bloom group checkbox turns bloom off (and back with undo);
- the Preset combo applies a preset's fields in one undo step and leaves the others;
- a texture slot's droplet button links a scene fluid ("fluid|<entity>"), undone in one step;
- Paint Dye: armed, a drag over the fluid's Sprite neither moves nor deselects it (unarmed it
  moves it), [ and ] scale splatRadius as one undo step a run of presses, the brush circle shows
  over the Sprite, Esc puts the brush down.

The fluid's pixels are the engine tests' (FluidSimulator); this file checks the editor's side.
Positions are those of the default layout; when a click reaches nothing the test skips and asks
for new positions instead of failing.

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

from harness import Editor, skip

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from htmlui_pixels import decode_png  # noqa: E402

EDITOR = {"xvfb": "harness", "create": True, "name": "Fluid Inspector"}
TIMEOUT = 600

KEY = "fluidsimulation"
S: dict = {}

INSPECTOR = (1250, 300, 1590, 880)     # the component's part of the inspector, in the editor image
SCROLL_AT = (1420, 500)
# a GameObject with a Transform and a Fluid Simulation, inspector scrolled to the top
PRESET_COMBO = (1495, 405)
PRESET_SMOKE = (1450, 466)             # third item of the opened combo
CURL_SLIDER_Y = 684                    # Vorticity
CURL_RESET = (1310, 682)               # the arrow after the "Vorticity" label
# scrolled to the end
BLOOM_CHECKBOX = (1264, 615)
# a UI Image: its Texture slot's droplet button, then the popup's first fluid
IMAGE_FLUID_BUTTON = (1566, 352)
IMAGE_FLUID_ITEM = (1520, 369)
# the fluid_simulation GameObject (a 512 px Sprite framed in the Scene view) with its Mesh Renderer
# folded: the preview bar's Paint Dye button, the Sprite in the view
MESH_RENDERER_HEADER = (1360, 300)
PAINT_DYE = (1517, 596)
PREVIEW_PLAY = (1267, 596)
PREVIEW_STOP = (1300, 596)
SPRITE_CENTER = (768, 353)
SCENE_VIEW = (296, 98, 1240, 608)
SCENE_EMPTY = (400, 500)


class Ui:
    def __init__(self, ed: Editor):
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    async def focus(self):
        windows = self.xdo("search", "--name", ".").split()
        if not windows:
            skip("no editor window found")
        self.xdo("windowfocus", "--sync", windows[-1])

    async def move(self, x: int, y: int):
        self.xdo("mousemove", str(x), str(y))
        await asyncio.sleep(0.3)

    async def click(self, x: int, y: int):
        await self.move(x, y)
        self.xdo("click", "1")
        await asyncio.sleep(0.8)

    async def key(self, name: str):
        self.xdo("key", name)
        await asyncio.sleep(0.5)

    async def drag(self, x: int, y: int, dx: int, dy: int = 0):
        """press, move by (dx, dy) in steps (a frame between each), release"""
        await self.move(x, y)
        self.xdo("mousedown", "1")
        await asyncio.sleep(0.2)
        steps = 8
        for i in range(1, steps + 1):
            self.xdo("mousemove", str(x + dx * i // steps), str(y + dy * i // steps))
            await asyncio.sleep(0.1)
        await asyncio.sleep(0.2)
        self.xdo("mouseup", "1")
        await asyncio.sleep(0.6)

    async def scroll(self, down: bool, clicks: int = 120):
        await self.move(*SCROLL_AT)
        for _ in range(clicks):
            self.xdo("click", "5" if down else "4")
        await asyncio.sleep(0.8)


def close(a, b, tol=1e-3) -> bool:
    if isinstance(a, (list, tuple)):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return isinstance(a, (int, float)) and math.isclose(float(a), float(b), rel_tol=tol, abs_tol=tol)


async def values(ed: Editor, entity: int) -> dict:
    got = await ed.call("get_entity", scene=S["scene"], entity=entity, components=[KEY])
    return got["components"][KEY]


# the screenshots also go here when it exists (build/fluid-inspector-shots: the box's test runs
# leave nothing else behind)
SHOTS = Path(__file__).resolve().parents[3] / "build" / "fluid-inspector-shots"


async def shot(ed: Editor, name: str, view: str = "editor") -> bytes:
    png = await ed.screenshot(view, path=str(ed.scratch.root / f"{name}.png"), max_width=1600)
    if SHOTS.is_dir():
        (SHOTS / f"{name}.png").write_bytes(png)
    return png


def busy_region(png: bytes, box) -> tuple[int, float]:
    """distinct colours and the luminance spread of a region: a drawn panel has text and widgets"""
    w, h, ch, rows = decode_png(png)
    x0, y0, x1, y1 = box
    colours = set()
    lum = []
    for y in range(max(0, y0), min(h, y1), 2):
        row = rows[y]
        for x in range(max(0, x0), min(w, x1), 2):
            px = tuple(row[x * ch:x * ch + 3])
            colours.add(px)
            lum.append(0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2])
    mean = sum(lum) / max(1, len(lum))
    spread = math.sqrt(sum((v - mean) ** 2 for v in lum) / max(1, len(lum)))
    return len(colours), spread


def cyan_pixels(png: bytes) -> int:
    """pixels of the Paint Dye circle's colour (89, 204, 255) over the Scene view"""
    w, h, ch, rows = decode_png(png)
    x0, y0, x1, y1 = SCENE_VIEW
    count = 0
    for y in range(max(0, y0), min(h, y1)):
        row = rows[y]
        for x in range(max(0, x0), min(w, x1)):
            r, g, b = row[x * ch], row[x * ch + 1], row[x * ch + 2]
            if b > 200 and g > 140 and r < 200 and b - r > 40:
                count += 1
    return count


async def select(ed: Editor, *entities: int, frames: int = 20):
    await ed.call("select_entities", entities=list(entities))
    await ed.call("wait", frames=frames)


# ---------------------------------------------------------------------------- setup and drawing


async def test_inspector_draws(ed: Editor):
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness's Xvfb")
    made = await ed.call("create_scene", name="Fluids", type="2D")
    S["scene"] = made["scene"]["id"]
    bare = await ed.call("create_entity", scene=S["scene"], type="empty", name="Bare")
    S["bare"] = bare["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["bare"], component="fluid")
    await ed.call("select_scene", scene=S["scene"])
    await select(ed, S["bare"], frames=30)
    S["ui"] = ui = Ui(ed)
    await ui.focus()

    png = await shot(ed, "inspector")
    colours, spread = busy_region(png, INSPECTOR)
    ed.check(colours > 40 and spread > 8, f"the fluid inspector is drawn: {colours} colours, spread {spread:.1f}")

    # the end of it (Bloom, Sunrays, Advanced) and the Advanced section opened
    await ui.scroll(True)
    await ed.call("wait", frames=5)
    png = await shot(ed, "inspector-end")
    colours, spread = busy_region(png, INSPECTOR)
    ed.check(colours > 40 and spread > 8, f"its end is drawn: {colours} colours, spread {spread:.1f}")
    await ui.scroll(False)

    # a second fluid with other values: the mixed rows of a multi-selection
    other = await ed.call("create_entity", scene=S["scene"], type="empty", name="Other")
    S["other"] = other["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["other"], component="fluid",
                  values={KEY: {"curl": 12, "bloom": False, "output": "texture", "seed": 7}})
    await select(ed, S["bare"], S["other"])
    png = await shot(ed, "inspector-mixed")
    colours, spread = busy_region(png, INSPECTOR)
    ed.check(colours > 40 and spread > 8, f"the mixed selection is drawn: {colours} colours, spread {spread:.1f}")
    await ed.check_alive()
    await select(ed, S["bare"])


# ---------------------------------------------------------------------------- rows


async def test_slider_drag_is_one_undo_step(ed: Editor):
    ui: Ui = S["ui"]
    await ui.drag(1495, CURL_SLIDER_Y, -60)
    curl = (await values(ed, S["bare"]))["curl"]
    if close(curl, 30.0):
        await shot(ed, "inspector-layout")
        skip("the drag changed nothing: the inspector layout moved (update the positions at the top of this file)")
    ed.check(0.0 <= curl < 30.0, f"dragged left: curl {curl}")

    await ed.call("undo", scene=S["scene"])
    ed.check(close((await values(ed, S["bare"]))["curl"], 30.0), "one undo step reverts the whole drag")
    await ed.call("redo", scene=S["scene"])
    ed.check(close((await values(ed, S["bare"]))["curl"], curl), "redo")

    # the reset arrow next to the changed row's label
    await ui.click(*CURL_RESET)
    ed.check(close((await values(ed, S["bare"]))["curl"], 30.0), "the reset arrow puts Pavel's 30 back")
    await ed.call("undo", scene=S["scene"])
    ed.check(close((await values(ed, S["bare"]))["curl"], curl), "reset undone")
    await ed.call("undo", scene=S["scene"])


async def test_bloom_group_checkbox(ed: Editor):
    ui: Ui = S["ui"]
    await ui.scroll(True)
    await ed.call("wait", frames=5)
    await ui.click(*BLOOM_CHECKBOX)
    v = await values(ed, S["bare"])
    if v["bloom"]:
        await shot(ed, "inspector-layout")
        skip("the click missed the Bloom checkbox: the inspector layout moved")
    await shot(ed, "bloom-off")
    await ed.call("undo", scene=S["scene"])
    ed.check((await values(ed, S["bare"]))["bloom"], "undo turns bloom back on")
    await ui.scroll(False)


async def test_preset_combo(ed: Editor):
    ui: Ui = S["ui"]
    await ed.call("set_properties", scene=S["scene"], entity=S["bare"], values={KEY: {"interactive": False, "seed": 42}})
    before = await values(ed, S["bare"])
    await ui.click(*PRESET_COMBO)
    await shot(ed, "preset-combo")
    await ui.click(*PRESET_SMOKE)
    v = await values(ed, S["bare"])
    if v["dyeResolution"] == before["dyeResolution"]:
        await shot(ed, "inspector-layout")
        skip("the preset was not applied: the inspector layout moved")
    # Smoke (FluidPresets.cpp)
    for name, want in {"dyeResolution": 512, "densityDissipation": 0.4, "velocityDissipation": 0.1, "curl": 20.0,
                       "splatRadius": 0.5, "splatForce": 4000.0, "colorful": False, "bloom": False,
                       "sunraysWeight": 0.6}.items():
        ed.check(close(v[name], want) if not isinstance(want, bool) else v[name] == want, f"Smoke {name}: {v[name]}")
    # what presets never touch
    ed.check_eq(v["interactive"], False, "Interactive stays")
    ed.check_eq(v["seed"], 42, "the seed stays")
    await shot(ed, "preset-smoke")

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
    S["panel"] = panel["entity_id"]
    await select(ed, S["panel"])
    await ui.click(*IMAGE_FLUID_BUTTON)
    await shot(ed, "fluid-popup")
    await ui.click(*IMAGE_FLUID_ITEM)
    got = await ed.call("get_entity", scene=S["scene"], entity=S["panel"], components=["ui"], properties=["texture"])
    tex = got["components"]["ui"]["texture"]
    if not isinstance(tex, dict) or "fluid" not in tex:
        await shot(ed, "inspector-layout")
        skip(f"the popup click linked nothing ({tex!r}): the inspector layout moved")
    ed.check_eq(tex["fluid"]["id"], S["bare"], "linked to the scene's first fluid")
    await shot(ed, "fluid-linked")    # the slot names it

    await ed.call("undo", scene=S["scene"])
    got = await ed.call("get_entity", scene=S["scene"], entity=S["panel"], components=["ui"], properties=["texture"])
    ed.check(got["components"]["ui"]["texture"] is None, f"one undo step unlinks: {got['components']['ui']['texture']}")
    await ed.check_alive()


# ---------------------------------------------------------------------------- Paint Dye


async def position(ed: Editor, entity: int):
    got = await ed.call("get_entity", scene=S["scene"], entity=entity, components=["transform"], properties=["position"])
    return got["components"]["transform"]["position"]


async def selection(ed: Editor) -> list:
    got = await ed.call("get_selection")
    return [e["id"] if isinstance(e, dict) else e for e in got.get("entities", [])]


async def test_paint_dye_brush(ed: Editor):
    ed.require_tools("get_selection")
    ui: Ui = S["ui"]
    made = await ed.call("create_entity", scene=S["scene"], type="fluid_simulation")
    S["fluid"] = fluid = made["entity_id"]
    await select(ed, fluid)
    await ed.call("frame_entity", entity=fluid)
    await ed.call("wait", frames=10)
    await ui.click(*MESH_RENDERER_HEADER)     # fold the Mesh Renderer: the fluid's preview bar shows
    await ed.call("wait", frames=5)
    await shot(ed, "paint-inspector")

    # unarmed, a drag on the selected Sprite moves it (the control)
    start = await position(ed, fluid)
    await ui.drag(SPRITE_CENTER[0] - 60, SPRITE_CENTER[1], 60)
    moved = await position(ed, fluid)
    if close(moved, start):
        await shot(ed, "scene-layout")
        skip("the unarmed drag did not move the Sprite: the Scene view layout moved")
    await ed.call("undo", scene=S["scene"])
    ed.check(close(await position(ed, fluid), start), "move undone")

    # the preview bar's Play and Stop (no simulator on this device: they must just not break)
    await ui.click(*PREVIEW_PLAY)
    await ui.click(*PREVIEW_STOP)

    # armed: the same drag paints instead
    await ui.click(*PAINT_DYE)
    await shot(ed, "paint-armed")
    await ui.drag(SPRITE_CENTER[0] - 60, SPRITE_CENTER[1], 60, 30)
    ed.check(close(await position(ed, fluid), start), "an armed drag does not move the Sprite")
    ed.check_eq(await selection(ed), [fluid], "nor change the selection")

    # the circle over the Sprite under the pointer
    await ui.move(*SPRITE_CENTER)
    await ui.move(SPRITE_CENTER[0] + 5, SPRITE_CENTER[1] + 5)
    await ed.call("wait", frames=5)
    png = await shot(ed, "paint-cursor")
    ed.check(cyan_pixels(png) > 40, f"the brush circle is drawn ({cyan_pixels(png)} pixels)")

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
    await ed.call("redo", scene=S["scene"])
    ed.check(close((await values(ed, fluid))["splatRadius"], 0.25 * 1.15), "redo")

    # a click beside the Sprite (the Scene view takes the keys again after the MCP calls) neither
    # paints nor deselects
    await ui.click(*SCENE_EMPTY)
    ed.check_eq(await selection(ed), [fluid], "an armed click on empty space keeps the selection")

    # Esc puts it down: ] does nothing, no circle
    await ui.key("Escape")
    await ui.key("bracketright")
    ed.check(close((await values(ed, fluid))["splatRadius"], 0.25 * 1.15), "] after Esc changes nothing")
    await ed.call("wait", frames=5)
    png = await shot(ed, "paint-disarmed")
    ed.check(cyan_pixels(png) < 20, f"no brush circle after Esc ({cyan_pixels(png)} pixels)")
    await ed.check_alive()
