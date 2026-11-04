# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The Terrain window's saved settings (agent-authoring, Doriax d3cc5503e): opened from Window >
Terrain (or the saved layout), not only from the Inspector, it reads the project's settings before
it writes its own back. Red before the fix: its constructor defaults (brush size 4, map resolutions
512) overwrote the saved ones on the first frame, and set_terrain_maps generated 512-pixel maps."""

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
        await again.call("run_command", id="window.terrain")
        await again.call("wait", frames=3)
        sid = (await again.call("create_scene", name="Hills", type="3d"))["scene"]["id"]
        terrain = (await again.call("create_entity", scene=sid, name="Ground", type="terrain"))["entity_id"]
        made = await again.call("set_terrain_maps", scene=sid, entity=terrain, heightmap={"generate": "middle"})
        again.check_eq(made["heightmap"]["resolution"], 64, "the saved resolution, not the window's default")
        await again.call("save_project")
        saved = yaml.safe_load(path.read_text())["terrainEditor"]
        again.check(saved["heightMapResolution"] == 64 and saved["brushSize"] == 9, f"kept: {saved}")
