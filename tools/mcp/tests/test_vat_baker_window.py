"""The Vertex Animation Baker window in the real editor (vertex animation spec 5.1, 5.2 first bullet), on
the pose fixture project (Hero, Robot) plus the VAT fixture rigid_arm.gltf, driven by REAL X input (xdotool
on the harness' Xvfb, widgets found through the UI probe, Editor(ui_probe=True)) and the public MCP tools.
The window's own logic (source detection, the estimate, fix buttons, the time-sliced bake and its undo,
cancel, rebake, the import flow, the Inspector entries' hooks, the preview views) is the editor unit test
editor/tests/VatBakerWindowTest.cpp.

Covered here, where the App's wiring, focus and real input matter:
- focus_window "vertex_animation_baker" opens the window (Window > Animation > Vertex Animation Baker);
- a source typed into the Source field (real keys, Return) lists the model's clips;
- a validation row's fix button clicked for real (output_exists -> "Overwrite it") applies to the settings;
- a real click on Bake bakes over the editor's frames, writes the files as one project undo step (undo puts
  the replaced file back) and logs the status line in the Console;
- the Inspector's Animation Player section: Bake ▾ › Vertex Animation… opens the baker on that GameObject,
  and a Shift-click bakes at once, placing "<name> (VAT)" beside it (one scene undo step);
- Preview then a real click on Heatmap shows its legend, a second click hides it;
- after a Shift-click bake, the source GameObject deleted and Bake clicked for real: no crash (the stored
  result is dropped, never written), an error in the window and the Console, nothing new placed;
- while the scene plays the Bake button is refused (the bake doesn't run).
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness", "ui_probe": True}
CI = True
TIMEOUT = 900

ARM = "assets/models/rigid_arm.gltf"
TAKEN = "assets/models/taken.vat"
CLIP = "assets/anim/hero_rise.anim"

S: dict = {}


# ---------------------------------------------------------------------------- helpers

def xdo(ed: Editor, *args: str) -> str:
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    if not shutil.which("xdotool"):
        raise harness.TestFailure("xdotool is not installed: the baker window's real-input tests need it with the "
                                  "harness' Xvfb (install xdotool, e.g. apt install xdotool)")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout


async def focus_x(ed: Editor, window: str = "vertex_animation_baker") -> None:
    await ed.call("focus_window", window=window)
    ids = xdo(ed, "search", "--onlyvisible", "--name", "JasperEngine").split()
    ed.check(ids, "the editor's X window")
    xdo(ed, "windowfocus", "--sync", ids[-1])
    await ed.call("wait", frames=6)


def center(rect):
    return (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0


async def click(ed: Editor, rect, shift: bool = False) -> None:
    x, y = center(rect)
    xdo(ed, "mousemove", str(int(x)), str(int(y)))
    await asyncio.sleep(0.3)
    if shift:
        xdo(ed, "keydown", "shift")
        await ed.call("wait", frames=3)
    xdo(ed, "click", "1")
    if shift:
        await ed.call("wait", frames=3)
        xdo(ed, "keyup", "shift")
    await ed.call("wait", frames=6)


async def type_into(ed: Editor, rect, text: str, enter: bool = True) -> None:
    """Click a text field, replace its text by `text` (real keys), Return."""
    await click(ed, rect)
    xdo(ed, "key", "ctrl+a")
    xdo(ed, "key", "BackSpace")
    xdo(ed, "type", "--delay", "20", text)
    if enter:
        xdo(ed, "key", "Return")
    await ed.call("wait", frames=10)


async def rect_of(ed: Editor, name: str, what: str, panel: str = "vatbaker"):
    """`name`'s rectangle, scrolling `panel` (the baker's body, or "inspector") until it is drawn whole."""
    rect = await ed.ui_rect(name, timeout=3.0)
    if rect:
        return rect
    area = await ed.ui_rect(panel, timeout=10.0)
    ed.check(area, f"the {panel} panel is drawn: {sorted(ed.ui_items())[:80]}")
    at = center(area)
    xdo(ed, "mousemove", str(int(at[0])), str(int(at[1])))
    await asyncio.sleep(0.2)
    for down in (False, True):
        for _ in range(40):
            await ed.call("wait", frames=3)
            rect = ed.ui_items().get(name)
            if rect:
                return rect
            xdo(ed, "click", "5" if down else "4")
    ed.fail(f"{what} ({name}) is never drawn: {sorted(ed.ui_items())[:80]}")


async def wait_for(ed: Editor, predicate, what: str, seconds: float = 120.0) -> None:
    for _ in range(int(seconds * 4)):
        if predicate():
            return
        await ed.call("wait", frames=4)
        await asyncio.sleep(0.25)
    ed.fail(f"timed out waiting for {what}")


async def count_named(ed: Editor, pattern: str) -> int:
    found = await ed.call("find_entities", name=pattern)
    return len(harness.find_key(found, "entities") or [])


# ---------------------------------------------------------------------------- setup

async def test_setup(ed: Editor):
    ed.require_tools("focus_window", "write_file", "sync_files", "add_component", "select_entities", "undo", "find_entities",
                     "read_console")
    src = harness.REPO / "tools" / "fixtures" / "vertex-animation" / "assets" / "models" / "rigid_arm.gltf"
    (ed.project / "assets" / "models").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, ed.project / ARM)
    (ed.project / TAKEN).write_text("{}")
    # Hero rises: a root position clip on its Animation Player (a GameObject source)
    pos = harness.find_key(await ed.call("get_entity", entity="Hero", detail="full"), "position")
    if isinstance(pos, dict):
        pos = pos.get("value", pos)
    rest = [float(v) for v in pos]
    clip = {"format": "jasperengine.anim", "version": 1, "frame_rate": 30, "wrap": "loop",
            "tracks": [{"path": "", "channel": "position", "keys": [[0, rest], [1, [rest[0], rest[1] + 1.0, rest[2]]]]}], "events": []}
    await ed.call("write_file", path=CLIP, content=json.dumps(clip, indent=2) + "\n")
    await ed.call("sync_files")
    await ed.call("add_component", entity="Hero", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": CLIP}]}})
    await ed.call("save_scene", scene="Main Scene")
    await ed.console_since(None)


# ---------------------------------------------------------------------------- the window


async def test_typed_source_lists_the_clips(ed: Editor):
    await focus_x(ed)
    await type_into(ed, await rect_of(ed, "vatbaker.source", "the Source field"), ARM)
    await rect_of(ed, "vatbaker.clip.0", "the first clip row (Swing)")
    await rect_of(ed, "vatbaker.clip.1", "the second clip row (Stretch)")
    ed.check("vatbaker.clip.2" not in ed.ui_items(), "rigid_arm has two clips")
    await rect_of(ed, "vatbaker.estimate.texture", "the estimate's texture line")


async def test_fix_button_applies(ed: Editor):
    """output_exists: its "Overwrite it" button, clicked for real, sets overwrite (the row goes)."""
    await focus_x(ed)
    await type_into(ed, await rect_of(ed, "vatbaker.output", "the Output field"), TAKEN, enter=False)
    xdo(ed, "key", "Tab")
    await ed.call("wait", frames=30)
    fix = None
    for _ in range(40):
        fixes = sorted(k for k in ed.ui_items() if k.startswith("vatbaker.fix.check."))
        if fixes:
            fix = fixes[0]
            break
        await ed.call("wait", frames=5)
    ed.check(fix, f"a fix button of the output_exists row: {sorted(ed.ui_items())[:80]}")
    await click(ed, ed.ui_items()[fix])
    await ed.call("wait", frames=30)
    ed.check(not any(k.startswith("vatbaker.fix.check.") for k in ed.ui_items()),
             f"the fix applied, no row left: {sorted(k for k in ed.ui_items() if k.startswith('vatbaker.fix'))}")


async def test_bake_writes_one_project_undo_step(ed: Editor):
    await focus_x(ed)
    await click(ed, await rect_of(ed, "vatbaker.bake", "the Bake button"))
    path = ed.project / TAKEN
    await wait_for(ed, lambda: path.read_text() != "{}", "the bake to write taken.vat")
    data = json.loads(path.read_text())
    ed.check_eq(data["format"], "jasperengine.vat", "a .vat")
    ed.check_eq(data["recipe"]["sources"][0]["path"], ARM, "its recipe")
    ed.check((ed.project / "assets/models/taken.vat.glb").exists(), "its mesh")
    entries = await ed.console_since()
    ed.check(any("Baked 'assets/models/taken.vat'" in e.get("message", "") for e in entries),
             f"the Console status line: {[e.get('message', '')[:120] for e in entries]}")
    await ed.call("undo", scope="project")
    await ed.call("wait", frames=4)
    ed.check_eq(path.read_text(), "{}", "undo put the replaced file back")
    ed.check(not (ed.project / "assets/models/taken.vat.glb").exists(), "undo removed the mesh")


async def test_preview_heatmap_toggles(ed: Editor):
    """Preview (in memory) then a real click on Heatmap: its legend shows; a second click hides it."""
    await focus_x(ed)
    await type_into(ed, await rect_of(ed, "vatbaker.source", "the Source field"), ARM)
    await click(ed, await rect_of(ed, "vatbaker.previewRun", "the Preview button"))
    heat = await rect_of(ed, "vatbaker.heatmap", "the Heatmap toggle")
    await click(ed, heat)
    await rect_of(ed, "vatbaker.heatmapLegend", "the heatmap's legend")
    await click(ed, await rect_of(ed, "vatbaker.heatmap", "the Heatmap toggle"))
    await ed.call("wait", frames=10)
    ed.check("vatbaker.heatmapLegend" not in ed.ui_items(), "the legend goes with the heatmap")
    ed.check(not (ed.project / "assets/models/rigid_arm.vat").exists(), "a preview writes nothing")


# ---------------------------------------------------------------------------- the Inspector's entries

async def test_inspector_entry_opens_the_baker_and_shift_bakes(ed: Editor):
    await ed.call("select_entities", entities=["Hero"])
    await focus_x(ed, "inspector")
    await click(ed, await rect_of(ed, "player.bake", "the Animation Player's Bake button", "inspector"))
    await click(ed, await rect_of(ed, "player.bakeMenu.vat", "Bake ▾ › Vertex Animation…", "inspector"))
    await rect_of(ed, "vatbaker.clip.0", "the baker on Hero's clips")
    # Shift-click: bakes now, placed beside Hero
    heroes = await count_named(ed, "Hero (VAT)")
    await focus_x(ed, "inspector")
    await click(ed, await rect_of(ed, "player.bake", "the Animation Player's Bake button", "inspector"))
    await click(ed, await rect_of(ed, "player.bakeMenu.vat", "Bake ▾ › Vertex Animation…", "inspector"), shift=True)
    for _ in range(240):
        if await count_named(ed, "Hero (VAT)") > heroes:
            break
        await ed.call("wait", frames=4)
    ed.check_eq(await count_named(ed, "Hero (VAT)"), heroes + 1, "Shift-click placed the bake beside Hero")
    found = harness.find_key(await ed.call("find_entities", name="Hero (VAT)"), "entities")
    ed.check(found, "Hero (VAT)")
    detail = await ed.call("get_entity", entity="Hero (VAT)", detail="full")
    ed.check("vertexanimator" in json.dumps(detail).lower(), f"a Vertex Animator: {json.dumps(detail)[:400]}")
    await ed.call("undo", scope="scene")
    await ed.call("wait", frames=4)
    ed.check_eq(await count_named(ed, "Hero (VAT)"), heroes, "one scene undo step")


async def test_deleted_source_then_bake_is_an_error_not_a_crash(ed: Editor):
    """The reviewers' crash: Shift-click Bake ▾ › Vertex Animation… places "Hero (VAT)", Hero is deleted,
    a real click on Bake wrote the stored result naming the gone Hero (FATAL: unordered_map::at)."""
    ed.require_tools("delete_entities")
    await ed.call("select_entities", entities=["Hero"])
    heroes = await count_named(ed, "Hero (VAT)")
    await focus_x(ed, "inspector")
    await click(ed, await rect_of(ed, "player.bake", "the Animation Player's Bake button", "inspector"))
    await click(ed, await rect_of(ed, "player.bakeMenu.vat", "Bake ▾ › Vertex Animation…", "inspector"), shift=True)
    for _ in range(240):
        if await count_named(ed, "Hero (VAT)") > heroes:
            break
        await ed.call("wait", frames=4)
    ed.check_eq(await count_named(ed, "Hero (VAT)"), heroes + 1, "Shift-click placed the bake beside Hero")
    await ed.console_since()
    await ed.call("delete_entities", entities=["Hero"])
    await ed.call("wait", frames=10)
    await focus_x(ed)
    await click(ed, await rect_of(ed, "vatbaker.bake", "the Bake button"))
    await ed.call("wait", frames=40)
    # the editor is alive (the calls answer), nothing new was placed, the error is said
    ed.check_eq(await count_named(ed, "Hero (VAT)"), heroes + 1, "no second placement of the gone Hero")
    entries = await ed.console_since()
    messages = [e.get("message", "") for e in entries]
    ed.check(any("Hero" in m and ("gone" in m or "no GameObject" in m) for m in messages),
             f"the Console says the source is gone: {[m[:160] for m in messages]}")
    # back as it was: Hero, then no Hero (VAT)
    await ed.call("undo", scope="scene")
    await ed.call("undo", scope="scene")
    await ed.call("wait", frames=4)
    ed.check_eq(await count_named(ed, "Hero (VAT)"), heroes, "the placement undone")
    ed.check(await count_named(ed, "Hero") >= 1, "Hero back")


async def test_playing_refuses_the_bake(ed: Editor):
    ed.require_tools("play")
    for name in ("assets/models/rigid_arm.vat", "assets/models/rigid_arm.vat.glb"):
        (ed.project / name).unlink(missing_ok=True)
    await focus_x(ed)
    await type_into(ed, await rect_of(ed, "vatbaker.source", "the Source field"), ARM)
    await type_into(ed, await rect_of(ed, "vatbaker.output", "the Output field"), "", enter=False)
    await ed.call("play", action="start")
    await ed.call("wait", frames=20)
    await focus_x(ed)
    await click(ed, await rect_of(ed, "vatbaker.bake", "the Bake button"))
    await ed.call("wait", frames=30)
    ed.check(not (ed.project / "assets/models/rigid_arm.vat").exists(), "nothing baked while playing")
    await ed.call("play", action="stop")
    await ed.call("wait", frames=20)
