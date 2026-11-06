"""S43 AC19 (R44): what S43 must leave unchanged, against files and renders the build before S43
made (fa493bae4, tools/mcp/tests/fixtures/palette-regressions): three 2D scenes using
library:sprite's COLOR_REPLACE and the library:gameboy and library:posterize post passes, with
their textures. Each opens, draws its Game view at 640 x 360 as the golden, and re-saves byte for
byte."""

from __future__ import annotations

import asyncio
import shutil

import pngdiff
from harness import REPO, Editor

CI = True
TIMEOUT = 600
EDITOR = {"create": True, "name": "Palette Regressions"}

FIXTURE = REPO / "tools" / "mcp" / "tests" / "fixtures" / "palette-regressions"
SCENES = ("ColorReplace", "GameBoy", "Posterize")


async def test_old_scenes_render_and_resave_unchanged(ed: Editor):
    await ed.call("get_project_summary")
    shutil.copytree(FIXTURE / "assets", ed.project / "assets", dirs_exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets", "graphics.canvas_width": 320,
                                                  "graphics.canvas_height": 180})
    await ed.call("sync_files", full=True)
    await ed.call("configure_game_view", resolution="640x360")
    for name in SCENES:
        path = f"assets/scenes/{name}.scene"
        original = (ed.project / path).read_bytes()
        opened = await ed.call("open_scene", scene=path, close_previous=True, unsaved="discard")
        sid = opened["scene"]["id"]
        await ed.call("wait", frames=30)
        shot = await ed.screenshot("game", ed.scratch.root / f"{name}.png", max_width=0)
        golden = (FIXTURE / "goldens" / f"{name}.png").read_bytes()
        ok, fraction, diff = pngdiff.compare(pngdiff.decode(shot), pngdiff.decode(golden), channel_tolerance=2, max_fraction=0.0)
        if not ok and diff:
            (ed.scratch.root / f"{name}.diff.png").write_bytes(diff)
        ed.check(ok, f"{name} draws as before S43 ({fraction:.4%} of the pixels differ)")
        # saved to its own path: written even with nothing changed (its time moves)
        before = (ed.project / path).stat().st_mtime_ns
        await asyncio.sleep(0.05)
        await ed.call("save_scene", scene=sid, path=path)
        ed.check((ed.project / path).stat().st_mtime_ns != before, f"{name}.scene was written again")
        ed.check((ed.project / path).read_bytes() == original, f"{name}.scene re-saves byte for byte")
