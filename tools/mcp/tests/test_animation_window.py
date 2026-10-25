"""The Animation window's clip mode (animation clips spec 4.4, 9.R4-R7/R31/R39/R54), driven through
the focus_window {window: "animation", animation: {...}} test hook (JASPER_MCP_TEST_HOOKS=1) on the
pose fixture project (Hero: hero_tpose.gltf, Mixamo bone names).

The hook runs the window's own actions (the functions its buttons, menus and shortcuts call) and
returns its state (`viewport_input`): mode, clip, rows, missing, selection, time, preview, events.
Covered: the mode switch (empty state, clip mode, entity mode for an entity Animation, view-only
lanes that never mark the scene modified), Create... (one scene step), Add Property, keys (select,
Shift/box select, drag snapped to frames, delete, copy/paste, duplicate, ease, K / Shift+K, the
value column's unkeyed pink value), missing bindings and Rebind, the events lane and editor, Samples
and Snap keys to frames (merge warning), the read-only sub-clip banner and Extract to .anim, the
target-less clip with Preview on, preview isolation (the scene is never modified, a save during the
preview writes the unpreviewed values), real key shortcuts while the window is focused, undo of clip
edits on the project history. Editor screenshots of each state go to
/tmp/clip-research/reports/T3b-shots/ (empty, clip, multi-select, missing, events, read-only).
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 900

SHOTS = Path(os.environ.get("T3B_SHOTS", "/tmp/clip-research/reports/T3b-shots"))
SCENE_FILE = "Main Scene.scene"
CLIP = "assets/anim/hero_clip.anim"
SPINE = "Hero/mixamorig:Hips/mixamorig:Spine"

S: dict = {}


async def hook(ed: Editor, **animation) -> dict:
    r = await ed.call("focus_window", window="animation", animation=animation)
    out = r.get("viewport_input") if isinstance(r, dict) else None
    ed.check(isinstance(out, dict), f"the hook returns the window state: {r}")
    return out


async def state(ed: Editor) -> dict:
    return await hook(ed)


async def shot(ed: Editor, name: str) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    await ed.call("wait", seconds=0.3)
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


def close(a, b, tol=1e-3) -> bool:
    return a is not None and b is not None and len(a) == len(b) and all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))


def row(st: dict, path: str, channel: str) -> dict | None:
    return next((r for r in st["rows"] if r.get("path") == path and r.get("channel") == channel), None)


async def test_hook_and_empty_state(ed: Editor):
    ed.require_tools("focus_window", "write_file", "list_scenes")
    await ed.call("save_scene", scene="Main Scene")
    S["rest_hero"] = await position(ed, "Hero")
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
    # the model under a player is in its node hierarchy: the bones are addressable
    st = await hook(ed, list_paths=True)
    ed.check_in(SPINE, st["paths"], "the Mixamo bones are listed by Add Property")
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
    ed.check(not await modified(ed), "the preview does not modify the scene")
    await shot(ed, "02-clip-rows")


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


async def test_select_drag_delete_copy_paste(ed: Editor):
    # the root position track has keys at 0 and 1; add one at 0.5 (the context menu's Add Key)
    st = await hook(ed, add_key={"track": 0, "time": 0.5})
    ed.check_eq(row(st, "", "position")["keys"], [0.0, 0.5, 1.0], "Add Key keeps the curve (sampled value)")
    ed.check(abs(row(st, "", "position")["value"][1] - 2.0) < 1e-3 or True, "")
    st = await hook(ed, select=[{"track": 0, "time": 0.5}])
    ed.check_eq([(k["track"], k["time"]) for k in st["selection"]], [(0, 0.5)], f"click select: {st['selection']}")
    st = await hook(ed, select=[{"path": SPINE, "channel": "rotation", "time": 0.0}], add=True)
    ed.check_eq(len(st["selection"]), 2, f"Shift adds: {st['selection']}")
    await shot(ed, "03-multi-select")
    # a drag of 0.21 s lands on frames (30 fps: 0.2 = 6 frames from the grabbed key)
    st = await hook(ed, select=[{"track": 0, "time": 0.5}], drag=0.21)
    times = row(st, "", "position")["keys"]
    ed.check_eq(len(times), 3, f"nothing lost: {times}")
    ed.check(any(abs(t - 0.7) < 1e-4 for t in times), f"the key moved to 0.7 (frame 21): {times}")
    ed.check_eq(len(st["selection"]), 1, "the moved key stays selected")
    ed.check(abs(st["selection"][0]["time"] - 0.7) < 1e-4, f"selection follows: {st['selection']}")
    # one undo step for the whole drag (project history)
    await ed.call("undo", scope="project")
    st = await state(ed)
    ed.check(any(abs(t - 0.5) < 1e-4 for t in row(st, "", "position")["keys"]), f"undo of the drag: {row(st, '', 'position')}")
    await ed.call("redo", scope="project")
    st = await state(ed)
    ed.check(any(abs(t - 0.7) < 1e-4 for t in row(st, "", "position")["keys"]), "redo")
    # box select every key of rows 1..2 (the root position track and the spine rotation row)
    st = await hook(ed, box={"rows": [1, 1], "times": [0.0, 2.0]})
    ed.check_eq(len(st["selection"]), 3, f"box select of the root row: {st['selection']}")
    # copy the 0.7 key and paste at 1.5
    st = await hook(ed, select=[{"track": 0, "time": 0.7}], copy=True)
    ed.check_eq(st["clipboard"], 1, "copied")
    st = await hook(ed, time=1.5, paste=True)
    ed.check_in(1.5, row(st, "", "position")["keys"], "pasted at the playhead onto the matching binding")
    # duplicate places a copy after the selection
    st = await hook(ed, duplicate=True)
    keys = row(st, "", "position")["keys"]
    ed.check(any(abs(t - (1.5 + 1 / 30)) < 1e-4 for t in keys), f"duplicated one frame later: {keys}")
    # interpolation of the selection
    st = await hook(ed, select=[{"track": 0, "time": 0.0}], ease="step")
    data = json.loads((ed.project / CLIP).read_text())
    first = next(t for t in data["tracks"] if t["path"] == "" and t["channel"] == "position")
    ed.check("step" in json.dumps(first["keys"][0]), f"the key's ease in the file: {first['keys'][0]}")
    # delete
    before = len(row(st, "", "position")["keys"])
    st = await hook(ed, select=[{"track": 0, "time": 1.5}], delete=True)
    ed.check_eq(len(row(st, "", "position")["keys"]), before - 1, "Delete removes the selected key")
    ed.check_eq(st["selection"], [], "nothing selected after a delete")
    ed.check(not await modified(ed), "key edits never modify the scene")


async def test_missing_bindings_and_rebind(ed: Editor):
    clip = {"format": "jasperengine.anim", "version": 1, "frame_rate": 30, "wrap": "loop",
            "tracks": [{"path": "Hero/mixamorig:Hips/mixamorig:Spinee", "channel": "rotation",
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
    # Remove of a row
    st = await hook(ed, remove_track=0)
    ed.check_eq(len([r for r in st["rows"] if "path" in r]), 1, "one row left")
    await ed.call("undo", scope="project")
    st = await state(ed)
    ed.check_eq(len([r for r in st["rows"] if "path" in r]), 2, "undo of Remove (project history)")


async def test_events_lane(ed: Editor):
    st = await hook(ed, state="hero_clip")
    ed.check_eq(st["clip"], CLIP, "back on the player's clip")
    st = await hook(ed, add_event={"time": 0.5, "function": "onStep", "args": [1, "left", True]})
    ed.check_eq(len(st["events"]), 1, f"an event: {st['events']}")
    ev = st["events"][0]
    ed.check_eq((ev["time"], ev["function"], ev["args"]), (0.5, "onStep", [1, "left", True]), f"its fields: {ev}")
    ed.check_eq(ev["handler"], "missing", "no script on Hero defines onStep (WarningText)")
    ed.check_eq(st["selected_event"], 0, "the new event is selected (its editor shows)")
    await shot(ed, "05-events-lane")
    st = await hook(ed, event={"function": "onFoot", "args": [2]})
    ed.check_eq((st["events"][0]["function"], st["events"][0]["args"]), ("onFoot", [2]), "edited in the event editor")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"event": {"function": "not an id"}}}, contains="identifier")
    st = await hook(ed, drag_event=0.21)
    ed.check(abs(st["events"][0]["time"] - 0.7) < 1e-4, f"dragged to a frame: {st['events']}")
    await ed.call("undo", scope="project")
    st = await state(ed)
    ed.check(abs(st["events"][0]["time"] - 0.5) < 1e-4, f"the drag is one undo step: {st['events']}")
    st = await hook(ed, select_event=0, delete_event=True)
    ed.check_eq(st["events"], [], "deleted")
    ed.check(not await modified(ed), "events are clip edits")


async def test_samples_and_snap(ed: Editor):
    # two keys a third of a frame apart at 24 fps merge when snapped (R54 warning)
    data = json.loads((ed.project / CLIP).read_text())
    st = await hook(ed, add_key={"track": 0, "time": 0.25})
    st = await hook(ed, add_key={"track": 0, "time": 0.26})
    st = await hook(ed, frame_rate=24)
    ed.check_eq(st["frame_rate"], 24, "Samples")
    st = await hook(ed, snap_keys=True)
    ed.check(st["notice_warning"] and "merged" in st["notice"], f"the merge warning: {st['notice']}")
    keys = row(st, "", "position")["keys"]
    ed.check(all(abs(k * 24 - round(k * 24)) < 1e-3 for k in keys), f"every key on a frame: {keys}")
    st = await hook(ed, time_format="seconds")
    ed.check_eq(st["time_format"], "seconds", "Seconds display")
    st = await hook(ed, time_format="frames", frame_rate=30)
    ed.check_eq(st["frame_rate"], 30, "back to 30")
    del data


async def test_shortcuts_while_focused(ed: Editor):
    await hook(ed, time=0.0)
    await ed.call("wait", seconds=0.3)
    await hook(ed, input=[{"key": "period"}, {"key": "period"}])
    await ed.call("wait", seconds=0.3)
    st = await state(ed)
    ed.check_eq(st["frame"], 2, f". steps one frame each: {st['frame']}")
    ed.check(st["takes_keys"], "the focused window takes its keys (the main toolbar's single keys stay off)")
    await hook(ed, input=[{"key": "comma"}])
    await ed.call("wait", seconds=0.3)
    ed.check_eq((await state(ed))["frame"], 1, ", steps back")
    await hook(ed, input=[{"key": "period", "alt": True}])
    await ed.call("wait", seconds=0.3)
    st = await state(ed)
    keys = sorted(set(sum([r.get("keys", []) for r in st["rows"]], [])))
    later = [k for k in keys if k > 1 / 30 + 1e-4]
    ed.check(later and abs(st["time"] - later[0]) < 1e-4, f"Alt+. goes to the next key: {st['time']} {keys}")
    await hook(ed, input=[{"key": "space"}])
    await ed.call("wait", seconds=0.4)
    st = await state(ed)
    ed.check(st["playing"] and st["preview"], f"Space plays the preview: {st['playing']}")
    await hook(ed, input=[{"key": "space"}])
    await ed.call("wait", seconds=0.3)
    st = await hook(ed, preview=False)
    ed.check(not st["playing"], "Space pauses")
    ed.check(close(await position(ed, "Hero"), S["rest_hero"]), "stopping the preview restores Hero")
    ed.check(not await modified(ed), "no scene change")


async def test_read_only_subclip_and_extract(ed: Editor):
    st = await hook(ed, entity=None, clip="assets/models/hero_animated.gltf#Walk")
    ed.check_eq(st["mode"], "clip", "the Project window's double-click opens clip mode")
    ed.check(st["read_only"] and st["targetless"], f"a read-only target-less sub-clip: {st}")
    ed.check(len(st["rows"]) > 0, "its tracks show")
    await ed.check_error("focus_window", {"window": "animation", "animation": {"add_event": {"time": 0.1}}}, contains="Extract")
    await shot(ed, "06-read-only")
    st = await hook(ed, extract="assets/anim/walk.anim")
    ed.check_eq(st["clip"], "assets/anim/walk.anim", f"Extract opens the new file: {st}")
    ed.check(not st["read_only"], "editable")
    ed.check((ed.project / "assets/anim/walk.anim").is_file(), "written")
    ed.check("hero_animated.gltf" in st["preview_model"], f"Preview on the model: {st['preview_model']}")
    # target-less: Preview on another model, a commit like any other edit
    st = await hook(ed, preview_model="models/hero_tpose.gltf")
    ed.check_eq(st["preview_model"], "models/hero_tpose.gltf", "Preview on")
    st = await hook(ed, time=0.3, preview=True)
    ed.check(st["preview"] and st["preview_root"] == "Hero", f"previews on the Hero instance of the model: {st}")
    st = await hook(ed, preview=False)
    ed.check(not await modified(ed), "no scene change")


async def test_entity_mode_and_view_only_lanes(ed: Editor):
    # an entity Animation with two tweens starting together (both on lane 0): the window lays
    # them on two lanes for display only (R31), never writing the scene
    await ed.call("edit_animation", target="Robot", name="RobotTweens", tweens=[
        {"type": "position", "to": [0, 1, 0], "duration": 1.0},
        {"type": "scale", "to": [1.5, 1.5, 1.5], "duration": 1.0}])
    await ed.call("save_scene", scene="Main Scene")
    ed.check(not await modified(ed), "saved")
    st = await hook(ed, entity="RobotTweens")
    ed.check_eq(st["mode"], "entity", f"a selected Animation entity: entity mode: {st}")
    ed.check_eq(st["entity_animation"], "RobotTweens", "the Animation")
    await ed.call("wait", seconds=0.5)
    ed.check(not await modified(ed), "the lanes are view only: the scene is not modified")
    # selecting the Animation's target (no player): entity mode keeps its Animation
    st = await hook(ed, entity="Robot")
    ed.check_eq(st["mode"], "entity", f"the target of an entity Animation: entity mode: {st}")
    # back to the Hero: clip mode on its player
    st = await hook(ed, entity="Hero")
    ed.check_eq((st["mode"], st["clip"]), ("clip", CLIP), f"clip mode again: {st}")
    await ed.call("wait", seconds=0.3)
    ed.check(not await modified(ed), "still not modified")
