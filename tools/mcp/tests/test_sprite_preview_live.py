"""The Sprite Preview shows agent edits live (docs/specs/S47-agent-sprite-drawing.md AC17, R34.3, R41,
R42): the panel's source follows the edit within a couple of frames (its texture carries the new
revision and shows the new pixels), the changed rect is outlined unless the preference is off, the
"Edited by" line's Undo works only while the step is on top, and an outside save over agent edits
shows its notice."""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
from pathlib import Path

import pngdiff
from harness import Editor
from sprite_draw_util import enable, ok

EDITOR = {"create": True, "name": "SpritePreviewLive", "xvfb": "harness", "ui_probe": True}
CI = True
TIMEOUT = 900
PATH = "assets/sprites/hero.aseprite"
OTHER = "assets/sprites/other.aseprite"
PREFERENCE = "preferences.pixel_art.agent_edit_highlight"
S: dict = {}


def center(rect):
    return ((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2)


class Ui:
    def __init__(self, ed: Editor):
        self.ed = ed
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)

    async def frames(self, n: int = 3):
        await self.ed.call("wait", frames=n)

    def xdo(self, *args: str) -> None:
        subprocess.run(["xdotool", *args], env=self.env, timeout=20)

    async def click(self, rect):
        x, y = center(rect)
        self.xdo("mousemove", str(int(x)), str(int(y)))
        await asyncio.sleep(0.3)
        self.xdo("click", "1")
        await asyncio.sleep(0.4)
        await self.frames(4)

    async def double_click(self, rect):
        x, y = center(rect)
        self.xdo("mousemove", str(int(x)), str(int(y)))
        await asyncio.sleep(0.3)
        self.xdo("click", "--repeat", "2", "--delay", "80", "1")
        await asyncio.sleep(0.4)

    async def project_file(self, name: str, folders=("assets", "sprites")):
        await self.ed.call("focus_window", window="project")
        for _ in range(12):
            await self.frames(4)
            items = self.ed.ui_items()
            if "project.file." + name in items:
                return items["project.file." + name]
            for folder in reversed(folders):
                if "project.file." + folder in items:
                    await self.double_click(items["project.file." + folder])
                    break
        self.ed.fail(f"the Project window never lists {name}")

    async def item(self, name: str, seconds: float = 30.0):
        deadline = asyncio.get_event_loop().time() + seconds
        while asyncio.get_event_loop().time() < deadline:
            await self.frames(2)
            rect = self.ed.ui_items().get(name)
            if rect:
                return rect
        self.ed.fail(f"{name} never appeared; items: {sorted(k for k in self.ed.ui_items() if 'spritePreview' in k)}")

    async def absent(self, name: str, seconds: float = 5.0):
        await self.frames(3)
        self.ed.check(name not in self.ed.ui_items(), f"{name} should not be drawn: {sorted(k for k in self.ed.ui_items() if 'agentEdit' in k)}")


async def pixel_rows(ed: Editor, rect):
    return (await ok(ed, "read_sprite_pixels", sprite=PATH, rect=rect))["frames"][0]["rows"]


async def test_setup_opens_the_preview(ed: Editor):
    if not shutil.which("xdotool"):
        ed.fail("xdotool is needed for the real-input UI tests")
    await enable(ed)
    await ok(ed, "create_sprite", path=PATH, size=[32, 32], palette=["#ff0000", "#00ff00"])
    await ok(ed, "create_sprite", path=OTHER, size=[32, 32], palette=["#ff0000", "#00ff00"])
    await ed.call("sync_files", paths=[PATH, OTHER])
    ui = Ui(ed)
    # the preview follows the Project window's selection
    await ui.click(await ui.project_file("hero.aseprite"))
    await ed.call("run_command", id="window.spritePreview")
    await ui.item("spritePreview.frameImage")


async def test_edit_shows_on_the_panel_within_two_frames(ed: Editor):
    ui = Ui(ed)
    stored = (await ed.call("get_project_settings", keys=[PREFERENCE]))["settings"][PREFERENCE]
    ed.check(stored is True, f"Highlight Agent Edits is on by default: {stored}")
    r = await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "rect", "rect": [2, 2, 6, 6], "color": 1, "fill": True}])
    # no file read, no import: the texture is the edited document's within a couple of frames
    texture = f"spritePreview.texture.{r['revision']}"
    for _ in range(3):
        await ui.frames(1)
        if texture in ed.ui_items():
            break
    ed.check(texture in ed.ui_items(), f"the panel's texture is this revision: {sorted(k for k in ed.ui_items() if 'spritePreview.texture' in k)}")
    items = ed.ui_items()
    ed.check("spritePreview.agentEdit.rect" in items, f"the changed rect is outlined: {sorted(k for k in items if 'agentEdit' in k)}")
    await ui.item("spritePreview.agentEdit.line")
    # the pixels really drawn: red inside the frame image, where the rect is
    frame = await ui.item("spritePreview.frameImage")
    w, h, px = pngdiff.decode(await ed.screenshot("editor", max_width=0))
    x = int(frame[0] + 4.5 / 32 * (frame[2] - frame[0]))
    y = int(frame[1] + 4.5 / 32 * (frame[3] - frame[1]))
    i = (y * w + x) * 4
    ed.check((px[i], px[i + 1], px[i + 2]) == (255, 0, 0), f"the edited pixels show red at ({x}, {y}): {(px[i], px[i + 1], px[i + 2])}")
    shots = Path(os.environ["JASPER_SHOTS_DIR"]) if os.environ.get("JASPER_SHOTS_DIR") else ed.scratch.root / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    (shots / "preview-agent-edit.png").write_bytes(await ed.screenshot("editor", max_width=0))


async def test_undo_works_while_the_step_is_on_top(ed: Editor):
    ui = Ui(ed)
    await ui.click(await ui.item("spritePreview.agentEdit.undo"))
    await ui.frames(3)
    ed.check(await pixel_rows(ed, [2, 2, 2, 2]) == ["..", ".."], "Undo took the edit back")


async def test_undo_is_disabled_under_a_newer_step(ed: Editor):
    ui = Ui(ed)
    await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "pixel", "points": [[3, 3]], "color": 1}])
    await ed.call("create_entity", type="empty", name="Newer step")
    await ed.call("run_command", id="window.spritePreview")   # it may have gone behind the Scene tab
    await ui.item("spritePreview.agentEdit.undoDisabled")
    await ui.click(await ui.item("spritePreview.agentEdit.undo"))
    await ui.frames(3)
    ed.check(await pixel_rows(ed, [3, 3, 1, 1]) == ["1"], "a disabled Undo leaves the sprite")
    found = await ed.call("find_entities", name="Newer step")
    ed.check("Newer step" in str(found), f"and leaves the step on top: {found}")


async def test_preference_off_keeps_the_line_and_hides_the_outline(ed: Editor):
    ui = Ui(ed)
    await ed.call("set_project_settings", values={PREFERENCE: False})
    try:
        await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "pixel", "points": [[5, 5]], "color": 2}])
        await ui.item("spritePreview.agentEdit.line")
        await ui.absent("spritePreview.agentEdit.rect")
    finally:
        await ed.call("set_project_settings", values={PREFERENCE: True})


async def test_undo_is_disabled_when_another_sprite_is_edited_next(ed: Editor):
    ui = Ui(ed)
    await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "pixel", "points": [[7, 7]], "color": 1}])
    await ui.item("spritePreview.agentEdit.undo")
    await ok(ed, "edit_sprite", sprite=OTHER, ops=[{"op": "pixel", "points": [[1, 1]], "color": 2}])
    await ui.item("spritePreview.agentEdit.undoDisabled")
    await ui.click(await ui.item("spritePreview.agentEdit.undo"))
    await ui.frames(3)
    ed.check(await pixel_rows(ed, [7, 7, 1, 1]) == ["1"], "the shown sprite stays")
    other = (await ok(ed, "read_sprite_pixels", sprite=OTHER, rect=[1, 1, 1, 1]))["frames"][0]["rows"]
    ed.check(other == ["2"], f"the other sprite stays: {other}")


async def test_outside_save_over_an_agent_edit_shows_its_notice(ed: Editor):
    ui = Ui(ed)
    await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "rect", "rect": [10, 10, 4, 4], "color": 2, "fill": True}])
    await ui.item("spritePreview.agentEdit.line")
    # another program saves over it: OTHER's bytes, which do not hold the agent's rect
    (ed.project / PATH).write_bytes((ed.project / OTHER).read_bytes())
    await ui.item("spritePreview.externalSave")
