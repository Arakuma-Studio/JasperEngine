"""T1 R39, R56 (AC29, AC42): the Terrain window's mouse paths with real input (xdotool through ui_input).

AC29: a real drag with JASPER_TERRAIN_STROKE_LOG logs the stroke as the edit_terrain call that
replays it; undone and replayed, the height map's hash is the drag's. AC42: with Alt Alternates
While Painting on, Alt+drag on an armed brush paints its alternate instead of orbiting.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import tempfile
from pathlib import Path

import harness
from harness import Editor, skip
import ui_input

LOG = Path(tempfile.gettempdir()) / f"jasper-terrain-strokes-{os.getpid()}.jsonl"
EDITOR = {"xvfb": "harness", "create": True, "name": "Terrain UI", "env": {"JASPER_TERRAIN_STROKE_LOG": str(LOG)},
          "settings": {"editor": {"scene_view": {"alt_alternates": True}}}}
CI = True
TIMEOUT = 900

TERRAIN_RAISE_BRUSH = "##terrain_raise"


async def frames(ed: Editor, count: int = 3):
    await ed.call("wait", frames=count)


async def height_hash(ed: Editor, sid: int, terrain: int) -> str:
    return (await ed.call("sample_terrain", scene=sid, entity=terrain, hash=True))["hashes"]["height"]


async def camera_position(ed: Editor, sid: int) -> list:
    shot = await ed.call_raw("screenshot", {"view": "scene", "scene": sid})
    return harness.find_key(shot.structured, "camera")["position"]


async def armed_terrain(ed: Editor, name: str):
    """A 20 m terrain seen from above, the Raise brush armed; the Scene view point to drag from."""
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness's Xvfb")
    ui = ui_input.Ui(ed)
    sid = (await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}"))["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    terrain = (await ed.call("create_entity", scene=sid, type="terrain", name="Ground"))["entity_id"]
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {"terrainSize": 20}})
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, heightmap={"generate": "flat", "base_height": 0.2})
    await ed.call("select_entities", scene=sid, entities=[terrain])
    await ed.call("set_scene_camera", scene=sid, position=[0, 14, -16], target=[0, 0, 0])
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
    start = ui.at(view, (view["width"] / 2 + 60, view["height"] / 2 + 60))
    return sid, terrain, ui, start


async def test_stroke_log_replays_exactly(ed: Editor):
    """AC29 (R39)."""
    sid, terrain, ui, start = await armed_terrain(ed, "StrokeLog")
    LOG.unlink(missing_ok=True)
    before = await height_hash(ed, sid, terrain)
    await ui.press(start)
    await frames(ed, 3)
    await ui.drag_to(start, 120, 30, steps=20)
    await ui.release()
    await frames(ed)
    dragged = await height_hash(ed, sid, terrain)
    ed.check(dragged != before, "the drag raised the terrain")
    ed.check(LOG.exists(), f"the stroke was logged to {LOG}")
    lines = [line for line in LOG.read_text().splitlines() if line.strip()]
    ed.check_eq(len(lines), 1, f"one line per stroke: {lines}")
    call = json.loads(lines[-1])
    ed.check_eq(call["space"], "local", "terrain-local points")
    ed.check_eq(call["ops"][0]["shape"]["type"], "stroke", "the drag as one stroke")

    await ed.call("undo", scene=sid)
    ed.check_eq(await height_hash(ed, sid, terrain), before, "undo puts the heights back")
    await ed.call("edit_terrain", scene=sid, entity=terrain, **call)
    ed.check_eq(await height_hash(ed, sid, terrain), dragged, "the replay gives the drag's bytes")


async def test_alt_alternates_while_painting(ed: Editor):
    """AC42 (R56): with the preference on, Alt+drag on an armed Raise brush lifts floors instead of orbiting."""
    sid, terrain, ui, start = await armed_terrain(ed, "AltPaint")
    # a bump to start the stroke on: Lift Floors raises the ground below the clicked height up to it
    await ed.call("edit_terrain", scene=sid, entity=terrain, space="local",
                  ops=[{"op": "raise", "delta": 2, "shape": {"type": "circle", "center": [0, 0], "radius": 3}}])
    camera = await camera_position(ed, sid)
    before = await height_hash(ed, sid, terrain)
    # from the bump's top (the view's centre) onto the flat ground: the floor rises to the clicked height
    centre = (start[0] - 60, start[1] - 60)
    await ui.drag(centre, (centre[0] + 80, centre[1]), steps=16, modifier="alt")
    await frames(ed)
    ed.check(await height_hash(ed, sid, terrain) != before, "Alt+drag painted the alternate")
    ed.check_eq(await camera_position(ed, sid), camera, "and did not orbit the view")
