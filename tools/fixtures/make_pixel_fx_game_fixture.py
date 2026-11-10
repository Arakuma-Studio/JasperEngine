#!/usr/bin/env python3
# (c) JasperDev and contributors
# SPDX-License-Identifier: MIT
"""Writes tools/fixtures/pixel-fx-game (docs/specs/S48-pixel-fx.md AC26), an editor-made project (driven
over MCP, tools/mcp/harness.py) for the exported-game smoke test: a 2D scene whose sprites carry every
library:sprite Pixel FX feature through the Pixel FX presets, Glitch on in its post-processing, and a Lua
script that, in the exported game, plays a PixelFX dissolve, a Hit-Stop and a glitch burst, then
Transition.loadScene to a second scene, which logs once the transition has revealed it. The lines the
game must log are smoke-expect.txt.

PFX_STAGE in the environment holds a static state instead, so tools/mcp/tests/test_pixel_fx_export.py
compares the exported game's frames with the editor's Game view (wall-clock screenshots do not line up
with game time): unset, the flow above, which ends on the revealed Level scene; "idle" plays nothing;
"set" holds Enemy's dissolve and Fade's dither fade at 0.5 (PixelFX.set); "covered" holds an iris cover.

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/fixtures/make_pixel_fx_game_fixture.py
    SMOKE_METHOD=compile SMOKE_GAME_SECONDS="ready 10" \
        SMOKE_GAME_RUNS="idle PFX_STAGE=idle; set PFX_STAGE=set; covered PFX_STAGE=covered" \
        tools/smoke-test.sh tools/fixtures/pixel-fx-game
"""

from __future__ import annotations

import asyncio
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "mcp"))

from harness import Editor  # noqa: E402
import pngdiff  # noqa: E402

TEXTURE = "assets/sprites/hero.png"
M = "submeshes[0].material."

# the presets cover DITHER_FADE, DISSOLVE, RAMP_RECOLOR, DROP_SHADOW, the pixel outline (and sel-out) and FLASH
SPRITES = (("Enemy", "pixel_enemy"), ("Fade", "teleport_fade"), ("Petrify", "petrify"), ("Shadow", "drop_shadow"),
           ("SelOut", "selout_outline"))

MAIN = """---@class PixelFxGame : ScriptInstance
-- S48 AC26 (docs/specs/S48-pixel-fx.md): the runtime Pixel FX in an exported game, logged for smoke-expect.txt.
-- PFX_STAGE holds a static state for the export test's frame comparison (tools/mcp/tests/test_pixel_fx_export.py):
-- unset, the whole flow (ending on the revealed Level scene); idle, set or covered
local PixelFxGame = { properties = {} }

function PixelFxGame:init()
    self.frames = 0
    self.stage = os.getenv("PFX_STAGE") or ""
    RegisterEngineEvent(self, "onUpdate")
end

function PixelFxGame:find(name)
    return GameObject(self.scene, self.scene:findEntity(name))
end

function PixelFxGame:onUpdate()
    self.frames = self.frames + 1
    if self.stage == "" then
        self:flow()
    elseif self.frames == 5 then
        self:hold()
    end
end

function PixelFxGame:flow()
    if self.frames == 5 then
        local dissolve = PixelFX.play(self:find("Enemy"), "dissolve", 0.4)
        Log.print("[gate] pixel_fx dissolve " .. tostring(dissolve.valid))
        HitStop.play(0.05)
        Log.print("[gate] pixel_fx hit_stop " .. tostring(HitStop.active))
        PixelFX.glitch(0.2, 0.8)
        Log.print("[gate] pixel_fx glitch burst")
    elseif self.frames == 30 then
        local handle = Transition.loadScene("Level", "iris", 0.5, {
            onCovered = function() Log.print("[gate] pixel_fx covered") end })
        Log.print("[gate] pixel_fx load_scene " .. tostring(handle.valid))
    end
end

function PixelFxGame:hold()
    if self.stage == "set" then
        PixelFX.set(self:find("Enemy"), "dissolve", 0.5)
        PixelFX.set(self:find("Fade"), "fade", 0.5)
        Log.print("[gate] pixel_fx stage set")
    elseif self.stage == "covered" then
        Transition.cover("iris", 0.3, { onCovered = function() Log.print("[gate] pixel_fx stage covered") end })
    else
        Log.print("[gate] pixel_fx stage " .. self.stage)
    end
end

return PixelFxGame
"""

LEVEL = """---@class PixelFxLevel : ScriptInstance
-- S48 AC26: the scene Transition.loadScene switched to, logged once the reveal has finished
local PixelFxLevel = { properties = {} }

function PixelFxLevel:init()
    self.logged = false
    RegisterEngineEvent(self, "onUpdate")
end

function PixelFxLevel:onUpdate()
    if not self.logged and not Transition.active then
        self.logged = true
        Log.print("[gate] pixel_fx level revealed")
    end
end

return PixelFxLevel
"""

EXPECT = """# tools/smoke-test.sh: lines the exported game logs (tools/fixtures/make_pixel_fx_game_fixture.py)
[gate] pixel_fx dissolve true
[gate] pixel_fx hit_stop true
[gate] pixel_fx glitch burst
[gate] pixel_fx load_scene true
[gate] pixel_fx covered
[gate] pixel_fx level revealed
"""


async def sprite_preset(ed: Editor, scene: int, entity: int, preset: str) -> None:
    # a new sprite has no submesh until its mesh is built: setting its shader makes the material a preset edits
    await ed.call("set_properties", scene=scene, entity=entity, values={"mesh": {M + "shader": "library:sprite"}})
    await ed.call("apply_material_preset", preset=preset, scene=scene, entity=entity)


async def main() -> int:
    target = REPO / "tools" / "fixtures" / "pixel-fx-game"
    async with Editor(create=True, name="Pixel FX Game", label="fixture-pixel-fx-game") as ed:
        texture = ed.project / TEXTURE
        texture.parent.mkdir(parents=True, exist_ok=True)
        # an 8x8 figure on a transparent 12x12 frame, so outlines and shadows have room
        pixels = bytearray(12 * 12 * 4)
        for y in range(2, 10):
            for x in range(2, 10):
                i = (y * 12 + x) * 4
                pixels[i:i + 4] = bytes([200 - 10 * y, 80 + 10 * x, 40, 255])
        texture.write_bytes(pngdiff.encode(12, 12, bytes(pixels)))
        await ed.call("sync_files", full=False)
        # the window the export test compares at (tools/mcp/tests/test_pixel_fx_export.py)
        await ed.call("set_project_settings", values={"graphics.canvas_width": 320, "graphics.canvas_height": 180,
                                                      "player.window_width": 1280, "player.window_height": 720})
        main = (await ed.call("create_scene", name="Main", type="2D", path="Main", close_previous=True, unsaved="discard"))["scene"]["id"]
        level = (await ed.call("create_scene", name="Level", type="2D", path="Level"))["scene"]["id"]
        await ed.call("edit_build_scenes", action="add", scene=level)
        await ed.call("edit_build_scenes", action="set_start", scene=main)
        value = {"path": TEXTURE, "min_filter": "nearest", "mag_filter": "nearest"}
        for i, (name, preset) in enumerate(SPRITES):
            made = await ed.call("create_entity", scene=main, type="sprite", name=name, position=[-100 + 50 * i, 0, 0],
                                 values={"mesh": {M + "baseColorTexture": value},
                                         "sprite": {"pivotPreset": "center", "width": 24, "height": 24}})
            await sprite_preset(ed, main, made["entity_id"], preset)
        await ed.call("set_scene_settings", scene=main, settings={"post_processing": {"glitch_enabled": True, "glitch_intensity": 0.0}})
        holder = (await ed.call("create_entity", scene=main, name="Driver"))["entity_id"]
        await ed.call("create_script", kind="lua", name="PixelFxGame", content=MAIN, scene=main, entity=holder)
        # what the revealed Level shows: a Ramp Recolour sprite (the flow's last frame, compared with the editor's)
        statue = await ed.call("create_entity", scene=level, type="sprite", name="Statue", position=[0, 0, 0],
                               values={"mesh": {M + "baseColorTexture": value},
                                       "sprite": {"pivotPreset": "center", "width": 48, "height": 48}})
        await sprite_preset(ed, level, statue["entity_id"], "frozen")
        sign = (await ed.call("create_entity", scene=level, name="Level"))["entity_id"]
        await ed.call("create_script", kind="lua", name="PixelFxLevel", content=LEVEL, scene=level, entity=sign)
        await ed.call("save_project")
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(ed.project, target, ignore=shutil.ignore_patterns(".jasperengine", "build", ".clangd", ".vscode", ".luarc.json", "AGENTS.md",
                                                                  "CLAUDE.md", "CMakeLists.txt", ".gitignore", ".gitattributes"))
    (target / "smoke-expect.txt").write_text(EXPECT)
    print(f"[fixture] wrote {target}: {sorted(str(p.relative_to(target)) for p in target.rglob('*') if p.is_file())}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
