#!/usr/bin/env python3
# (c) JasperDev and contributors
# SPDX-License-Identifier: MIT
"""Writes tools/fixtures/pixel-fonts (docs/specs/S51-pixel-fonts.md AC14, AC22), an editor-made
project (driven over MCP, tools/mcp/harness.py) with bitmap fonts: a .bitmapfont over a 6x9 grid
sheet (engine/tests/core/fixtures/fonts, tools/pixelart/make_font_fixtures.py), a coloured one, the
built-in builtin:pixel, Texts at whole scales with a pixel outline, a Lua script that logs what the
exported game loaded (the cooked .jfont: the sheets do not ship) for smoke-expect.txt, and an HTML
document whose @font-face is a bitmap font with a glow `jasperengine-editor check --ui` reports:

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/fixtures/make_pixel_fonts_fixture.py
    tools/smoke-test.sh tools/fixtures/pixel-fonts
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

FONTS = REPO / "engine" / "tests" / "core" / "fixtures" / "fonts"

CHECKER = """---@class PixelFontsCheck : ScriptInstance
-- S51 AC22 (docs/specs/S51-pixel-fonts.md R34): in the exported game the bitmap fonts load from their
-- cooked .jfont (the sources are not shipped), measure as the editor does and draw at whole scales
local PixelFontsCheck = { properties = {} }

function PixelFontsCheck:init()
    self.frames = 0
    RegisterEngineEvent(self, "onUpdate")
end

function PixelFontsCheck:onUpdate()
    self.frames = self.frames + 1
    if self.frames ~= 10 then return end
    local hud, err = BitmapFont.load("assets/fonts/hud.bitmapfont")
    if hud == nil then
        Log.error("[gate] pixel_fonts: assets/fonts/hud.bitmapfont does not load: " .. tostring(err))
        return
    end
    local w, h = hud:measure("SCORE 120", 2)
    Log.print("[gate] pixel_fonts hud " .. hud.nativeSize .. " px, " .. hud.glyphCount .. " glyphs, SCORE 120 x2 " .. w .. "x" .. h)
    local pixel = BitmapFont.load("builtin:pixel")
    Log.print("[gate] pixel_fonts builtin " .. pixel.nativeSize .. " px, " .. pixel.glyphCount .. " glyphs")
    local title = Text(self.scene, self.scene:findEntity("Title"))
    Log.print("[gate] pixel_fonts Title x" .. title.pixelScale .. " (" .. title.effectiveFontSize .. ")")
    local styled = Text(self.scene, self.scene:findEntity("Styled"))
    Log.print("[gate] pixel_fonts Styled outline " .. styled.outlineWidth .. " neighbors " .. styled.outlineNeighbors)
end

return PixelFontsCheck
"""

EXPECT = """# tools/smoke-test.sh: lines the exported game logs (tools/fixtures/make_pixel_fonts_fixture.py)
[gate] pixel_fonts hud 9 px, 95 glyphs, SCORE 120 x2
[gate] pixel_fonts builtin 9 px, 159 glyphs
[gate] pixel_fonts Title x2 (18)
[gate] pixel_fonts Styled outline 1 neighbors 4
"""

HTML = """<!DOCTYPE html>
<html><head><style>
@font-face { font-family: Hud; src: url(../fonts/hud.bitmapfont); }
.score { font-family: Hud; font-size: 18px; color: #ffffff; }
.glowing { font-family: Hud; font-size: 18px; text-shadow: 0 0 4px #00ff00; }
</style></head>
<body><p class="score">SCORE 120</p><p class="glowing">GLOW</p></body></html>
"""


async def main() -> int:
    target = REPO / "tools" / "fixtures" / "pixel-fonts"
    async with Editor(create=True, name="Pixel Fonts", label="fixture-pixel-fonts") as ed:
        fonts = ed.project / "assets" / "fonts"
        fonts.mkdir(parents=True, exist_ok=True)
        shutil.copy(FONTS / "grid@6x9.png", fonts / "hud@6x9.png")
        shutil.copy(FONTS / "colored@6x9.png", fonts / "colored@6x9.png")
        for name, image in (("hud", "hud@6x9.png"), ("colored", "colored@6x9.png")):
            (fonts / f"{name}.bitmapfont").write_text(json.dumps(
                {"format": "jasperengine.bitmapfont", "version": 1, "source": {"type": "grid", "image": image}}, indent=2) + "\n")
        (ed.project / "assets" / "ui").mkdir(parents=True, exist_ok=True)
        (ed.project / "assets" / "ui" / "hud.html").write_text(HTML)
        await ed.call("sync_files", full=False)
        await ed.call("set_project_settings", values={"graphics.canvas_width": 320, "graphics.canvas_height": 180})
        made = await ed.call("create_scene", name="PixelFonts", type="UI", path="PixelFonts", close_previous=True, unsaved="discard")
        sid = made["scene"]["id"]
        await ed.call("edit_build_scenes", action="set_start", scene=sid)
        await ed.call("set_scene_settings", scene=sid, settings={"background_color": [0.05, 0.05, 0.1, 1]})
        for root in ({"type": "text", "name": "Title", "text": "SCORE 120", "font": "assets/fonts/hud.bitmapfont", "font_size": 18,
                      "text_color": [1, 1, 1, 1], "layout": {"anchor": "top_left", "position": [10, 10]}},
                     {"type": "text", "name": "Styled", "text": "Hp 99!", "font": "builtin:pixel", "font_size": 27,
                      "text_color": [1, 0.85, 0.2, 1], "outline": {"width": 1, "color": [0, 0, 0, 1]},
                      "layout": {"anchor": "top_left", "position": [10, 40]}},
                     {"type": "text", "name": "Colored", "text": "ABC 123", "font": "assets/fonts/colored.bitmapfont", "font_size": 18,
                      "text_color": [1, 1, 1, 1], "layout": {"anchor": "top_left", "position": [10, 90]}}):
            await ed.call("create_ui", scene=sid, root=root)
        await ed.call("set_properties", scene=sid, entity="Styled", values={"text": {"outlineNeighbors": 4}})
        await ed.call("create_script", kind="lua", name="PixelFontsCheck", content=CHECKER, scene=sid, entity="Title")
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
