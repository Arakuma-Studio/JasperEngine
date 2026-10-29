#!/usr/bin/env python3
"""Makes project.yaml and Main Scene.scene of the pose-animation fixture through the real editor.

The editor writes both files in its own format: a new project (MCP `--create`), assets_dir set
to `assets`, both models loaded with set_model (Hero at x = -0.75, Robot at x = +0.75), the
default directional light turned to light the figures from the front, a ground plane, a camera
framing both as the main camera, then save_scene / save_project. The two files are copied back
here; the models must have been generated first (make_humanoids.py).

    Run it with the harness dependencies (a real editor under Xvfb):
    JASPER_EDITOR=.../jasperengine-editor \\
        uv run --no-project --with mcp --with pyyaml python tools/fixtures/pose-animation/make_scene.py

The scene must be made again whenever the models change: it stores each model's materials,
submesh sizes, bounds and bone entities.
"""

import asyncio
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "mcp"))

from harness import Editor  # noqa: E402

SCENE = "Main Scene.scene"


async def main():
    async with Editor(create=True, name="PoseAnimation") as ed:
        proj = ed.project
        shutil.copytree(os.path.join(HERE, "assets"), proj / "assets")
        await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
        await ed.call("sync_files")
        await ed.call("rename_scene", name="Main Scene")
        await ed.call("set_model", create={"name": "Hero", "position": [-0.75, 0, 0]},
                      path="assets/models/hero_tpose.gltf")
        await ed.call("set_model", create={"name": "Robot", "position": [0.75, 0, 0]},
                      path="assets/models/robot_apose.gltf")
        await ed.call("set_properties", entity="Directional Light",
                      values={"light": {"direction": [0.35, -0.6, -0.72]}})
        await ed.call("create_entity", type="plane", name="Ground", position=[0, 0, 0], scale=[8, 1, 8])
        await ed.call("create_entity", type="camera", name="Main Camera", position=[0, 0.9, 3.5])
        await ed.call("set_main_camera", entity="Main Camera")
        await ed.call("save_scene", path=SCENE)
        await ed.call("save_project")
        for name in ("project.yaml", SCENE):
            shutil.copyfile(proj / name, os.path.join(HERE, name))
            print("wrote", name)


if __name__ == "__main__":
    asyncio.run(main())
