#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Writes tools/fixtures/title3d-fixed (docs/specs/S86-title-3d.md AC26, S81 R51), the smoke test's
project of a fixed-text Realtime 3D title: an editor-made project (driven over MCP,
tools/mcp/harness.py) whose UI scene shows "3D" in gold with a bright ring, lit by the softbox (an HDRI
only the editor has), as a Realtime Title Art Renderer without a prebaked first image, at the Low tier
(Realtime 3D Quality: Linux low). No Exposed parameter changes its geometry, so a Compile export turns
JASPERENGINE_TITLE_ART_MESHER off and ships the title's cooked meshes and environment cube; the game
draws the title itself from them. A Lua script logs its mesh once it is ready.

smoke-expect.txt lists what the game must log, smoke-export-expect.txt what the export's CMake settings
must hold, and smoke-golden.json the image the game's own frame must match: the editor's frame of the
same scene (record_game, which waits for the Realtime title like the game's "Engine ready" line),
drawn with the mesher and the HDRI-built cube:

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/fixtures/make_title3d_fixed_fixture.py
    SMOKE_METHOD=compile SMOKE_PLAY=1 tools/smoke-test.sh tools/fixtures/title3d-fixed
"""

from __future__ import annotations

import asyncio
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "mcp"))

from harness import Editor  # noqa: E402

WIDTH, HEIGHT = 640, 360
DOC = "assets/titles/logo.titleart"

CHECKER = """---@class Title3DCheck : ScriptInstance
-- On the Realtime 3D title: once it is ready, its mesh (cooked in a build without the mesher)
local Title3DCheck = { properties = {} }

function Title3DCheck:init()
    self.title = TitleArt(self.scene, self.entity)
    self.done = false
    RegisterEngineEvent(self, "onUpdate")
end

function Title3DCheck:onUpdate()
    if self.done or not self.title.ready then return end
    self.done = true
    local stats = self.title:getMeshStats("Title")
    if stats == nil then
        Log.error("[gate] title3d: ready without a mesh")
        return
    end
    Log.print("[gate] title3d ready: tier=" .. stats.tier .. " cooked=" .. tostring(stats.cooked) .. " triangles=" .. stats.triangles)
end

return Title3DCheck
"""

EXPECT = """# tools/smoke-test.sh: lines the exported game logs (tools/fixtures/make_title3d_fixed_fixture.py)
its 3D layers draw the meshes the export cooked (this build has no 3D mesher)
[gate] title3d ready: tier=low cooked=true
"""

EXPORT_EXPECT = """# tools/smoke-test.sh (SMOKE_METHOD=compile): lines of the exported project (S81 R51, S86 R38)
set(JASPERENGINE_TITLE_ART ON)
set(JASPERENGINE_TITLE_ART_3D ON)
set(JASPERENGINE_TITLE_ART_MESHER OFF)
"""


async def main() -> int:
    target = REPO / "tools" / "fixtures" / "title3d-fixed"
    async with Editor(create=True, name="Title 3D Fixed", label="fixture-title3d-fixed") as ed:
        await ed.call("tool_groups", enable=["title_art"])
        await ed.call("set_project_settings", values={
            "title_art.auto_bake": False,
            "title_art.realtime_3d_quality": {"linux": "low"},
            "graphics.canvas_width": WIDTH, "graphics.canvas_height": HEIGHT,
            "player.window_width": WIDTH, "player.window_height": HEIGHT})
        await ed.call("edit_title", path=DOC, create={"text": "3D", "canvas": [400, 200]}, bake="never", ops=[
            {"op": "set_dimension", "layer": "Title", "dimension": "3d"},
            {"op": "apply_material", "layer": "Title", "part": "face", "preset": "gold"},
            {"op": "add_band", "layer": "Title", "preset": "bright_ring"},
            {"op": "set", "values": {"scene_3d.environment.preset": "softbox", "scene_3d.environment.rotation": 25}}])
        # the bake's sidecar gives the frame a Realtime image covers
        await ed.call("export_title", path=DOC)
        made = await ed.call("create_scene", name="Title3D", type="UI", path="scenes/Title3D", close_previous=True, unsaved="discard")
        sid = made["scene"]["id"]
        await ed.call("edit_build_scenes", action="set_start", scene=sid)
        await ed.call("create_ui", scene=sid, root={"type": "image", "name": "Logo", "layout": {"anchor": "center", "size": [400, 200]}})
        await ed.call("add_component", scene=sid, entity="Logo", component="titleart")
        await ed.call("set_properties", scene=sid, entity="Logo",
                      values={"titleart": {"title": DOC, "renderMode": "realtime", "prebakeInitialState": False}})
        await ed.call("create_script", kind="lua", name="Title3DCheck", content=CHECKER, scene=sid, entity="Logo")
        await ed.call("save_project")
        # the editor's frame: the mesher and the HDRI-built cube (the Recorder waits for the title)
        frames = ed.scratch.root / "golden"
        await ed.call("record_game", frames=1, format="png", width=WIDTH, height=HEIGHT, output=str(frames), timeout_seconds=900)
        golden = sorted(frames.rglob("*.png"))
        if not golden:
            print("[fixture] record_game wrote no frame", file=sys.stderr)
            return 1
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(ed.project, target, ignore=shutil.ignore_patterns(".jasperengine", "build", ".clangd", ".vscode", ".luarc.json", "AGENTS.md",
                                                                  "CLAUDE.md", "CMakeLists.txt", ".gitignore", ".gitattributes"))
        shutil.copyfile(golden[-1], target / "smoke-golden.png")
    (target / "smoke-expect.txt").write_text(EXPECT)
    (target / "smoke-export-expect.txt").write_text(EXPORT_EXPECT)
    (target / "smoke-golden.json").write_text(json.dumps(
        {"image": "smoke-golden.png", "tolerance": 24, "max_fraction": 0.01,
         "about": "the editor's frame of the start scene (record_game); the game's own frame (SMOKE_PLAY=1) must match it"}, indent=1) + "\n")
    print(f"[fixture] wrote {target}: {sorted(str(p.relative_to(target)) for p in target.rglob('*') if p.is_file())}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
