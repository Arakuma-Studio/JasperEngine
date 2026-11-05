#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Writes tools/fixtures/audio-capture: a looping Audio Source and a Lua probe, for the player capture check
of tools/ci/test_player_cli.py (docs/specs/S35-agent-audio.md AC3). Needs a linux build; run with the
tools/mcp prefix:

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/fixtures/make_audio_capture_fixture.py
"""

import asyncio
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "mcp"))

from audio_refs import write_tone  # noqa: E402
from harness import Editor  # noqa: E402

PROBE = """---@class CaptureProbe : ScriptInstance
local CaptureProbe = { properties = {} }

function CaptureProbe:init()
    self.frame = 0
    self.source = AudioSource(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

function CaptureProbe:onUpdate()
    self.frame = self.frame + 1
    if self.frame == 60 then
        print("capture probe frame 60 playing " .. tostring(self.source:isPlaying()))
        Audio.mark("frame 60")
    end
end

return CaptureProbe
"""


async def main() -> None:
    out = REPO / "tools" / "fixtures" / "audio-capture"
    async with Editor(create=True, name="Audio Capture") as ed:
        write_tone(ed.project / "sounds/loop.wav", seconds=0.5, frequency=440.0, rate=48000)
        await ed.call("sync_files")
        scene = (await ed.call("create_scene", name="Main", type="2D", path="scenes/Main.scene"))["scene"]["id"]
        await ed.call("create_entity", scene=scene, name="Source", type="audio_source")
        await ed.call("set_properties", scene=scene, entity="Source", values={"audiosource": {"filename": "sounds/loop.wav", "looping": True, "state": "playing"}})
        await ed.call("create_script", kind="lua", name="CaptureProbe", content=PROBE, entity="Source", scene=scene)
        await ed.call("save_scene", scene=scene)
        await ed.call("edit_build_scenes", action="set_start", scene=scene)
        await ed.call("edit_build_scenes", action="remove", scene="New Scene")
        await ed.call("save_project")
        shutil.rmtree(out, ignore_errors=True)
        shutil.copytree(ed.project, out, ignore=shutil.ignore_patterns(".jasperengine", "build", "AGENTS.md", "CLAUDE.md", ".claude", ".mcp.json", ".cursor", ".codex",
                                                                  ".vscode", ".clangd", ".luarc.json", ".gitignore", ".gitattributes", "CMakeLists.txt", "New Scene.scene"))
    print(f"wrote {out}")


asyncio.run(main())
