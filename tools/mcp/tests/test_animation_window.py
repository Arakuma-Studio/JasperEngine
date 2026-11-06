"""The Animation window in the real editor (animation clips spec 4.4, 9.R5b/R6/R7/R31), on the pose fixture
project (Hero: hero_tpose.gltf, Mixamo bone names; Robot), driven by REAL X input (xdotool on the harness'
Xvfb, widgets found through the UI probe, Editor(ui_probe=True)) and the public MCP tools. The window's own
logic (keys, values, events, bindings, sub-clips, drops, Record and the curves view through the functions
its buttons, menus and shortcuts run) is the editor unit test editor/tests/AnimationWindowClipTest.cpp.

Covered here, where the App's key routing, focus and real mouse input matter:
- C1: Delete / Backspace / Ctrl+D / F2 with the window focused (its empty state, and with a key selected
  by a real click) never delete, duplicate or rename the scene selection;
- R5b: real Ctrl+Z / Ctrl+Y go to the project history while the newest step is a clip commit the window
  made (K, Delete, Ctrl+D), to the scene's when a scene step is newer;
- the window's keys act only while it is focused (K with the Scene view focused keys nothing);
- a real Ctrl+Z during a Physics Placement gesture cancels the gesture even when a clip key is the
  newest step (the key stays);
- the playhead shortcuts (. , Alt+. Alt+, Space) on the focused window;
- I2: a real right-click on a track row's label opens the row menu, whose Retarget... opens its popup;
- R31: the entity mode's lanes are view only (showing them writes nothing) and a real frame drag to
  another lane is one scene step writing that frame's track; its dropdown lists the clip entries;
- R6 / R7: a save during the entity preview writes the unpreviewed scene and stops it; a clip preview
  of a hierarchy the entity preview writes (also through a nested Animation, I9) stops it;
- runtime-core 6.6 / 6.7: edits made during the entity preview are kept (and undo to the authored value);
  edit_animation's random_start, an Action's startOffset through set_properties.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import subprocess

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness", "ui_probe": True}
CI = True
TIMEOUT = 900

SCENE_FILE = "Main Scene.scene"
CLIP = "assets/anim/hero_clip.anim"
ROBOT_CLIP = "assets/anim/robot_walk.anim"

S: dict = {}


# ---------------------------------------------------------------------------- helpers

def xdo(ed: Editor, *args: str) -> str:
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    if not shutil.which("xdotool"):
        # agents review M5: these tests carry the window's real-input coverage (no editor test hook):
        # with the harness' Xvfb, a host without xdotool fails them instead of skipping quietly
        raise harness.TestFailure("xdotool is not installed: the Animation window's real-input tests need it with the "
                                  "harness' Xvfb (install xdotool, e.g. apt install xdotool)")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout


async def focus_x(ed: Editor, window: str = "animation") -> None:
    """`window` focused in ImGui and the editor's X window focused for xdotool."""
    await ed.call("focus_window", window=window)
    ids = xdo(ed, "search", "--onlyvisible", "--name", "JasperEngine").split()
    ed.check(ids, "the editor's X window")
    xdo(ed, "windowfocus", "--sync", ids[-1])
    await ed.call("wait", frames=6)


async def key(ed: Editor, combo: str) -> None:
    """A real key press (modifiers down, key down/up, modifiers up: `xdotool key ctrl+z` can land in one
    frame with the release first), then a few frames."""
    *modifiers, last = combo.split("+")
    for modifier in modifiers:
        xdo(ed, "keydown", "--clearmodifiers", modifier)
    xdo(ed, "keydown", last)
    xdo(ed, "keyup", last)
    for modifier in reversed(modifiers):
        xdo(ed, "keyup", modifier)
    await ed.call("wait", frames=8)


def center(rect):
    return (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0


async def click(ed: Editor, rect, button: str = "1") -> None:
    x, y = center(rect)
    xdo(ed, "mousemove", str(int(x)), str(int(y)))
    await asyncio.sleep(0.3)
    xdo(ed, "click", button)
    await ed.call("wait", frames=6)


async def drag(ed: Editor, start, end, steps: int = 6) -> None:
    xdo(ed, "mousemove", str(int(start[0])), str(int(start[1])))
    await ed.call("wait", frames=4)
    xdo(ed, "mousedown", "1")
    await ed.call("wait", frames=3)
    for n in range(1, steps + 1):
        x = start[0] + (end[0] - start[0]) * n / steps
        y = start[1] + (end[1] - start[1]) * n / steps
        xdo(ed, "mousemove", str(int(x)), str(int(y)))
        await ed.call("wait", frames=3)
    xdo(ed, "mouseup", "1")
    await ed.call("wait", frames=8)


async def modified(ed: Editor) -> bool:
    scenes = await ed.call("list_scenes")
    main = next(s for s in harness.find_key(scenes, "scenes") if s["name"] == "Main Scene")
    return bool(main["modified"])


async def position(ed: Editor, entity: str) -> list:
    pos = harness.find_key(await ed.call("get_entity", entity=entity, detail="full"), "position")
    if isinstance(pos, dict):
        pos = pos.get("value", pos)
    return [float(v) for v in pos]


async def count_named(ed: Editor, pattern: str) -> int:
    found = await ed.call("find_entities", name=pattern)
    return len(harness.find_key(found, "entities") or [])


def close(a, b, tol=1e-3) -> bool:
    return a is not None and b is not None and len(a) == len(b) and all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))


def clip_keys(ed: Editor, path: str = CLIP, track: int = 0) -> list:
    """The key times of a track of a clip file (each commit of the window writes the file)."""
    data = json.loads((ed.project / path).read_text())
    return [round(k[0], 4) for k in data["tracks"][track]["keys"]]


async def playhead(ed: Editor) -> dict:
    """The clip controller's state (the window's playhead), paused: preview_animation pause on Hero."""
    return await ed.call("preview_animation", animation="Hero", action="pause")


def saved_position(text: str, name: str):
    match = re.search(r"name: " + re.escape(name) + r"\n\s+components:\n\s+transform:\n\s+position: \[([^\]]*)\]", text)
    return [float(v) for v in match.group(1).split(",")] if match else None


def saved_tracks(text: str, animation: str) -> list:
    """The ActionFrame track values of the entity Animation `animation` in a scene file's text."""
    start = text.find("name: " + animation)
    block = text[start:start + 4000]
    actions = block[block.find("actions:"):]
    return [int(v) for v in re.findall(r"track: (\d+)", actions.split("\n  - ")[0])]


async def saved_scene(ed: Editor) -> str:
    await ed.call("save_scene", scene="Main Scene")
    return (ed.project / SCENE_FILE).read_text(encoding="utf-8")


# ---------------------------------------------------------------------------- setup, C1 empty state

async def test_setup(ed: Editor):
    ed.require_tools("focus_window", "write_file", "add_component", "select_entities", "preview_animation", "edit_animation", "undo")
    S["rest_hero"] = await position(ed, "Hero")
    rest = S["rest_hero"]
    clip = {"format": "jasperengine.anim", "version": 1, "frame_rate": 30, "wrap": "loop",
            "tracks": [{"path": "", "channel": "position", "keys": [[0, rest], [1, [rest[0], 2.0, rest[2]]]]}], "events": []}
    await ed.call("write_file", path=CLIP, content=json.dumps(clip, indent=2) + "\n")
    robot = {"format": "jasperengine.anim", "version": 1, "frame_rate": 30, "wrap": "loop",
             "tracks": [{"path": "", "channel": "position", "keys": [[0, [0.75, 0, 0]], [1, [0.75, 0.5, 0]]]}], "events": []}
    await ed.call("write_file", path=ROBOT_CLIP, content=json.dumps(robot, indent=2) + "\n")
    await ed.call("sync_files")
    await ed.call("add_component", entity="Hero", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": CLIP}]}})
    await ed.call("save_scene", scene="Main Scene")
    # (the player reloaded the model with its node hierarchy)
    S["heroes"] = await count_named(ed, "Hero*")
    S["robots"] = await count_named(ed, "Robot*")


async def test_scene_keys_stay_off_in_the_empty_state(ed: Editor):
    """C1: Delete / Backspace / Ctrl+D / F2 in the focused window (its empty state, Robot selected in the
    Hierarchy) never delete, duplicate or rename the scene selection."""
    await ed.call("select_entities", entities=["Robot"])
    await focus_x(ed)
    ed.check(await ed.ui_rect("clip.emptyState.create"), f"Robot (no player) shows the empty state: {sorted(ed.ui_items())}")
    for combo in ("Delete", "BackSpace", "ctrl+d", "F2"):
        await key(ed, combo)
    ed.check_eq(await count_named(ed, "Robot*"), S["robots"], "Robot was neither deleted nor duplicated")
    ed.check(not await modified(ed), "no scene step")


# ---------------------------------------------------------------------------- R5b, C1 with a key selected

async def test_keyboard_undo_goes_to_the_clip_after_a_window_edit(ed: Editor):
    """R5b: K in the focused window keys the clip (a clip commit); real Ctrl+Z / Ctrl+Y then go to the
    project history; a key selected by a real click is what Delete and Ctrl+D act on, never Hero."""
    await ed.call("select_entities", entities=["Hero"])
    await focus_x(ed)
    ed.check(await ed.ui_rect("clip.rowLabel.0"), f"Hero's clip shows: {sorted(ed.ui_items())}")
    await ed.call("preview_animation", animation="Hero", action="seek", time=0.0)
    await ed.call("preview_animation", animation="Hero", action="stop")
    await focus_x(ed)
    for _ in range(3):
        await key(ed, "period")
    await key(ed, "k")
    ed.check_eq(clip_keys(ed), [0.0, 0.1, 1.0], "K keyed the root at frame 3")
    ed.check(not await modified(ed), "a clip edit, no scene step")
    await key(ed, "ctrl+z")
    ed.check_eq(clip_keys(ed), [0.0, 1.0], "real Ctrl+Z undid the key (project history)")
    await key(ed, "ctrl+y")
    ed.check_eq(clip_keys(ed), [0.0, 0.1, 1.0], "real Ctrl+Y redid it")
    # a real click selects the key at 0.1: Delete deletes it, not Hero
    rect = await ed.ui_rect("clip.key.0.1")
    ed.check(rect, f"the key is drawn: {sorted(k for k in ed.ui_items() if k.startswith('clip.'))}")
    await click(ed, rect)
    await key(ed, "Delete")
    ed.check_eq(clip_keys(ed), [0.0, 1.0], "Delete removed the selected key")
    ed.check_eq(await count_named(ed, "Hero*"), S["heroes"], "Hero survives Delete")
    await key(ed, "ctrl+z")
    ed.check_eq(clip_keys(ed), [0.0, 0.1, 1.0], "real Ctrl+Z restored the key")
    # Ctrl+D duplicates the selected key one frame later, not Hero
    await click(ed, await ed.ui_rect("clip.key.0.1"))
    await key(ed, "ctrl+d")
    ed.check_eq(clip_keys(ed), [0.0, 0.1, 0.1333, 1.0], "Ctrl+D duplicated the key")
    ed.check_eq(await count_named(ed, "Hero*"), S["heroes"], "Hero was not duplicated")
    await key(ed, "ctrl+z")
    ed.check_eq(clip_keys(ed), [0.0, 0.1, 1.0], "the duplicate undone")
    ed.check(not await modified(ed), "key edits never modify the scene")


async def test_keyboard_undo_goes_to_the_scene_after_a_scene_step(ed: Editor):
    """R5b: a scene edit made last: real Ctrl+Z with the window focused undoes the scene, the clip stays."""
    rest = await position(ed, "Robot")
    await ed.call("set_properties", entity="Robot", values={"transform": {"position": [rest[0] + 0.2, rest[1], rest[2]]}})
    await focus_x(ed)
    await key(ed, "ctrl+z")
    ed.check(close(await position(ed, "Robot"), rest), "real Ctrl+Z undid the scene edit")
    ed.check_eq(clip_keys(ed), [0.0, 0.1, 1.0], "the clip is untouched")


async def test_keys_only_while_focused(ed: Editor):
    """The window's keys act only while it has focus: with the Scene view focused K keys nothing."""
    before = clip_keys(ed)
    await focus_x(ed, "scene")
    await ed.call("preview_animation", animation="Hero", action="seek", time=0.5)
    await ed.call("preview_animation", animation="Hero", action="stop")
    await focus_x(ed, "scene")
    await key(ed, "k")
    ed.check_eq(clip_keys(ed), before, "K in the Scene view keyed nothing")


async def test_undo_cancels_a_physics_gesture_before_a_clip_key(ed: Editor):
    """Final review M3: with a clip commit the newest step (K in the window), a real Ctrl+Z during a
    Physics Placement gesture of the Scene view cancels the gesture (its rule since before clips:
    the scene history's undo cancels it) and leaves the clip's key."""
    await ed.call("create_entity", name="Dropper", type="cube", position=[3, 4, 3])
    start = await position(ed, "Dropper")
    await ed.call("select_entities", entities=["Hero"])
    await focus_x(ed)
    await ed.call("preview_animation", animation="Hero", action="seek", time=0.5)
    await ed.call("preview_animation", animation="Hero", action="stop")
    await focus_x(ed)
    await key(ed, "k")
    keyed = clip_keys(ed)
    ed.check_eq(keyed, [0.0, 0.1, 0.5, 1.0], "K keyed the root at 0.5: a clip commit is the newest step")
    await ed.call("frame_entity", entity="Dropper")
    await ed.call("select_entities", entities=["Dropper"])
    await focus_x(ed, "scene")
    # the pointer over the Scene view, G: Physics on; holding Space drops the selection
    xdo(ed, "mousemove", "800", "300")
    await asyncio.sleep(0.3)
    xdo(ed, "key", "g")
    await asyncio.sleep(0.3)
    xdo(ed, "keydown", "space")
    try:
        moved = False
        for _ in range(100):
            if not close(await position(ed, "Dropper"), start):
                moved = True
                break
            await asyncio.sleep(0.05)
        ed.check(moved, "holding Space drops the selection (a running Physics Placement gesture)")
        await key(ed, "ctrl+z")
        ed.check(close(await position(ed, "Dropper"), start, 1e-4), "Ctrl+Z cancelled the gesture: Dropper back where it was")
        ed.check_eq(clip_keys(ed), keyed, "the clip's key is still there (the undo went to the gesture)")
    finally:
        xdo(ed, "keyup", "space")
    await ed.call("wait", frames=20)
    ed.check(close(await position(ed, "Dropper"), start, 1e-4), "nothing committed after the release")
    xdo(ed, "key", "g")
    await ed.call("wait", frames=4)
    # the key goes (project undo), and Dropper with it
    await ed.call("undo", scope="project")
    ed.check_eq(clip_keys(ed), [0.0, 0.1, 1.0], "the key undone")
    await ed.call("delete_entities", entities=["Dropper"])
    await saved_scene(ed)   # (the next tests check the scene stays unmodified)


async def test_playhead_shortcuts_while_focused(ed: Editor):
    await ed.call("preview_animation", animation="Hero", action="seek", time=0.0)
    await focus_x(ed)
    await key(ed, "period")
    await key(ed, "period")
    st = await playhead(ed)
    ed.check(abs(st["time"] - 2 / 30) < 1e-4, f". steps one frame each: {st['time']}")
    await focus_x(ed)
    await key(ed, "comma")
    ed.check(abs((await playhead(ed))["time"] - 1 / 30) < 1e-4, ", steps back")
    await focus_x(ed)
    await key(ed, "alt+period")
    ed.check(abs((await playhead(ed))["time"] - 0.1) < 1e-4, "Alt+. goes to the next key (0.1)")
    await focus_x(ed)
    await key(ed, "alt+comma")
    ed.check(abs((await playhead(ed))["time"]) < 1e-4, "Alt+, back to the previous key (0)")
    await focus_x(ed)
    await key(ed, "space")
    # (frames, not seconds: the preview advances by the frame time, whatever the host load)
    await ed.call("wait", frames=20)
    await key(ed, "space")
    first = await playhead(ed)
    ed.check(first["previewing"] and first["time"] >= 1 / 30 - 1e-4, f"Space played the preview (paused on a frame after it): {first}")
    await ed.call("wait", frames=10)
    ed.check(abs((await playhead(ed))["time"] - first["time"]) < 1e-4, "Space paused it")
    await ed.call("preview_animation", animation="Hero", action="stop")
    ed.check(close(await position(ed, "Hero"), S["rest_hero"]), "stopping the preview restores Hero")
    ed.check(not await modified(ed), "no scene change")


# ---------------------------------------------------------------------------- I2: the row menu

async def test_row_menu_opens_on_a_real_right_click(ed: Editor):
    """I2: the row label's right-click opens the menu (it was opened in the row's ID scope and never showed)"""
    await ed.call("select_entities", entities=["Hero"])
    await focus_x(ed)
    label = await ed.ui_rect("clip.rowLabel.0")
    ed.check(label, f"the row label is drawn: {sorted(ed.ui_items())}")
    ed.check("clip.rowMenu.retarget" not in ed.ui_items(), "no menu before the click")
    await click(ed, label, "3")
    item = await ed.ui_rect("clip.rowMenu.retarget", timeout=5)
    ed.check(item, f"the row menu is open after a real right-click: {sorted(ed.ui_items())}")
    await click(ed, item)
    title = await ed.ui_rect("clip.rebind.title", timeout=5)
    ed.check(title, f"Retarget... opens the Retarget popup: {sorted(ed.ui_items())}")
    xdo(ed, "key", "Escape")
    await ed.call("wait", frames=6)


# ---------------------------------------------------------------------------- entity mode

async def test_entity_mode_lanes_are_view_only_and_a_frame_drag_moves_one(ed: Editor):
    # an entity Animation with two tweens starting together, both stored on track 0: the window lays
    # them on two lanes for display only (R31); the saved tracks change only when a frame is moved
    await ed.call("edit_animation", target="Robot", name="RobotTweens", tweens=[
        {"type": "position", "to": [0.75, 1, 0], "duration": 1.0},
        {"type": "scale", "to": [1.5, 1.5, 1.5], "duration": 1.0}])
    await ed.call("edit_animation", animation="RobotTweens", set_frames=[{"index": 1, "lane": 0}])
    ed.check_eq(saved_tracks(await saved_scene(ed), "RobotTweens"), [0, 0], "both frames stored on track 0")
    await ed.call("select_entities", entities=["RobotTweens"])
    await focus_x(ed)
    first = await ed.ui_rect("entity.frame.0")
    second = await ed.ui_rect("entity.frame.1")
    ed.check(first and second, f"entity mode shows both frames: {sorted(k for k in ed.ui_items() if k.startswith('entity.'))}")
    ed.check(center(second)[1] > center(first)[1] + 2, f"shown on two lanes: {first} {second}")
    await ed.call("wait", seconds=0.5)
    ed.check(not await modified(ed), "the lanes are view only: the scene is not modified")
    ed.check_eq(saved_tracks(await saved_scene(ed), "RobotTweens"), [0, 0], "saved tracks unchanged")
    # a real drag of the second frame one lane further down: lane 2, one scene step
    pitch = center(second)[1] - center(first)[1]
    start = center(second)
    await drag(ed, start, (start[0], start[1] + pitch))
    ed.check(await modified(ed), "the move is a scene step")
    ed.check_eq(saved_tracks(await saved_scene(ed), "RobotTweens"), [0, 2], "moved where dropped")
    await ed.call("undo", scope="scene")
    ed.check_eq(saved_tracks(await saved_scene(ed), "RobotTweens"), [0, 0], "one undo step")


async def test_entity_mode_dropdown_lists_the_clip_entries(ed: Editor):
    """R31 (final review M6): with a GameObject selected that an entity Animation animates (Robot, no
    Animation Player), a real click on the entity mode's dropdown shows the clip entries above the
    entity Animations: Create New Clip... only (a second click closes it, nothing changes)."""
    await ed.call("select_entities", entities=["Robot"])
    await saved_scene(ed)
    await focus_x(ed)
    combo = await ed.ui_rect("entity.animationCombo")
    ed.check(combo, f"entity mode shows its dropdown: {sorted(k for k in ed.ui_items() if k.startswith('entity.'))}")
    await click(ed, combo)
    entry = await ed.ui_rect("entity.clipEntry.0")
    ed.check(entry and center(entry)[1] > center(combo)[1], f"the dropdown's first entry is below it: {entry} {combo}")
    ed.check(not await ed.ui_rect("entity.clipEntry.1", timeout=1.0), "one clip entry (Create New Clip...): Robot has no player")
    await click(ed, combo)
    ed.check(not await ed.ui_rect("entity.clipEntry.0", timeout=1.0), "a second click closes the dropdown")
    ed.check(not await modified(ed), "opening the dropdown changes nothing")


async def test_save_during_the_entity_preview_writes_the_unpreviewed_scene(ed: Editor):
    """R6: a save during the entity preview writes the unpreviewed Robot and stops the preview."""
    rest = await position(ed, "Robot")
    r = await ed.call("preview_animation", animation="RobotTweens", action="play", wait_ms=500)
    ed.check(r["previewing"], f"the entity preview runs: {r}")
    ed.check(not close(await position(ed, "Robot"), rest), "the entity preview moves Robot")
    # (save_scene runs the editor's save: App::stopTransientPreviews first)
    saved = saved_position(await saved_scene(ed), "Robot")
    ed.check(close(saved, rest), f"the save wrote the unpreviewed Robot: {saved} vs {rest}")
    r = await ed.call("preview_animation", animation="RobotTweens", action="pause")
    ed.check(not r["previewing"], f"the save stopped the entity preview: {r}")


async def test_a_clip_preview_stops_the_entity_preview_of_its_hierarchy(ed: Editor):
    """R7: the clip preview of Robot (its Animation Player) stops the entity preview that writes Robot,
    also when the entity Animation is the Hero's and only nests one of the Robot (I9)."""
    await ed.call("add_component", entity="Robot", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": ROBOT_CLIP}]}})
    r = await ed.call("preview_animation", animation="RobotTweens", action="play", wait_ms=300)
    ed.check(r["previewing"], f"entity preview running: {r}")
    await ed.call("preview_animation", animation="Robot", action="seek", time=0.2)
    r = await ed.call("preview_animation", animation="RobotTweens", action="pause")
    ed.check(not r["previewing"], f"the clip preview of Robot stopped the entity preview (PreviewOwner): {r}")
    await ed.call("preview_animation", animation="Robot", action="stop")
    # I9: an Animation on the Hero that plays an Animation of the Robot owns the Robot too
    await ed.call("edit_animation", target="Robot", name="FxRobotSpin", tweens=[{"type": "scale", "to": [1.5, 1.5, 1.5], "duration": 1.0}])
    await ed.call("edit_animation", target="Hero", name="FxHeroMix", tweens=[{"type": "position", "to": [0, 0.5, 0], "duration": 1.0}],
                  add_actions=[{"action": "FxRobotSpin", "start_time": 0}])
    r = await ed.call("preview_animation", animation="FxHeroMix", action="play", wait_ms=300)
    ed.check(r["previewing"], f"the nesting entity preview runs: {r}")
    await ed.call("preview_animation", animation="Robot", action="seek", time=0.2)
    r = await ed.call("preview_animation", animation="FxHeroMix", action="pause")
    ed.check(not r["previewing"], f"the clip preview of the Robot stopped the entity preview that writes it: {r}")
    await ed.call("preview_animation", animation="Robot", action="stop")
    ed.check(close(await position(ed, "Hero"), S["rest_hero"]), "Hero at rest")


# ---------------------------------------------------------------------------- runtime-core: edits during the preview, start offsets

async def field(ed: Editor, entity, component: str, name: str):
    """A component field's value from get_entity (the value of a {value, ...} entry)."""
    comp = harness.find_key(await ed.call("get_entity", entity=entity, detail="full"), component)
    value = harness.find_key(comp, name) if comp is not None else None
    return value.get("value", value) if isinstance(value, dict) else value


async def test_edits_during_the_entity_preview_are_kept(ed: Editor):
    """runtime-core 6.6: values written while the entity preview runs (set_properties, as an Inspector field)
    stay when it seeks and stops; what the preview animated goes back; the undo of an edit made then goes
    back to the authored value, not to a previewed pose."""
    await ed.call("edit_animation", target="Robot", name="RobotEdits", tweens=[{"type": "position", "to": [0.75, 1, 0], "duration": 1.0}])
    tween = harness.find_key(await ed.call("get_animation", entity="RobotEdits"), "actions")[0]["action"]
    tween_ref = tween["id"] if isinstance(tween, dict) else tween
    rest = await position(ed, "Robot")
    rest_scale = await field(ed, "Robot", "transform", "scale")
    r = await ed.call("preview_animation", animation="RobotEdits", action="play", wait_ms=300)
    ed.check(r["previewing"], f"entity preview running: {r}")
    await ed.call("preview_animation", animation="RobotEdits", action="pause")
    ed.check(not close(await position(ed, "Robot"), rest), "the preview moved Robot")

    await ed.call("set_properties", entity=tween_ref, values={"tween": {"duration": 2.0}})
    await ed.call("set_properties", entity="Robot", values={"transform": {"scale": [2, 2, 2]}})
    await ed.call("preview_animation", animation="RobotEdits", action="seek", time=0.2)
    ed.check_eq(float(await field(ed, tween_ref, "tween", "duration")), 2.0, "a seek keeps the tween's new duration")
    await ed.call("preview_animation", animation="RobotEdits", action="stop")

    ed.check_eq(float(await field(ed, tween_ref, "tween", "duration")), 2.0, "the edited duration stays after Stop")
    ed.check(close(await field(ed, "Robot", "transform", "scale"), [2, 2, 2]), "the edited scale stays after Stop")
    ed.check(close(await position(ed, "Robot"), rest), "the previewed position went back")

    await ed.call("undo", scope="scene")
    ed.check(close(await field(ed, "Robot", "transform", "scale"), rest_scale), "the scale's undo gives the authored value")
    await ed.call("undo", scope="scene")
    ed.check_eq(float(await field(ed, tween_ref, "tween", "duration")), 1.0, "the duration's undo gives the authored value")
    ed.check(close(await position(ed, "Robot"), rest), "no previewed pose came back with the undos")


async def test_start_offset_through_edit_animation(ed: Editor):
    """runtime-core 6.7: edit_animation random_start and set_properties' action startOffset set the
    Animation's Action; get_animation reports them; each is one undo step; a nested action's own Start
    Offset is reported; a clip refuses random_start."""
    await ed.call("edit_animation", target="Robot", name="RobotOffsets", tweens=[{"type": "scale", "to": [1.2, 1.2, 1.2], "duration": 1.0}])
    r = await ed.call("edit_animation", animation="RobotOffsets", random_start=True)
    ed.check_eq(harness.find_key(r, "random_start"), True, f"reported: {r}")
    await ed.call("set_properties", entity="RobotOffsets", values={"action": {"startOffset": 0.5}})
    got = await ed.call("get_animation", entity="RobotOffsets")
    ed.check_eq((harness.find_key(got, "start_offset"), harness.find_key(got, "random_start")), (0.5, True), f"get_animation: {got}")
    await ed.call("undo", scope="scene")
    await ed.call("undo", scope="scene")
    got = await ed.call("get_animation", entity="RobotOffsets")
    ed.check_eq((harness.find_key(got, "start_offset"), harness.find_key(got, "random_start")), (0.0, False), "one undo step each")

    tween = harness.find_key(got, "actions")[0]["action"]
    tween_ref = tween["id"] if isinstance(tween, dict) else tween
    await ed.call("set_properties", entity=tween_ref, values={"action": {"startOffset": 0.25}})
    ed.check_eq(float(await field(ed, tween_ref, "action", "startOffset")), 0.25, "a nested action's Start Offset")
    got = await ed.call("get_animation", entity="RobotOffsets")
    ed.check_eq(harness.find_key(got, "actions")[0].get("start_offset"), 0.25, f"reported on the nested action: {got}")
    await ed.call("edit_animation", clip=ROBOT_CLIP, random_start=True, expect_error=True)
