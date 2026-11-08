"""The Terrain window's pen options (T1 R37, AC27), driven like a human with xdotool in the harness's Xvfb:
Pressure Strength and Pressure Size are rows of the Brushes section, and a click on one is saved with the
project's terrain settings. Screenshot to $JE_EVIDENCE_DIR when set (else the scratch directory).

Xvfb has no tablet: what a pen's pressure does to a stroke is pinned by TerrainStroke.* in the editor's unit tests,
and real tablets are docs/specs/checklists/T1.md.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

import yaml

from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Terrain Pressure UI", "xvfb": "harness", "ui_probe": True}

WINDOW = "Terrain"


def mid(ui: Ui, rect) -> tuple[int, int]:
    """the screen centre of a UI probe rect: the probe gives [x0, y0, x1, y1], Ui.centre takes [x, y, width, height]"""
    return ui.centre([rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]])


async def test_pressure_options_are_saved(ed: Editor):
    ui = Ui(ed)
    await ed.call("tool_groups", enable=["terrain"])
    made = await ed.call("create_scene", name="Pen", type="3D", path="scenes/Pen")
    sid = made["scene"]["id"] if isinstance(made.get("scene"), dict) else made.get("scene", made.get("scene_id"))
    terrain = (await ed.call("create_entity", scene=sid, name="Ground", type="terrain"))["entity_id"]
    await ed.call("set_terrain_maps", scene=sid, entity=terrain, heightmap={"generate": "flat", "resolution": 65, "base_height": 0.25})
    await ed.call("select_entities", scene=sid, entities=[terrain])
    await ed.call("focus_window", window="terrain")

    await ui.reveal("Pressure Size", WINDOW)
    await ui.frames(5)
    items = ed.ui_items()
    for name in ("terrain.brush.pressure_strength", "terrain.brush.pressure_size"):
        ed.check(name in items, f"{name} is drawn: {sorted(k for k in items if k.startswith('terrain.brush'))}")

    relative = "shots/terrain-pressure-options.png"
    shot = await ed.call_raw("screenshot", {"view": "editor", "save_path": relative})
    ed.check(not shot.is_error, shot.text)
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root)
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copy(ed.project / relative, folder / "terrain-pressure-options.png")

    # Pressure Size starts off and Pressure Strength on: a click on each turns them around
    await ui.click(mid(ui, items["terrain.brush.pressure_size"]))
    await ui.frames(5)
    await ui.click(mid(ui, ed.ui_items()["terrain.brush.pressure_strength"]))
    await ui.frames(5)
    await ed.call("save_project", untitled_scene_dir="scenes")
    saved = yaml.safe_load((ed.project / ".jasperengine" / "user" / "workspace.yaml").read_text())["terrainEditor"]
    ed.check(saved.get("pressureSize") is True, f"Pressure Size is saved on: {saved}")
    ed.check(saved.get("pressureStrength") is False, f"Pressure Strength is saved off: {saved}")
