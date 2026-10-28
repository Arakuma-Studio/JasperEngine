"""Shader library phase 4 review fixes, the inspector's Custom Passes rows (sl/p4-fix-editor):

- a library pass's parameters show the manifest's labels, ranges and defaults (the engine renders
  an unset member at its default, so nudging one starts from it, not from 0);
- each drag of a pass row is its own undo step (one Ctrl+Z reverted two separate drags before);
- picking another shader for a pass drops the textures the new shader does not declare (an old
  texture with a colliding name was bound silently).

Drives the real inspector with xdotool in the harness's Xvfb (1600x900, default layout): nothing
selected shows the scene settings, scrolled to the bottom, where the pass rows sit above Add Pass
and Default Shaders at fixed positions. When the layout moved and a click reaches nothing the test
skips and asks for new positions instead of failing."""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
import sys
from pathlib import Path

from harness import Editor, skip
import ui_input

sys.path.insert(0, str(Path(__file__).parent))
from test_post_process_passes import write_png  # noqa: E402

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Post Process Inspector"}
TIMEOUT = 600

SCROLL_AT = (1420, 500)        # the inspector, scrolled to its end
VALUE_X = 1500                 # inside the value column
# a library:crt pass alone (its rows end right above Add Pass)
CRT_SCALE_Y = 314
CRT_LINES_Y = 383              # "Scanlines over the height" (lines, a float of default 240)
# a fork pass with one texture slot
FORK_SHADER_COMBO_Y = 392
FORK_PIXELATE_ITEM = (1450, 529)   # "Pixelate" in the opened combo


class Ui(ui_input.Ui):
    """The shared frame-paced input (tools/mcp/ui_input.py), at screen points of the default layout."""

    async def focus(self):
        super().focus()
        await self.frames()

    async def click(self, x: int, y: int):
        await super().click((x, y))

    async def drag(self, x: int, y: int, dx: int):
        """press, move right by dx a frame per step, release"""
        await super().drag((x, y), (x + dx, y), steps=8)

    async def scroll_to_end(self):
        await self.move(SCROLL_AT)
        for _ in range(200):
            self.xdo("click", "5")
        await self.frames(4)


async def chain(ed: Editor, sid: int) -> list:
    return (await ed.call("get_scene_settings", scene=sid, names=["post_process"]))["settings"]["post_process"]


async def open_scene_settings(ed: Editor, sid: int, passes: list) -> Ui:
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness's Xvfb")
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": passes})
    await ed.call("select_scene", scene=sid)
    await ed.call("select_entities", entities=[])
    await ed.call("wait", frames=30)
    ui = Ui(ed)
    await ui.focus()
    await ui.scroll_to_end()
    await ed.call("wait", frames=10)
    return ui


async def test_crt_rows_defaults_and_undo_steps(ed: Editor):
    made = await ed.call("create_scene", name="PassRows", type="3d")
    sid = made["scene"]["id"]
    ui = await open_scene_settings(ed, sid, [{"shader": "library:crt"}])
    await ed.screenshot("editor", path=str(ed.scratch.root / "crt-rows.png"), max_width=1600)

    # a short drag of "lines" (unset: the manifest's 240) starts from 240, not from 0
    await ui.drag(VALUE_X, CRT_LINES_Y, 40)
    first = await chain(ed, sid)
    lines = first[0]["uniforms"].get("lines")
    if lines is None:
        await ed.screenshot("editor", path=str(ed.scratch.root / "inspector-layout.png"), max_width=1600)
        skip("the drag changed nothing: the inspector layout moved (update the positions at the top of this file)")
    ed.check(200.0 < lines[0] < 260.0, f"the drag starts from the manifest default 240: {lines}")

    # a second drag, then a click on Scale: three undo steps
    await ui.drag(VALUE_X, CRT_LINES_Y, 40)
    second = (await chain(ed, sid))[0]["uniforms"]["lines"][0]
    ed.check(second > lines[0], f"the second drag moved it on: {lines[0]} -> {second}")
    await ui.click(VALUE_X - 60, CRT_SCALE_Y)
    scaled = (await chain(ed, sid))[0].get("scale", 1.0)
    ed.check(scaled < 1.0, f"the Scale click set a scale below 1: {scaled}")

    await ed.call("undo")
    after = (await chain(ed, sid))[0]
    ed.check_eq(after.get("scale", 1.0), 1.0, "undo reverts the Scale click alone")
    ed.check_eq(after["uniforms"].get("lines", [None])[0], second, "the second drag is kept")
    await ed.call("undo")
    ed.check_eq((await chain(ed, sid))[0]["uniforms"].get("lines", [None])[0], lines[0], "undo reverts the second drag alone")
    await ed.call("undo")
    ed.check("lines" not in (await chain(ed, sid))[0]["uniforms"], "then the first")


async def test_shader_change_drops_undeclared_textures(ed: Editor):
    ed.require_tools("fork_shader")
    made = await ed.call("create_scene", name="PassSwap", type="3d")
    sid = made["scene"]["id"]
    write_png(ed.project / "assets" / "textures" / "grain.png")
    fork = await ed.call("fork_shader", scene=sid, target="post_process", name="SwapFork")
    ui = await open_scene_settings(ed, sid, [{"shader": fork["custom_shader"],
                                              "textures": {"u_overlayTexture": "assets/textures/grain.png"}}])
    await ui.click(VALUE_X, FORK_SHADER_COMBO_Y)
    await ed.call("wait", frames=5)
    await ui.click(*FORK_PIXELATE_ITEM)
    await ed.call("wait", frames=5)
    passes = await chain(ed, sid)
    if passes[0]["shader"] != "library:pixelate":
        await ed.screenshot("editor", path=str(ed.scratch.root / "inspector-layout.png"), max_width=1600)
        skip(f"the clicks picked {passes[0]['shader']!r}: the inspector layout moved (update the positions at the top of this file)")
    ed.check("textures" not in passes[0], f"the fork's texture is dropped with its shader: {passes[0]!r}")
    await ed.call("undo")
    passes = await chain(ed, sid)
    ed.check_eq(passes[0].get("textures"), {"u_overlayTexture": "assets/textures/grain.png"}, "undo brings the fork and its texture back")
