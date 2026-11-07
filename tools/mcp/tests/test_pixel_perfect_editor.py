"""S41 AC15, AC18 (docs/specs/S41-pixel-perfect-camera.md R19-R23): the Pixel Perfect Camera through
MCP and the editor. The component goes on a camera only (one undo step), its layout values read
for the Game view's size (k = 4 for 320 x 180 at 1280 x 720), configure_game_view's pixel_scale
forces k (1x is the base image at one physical pixel per pixel), the project's Pixels Per Unit and
integer Scaling modes are settings (an .aseprite sheet of pixels per unit "project" imports
again with the project's), and in the Scene view (real input in the harness's Xvfb) a
wheel notch steps the Pixel Zoom (2x to 3x) and the Move tool moves on whole pixels.

Runs on an owned empty project with a 320 x 180 canvas.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess

from harness import REPO, Editor, skip
from htmlui_pixels import decode_png

EDITOR = {"xvfb": "harness", "create": True, "name": "Pixel Perfect"}
CI = True
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("set_project_settings", values={"graphics.canvas_width": 320, "graphics.canvas_height": 180})
    made = await ed.call("create_scene", name="PixelMain", type="2D", path="scenes/PixelMain")
    S["scene"] = made["scene"]["id"]
    cam = await ed.call("create_entity", scene=S["scene"], type="camera", name="Main Camera")
    S["camera"] = cam["entity_id"]
    await ed.call("set_main_camera", scene=S["scene"], entity="Main Camera")
    empty = await ed.call("create_entity", scene=S["scene"], type="empty", name="Not A Camera")
    S["empty"] = empty["entity_id"]
    sprite = await ed.call("create_entity", scene=S["scene"], type="sprite", name="Hero", position=[100.5, 60.25, 0])
    S["sprite"] = sprite["entity_id"]
    # dark on the white background, so a comparison of the Game view's pixels sees it
    await ed.call("set_properties", scene=S["scene"], entity=S["sprite"],
                  values={"sprite": {"width": 15, "height": 16},
                          "mesh": {"submeshes[0].material.baseColorFactor": [0.1, 0.2, 0.6, 1]}})
    # a white inside of the viewport, black bars: the image's bounds are the viewport
    await ed.call("set_scene_settings", scene=S["scene"], settings={"background_color": [1, 1, 1, 1]})
    await ed.call("configure_game_view", resolution="1280x720")
    await ed.call("save_scene", scene=S["scene"])
    return S


async def ppc_values(ed: Editor) -> dict:
    got = await ed.call("get_entity", scene=S["scene"], entity=S["camera"], components=["pixelperfectcamera"])
    return got["components"]["pixelperfectcamera"]


def white_bounds(png: bytes) -> tuple[int, int, int, int]:
    """(x0, y0, x1, y1) of the non-black pixels"""
    width, height, channels, rows = decode_png(png)
    xs, ys = [], []
    for y in range(height):
        row = rows[y]
        for x in range(width):
            if row[x * channels] > 128:
                xs.append(x)
                ys.append(y)
    return (min(xs), min(ys), max(xs) + 1, max(ys) + 1) if xs else (0, 0, 0, 0)


async def test_component_goes_on_a_camera_with_one_undo_step(ed: Editor):
    s = await setup(ed)
    bad = await ed.call_raw("add_component", {"scene": s["scene"], "entity": s["empty"], "component": "pixelperfectcamera"})
    ed.check(bad.is_error and "Camera" in bad.text, f"refused on a GameObject without a Camera: {bad.text}")
    await ed.call("add_component", scene=s["scene"], entity=s["camera"], component="pixelperfectcamera")
    got = await ed.call("get_entity", scene=s["scene"], entity=s["camera"])
    ed.check("pixelperfectcamera" in got["components"], f"added: {list(got['components'])}")
    await ed.call("undo", scene=s["scene"])
    got = await ed.call("get_entity", scene=s["scene"], entity=s["camera"])
    ed.check("pixelperfectcamera" not in got["components"], "one undo removes it")
    await ed.call("redo", scene=s["scene"])
    got = await ed.call("get_entity", scene=s["scene"], entity=s["camera"])
    ed.check("pixelperfectcamera" in got["components"], "redo adds it back")


async def test_layout_values_and_pixel_scale(ed: Editor):
    s = await setup(ed)
    got = await ed.call("get_entity", scene=s["scene"], entity=s["camera"])
    if "pixelperfectcamera" not in got["components"]:
        await ed.call("add_component", scene=s["scene"], entity=s["camera"], component="pixelperfectcamera")
    await ed.call("set_properties", scene=s["scene"], entity=s["camera"],
                  values={"pixelperfectcamera": {"referenceWidth": 320, "referenceHeight": 180}})
    await ed.call("configure_game_view", pixel_scale=0)
    values = await ppc_values(ed)
    ed.check(values.get("pixelRatio") == 4, f"k 4 for 320x180 at 1280x720: {values}")
    ed.check(values.get("baseWidth") == 320 and values.get("baseHeight") == 180, f"the base: {values}")
    bad = await ed.call_raw("set_properties", {"scene": s["scene"], "entity": s["camera"],
                                               "values": {"pixelperfectcamera": {"pixelRatio": 2}}})
    ed.check(bad.is_error, f"the layout values are read only: {bad.text}")
    # Use Project Canvas (0 x 0): the project's 320 x 180, not the editor view's canvas
    await ed.call("set_properties", scene=s["scene"], entity=s["camera"],
                  values={"pixelperfectcamera": {"referenceWidth": 0, "referenceHeight": 0}})
    values = await ppc_values(ed)
    ed.check(values.get("pixelRatio") == 4 and values.get("baseWidth") == 320, f"the project canvas as the reference: {values}")
    await ed.call("set_properties", scene=s["scene"], entity=s["camera"],
                  values={"pixelperfectcamera": {"referenceWidth": 320, "referenceHeight": 180}})

    view = await ed.call("configure_game_view", pixel_scale=1)
    ed.check(view.get("pixel_scale") == 1, f"the view reports it: {view}")
    values = await ppc_values(ed)
    ed.check(values.get("pixelRatio") == 1, f"1x forces k: {values}")
    one = await ed.screenshot(view="game", max_width=0)
    x0, y0, x1, y1 = white_bounds(one)
    ed.check((x1 - x0, y1 - y0) == (320, 180), f"the base image at 1:1: {(x0, y0, x1, y1)}")

    await ed.call("configure_game_view", pixel_scale=0)
    four = await ed.screenshot(view="game", max_width=0)
    fx0, fy0, fx1, fy1 = white_bounds(four)
    ed.check((fx1 - fx0, fy1 - fy0) == (1280, 720), f"Auto: 4x fills 1280x720: {(fx0, fy0, fx1, fy1)}")
    # pixel exact: the 1x image is the base image, each of its pixels the 4 x 4 block of the 4x one
    _, _, c1, rows1 = decode_png(one)
    _, _, c4, rows4 = decode_png(four)
    differing = 0
    for by in range(180):
        for bx in range(320):
            p1 = tuple(rows1[y0 + by][(x0 + bx) * c1:(x0 + bx) * c1 + 3])
            for dy in (0, 3):
                for dx in (0, 3):
                    q = (fx0 + bx * 4 + dx) * c4
                    if tuple(rows4[fy0 + by * 4 + dy][q:q + 3]) != p1:
                        differing += 1
    ed.check(differing == 0, f"the 1x screenshot is the 4x one's base pixels: {differing} differ")
    hero = sum(1 for by in range(180) for bx in range(320) if rows1[y0 + by][(x0 + bx) * c1] < 128)
    ed.check(hero > 0, "the comparison sees the sprite (not only the background)")
    bad = await ed.call_raw("configure_game_view", {"pixel_scale": 9})
    ed.check(bad.is_error, f"pixel_scale is 0 to 8: {bad.text}")


async def test_configure_game_view_group(ed: Editor):
    await setup(ed)
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("configure_game_view" not in default, "configure_game_view is hidden until editor_ux is enabled")
    await ed.call("tool_groups", enable=["editor_ux"])
    ed.check("configure_game_view" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")
    await ed.call("tool_groups", disable=["editor_ux"])


async def test_project_settings(ed: Editor):
    await setup(ed)
    await ed.call("set_project_settings", values={"graphics.pixels_per_unit": 16, "graphics.scaling_mode": "integer_expand"})
    got = await ed.call("get_project_settings", keys=["graphics.pixels_per_unit", "graphics.scaling_mode"])
    text = str(got)
    ed.check("16" in text and "integer_expand" in text, f"the settings read back: {got}")
    bad = await ed.call_raw("set_project_settings", {"values": {"graphics.pixels_per_unit": 300}})
    ed.check(bad.is_error, f"1 to 256: {bad.text}")
    await ed.call("set_project_settings", values={"graphics.pixels_per_unit": 1, "graphics.scaling_mode": "fit_width"})


async def sheet_pixels_per_unit(ed: Editor, path: str, previous_key: str = "") -> tuple[float, str]:
    """the imported sheet's pixels per unit (its sheet.json in the import cache) and the import's key"""
    for _ in range(300):
        info = (await ed.call("get_asset_info", path=path)).get("aseprite", {})
        key = info.get("import", {}).get("cache", {}).get("key", "")
        if info.get("import_state") == "imported" and key and key != previous_key:
            sheets = list(ed.project.rglob(f"{key}/sheet.json"))
            ed.check(len(sheets) == 1, f"one sheet.json for the import {key}: {sheets}")
            return float(json.loads(sheets[0].read_text())["pixels_per_unit"]), key
        await ed.call("wait", seconds=0.1)
    ed.fail(f"{path} has no new import (key {previous_key})")


async def test_aseprite_sheet_takes_project_pixels_per_unit(ed: Editor):
    await setup(ed)
    sheet = "assets/sprites/ppu.aseprite"
    (ed.project / "assets" / "sprites").mkdir(parents=True, exist_ok=True)
    shutil.copy(REPO / "editor" / "tests" / "fixtures" / "aseprite" / "asefile" / "basic-16x16.aseprite", ed.project / sheet)
    await ed.call("sync_files", paths=[sheet])
    ppu, key = await sheet_pixels_per_unit(ed, sheet)
    ed.check(ppu == 1, f"the sheet takes the project's 1 pixel per unit: {ppu}")
    await ed.call("set_project_settings", values={"graphics.pixels_per_unit": 16})
    ppu, _ = await sheet_pixels_per_unit(ed, sheet, key)
    ed.check(ppu == 16, f"changing the project's Pixels Per Unit imports the sheet again with it: {ppu}")
    await ed.call("set_project_settings", values={"graphics.pixels_per_unit": 1})


class Ui:
    """xdotool input in the harness's Xvfb, each action followed by editor frames"""

    def __init__(self, ed: Editor):
        self.ed = ed
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)
        windows = self.xdo("search", "--name", "JasperEngine").split() or self.xdo("search", "--name", ".").split()
        if not windows:
            skip("no editor window found")
        self.window = windows[-1]
        x, y = 0, 0
        for line in self.xdo("getwindowgeometry", self.window).splitlines():
            if "Position:" in line:
                x, y = (int(v) for v in line.split("Position:")[1].split("(")[0].strip().split(","))
        self.origin = (x, y)

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    async def settle(self, seconds: float):
        await asyncio.sleep(seconds)
        await self.ed.call("wait", frames=2)

    async def move(self, point: tuple[int, int]):
        self.xdo("mousemove", str(point[0]), str(point[1]))
        await self.settle(0.25)

    async def wheel(self, point: tuple[int, int], up: bool):
        await self.move(point)
        self.xdo("click", "4" if up else "5")
        await self.settle(0.3)

    async def drag(self, start: tuple[int, int], end: tuple[int, int], steps: int = 12):
        await self.move(start)
        self.xdo("mousedown", "1")
        await self.settle(0.2)
        for k in range(1, steps + 1):
            self.xdo("mousemove", str(int(start[0] + (end[0] - start[0]) * k / steps)),
                     str(int(start[1] + (end[1] - start[1]) * k / steps)))
            await asyncio.sleep(0.06)
        await self.settle(0.4)
        self.xdo("mouseup", "1")
        await self.settle(0.6)


async def scene_view(ed: Editor) -> tuple[dict, dict]:
    shot = await ed.call_raw("screenshot", {"view": "scene", "scene": S["scene"], "max_width": 0})
    return shot.structured["view_rect_in_window"], shot.structured


async def test_scene_view_pixel_zoom_and_move_snap(ed: Editor):
    ed.check(shutil.which("xdotool") and ed._xvfb and ed._xvfb.display, "needs xdotool and the harness's Xvfb")
    s = await setup(ed)
    got = await ed.call("get_entity", scene=s["scene"], entity=s["camera"])
    if "pixelperfectcamera" not in got["components"]:
        await ed.call("add_component", scene=s["scene"], entity=s["camera"], component="pixelperfectcamera")
    await ed.call("select_scene", scene=s["scene"])
    # 2x: one art pixel over two view pixels (zoom is world units per view pixel, PPU 1)
    cam = await ed.call("set_scene_camera", scene=s["scene"], center=[100, 60], zoom=0.5, display={"pixel_zoom": True})
    ed.check(cam.get("display", {}).get("pixel_zoom") is True, f"the flag is set: {cam.get('display')}")
    ui = Ui(ed)
    ui.xdo("windowactivate", "--sync", ui.window)
    rect, _ = await scene_view(ed)
    centre = (int(ui.origin[0] + rect["x"] + rect["width"] / 2), int(ui.origin[1] + rect["y"] + rect["height"] / 2))
    await ui.wheel(centre, up=True)
    _, info = await scene_view(ed)
    zoom = info["camera"]["zoom"]
    ed.check(abs(zoom - 1.0 / 3.0) < 1e-4, f"one notch in from 2x is 3x: zoom {zoom}")

    # Pixel Move Snap: the sprite dragged by its centre lands on whole pixels
    await ed.call("select_entities", scene=s["scene"], entities=[s["sprite"]])
    await ed.call("frame_entity", scene=s["scene"], entity=s["sprite"])
    rect, info = await scene_view(ed)
    centre = (int(ui.origin[0] + rect["x"] + rect["width"] / 2), int(ui.origin[1] + rect["y"] + rect["height"] / 2))
    before = (await ed.call("get_entity", scene=s["scene"], entity=s["sprite"], components=["transform"]))["components"]["transform"]["position"]
    await ui.drag(centre, (centre[0] + 37, centre[1] - 23))
    after = (await ed.call("get_entity", scene=s["scene"], entity=s["sprite"], components=["transform"]))["components"]["transform"]["position"]
    ed.check(after[:2] != before[:2], f"the drag moved it: {before} -> {after}")
    ed.check(float(after[0]).is_integer() and float(after[1]).is_integer(), f"on whole pixels: {after}")
