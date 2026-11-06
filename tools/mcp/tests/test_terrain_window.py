# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The Terrain window's saved settings (agent-authoring, Doriax d3cc5503e): opened from Window >
Terrain (or the saved layout), not only from the Inspector, it reads the project's settings before
it writes its own back, and an MCP brush stroke uses them too. Red before the fix: a stroke
created a missing map at the window's constructor default (512 pixels), and once drawn the window
wrote its defaults over the saved settings."""

import yaml

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Terrain Window"}


async def test_window_keeps_saved_settings(ed: Editor):
    await ed.call("save_project", untitled_scene_dir="scenes")
    path = ed.project / ".jasperengine" / "user" / "workspace.yaml"
    doc = yaml.safe_load(path.read_text()) or {}
    doc["terrainEditor"] = dict(doc.get("terrainEditor") or {}, heightMapResolution=64, brushSize=9)
    path.write_text(yaml.safe_dump(doc))
    async with Editor(ed.project, name="Terrain Window Reopened") as again:
        # a brush stroke on a terrain with no height map creates it at the window's resolution:
        # the saved 64, not the constructor's 512 (sculpt_terrain runs before the window ever opened)
        sid = (await again.call("create_scene", name="Dunes", type="3d"))["scene"]["id"]
        dunes = (await again.call("create_entity", scene=sid, name="Dunes", type="terrain"))["entity_id"]
        await again.call("sculpt_terrain", scene=sid, entity=dunes, strokes=[{"mode": "raise", "points": [[0, 0]]}])
        info = (await again.call("sample_terrain", scene=sid, entity=dunes, grid=0))["info"]
        header = (again.project / info["height_map"]).read_bytes()[:24]
        width = int.from_bytes(header[16:20], "big")
        again.check_eq(width, 64, f"the stroke's new height map ({info['height_map']}) has the saved resolution")
        await again.call("run_command", id="window.terrain")
        await again.call("wait", frames=3)
        sid = (await again.call("create_scene", name="Hills", type="3d"))["scene"]["id"]
        terrain = (await again.call("create_entity", scene=sid, name="Ground", type="terrain"))["entity_id"]
        made = await again.call("set_terrain_maps", scene=sid, entity=terrain, heightmap={"generate": "middle"})
        again.check_eq(made["heightmap"]["resolution"], 64, "the saved resolution, not the window's default")
        await again.call("save_project")
        saved = yaml.safe_load(path.read_text())["terrainEditor"]
        again.check(saved["heightMapResolution"] == 64 and saved["brushSize"] == 9, f"kept: {saved}")
