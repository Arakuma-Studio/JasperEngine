"""Shader library phase 4 review fixes, the inspector's Custom Passes rows (sl/p4-fix-editor):

- a library pass's parameters show the manifest's labels, ranges and defaults (the engine renders
  an unset member at its default, so nudging one starts from it, not from 0);
- each drag of a pass row is its own undo step (one Ctrl+Z reverted two separate drags before);
- picking another shader for a pass drops the textures the new shader does not declare (an old
  texture with a colliding name was bound silently).

Drives the real inspector with xdotool in the harness's Xvfb: nothing selected shows the scene
settings, whose pass rows are found where the editor draws them now (find_ui_item, scrolled into
view with the wheel), so a layout change does not move the clicks off them."""

from __future__ import annotations

import sys
from pathlib import Path

from harness import Editor
import ui_input

sys.path.insert(0, str(Path(__file__).parent))
from test_post_process_passes import write_png  # noqa: E402

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Post Process Inspector"}
TIMEOUT = 600

INSPECTOR = "Inspector"
# the first pass's rows (drawn under PushID(pass index)): a manifest property's value, the Scale
# field (the only "##value" of the scene settings) and the shader combo
CRT_LINES = "##pp_param_lines"     # "Scanlines over the height" (lines, a float of default 240)
SCALE = "##value"
SHADER_COMBO = "##shader_picker_Edit Shader Files##post_process_0"


async def chain(ed: Editor, sid: int) -> list:
    return (await ed.call("get_scene_settings", scene=sid, names=["post_process"]))["settings"]["post_process"]


async def open_scene_settings(ed: Editor, sid: int, passes: list) -> ui_input.Ui:
    ui = ui_input.Ui(ed)               # skips without xdotool or the harness's display
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": passes})
    await ed.call("select_scene", scene=sid)
    await ed.call("select_entities", entities=[])
    await ed.call("focus_window", window="inspector")
    await ui.frames(10)
    ui.focus()
    await ui.frames()
    return ui


async def drag_right(ui: ui_input.Ui, label: str, dx: int):
    """press in the middle of the field, move right by dx a frame per step, release (the rows of a
    pass show once its shader is built: "Building shader..." until then)"""
    start = ui.centre((await ui.reveal(label, INSPECTOR, frames=1200))["rect"])
    await ui.drag(start, (start[0] + dx, start[1]), steps=8)


async def test_crt_rows_defaults_and_undo_steps(ed: Editor):
    made = await ed.call("create_scene", name="PassRows", type="3d")
    sid = made["scene"]["id"]
    ui = await open_scene_settings(ed, sid, [{"shader": "library:crt"}])
    await ed.screenshot("editor", path=str(ed.scratch.root / "crt-rows.png"), max_width=1600)

    # a short drag of "lines" (unset: the manifest's 240) starts from 240, not from 0
    await drag_right(ui, CRT_LINES, 40)
    first = await chain(ed, sid)
    lines = first[0]["uniforms"].get("lines")
    ed.check(lines is not None, f"the drag of Scanlines over the height set lines: {first}")
    ed.check(200.0 < lines[0] < 260.0, f"the drag starts from the manifest default 240: {lines}")

    # a second drag, then a click on Scale (the middle of its 0.1 to 1 slider): three undo steps
    await drag_right(ui, CRT_LINES, 40)
    second = (await chain(ed, sid))[0]["uniforms"]["lines"][0]
    ed.check(second > lines[0], f"the second drag moved it on: {lines[0]} -> {second}")
    await ui.click_revealed(SCALE, INSPECTOR)
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
    await ui.click_revealed(SHADER_COMBO, INSPECTOR)
    # "Pixelate" in the opened combo's popup (scrolled to inside the popup when its list is long)
    await ui.click_revealed("Pixelate", "##Combo")
    passes = await chain(ed, sid)
    ed.check_eq(passes[0]["shader"], "library:pixelate", "the combo picked Pixelate")
    ed.check("textures" not in passes[0], f"the fork's texture is dropped with its shader: {passes[0]!r}")
    await ed.call("undo")
    passes = await chain(ed, sid)
    ed.check_eq(passes[0].get("textures"), {"u_overlayTexture": "assets/textures/grain.png"}, "undo brings the fork and its texture back")
