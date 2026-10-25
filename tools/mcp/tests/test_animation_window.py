"""The Animation window's clip mode (animation clips spec 4.4, 9.R4-R7/R31/R39/R54), on the pose fixture
project (Hero: hero_tpose.gltf, Mixamo bone names), driven through the focus_window
{window: "animation", animation: {...}} test hook (JASPER_MCP_TEST_HOOKS=1) and REAL X key presses
(xdotool on the harness' Xvfb) where the App's key routing matters.

The hook runs the window's own actions (the functions its buttons, menus and shortcuts call) and
returns its state (`viewport_input`): mode, clip, rows, missing, selection, time, preview, events.
Covered: the mode switch (empty state, clip mode, entity mode, view-only lanes never written: the
saved ActionFrame tracks, an explicit lane move as one step), Delete / Backspace / Ctrl+D / F2 with the
window focused never touching the scene selection (also in the empty state and after a scene step),
real Ctrl+Z / Ctrl+Y routing (R5b), the window's keys only while focused, Create... (one scene step),
Add Property (R39 names refused), keys (select, Shift, box by row and time, drag snapped to frames,
delete, copy/paste with skipped / replaced keys, duplicate, ease, K / Shift+K on the Samples grid after
play/pause, the value column's unkeyed pink value, an Euler edit), missing bindings and Rebind, the events
lane with a Lua script on the root (handler found / missing, selection by identity across undo), Samples and
Snap keys to frames (which key survives), the read-only sub-clip (previewable on its model, Extract to
.anim alone and through the player's state), preview isolation (saves write the unpreviewed scene, also during
the entity preview, PreviewOwner between the entity and the clip preview), a 36 000-key clip staying
responsive. Editor screenshots go to /tmp/clip-research/reports/T3b-shots/round1/.
"""

from __future__ import annotations

import json
import math
import os
import re
import shutil
import subprocess
from pathlib import Path

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness"}
TIMEOUT = 1200

SHOTS = Path(os.environ.get("T3B_SHOTS", "/tmp/clip-research/reports/T3b-shots/round1"))
SCENE_FILE = "Main Scene.scene"
CLIP = "assets/anim/hero_clip.anim"
SPINE = "Hero/mixamorig:Hips/mixamorig:Spine"

S: dict = {}


# ---------------------------------------------------------------------------- helpers

async def hook(ed: Editor, **animation) -> dict:
    r = await ed.call("focus_window", window="animation", animation=animation)
    out = r.get("viewport_input") if isinstance(r, dict) else None
    ed.check(isinstance(out, dict), f"the hook returns the window state: {r}")
    return out


async def state(ed: Editor) -> dict:
    return await hook(ed)


async def shot(ed: Editor, name: str) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    await ed.call("wait", seconds=0.4)
    png = await ed.screenshot(view="editor", max_width=0)
    (SHOTS / f"{name}.png").write_bytes(png)


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
    items = harness.find_key(found, "entities") or []
    return len(items)


def close(a, b, tol=1e-3) -> bool:
    return a is not None and b is not None and len(a) == len(b) and all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))


def row(st: dict, path: str, channel: str) -> dict | None:
    return next((r for r in st["rows"] if r.get("path") == path and r.get("channel") == channel), None)


def keys_of(st: dict, path: str, channel: str) -> list:
    r = row(st, path, channel)
    return [round(t, 4) for t in r["keys"]] if r else []


def on_grid(time: float, fps: float) -> bool:
    return abs(time * fps - round(time * fps)) < 1e-3


def run_x(ed: Editor, *args: str) -> bytes:
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(list(args), env=env, check=True, capture_output=True).stdout


async def focus_keys(ed: Editor) -> None:
    """The Animation window focused in ImGui and the editor's X window focused for xdotool."""
    if not shutil.which("xdotool"):
        harness.skip("xdotool is not installed")
    await ed.call("focus_window", window="animation")
    ids = run_x(ed, "xdotool", "search", "--onlyvisible", "--name", "JasperEngine").split()
    ed.check(ids, "the editor's X window")
    run_x(ed, "xdotool", "windowfocus", "--sync", ids[-1].decode())
    await ed.call("wait", frames=6)


async def key(ed: Editor, combo: str) -> None:
    """A real key press (modifiers down, key down/up, modifiers up, then a few frames)."""
    *modifiers, last = combo.split("+")
    for modifier in modifiers:
        run_x(ed, "xdotool", "keydown", "--clearmodifiers", modifier)
    run_x(ed, "xdotool", "keydown", last)
    run_x(ed, "xdotool", "keyup", last)
    for modifier in reversed(modifiers):
        run_x(ed, "xdotool", "keyup", modifier)
    await ed.call("wait", frames=8)


def saved_position(text: str, name: str):
    match = re.search(r"name: " + re.escape(name) + r"\n\s+components:\n\s+transform:\n\s+position: \[([^\]]*)\]", text)
    return [float(v) for v in match.group(1).split(",")] if match else None


def saved_tracks(text: str, animation: str) -> list:
    """The ActionFrame track values of the entity Animation `animation` in a scene file's text."""
    start = text.find("name: " + animation)
    block = text[start:start + 4000]
    actions = block[block.find("actions:"):]
    return [int(v) for v in re.findall(r"track: (\d+)", actions.split("\n  - ")[0])]


# ---------------------------------------------------------------------------- empty state, C1

async def test_hook_and_empty_state(ed: Editor):
    ed.require_tools("focus_window", "write_file", "list_scenes", "find_entities", "create_script", "add_component")
    await ed.call("save_scene", scene="Main Scene")
    S["rest_hero"] = await position(ed, "Hero")
    S["heroes"] = await count_named(ed, "Hero*")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"bogus": 1}}, contains="unknown key")
    await ed.check_error("focus_window", {"window": "console", "animation": {}}, contains="animation")
    st = await hook(ed, entity="Hero")
    ed.check_eq(st["mode"], "clip", f"a GameObject without player or entity Animation: clip mode: {st}")
    ed.check(st["empty_state"], f"the empty state: {st}")
    ed.check_eq(st["root"], "Hero", "bound to the selection")
    ed.check("To begin animating Hero" in st.get("empty_text", ""), st.get("empty_text"))
    ed.check(st["clip"] is None and not st["has_player"], f"no clip, no player: {st}")
    await shot(ed, "01-empty-state")
    ed.check(not await modified(ed), "opening the window does not modify the scene")


async def test_scene_keys_stay_off_in_the_empty_state(ed: Editor):
    """C1: Delete / Backspace / Ctrl+D / F2 in the focused window (empty state, Hero selected in the
    Hierarchy) never delete, duplicate or rename the scene selection."""
    await focus_keys(ed)
    st = await state(ed)
    ed.check(st["takes_keys"], f"the focused window takes its keys: {st['takes_keys']}")
    for combo in ("Delete", "BackSpace", "ctrl+d", "F2"):
        await key(ed, combo)
    ed.check_eq(await count_named(ed, "Hero*"), S["heroes"], "Hero was neither deleted nor duplicated")
    ed.check(not await modified(ed), "no scene step")


async def test_create_clip(ed: Editor):
    st = await hook(ed, create_clip=CLIP)
    ed.check_eq(st["clip"], CLIP, f"Create... opens the new clip: {st}")
    ed.check(st["has_player"], "an Animation Player was added")
    ed.check_eq(st["states"], ["hero_clip"], "the player lists the clip")
    ed.check_eq(st["state"], "hero_clip", "the clip is the player's state")
    ed.check((ed.project / CLIP).is_file(), "the file was written")
    ed.check(not st["empty_state"], "no empty state any more")
    ed.check(await modified(ed), "Create... is a scene step (the player)")
    ed.check(not st["undo_to_project"], "its undo is the scene's (the newest step is a scene step)")
    st = await hook(ed, list_paths=True)
    ed.check_in(SPINE, st["paths"], "the Mixamo bones are listed by Add Property")
    # (the player reloaded the model with its node hierarchy: count the Hero GameObjects again)
    S["heroes"] = await count_named(ed, "Hero*")
    await ed.call("save_scene", scene="Main Scene")
    S["saved_scene"] = (ed.project / SCENE_FILE).read_text(encoding="utf-8")


async def test_add_property_and_keys(ed: Editor):
    st = await hook(ed, add_property={"path": "", "channel": "position"})
    r = row(st, "", "position")
    ed.check(r is not None, f"a root position row: {st['rows']}")
    ed.check_eq(r["keys"], [0.0], "Add Property keys the current value at the playhead")
    ed.check(close(r["value"], S["rest_hero"]), f"the key holds the current value: {r['value']} vs {S['rest_hero']}")
    ed.check(st["undo_to_project"], "a clip edit: Ctrl+Z goes to the project history")
    ed.check(not await modified(ed), "clip edits never modify the scene")
    st = await hook(ed, add_property={"path": SPINE, "channel": "rotation"})
    ed.check(row(st, SPINE, "rotation") is not None, "a bone rotation row")
    ed.check(any(x.get("component") == 2 for x in st["rows"]), f"new rows are unfolded (x/y/z): {st['rows']}")
    # R39: a name ending in "#<digits>" can't be addressed: refused, not listed
    await ed.call("create_entity", name="Bone#1", type="empty", parent="Hero")
    st = await hook(ed, list_paths=True)
    ed.check(not any(p.endswith("Bone#1") for p in st["paths"]), f"'Bone#1' is not offered: {[p for p in st['paths'] if 'Bone' in p]}")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"add_property": {"path": "Bone#1", "channel": "position"}}})
    await ed.call("undo", scope="scene")
    # the value column: an unkeyed (pink) value, keyed by K
    st = await hook(ed, frame=30, set_value={"track": 0, "component": 1, "value": 2.0})
    ed.check(st["preview"], "editing a value previews it")
    ed.check(row(st, "", "position")["pending"], f"the edit is pending (pink): {row(st, '', 'position')}")
    pos = await position(ed, "Hero")
    ed.check(abs(pos[1] - 2.0) < 1e-3, f"the Scene view shows the pending value: {pos}")
    ed.check_eq(row(st, "", "position")["keys"], [0.0], "not keyed yet")
    st = await hook(ed, key="all")
    r = row(st, "", "position")
    ed.check_eq(r["keys"], [0.0, 1.0], f"K keys it at the playhead: {r}")
    ed.check(abs(r["value"][1] - 2.0) < 1e-3, f"the key value: {r}")
    ed.check(not r["pending"], "keyed: no longer pending")
    # an Euler edit of the spine rotation keys the quaternion of (30, 0, 0)
    spine = next(x["track"] for x in st["rows"] if x.get("path") == SPINE)
    st = await hook(ed, set_value={"track": spine, "component": 0, "value": 30.0})
    st = await hook(ed, key="all")
    value = row(st, SPINE, "rotation")["value"]
    expected = [math.sin(math.radians(15)), 0.0, 0.0, math.cos(math.radians(15))]
    ed.check(close(value, expected, 1e-3) or close([-v for v in value], expected, 1e-3), f"30 degrees about x: {value}")
    ed.check(not await modified(ed), "the preview does not modify the scene")
    await shot(ed, "02-clip-rows")


async def test_scene_keys_after_a_scene_step(ed: Editor):
    """C1 after a scene step (the newest undoable step is the scene's): real Delete / Ctrl+D act on the
    selected key only, never on Hero (selected in the Hierarchy)."""
    await ed.call("set_properties", entity="Robot", values={"transform": {"position": [0.8, 0, 0]}})
    st = await hook(ed, entity="Hero", select=[{"track": 0, "time": 1.0}])
    ed.check(not st["undo_to_project"], "the newest step is the scene step")
    ed.check_eq(len(st["selection"]), 1, f"one key selected: {st['selection']}")
    before = keys_of(st, "", "position")
    await focus_keys(ed)
    await key(ed, "Delete")
    st = await state(ed)
    ed.check_eq(keys_of(st, "", "position"), [t for t in before if abs(t - 1.0) > 1e-4], "Delete removed the key")
    ed.check_eq(await count_named(ed, "Hero*"), S["heroes"], "Hero survives Delete")
    # Ctrl+Z (a clip commit is newest now) undoes the key delete on the project history
    await key(ed, "ctrl+z")
    st = await state(ed)
    ed.check_eq(keys_of(st, "", "position"), before, "real Ctrl+Z restored the key (project history)")
    await key(ed, "ctrl+y")
    ed.check_eq(keys_of(await state(ed), "", "position"), [t for t in before if abs(t - 1.0) > 1e-4], "real Ctrl+Y redoes it")
    await key(ed, "ctrl+z")
    # Ctrl+D duplicates the selected key, not Hero
    st = await hook(ed, select=[{"track": 0, "time": 0.0}])
    await focus_keys(ed)
    await key(ed, "ctrl+d")
    st = await state(ed)
    ed.check_eq(len(keys_of(st, "", "position")), len(before) + 1, f"Ctrl+D duplicated the key: {keys_of(st, '', 'position')}")
    ed.check_eq(await count_named(ed, "Hero*"), S["heroes"], "Hero was not duplicated")
    await key(ed, "ctrl+z")
    ed.check_eq(keys_of(await state(ed), "", "position"), before, "the duplicate undone")
    # a scene edit made last: Ctrl+Z with the window focused undoes the scene (R5b)
    await ed.call("set_properties", entity="Robot", values={"transform": {"position": [0.9, 0, 0]}})
    await focus_keys(ed)
    await key(ed, "ctrl+z")
    ed.check(close(await position(ed, "Robot"), [0.8, 0, 0]), "real Ctrl+Z undid the scene edit")
    ed.check_eq(keys_of(await state(ed), "", "position"), before, "the clip is untouched")
    await ed.call("undo", scope="scene")
    ed.check(close(await position(ed, "Robot"), [0.75, 0, 0], 0.06), "Robot back")
    await ed.call("save_scene", scene="Main Scene")
    S["saved_scene"] = (ed.project / SCENE_FILE).read_text(encoding="utf-8")


async def test_keys_only_while_focused(ed: Editor):
    """The window's keys act only while it has focus: with the Scene view focused K keys nothing."""
    before = keys_of(await state(ed), "", "position")
    await ed.call("focus_window", window="scene")
    await ed.call("wait", frames=6)
    await key(ed, "k")
    st = await state(ed)
    ed.check(not st["takes_keys"], "the Scene view focused: the window does not take keys")
    ed.check_eq(keys_of(st, "", "position"), before, "K in the Scene view keyed nothing")


async def test_preview_isolation_and_save(ed: Editor):
    st = await hook(ed, time=1.0, preview=True)
    ed.check(st["preview"], "previewing")
    pos = await position(ed, "Hero")
    ed.check(abs(pos[1] - 2.0) < 1e-3, f"the preview shows the clip at 1 s: {pos}")
    await ed.call("save_scene", scene="Main Scene")
    text = (ed.project / SCENE_FILE).read_text(encoding="utf-8")
    ed.check(text == S["saved_scene"], "a save during the preview writes the unpreviewed scene")
    ed.check(not await modified(ed), "not modified")
    st = await hook(ed, preview=False)
    ed.check(not st["preview"], "stopped")
    ed.check(close(await position(ed, "Hero"), S["rest_hero"]), "stop restores the scene")


async def test_play_pause_keys_on_the_grid(ed: Editor):
    """I3: after Space-play then pause the playhead is on a frame, and K keys there."""
    await hook(ed, time=0.0, play=True)
    await ed.call("wait", seconds=0.37)
    st = await hook(ed, play=False)
    ed.check(on_grid(st["time"], st["frame_rate"]), f"the paused playhead is on a frame: {st['time']}")
    st = await hook(ed, key="all")
    for r in st["rows"]:
        for t in r.get("keys", []):
            ed.check(on_grid(t, st["frame_rate"]), f"every key on the grid: {t} in {r['label']}")
    await ed.call("undo", scope="project")
    await hook(ed, preview=False)


async def test_select_drag_delete_copy_paste(ed: Editor):
    # the root position track has keys at 0 and 1; add one at 0.5 (the context menu's Add Key)
    st = await hook(ed, add_key={"track": 0, "time": 0.5})
    ed.check_eq(keys_of(st, "", "position"), [0.0, 0.5, 1.0], "Add Key keeps the curve (sampled value)")
    st = await hook(ed, time=0.5)
    ed.check(abs(row(st, "", "position")["value"][1] - 1.0) < 1e-3, f"the new key holds the linear midpoint: {row(st, '', 'position')}")
    st = await hook(ed, select=[{"track": 0, "time": 0.5}])
    ed.check_eq([(k["track"], k["time"]) for k in st["selection"]], [(0, 0.5)], f"click select: {st['selection']}")
    st = await hook(ed, select=[{"path": SPINE, "channel": "rotation", "time": 0.0}], add=True)
    ed.check_eq(len(st["selection"]), 2, f"Shift adds: {st['selection']}")
    await shot(ed, "03-multi-select")
    # box by row and time: only the 0.5 key of row 1; the summary row (0) takes every track
    st = await hook(ed, box={"rows": [1, 1], "times": [0.4, 0.6]})
    ed.check_eq([(k["track"], round(k["time"], 4)) for k in st["selection"]], [(0, 0.5)], f"box in time: {st['selection']}")
    st = await hook(ed, box={"rows": [0, 0], "times": [0.0, 0.01]})
    ed.check_eq(sorted(k["path"] for k in st["selection"]), sorted(["", SPINE]), f"the summary row boxes every track: {st['selection']}")
    # a drag of 0.21 s lands on frames (30 fps: the grabbed key at 0.7)
    st = await hook(ed, select=[{"track": 0, "time": 0.5}], drag=0.21)
    times = keys_of(st, "", "position")
    ed.check_eq(times, [0.0, 0.7, 1.0], f"the key moved to frame 21, nothing lost: {times}")
    ed.check(abs(st["selection"][0]["time"] - 0.7) < 1e-4, f"selection follows: {st['selection']}")
    await ed.call("undo", scope="project")
    ed.check_eq(keys_of(await state(ed), "", "position"), [0.0, 0.5, 1.0], "the drag is one undo step")
    await ed.call("redo", scope="project")
    # copy the 0.7 key: paste at 1.5 adds it (source kept); paste onto 1.0 replaces that key
    st = await hook(ed, select=[{"track": 0, "time": 0.7}], copy=True)
    ed.check_eq(st["clipboard"], 1, "copied")
    st = await hook(ed, time=1.5, paste=True)
    ed.check_eq(keys_of(st, "", "position"), [0.0, 0.7, 1.0, 1.5], "pasted at the playhead, the source kept")
    st = await hook(ed, paste=1.0)
    ed.check_eq(keys_of(st, "", "position"), [0.0, 0.7, 1.0, 1.5], "pasted onto 1.0: replaced, no new key")
    ed.check("replacing 1" in st["notice"], f"the notice counts the replaced key: {st['notice']}")
    # a copied key without a matching property is skipped with a notice
    spine = next(x["track"] for x in st["rows"] if x.get("path") == SPINE)
    st = await hook(ed, select=[{"track": 0, "time": 0.7}, {"track": spine, "time": 0.0}], copy=True)
    st = await hook(ed, remove_track=spine)
    st = await hook(ed, paste=2.0)
    ed.check("skipped" in st["notice"] and st["notice_warning"], f"the unmatched key is skipped: {st['notice']}")
    # (the copied keys keep their spacing: the position key was 0.7 s after the spine key)
    ed.check_in(2.7, keys_of(st, "", "position"), "the matched key pasted")
    await ed.call("undo", scope="project")
    await ed.call("undo", scope="project")
    st = await state(ed)
    ed.check(row(st, SPINE, "rotation") is not None, "the spine track is back")
    # duplicate: one more key, the source unchanged
    st = await hook(ed, select=[{"track": 0, "time": 0.7}], duplicate=True)
    keys = keys_of(st, "", "position")
    ed.check_eq(keys, [0.0, 0.7, 0.7333, 1.0, 1.5], f"duplicated one frame later, source kept: {keys}")
    # interpolation of the selection
    st = await hook(ed, select=[{"track": 0, "time": 0.0}], ease="step")
    data = json.loads((ed.project / CLIP).read_text())
    first = next(t for t in data["tracks"] if t["path"] == "" and t["channel"] == "position")
    ed.check("step" in json.dumps(first["keys"][0]), f"the key's ease in the file: {first['keys'][0]}")
    # delete
    st = await hook(ed, select=[{"track": 0, "time": 1.5}], delete=True)
    ed.check_eq(keys_of(st, "", "position"), [0.0, 0.7, 0.7333, 1.0], "Delete removes the selected key")
    ed.check_eq(st["selection"], [], "nothing selected after a delete")
    ed.check(not await modified(ed), "key edits never modify the scene")


async def test_missing_bindings_and_rebind(ed: Editor):
    clip = {"format": "jasperengine.anim", "version": 1, "frame_rate": 30, "wrap": "loop",
            "tracks": [{"path": SPINE + "e", "channel": "rotation",
                        "keys": [[0, [0, 0, 0, 1]], [1, [0, 0.3826834, 0, 0.9238795]]]},
                       {"path": "", "channel": "position", "keys": [[0, [0, 0, 0]], [1, [0, 1, 0]]]}],
            "events": []}
    await ed.call("write_file", path="assets/anim/missing.anim", content=json.dumps(clip, indent=2) + "\n")
    st = await hook(ed, clip="assets/anim/missing.anim", root="Hero")
    ed.check_eq(st["clip"], "assets/anim/missing.anim", "opened on Hero")
    ed.check_eq(len(st["missing"]), 1, f"one missing binding: {st['missing']}")
    ed.check("(Missing!)" in st["missing"][0], st["missing"][0])
    bad = next(r for r in st["rows"] if r.get("missing") and "path" in r)
    ed.check("Did you mean" in bad.get("message", "") and SPINE in bad["message"], f"nearest names: {bad}")
    ed.check_eq(st["bindings"], {"resolved": 1, "total": 2, "legacy_model_layout": False}, "1 of 2 resolve")
    await shot(ed, "04-missing-rows")
    st = await hook(ed, rebind={"track": bad["track"], "path": SPINE})
    ed.check_eq(st["missing"], [], f"rebound: {st['rows']}")
    ed.check("changes the clip for" in st["notice"], f"the notice names the users: {st['notice']}")
    data = json.loads((ed.project / "assets/anim/missing.anim").read_text())
    ed.check(any(t["path"] == SPINE for t in data["tracks"]), "the file has the new path")
    st = await hook(ed, remove_track=0)
    ed.check_eq(len([r for r in st["rows"] if "path" in r]), 1, "one row left")
    await ed.call("undo", scope="project")
    st = await state(ed)
    ed.check_eq(len([r for r in st["rows"] if "path" in r]), 2, "undo of Remove (project history)")


async def test_events_lane_with_a_script(ed: Editor):
    script = ("local HeroEvents = {}\n\nfunction HeroEvents:init()\nend\n\n"
              "function HeroEvents:onStep(side)\nend\n\nreturn HeroEvents\n")
    await ed.call("create_script", kind="lua", name="HeroEvents", content=script, entity="Hero")
    await ed.call("save_scene", scene="Main Scene")
    S["saved_scene"] = (ed.project / SCENE_FILE).read_text(encoding="utf-8")
    st = await hook(ed, state="hero_clip")
    ed.check_eq(st["clip"], CLIP, "back on the player's clip")
    await ed.call("wait", seconds=1.2)
    st = await hook(ed, add_event={"time": 0.5, "function": "onStep", "args": [1, "left", True]})
    ed.check_in("onStep", st["script_functions"], f"the root's script methods: {st['script_functions']}")
    ed.check_eq(len(st["events"]), 1, f"an event: {st['events']}")
    ev = st["events"][0]
    ed.check_eq((ev["time"], ev["function"], ev["args"]), (0.5, "onStep", [1, "left", True]), f"its fields: {ev}")
    ed.check_eq(ev["handler"], "found", "the Lua script on Hero defines onStep")
    ed.check_eq(st["selected_event"], 0, "the new event is selected (its editor opens)")
    await shot(ed, "05-events-lane")
    st = await hook(ed, event={"function": "onMissing", "args": [2]})
    ed.check_eq((st["events"][0]["function"], st["events"][0]["args"]), ("onMissing", [2]), "edited in the event editor")
    ed.check_eq(st["events"][0]["handler"], "missing", "no script defines onMissing (WarningText)")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"event": {"function": "not an id"}}}, contains="identifier")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"event": {"function": "end"}}}, contains="keyword")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"event": {"args": list(range(9))}}}, contains="at most")
    st = await hook(ed, drag_event=0.21)
    ed.check(abs(st["events"][0]["time"] - 0.7) < 1e-4, f"dragged to a frame: {st['events']}")
    await ed.call("undo", scope="project")
    st = await state(ed)
    ed.check(abs(st["events"][0]["time"] - 0.5) < 1e-4, f"the drag is one undo step: {st['events']}")
    # I4: the selection is the event itself, never an index: an event added before it and undone
    st = await hook(ed, add_event={"time": 0.1, "function": "onStep"})
    ed.check_eq((st["selected_event"], len(st["events"])), (0, 2), f"B at 0.1 selected (index 0): {st['events']}")
    await ed.call("undo", scope="project")
    st = await state(ed)
    ed.check_eq((st["selected_event"], len(st["events"])), (-1, 1), f"B undone: nothing selected, A kept: {st}")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"delete_event": True}}, contains="no event selected")
    ed.check_eq(len((await state(ed))["events"]), 1, "Delete with nothing selected deleted nothing")
    st = await hook(ed, select_event=0, delete_event=True)
    ed.check_eq(st["events"], [], "deleted")
    ed.check(not await modified(ed), "events are clip edits")


async def test_samples_and_snap(ed: Editor):
    """R54: snapping keeps the key nearer the frame; Samples refuses out-of-range values."""
    clip = {"format": "jasperengine.anim", "version": 1, "frame_rate": 24, "wrap": "once",
            "tracks": [{"path": "", "channel": "position",
                        "keys": [[0, [0, 0, 0]], [0.25, [0, 0.25, 0]], [0.26, [0, 0.26, 0]], [1, [0, 1, 0]]]}],
            "events": []}
    await ed.call("write_file", path="assets/anim/snap.anim", content=json.dumps(clip, indent=2) + "\n")
    st = await hook(ed, clip="assets/anim/snap.anim", root="Hero")
    st = await hook(ed, snap_keys=True)
    ed.check(st["notice_warning"] and "merged" in st["notice"], f"the merge warning: {st['notice']}")
    ed.check_eq(keys_of(st, "", "position"), [0.0, 0.25, 1.0], "one key at 6/24 s")
    st = await hook(ed, time=0.25)
    ed.check(abs(row(st, "", "position")["value"][1] - 0.25) < 1e-4, f"the nearer key (0.25) survived: {row(st, '', 'position')}")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"frame_rate": 500}}, contains="1 to 240")
    st = await hook(ed, frame_rate=30)
    ed.check_eq(st["frame_rate"], 30, "Samples")
    ed.check("between frames" in st["notice"], f"keys off the new grid are named: {st['notice']}")
    st = await hook(ed, time_format="seconds")
    ed.check_eq(st["time_format"], "seconds", "Seconds display")
    st = await hook(ed, time_format="frames", state="hero_clip")
    ed.check_eq(st["clip"], CLIP, "back to hero_clip")


async def test_shortcuts_while_focused(ed: Editor):
    await hook(ed, time=0.0)
    await focus_keys(ed)
    await key(ed, "period")
    await key(ed, "period")
    st = await state(ed)
    ed.check_eq(st["frame"], 2, f". steps one frame each: {st['frame']}")
    await key(ed, "comma")
    ed.check_eq((await state(ed))["frame"], 1, ", steps back")
    await key(ed, "alt+period")
    st = await state(ed)
    keys = sorted(set(sum([r.get("keys", []) for r in st["rows"]], [])))
    later = [k for k in keys if k > 1 / 30 + 1e-4]
    ed.check(later and abs(st["time"] - later[0]) < 1e-4, f"Alt+. goes to the next key: {st['time']} {keys}")
    await key(ed, "alt+comma")
    st = await state(ed)
    ed.check(abs(st["time"]) < 1e-4, f"Alt+, back to the previous key (0): {st['time']}")
    await key(ed, "space")
    await ed.call("wait", seconds=0.4)
    st = await state(ed)
    ed.check(st["playing"] and st["preview"], f"Space plays the preview: {st['playing']}")
    await focus_keys(ed)
    await key(ed, "space")
    st = await hook(ed, preview=False)
    ed.check(not st["playing"], "Space pauses")
    ed.check(close(await position(ed, "Hero"), S["rest_hero"]), "stopping the preview restores Hero")
    ed.check(not await modified(ed), "no scene change")


async def test_large_clip_stays_responsive(ed: Editor):
    """I1: a 60 x 600 key clip, every key selected: the clip mode draws in a few ms, a drag is one step."""
    paths = [p for p in (await hook(ed, list_paths=True))["paths"] if p.startswith("Hero/")][:20]
    tracks = []
    for path in paths:
        for channel, value in (("position", [0, 0, 0]), ("rotation", [0, 0, 0, 1]), ("scale", [1, 1, 1])):
            tracks.append({"path": path, "channel": channel, "keys": [[round(i / 60, 6), value] for i in range(600)]})
    clip = {"format": "jasperengine.anim", "version": 1, "frame_rate": 60, "wrap": "loop", "tracks": tracks, "events": []}
    (ed.project / "assets/anim/big.anim").write_text(json.dumps(clip) + "\n")
    small = await hook(ed, brief=True)
    await ed.call("wait", seconds=1.0)
    small_after = await hook(ed, brief=True)
    small_fps = (small_after["frame_count"] - small["frame_count"])
    st = await hook(ed, clip="assets/anim/big.anim", root="Hero", brief=True)
    st = await hook(ed, box={"rows": [0, 0], "times": [0, 11]}, brief=True)
    ed.check_eq(st["selection_count"], 36000, f"every key selected: {st['selection_count']}")
    before = st["frame_count"]
    await ed.call("wait", seconds=1.0)
    st = await hook(ed, brief=True)
    big_fps = st["frame_count"] - before
    ed.check(st["draw_ms"] < 30.0, f"the clip mode draws in {st['draw_ms']:.1f} ms (moving average)")
    ed.check(big_fps >= max(2, small_fps // 2), f"the editor keeps its frame rate: {big_fps} vs {small_fps} frames/s")
    await shot(ed, "07-large-clip")
    # a drag of every key by one frame is one undo step
    st = await hook(ed, drag=1 / 60, brief=True)
    data = json.loads((ed.project / "assets/anim/big.anim").read_text())
    ed.check(abs(data["tracks"][0]["keys"][0][0] - 1 / 60) < 1e-4, f"moved by a frame: {data['tracks'][0]['keys'][0]}")
    await ed.call("undo", scope="project")
    data = json.loads((ed.project / "assets/anim/big.anim").read_text())
    ed.check(abs(data["tracks"][0]["keys"][0][0]) < 1e-6, "one undo step")
    await hook(ed, state="hero_clip")


async def test_read_only_subclip_and_extract(ed: Editor):
    st = await hook(ed, entity=None, clip="assets/models/hero_animated.gltf#Walk")
    ed.check_eq(st["mode"], "clip", "the Project window's double-click opens clip mode")
    ed.check(st["read_only"] and st["targetless"], f"a read-only target-less sub-clip: {st}")
    ed.check(len(st["rows"]) > 0, "its tracks show")
    ed.check("hero_animated.gltf" in st["preview_on"], f"it previews on its own model: {st['preview_on']}")
    ed.check("No instance of hero_animated.gltf" in st["preview_blocker"], f"Preview says why it can't: {st['preview_blocker']}")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"add_event": {"time": 0.1}}}, contains="Extract")
    # Preview on the Hero (same rig): a session override, nothing written
    st = await hook(ed, preview_model="models/hero_tpose.gltf")
    ed.check_eq((st["preview_on"], st["preview_blocker"]), ("models/hero_tpose.gltf", ""), f"previewable on Hero: {st}")
    st = await hook(ed, time=0.3, preview=True)
    ed.check(st["preview"] and st["preview_root"] == "Hero", f"the read-only clip previews on Hero: {st}")
    await shot(ed, "06-read-only")
    st = await hook(ed, preview=False)
    st = await hook(ed, extract="assets/anim/walk.anim")
    ed.check_eq(st["clip"], "assets/anim/walk.anim", f"Extract opens the new file: {st}")
    ed.check(not st["read_only"], "editable")
    ed.check((ed.project / "assets/anim/walk.anim").is_file(), "written")
    ed.check("hero_animated.gltf" in st["preview_model"], f"Preview on the model: {st['preview_model']}")
    st = await hook(ed, preview_model="models/hero_tpose.gltf")
    ed.check_eq(st["preview_model"], "models/hero_tpose.gltf", "Preview on (written into the editable clip)")
    ed.check(not await modified(ed), "no scene change")


async def test_extract_through_the_players_state(ed: Editor):
    """Extract of a player's sub-clip state switches the state to the file in one scene step."""
    walker = (await ed.call("create_entity", name="Walker", type="cube", position=[3, 0, 0]))["entity_id"]
    await ed.call("add_component", entity=walker, component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": "models/hero_animated.gltf#Walk"}]}})
    st = await hook(ed, entity="Walker")
    ed.check(st["read_only"] and st["state"] == "Walk", f"the player's sub-clip state: {st}")
    st = await hook(ed, extract="assets/anim/walker_walk.anim")
    ed.check_eq((st["clip"], st["state"], st["read_only"]), ("assets/anim/walker_walk.anim", "Walk", False), f"switched: {st}")
    ed.check("walker_walk.anim" in json.dumps(await ed.call("get_entity", entity="Walker", detail="full")), "the player plays the file")
    await ed.call("undo", scope="scene")
    ed.check("hero_animated.gltf#Walk" in json.dumps(await ed.call("get_entity", entity="Walker", detail="full")),
             "undo puts the sub-clip back")


async def test_entity_mode_lanes_and_previews(ed: Editor):
    # an entity Animation with two tweens starting together (both on track 0): the window lays them on
    # two lanes for display only (R31); the saved tracks never change unless a frame is moved
    await ed.call("edit_animation", target="Robot", name="RobotTweens", tweens=[
        {"type": "position", "to": [0.75, 1, 0], "duration": 1.0},
        {"type": "scale", "to": [1.5, 1.5, 1.5], "duration": 1.0}])
    # both frames on one stored track, overlapping (as engine-created frames are)
    await ed.call("edit_animation", animation="RobotTweens", set_frames=[{"index": 1, "lane": 0}])
    await ed.call("save_scene", scene="Main Scene")
    tracks = saved_tracks((ed.project / SCENE_FILE).read_text(encoding="utf-8"), "RobotTweens")
    ed.check_eq(tracks, [0, 0], f"both frames stored on track 0: {tracks}")
    st = await hook(ed, entity="RobotTweens")
    ed.check_eq((st["mode"], st["entity_animation"]), ("entity", "RobotTweens"), f"entity mode: {st}")
    ed.check_eq((st["entity_lanes"], st["entity_tracks"]), ([0, 1], [0, 0]), f"shown on two lanes, stored on one: {st}")
    await ed.call("wait", seconds=0.5)
    ed.check(not await modified(ed), "the lanes are view only: the scene is not modified")
    await ed.call("save_scene", scene="Main Scene")
    ed.check_eq(saved_tracks((ed.project / SCENE_FILE).read_text(encoding="utf-8"), "RobotTweens"), [0, 0], "saved tracks unchanged")
    # an explicit move to lane 2 writes that frame's track (and the lanes shown), one scene step
    st = await hook(ed, entity_move_frame={"index": 1, "lane": 2})
    ed.check_eq((st["entity_tracks"], st["entity_lanes"]), ([0, 2], [0, 2]), f"moved where dropped: {st}")
    await ed.call("undo", scope="scene")
    ed.check_eq((await hook(ed, entity_animation="RobotTweens"))["entity_tracks"], [0, 0], "one undo step")
    # the selection's clip entries in the entity dropdown (Robot has no player)
    await ed.call("select_entities", entities=["Robot"])
    st = await hook(ed, entity_mode_entries=True)
    ed.check_eq(st["mode"], "entity", f"the target of an entity Animation: entity mode: {st}")
    ed.check_eq(st["entity_mode_entries"], ["Create New Clip..."], f"its dropdown's clip entries: {st}")
    # R6: a save during the entity preview writes the unpreviewed Robot
    rest = await position(ed, "Robot")
    st = await hook(ed, entity_animation="RobotTweens", entity_play=True)
    await ed.call("wait", seconds=0.5)
    ed.check(not close(await position(ed, "Robot"), rest), "the entity preview moves Robot")
    # (save_scene runs the editor's save: App::stopTransientPreviews first; Ctrl+S asks where to save the
    # fixture's copied project)
    await ed.call("save_scene", scene="Main Scene")
    saved = saved_position((ed.project / SCENE_FILE).read_text(encoding="utf-8"), "Robot")
    ed.check(close(saved, rest), f"the save wrote the unpreviewed Robot: {saved} vs {rest}")
    ed.check_eq((await hook(ed, entity_animation="RobotTweens"))["entity_preview"], False, "the save stopped the entity preview")
    # R7: a clip preview of the same hierarchy stops the entity preview
    await ed.call("add_component", entity="Robot", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": "assets/anim/walk.anim"}]}})
    st = await hook(ed, entity_animation="RobotTweens", entity_play=True)
    await ed.call("wait", seconds=0.3)
    ed.check((await hook(ed, entity_animation="RobotTweens"))["entity_preview"], "entity preview running")
    await ed.call("preview_animation", animation="Robot", action="seek", time=0.2)
    st = await hook(ed, entity_animation="RobotTweens")
    ed.check(not st["entity_preview"], f"the clip preview of Robot stopped the entity preview (PreviewOwner): {st}")
    await ed.call("preview_animation", animation="Robot", action="stop")
    await ed.call("undo", scope="scene")
    # back to the Hero: clip mode on its player
    st = await hook(ed, entity="Hero")
    ed.check_eq((st["mode"], st["clip"]), ("clip", CLIP), f"clip mode again: {st}")
    await shot(ed, "08-clip-mode-final")


async def test_drop_clip_on_the_window(ed: Editor):
    """Spec 4.1: a .anim dropped on the Animation window opens it for the bound GameObject: added to its
    Animation Player (one scene step; a player is added when it has none, a listed clip is reused), or
    target-less when nothing is bound; other files are refused."""
    dropper = (await ed.call("create_entity", name="Dropper", type="cube", position=[-3, 0, 0]))["entity_id"]
    await ed.call("save_scene", scene="Main Scene")
    st = await hook(ed, entity="Dropper")
    ed.check(st["empty_state"], f"the empty state of Dropper: {st}")
    st = await hook(ed, drop=["assets/anim/walk.anim"])
    ed.check_eq((st["mode"], st["root"], st["clip"], st["state"]), ("clip", "Dropper", "assets/anim/walk.anim", "walk"),
                f"the drop opens the clip as the new player's state: {st}")
    ed.check(st["has_player"] and st["states"] == ["walk"], f"a player was added listing it: {st}")
    ed.check(await modified(ed), "a scene step")
    await ed.call("undo", scope="scene")
    entity = json.dumps(await ed.call("get_entity", entity=dropper, detail="full"))
    ed.check("animationplayer" not in entity.lower().replace(" ", ""), f"one undo step: the player is gone: {entity}")
    # a player with clips: appended (a state of its own), and the same file again reuses its entry
    st = await hook(ed, entity="Walker", drop=[str(ed.project / "assets/anim/walk.anim")])
    ed.check_eq((st["root"], st["state"]), ("Walker", "walk"), f"opened as Walker's new state (an absolute path): {st}")
    ed.check_eq(st["states"], ["Walk", "walk"], "appended after the player's own clip")
    st = await hook(ed, drop=["assets/anim/walk.anim"])
    ed.check_eq((st["states"], st["state"]), (["Walk", "walk"], "walk"), f"a listed clip is reused: {st}")
    await ed.call("undo", scope="scene")
    st = await hook(ed, entity="Walker")
    ed.check_eq(st["states"], ["Walk"], f"the second drop made no step of its own: {st}")
    # nothing bound (a clip open target-less): target-less too (the file is edited, nothing is added)
    st = await hook(ed, entity=None, clip="assets/models/hero_animated.gltf#Walk")
    ed.check(st["targetless"], f"nothing bound: {st}")
    st = await hook(ed, drop=["assets/anim/walk.anim"])
    ed.check(st["targetless"] and st["clip"] == "assets/anim/walk.anim", f"target-less: {st}")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"drop": ["Main Scene.scene"]}}, contains=".anim")
    await ed.call("delete_entities", entities=[dropper])
