"""The Sprite Preview shows agent edits live (docs/specs/S47-agent-sprite-drawing.md AC17, R41,
R42): the panel's source follows the edit within a couple of frames, the changed rect is outlined,
the "Edited by" line's Undo works while the step is on top and is disabled after a newer one."""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
from pathlib import Path

from harness import Editor
from sprite_draw_util import enable, ok

EDITOR = {"create": True, "name": "SpritePreviewLive", "xvfb": "harness", "ui_probe": True}
CI = True
TIMEOUT = 900
PATH = "assets/sprites/hero.aseprite"


def center(rect):
    return ((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2)


class Ui:
    def __init__(self, ed: Editor):
        self.ed = ed
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)

    async def frames(self, n: int = 3):
        await self.ed.call("wait", frames=n)

    async def click(self, rect):
        x, y = center(rect)
        subprocess.run(["xdotool", "mousemove", str(int(x)), str(int(y))], env=self.env, timeout=20)
        await asyncio.sleep(0.3)
        subprocess.run(["xdotool", "click", "1"], env=self.env, timeout=20)
        await asyncio.sleep(0.4)
        await self.frames(4)

    async def project_file(self, name: str, folders=("assets", "sprites")):
        await self.ed.call("focus_window", window="project")
        for _ in range(12):
            await self.frames(4)
            items = self.ed.ui_items()
            if "project.file." + name in items:
                return items["project.file." + name]
            for folder in reversed(folders):
                if "project.file." + folder in items:
                    x, y = center(items["project.file." + folder])
                    subprocess.run(["xdotool", "mousemove", str(int(x)), str(int(y))], env=self.env, timeout=20)
                    await asyncio.sleep(0.3)
                    subprocess.run(["xdotool", "click", "--repeat", "2", "--delay", "80", "1"], env=self.env, timeout=20)
                    await asyncio.sleep(0.4)
                    break
        self.ed.fail(f"the Project window never lists {name}")

    async def item(self, name: str, seconds: float = 30.0):
        deadline = asyncio.get_event_loop().time() + seconds
        while asyncio.get_event_loop().time() < deadline:
            await self.frames(2)
            rect = self.ed.ui_items().get(name)
            if rect:
                return rect
        self.ed.fail(f"{name} never appeared; items: {sorted(self.ed.ui_items())[:30]}")


async def test_live_view(ed: Editor):
    if not shutil.which("xdotool"):
        ed.fail("xdotool is needed for the real-input UI tests")
    await enable(ed)
    await ok(ed, "create_sprite", path=PATH, size=[32, 32], palette=["#ff0000", "#00ff00"])
    await ed.call("sync_files", paths=[PATH])
    ui = Ui(ed)
    # the preview follows the Project window's selection
    await ui.click(await ui.project_file("hero.aseprite"))
    await ed.call("run_command", id="window.spritePreview")
    await ui.item("spritePreview.frameImage")
    r = await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "rect", "rect": [2, 2, 6, 6], "color": 1, "fill": True}])
    # the next frames show it: no file read, no import needed
    await ui.frames(2)
    items = ed.ui_items()
    ed.check("spritePreview.agentEdit.rect" in items, f"the changed rect is outlined: {sorted(k for k in items if 'agentEdit' in k)}")
    await ui.item("spritePreview.agentEdit.line")
    ed.check(f"spritePreview.agentEdit.reopen.{r['revision']}" in ed.ui_items(), "the status line names this revision")
    shots = Path(os.environ["JASPER_SHOTS_DIR"]) if os.environ.get("JASPER_SHOTS_DIR") else ed.scratch.root / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    (shots / "preview-agent-edit.png").write_bytes(await ed.screenshot("editor", max_width=0))

    # Undo while it is the newest step
    undo = await ui.item("spritePreview.agentEdit.undo")
    await ui.click(undo)
    await ui.frames(3)
    rows = (await ok(ed, "read_sprite_pixels", sprite=PATH, rect=[2, 2, 2, 2]))["frames"][0]["rows"]
    ed.check(rows == ["..", ".."], f"Undo took the edit back: {rows}")

    # a newer step on top: Undo is disabled and leaves the file alone
    second = await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "pixel", "points": [[3, 3]], "color": 1}])
    await ed.call("create_entity", type="node", name="Newer step")
    await ui.frames(3)
    undo = await ui.item("spritePreview.agentEdit.undo")
    await ui.click(undo)
    await ui.frames(3)
    after = (await ok(ed, "read_sprite_pixels", sprite=PATH, rect=[3, 3, 1, 1]))["frames"][0]["rows"]
    ed.check(after == ["1"], f"a disabled Undo leaves the file: {after} (revision {second['revision']})")
