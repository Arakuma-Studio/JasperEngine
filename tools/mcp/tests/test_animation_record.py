"""The Animation window's Record mode and curves view (animation clips spec 4.4, 9.R5/R5b/R6/R50), on
the pose fixture project (Hero: hero_tpose.gltf, Mixamo bones), driven through the focus_window
{window: "animation", animation: {...}} test hook (JASPER_MCP_TEST_HOOKS=1) and real X key presses
(xdotool on the harness' Xvfb) where the App's key routing matters.

The record sources run through the editor's own code: `gizmo` through SceneRender's gizmo drag path
(recordGizmoTransform, else the ObjectTransformCmd the drag builds), `inspector` / `morph` through
the Inspector's Transform / Blend Shapes row functions, `lattice_drag` through LatticeEditorOps'
continuous edit (the Scene view's lattice point drag). Covered: Record on/off (R, the toolbar
state, the red status), gizmo / Inspector / morph / lattice edits of the bound hierarchy keyed at the
playhead with no scene step and one undo step per gesture, real Ctrl+Z from the Scene view going to
the project history while a recorded key is the newest step (R5b), a multi-selection with an unbound
GameObject (only the bound one keyed, the other unchanged, the status says so), unbound edits and MCP
set_properties staying scene steps, leaving Record restoring the scene; the curves view (curves of
every component with the runtime sampler's values, rotation as continuous Euler degrees, a vertical
key drag changing the value in one undo step, a time drag landing on frames, a value drag of keys of
two properties moving time only, visibility toggles, framing). Screenshots go to
/tmp/clip-research/reports/T3c-shots/.
"""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
from pathlib import Path

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness"}
TIMEOUT = 1200

SHOTS = Path(os.environ.get("T3C_SHOTS", "/tmp/clip-research/reports/T3c-shots/round1"))
CLIP = "assets/anim/rec.anim"
# a bone under the Hero root (the clip path) and its GameObject path in the scene
SPINE = "Hero/mixamorig:Hips/mixamorig:Spine"
SPINE_ENTITY = "Hero/" + SPINE
MORPH_MODEL = harness.REPO / "tools" / "fixtures" / "runtime-shaders" / "assets" / "skinned_morph.gltf"

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


async def transform(ed: Editor, entity: str, what: str = "position") -> list:
    value = harness.find_key(await ed.call("get_entity", entity=entity, detail="full"), what)
    if isinstance(value, dict):
        value = value.get("value", value)
        # rotations: {"quat": [x, y, z, w], "euler": [...]}
        value = value.get("quat", value) if isinstance(value, dict) else value
    return [float(v) for v in value]


def close(a, b, tol=1e-3) -> bool:
    return a is not None and b is not None and len(a) == len(b) and all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))


def same_rotation(a, b, tol=1e-3) -> bool:
    return close(a, b, tol) or close([-v for v in a], b, tol)


def euler_xyzw(x: float, y: float, z: float) -> list:
    """The clip's Euler convention (anim::eulerDegreesToQuaternion, ZYX: q = qz * qy * qx)."""
    hx, hy, hz = (math.radians(v) / 2 for v in (x, y, z))
    cx, sx, cy, sy, cz, sz = math.cos(hx), math.sin(hx), math.cos(hy), math.sin(hy), math.cos(hz), math.sin(hz)
    return [sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz]


async def morph_weight(ed: Editor, entity: str, index: int = 0) -> float:
    """MeshComponent morphWeights[index] (a Catalog property) of a GameObject path."""
    got = await ed.call("get_entity", entity=entity, components=["mesh"], properties=[f"morphWeights[{index}]"])
    value = harness.find_key(got, f"morphWeights[{index}]")
    if isinstance(value, dict):
        value = value.get("value", value)
    return float(value)


def row(st: dict, path: str, channel: str) -> dict | None:
    return next((r for r in st["rows"] if r.get("path") == path and r.get("channel") == channel), None)


def keys_of(st: dict, path: str, channel: str) -> list:
    r = row(st, path, channel)
    return [round(t, 4) for t in r["keys"]] if r else []


def curve(st: dict, track: int, component: int) -> dict | None:
    return next((c for c in st.get("curves", []) if c["track"] == track and c["component"] == component), None)


def run_x(ed: Editor, *args: str) -> bytes:
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(list(args), env=env, check=True, capture_output=True).stdout


async def focus_x(ed: Editor, window: str) -> None:
    """`window` focused in ImGui and the editor's X window focused for xdotool."""
    if not shutil.which("xdotool"):
        harness.skip("xdotool is not installed")
    await ed.call("focus_window", window=window)
    ids = run_x(ed, "xdotool", "search", "--onlyvisible", "--name", "JasperEngine").split()
    ed.check(ids, "the editor's X window")
    run_x(ed, "xdotool", "windowfocus", "--sync", ids[-1].decode())
    await ed.call("wait", frames=6)


async def key(ed: Editor, combo: str) -> None:
    *modifiers, last = combo.split("+")
    for modifier in modifiers:
        run_x(ed, "xdotool", "keydown", "--clearmodifiers", modifier)
    run_x(ed, "xdotool", "keydown", last)
    run_x(ed, "xdotool", "keyup", last)
    for modifier in reversed(modifiers):
        run_x(ed, "xdotool", "keyup", modifier)
    await ed.call("wait", frames=8)


# ---------------------------------------------------------------------------- setup

async def test_setup_clip(ed: Editor):
    ed.require_tools("focus_window", "list_scenes", "get_entity", "set_properties", "create_entity", "add_component")
    await ed.call("save_scene", scene="Main Scene")
    S["rest_hero"] = await transform(ed, "Hero")
    S["rest_robot"] = await transform(ed, "Robot")
    st = await hook(ed, entity="Hero", create_clip=CLIP)
    ed.check_eq(st["clip"], CLIP, f"the clip opened: {st}")
    st = await hook(ed, add_property={"path": "", "channel": "position"})
    ed.check_eq(keys_of(st, "", "position"), [0.0], "a root position track with a key at 0")
    S["rest_spine"] = await transform(ed, SPINE_ENTITY, "rotation")
    ed.check(not st["recording"], "Record is off")
    await ed.call("save_scene", scene="Main Scene")
    S["saved_scene"] = (ed.project / "Main Scene.scene").read_text(encoding="utf-8")


# ---------------------------------------------------------------------------- Record

async def test_record_gizmo_move(ed: Editor):
    st = await hook(ed, frame=15, record=True)
    ed.check(st["recording"] and st["preview"], f"Record on previews: {st['recording']} {st['preview']}")
    ed.check("Recording" in st["record_status"], f"the red status: {st['record_status']}")
    target = [S["rest_hero"][0], S["rest_hero"][1] + 1.0, S["rest_hero"][2]]
    st = await hook(ed, gizmo={"entities": ["Hero"], "tool": "move", "value": target})
    ed.check_eq(keys_of(st, "", "position"), [0.0, 0.5], "the gizmo drag keyed the root at the playhead")
    ed.check(close(row(st, "", "position")["value"], target), f"the key holds the dragged value: {row(st, '', 'position')}")
    ed.check(close(await transform(ed, "Hero"), target), "the live value shows")
    ed.check(not await modified(ed), "no scene step")
    ed.check(st["undo_to_project"], "a recorded key is the newest step")
    # one undo step for the drag (4 steps through the gizmo path, one merge key)
    await ed.call("undo", scope="project")
    st = await state(ed)
    ed.check_eq(keys_of(st, "", "position"), [0.0], "one undo step removed the drag's key")
    await ed.call("redo", scope="project")
    ed.check_eq(keys_of(await state(ed), "", "position"), [0.0, 0.5], "redo")


async def test_record_undo_from_the_scene_view(ed: Editor):
    """R5b: with the Scene view focused, real Ctrl+Z goes to the project history while a recorded key
    is the newest step; Record never takes a scene step's undo."""
    await focus_x(ed, "scene")
    await key(ed, "ctrl+z")
    st = await state(ed)
    ed.check_eq(keys_of(st, "", "position"), [0.0], "Ctrl+Z in the Scene view undid the recorded key")
    ed.check(not await modified(ed), "the scene was not touched")
    await focus_x(ed, "scene")
    await key(ed, "ctrl+y")
    ed.check_eq(keys_of(await state(ed), "", "position"), [0.0, 0.5], "Ctrl+Y redid it")


async def test_record_rotation_and_inspector_rows(ed: Editor):
    st = await hook(ed, gizmo={"entities": [SPINE_ENTITY], "tool": "rotate", "value": [0, 0, 40]})
    r = row(st, SPINE, "rotation")
    ed.check(r is not None, f"the rotate gizmo added the bone's rotation track: {[x.get('label') for x in st['rows']]}")
    ed.check_eq(keys_of(st, SPINE, "rotation"), [0.5], "keyed at the playhead")
    ed.check(same_rotation(r["value"], euler_xyzw(0, 0, 40)), f"40 degrees about z: {r['value']}")
    # an Inspector Rotation row edit (Euler degrees) re-keys it
    st = await hook(ed, inspector={"entities": [SPINE_ENTITY], "property": "rotation", "value": [20, 0, 0]})
    ed.check(same_rotation(row(st, SPINE, "rotation")["value"], euler_xyzw(20, 0, 0)), f"the Inspector row: {row(st, SPINE, 'rotation')}")
    ed.check(same_rotation(await transform(ed, SPINE_ENTITY, "rotation"), euler_xyzw(20, 0, 0)), "the live bone")
    # an Inspector Position row edit of the root
    target = [S["rest_hero"][0], S["rest_hero"][1] + 1.5, S["rest_hero"][2]]
    st = await hook(ed, inspector={"entities": ["Hero"], "property": "position", "value": target})
    ed.check(close(row(st, "", "position")["value"], target), f"the root key took the row value: {row(st, '', 'position')}")
    ed.check(not await modified(ed), "Inspector edits under Record are not scene steps")
    # each edit is one undo step: undo the position edit, the rotation stays
    await ed.call("undo", scope="project")
    st = await state(ed)
    ed.check(close(row(st, "", "position")["value"], [S["rest_hero"][0], S["rest_hero"][1] + 1.0, S["rest_hero"][2]]), "the position edit undone")
    ed.check(same_rotation(row(st, SPINE, "rotation")["value"], euler_xyzw(20, 0, 0)), "the rotation edit stays")
    await shot(ed, "02-record-dopesheet")


async def test_multi_selection_and_unclaimed_edits(ed: Editor):
    before = row(await state(ed), "", "position")["value"]
    target = [S["rest_hero"][0] + 0.5, S["rest_hero"][1] + 1.0, S["rest_hero"][2]]
    st = await hook(ed, gizmo={"entities": ["Hero", "Robot"], "tool": "move", "value": target})
    ed.check(close(row(st, "", "position")["value"], target), f"Hero keyed: {row(st, '', 'position')} (was {before})")
    ed.check(close(await transform(ed, "Robot"), S["rest_robot"]), "Robot, not under Hero, stays as it is")
    ed.check("not under Hero" in st["record_status"], f"the status says so: {st['record_status']}")
    ed.check("Rec frame 15" in st["record_status_short"] and "1 not keyed" in st["record_status_short"],
             f"the short status: {st['record_status_short']}")
    ed.check(not await modified(ed), "no scene step")
    # Robot alone selected: the window keeps its binding while recording (Unity's lock); Record
    # does not claim Robot, the drag is a scene step
    moved = [S["rest_robot"][0] + 0.3, S["rest_robot"][1], S["rest_robot"][2]]
    st = await hook(ed, gizmo={"entities": ["Robot"], "tool": "move", "value": moved})
    await ed.call("wait", frames=4)
    st = await state(ed)
    ed.check(st["recording"] and st["root"] == "Hero", f"still recording Hero with Robot selected: {st['recording']} {st['root']}")
    ed.check(close(await transform(ed, "Robot"), moved), "Robot moved")
    ed.check(await modified(ed), "a scene step")
    ed.check(not st["undo_to_project"], "the scene step is the newest: Ctrl+Z goes to the scene")
    await ed.call("undo", scope="scene")
    ed.check(close(await transform(ed, "Robot"), S["rest_robot"]), "undone")
    # MCP set_properties is never recorded: a scene step of Hero's rest (the snapshot follows it)
    keys = keys_of(st, "", "position")
    value = row(await state(ed), "", "position")["value"]
    await ed.call("set_properties", entity="Hero", values={"transform": {"scale": [1.2, 1.2, 1.2]}})
    st = await state(ed)
    ed.check_eq(keys_of(st, "", "position"), keys, "no key added")
    ed.check(close(row(st, "", "position")["value"], value), "the clip unchanged")
    ed.check(row(st, "", "scale") is None, "no scale track")
    ed.check(await modified(ed), "set_properties is a scene step")
    await ed.call("undo", scope="scene")
    ed.check(not await modified(ed), "undone")
    # the selection back to Hero
    await hook(ed, entity="Hero")


async def test_record_blend_shapes(ed: Editor):
    (ed.project / "assets/models").mkdir(parents=True, exist_ok=True)
    shutil.copy(MORPH_MODEL, ed.project / "assets/models/skinned_morph.gltf")
    await ed.call("create_entity", type="model", name="Jelly", model="assets/models/skinned_morph.gltf", parent="Hero")
    await ed.call("wait", frames=10)
    paths = [p for p in (await hook(ed, list_paths=True))["paths"] if p.startswith("Jelly")]
    ed.check(paths, f"Jelly under Hero: {paths}")
    hit = None
    for path in paths:
        r = await ed.call_raw("focus_window", {"window": "animation", "animation": {"morph": {"entities": ["Hero/" + path], "index": 0, "value": 0.8}}})
        if not r.is_error:
            hit = path
            st = r.structured["viewport_input"]
            break
    ed.check(hit is not None, f"a mesh with blend shapes under Jelly: {paths}")
    S["morph_path"] = hit
    r = row(st, hit, "morph")
    ed.check(r is not None, f"the Blend Shapes row keyed a morph track: {[x.get('label') for x in st['rows']]}")
    ed.check_eq(keys_of(st, hit, "morph"), [0.5], "at the playhead")
    ed.check(abs(r["value"][0] - 0.8) < 1e-4, f"weight 0 = 0.8: {r['value']}")
    # Record off (the preview was on before Record: it stays, Unity): the same row is a scene step
    # (MorphWeightsCmd, one per drag) that changes the weight's rest only
    # (a save stops the preview and Record with it, 9.R6)
    await ed.call("save_scene", scene="Main Scene")
    st = await hook(ed, record=False)
    ed.check(not st["recording"] and not st["preview"], f"the save ended Record and the preview: {st['recording']} {st['preview']}")
    st = await hook(ed, frame=15, preview=True)
    ed.check(st["preview"] and not st["recording"], "previewing, Record off")
    S["morph_entity"] = "Hero/" + hit
    st = await hook(ed, morph={"entities": ["Hero/" + hit], "index": 0, "value": 0.3})
    ed.check(await modified(ed), "an unrecorded Blend Shapes edit is a scene step")
    ed.check(abs(row(st, hit, "morph")["value"][0] - 0.8) < 1e-4, "the clip is unchanged")
    st = await hook(ed, preview=False)
    weight = await morph_weight(ed, "Hero/" + hit)
    ed.check(abs(weight - 0.3) < 1e-4, f"after the preview the scene has the edited weight, not the previewed one: {weight}")
    await ed.call("undo", scope="scene")
    ed.check(not await modified(ed), "one undo step for the slider drag")
    ed.check(abs(await morph_weight(ed, "Hero/" + hit)) < 1e-4, "undone")
    st = await hook(ed, frame=15, record=True)
    ed.check(st["recording"], "Record on again")


async def test_record_lattice_drag(ed: Editor):
    cage = (await ed.call("create_entity", type="empty", name="Cage", parent="Hero"))["entity_id"]
    await ed.call("add_component", entity=cage, component="lattice")
    # (a save stops the preview, and Record with it: 9.R6)
    await ed.call("save_scene", scene="Main Scene")
    st = await hook(ed, frame=15, record=True)
    ed.check(st["recording"], "Record on")
    st = await hook(ed, lattice_drag={"entity": "Hero/Cage", "points": [13], "offsets": [[0, 0.3, 0]]})
    r = row(st, "Cage", "lattice")
    ed.check(r is not None, f"the lattice drag keyed a lattice track: {[x.get('label') for x in st['rows']]}")
    ed.check_eq(keys_of(st, "Cage", "lattice"), [0.5], "at the playhead")
    ed.check(close(r["value"], [0, 0.3, 0], 1e-4), f"point 13's offset: {r['value']}")
    ed.check(not await modified(ed), "no scene step")
    offsets = harness.find_key(await ed.call("get_entity", entity=cage, components=["lattice"], properties=["offsets"]), "offsets")
    ed.check(close(offsets[13], [0, 0.3, 0], 1e-4), f"the live point: {offsets[13]}")
    S["cage"] = cage
    await shot(ed, "03-record-lattice")


async def test_record_shortcut_and_leaving_restores(ed: Editor):
    await focus_x(ed, "animation")
    await key(ed, "r")
    st = await state(ed)
    # (Record started with the preview on: leaving Record keeps it, Unity)
    ed.check(not st["recording"] and st["preview"], f"R left Record, the preview stays: {st['recording']} {st['preview']}")
    st = await hook(ed, preview=False)
    ed.check(not st["preview"], "the preview off")
    ed.check(close(await transform(ed, "Hero"), S["rest_hero"]), "Hero back to how it was")
    ed.check(same_rotation(await transform(ed, SPINE_ENTITY, "rotation"), S["rest_spine"]), "the spine too")
    offsets = harness.find_key(await ed.call("get_entity", entity=S["cage"], components=["lattice"], properties=["offsets"]), "offsets")
    ed.check(close(offsets[13], [0, 0, 0], 1e-6), f"the cage at rest: {offsets[13]}")
    ed.check(not await modified(ed), "the scene as saved")
    await focus_x(ed, "animation")
    await key(ed, "r")
    st = await state(ed)
    ed.check(st["recording"] and st["preview"], "R starts Record again (and the preview)")
    await key(ed, "r")
    st = await state(ed)
    ed.check(not st["recording"] and not st["preview"], "and stops it with the preview it started")


# ---------------------------------------------------------------------------- curves view

async def test_curves_view(ed: Editor):
    st = await hook(ed, view="curves", frame_curves="all")
    ed.check_eq(st["view"], "curves", "the Curves toggle")
    root = row(st, "", "position")["track"]
    spine = row(st, SPINE, "rotation")["track"]
    S["root_track"], S["spine_track"] = root, spine
    labels = [(c["track"], c["label"]) for c in st["curves"]]
    for axis in ("x", "y", "z"):
        ed.check_in((root, axis), labels, "position x/y/z curves")
    y = curve(st, root, 1)
    ed.check(close(y["key_values"], [S["rest_hero"][1], S["rest_hero"][1] + 1.0]), f"the position y key points: {y['key_values']}")
    sx = curve(st, spine, 0)
    ed.check(abs(sx["key_values"][0] - 20.0) < 0.05, f"rotation as Euler degrees: {sx['key_values']}")
    low, high = st["curve_range"]
    ed.check(low < S["rest_hero"][1] and high > S["rest_hero"][1] + 1.0, f"A frames every curve: {st['curve_range']}")
    if S.get("morph_path"):
        morph = row(st, S["morph_path"], "morph")["track"]
        ed.check(curve(st, morph, 0) is not None, "a blend shape curve")
    cage = row(st, "Cage", "lattice")["track"]
    ed.check_in((cage, "Point 13 y"), labels, "lattice point curves")
    await shot(ed, "01-curves")


async def test_curves_vertical_drag_one_undo_step(ed: Editor):
    root = S["root_track"]
    st = await state(ed)
    before = curve(st, root, 1)["key_values"]
    x_before = curve(st, root, 0)["key_values"]
    st = await hook(ed, curve_drag={"track": root, "time": 0.5, "component": 1, "value_delta": 0.5})
    after = curve(st, root, 1)["key_values"]
    ed.check(abs(after[1] - (before[1] + 0.5)) < 1e-4, f"the y value moved by 0.5: {before} -> {after}")
    ed.check(close(curve(st, root, 0)["key_values"], x_before), "x untouched (only the y point was selected)")
    ed.check_eq(curve(st, root, 1)["selected"], [0.5], "the y point stays selected")
    ed.check_eq(curve(st, root, 0)["selected"], [], "x is not")
    ed.check_eq(keys_of(st, "", "position"), [0.0, 0.5], "no key lost or added")
    ed.check(not await modified(ed), "a clip edit")
    await ed.call("undo", scope="project")
    ed.check(close(curve(await state(ed), root, 1)["key_values"], before, 1e-5), "one undo step for the drag")


async def test_curves_time_drag_and_rotation_value(ed: Editor):
    root, spine = S["root_track"], S["spine_track"]
    st = await hook(ed, curve_drag={"track": root, "time": 0.5, "component": 1, "time_delta": 0.21})
    ed.check_eq(keys_of(st, "", "position"), [0.0, 0.7], "the key lands on a frame (30 fps)")
    await ed.call("undo", scope="project")
    ed.check_eq(keys_of(await state(ed), "", "position"), [0.0, 0.5], "undone")
    # a rotation key's Euler x by +10 degrees
    st = await hook(ed, curve_drag={"track": spine, "time": 0.5, "component": 0, "value_delta": 10})
    ed.check(abs(curve(st, spine, 0)["key_values"][0] - 30.0) < 0.05, f"20 -> 30 degrees: {curve(st, spine, 0)['key_values']}")
    ed.check(same_rotation(row(await hook(ed, frame=15), SPINE, "rotation")["value"], euler_xyzw(30, 0, 0)), "the key's quaternion")
    await ed.call("undo", scope="project")
    # points of two properties (a position y and a rotation x) drag together, each on its curve
    st = await hook(ed, curve_select=[{"track": root, "time": 0.5, "component": 1}, {"track": spine, "time": 0.5, "component": 0}])
    y_before = curve(st, root, 1)["key_values"][1]
    rx_before = curve(st, spine, 0)["key_values"][0]
    st = await hook(ed, curve_drag={"track": root, "time": 0.5, "component": 1, "value_delta": 1.0})
    ed.check(abs(curve(st, root, 1)["key_values"][1] - (y_before + 1.0)) < 1e-4, f"position y +1: {curve(st, root, 1)['key_values']}")
    ed.check(abs(curve(st, spine, 0)["key_values"][0] - (rx_before + 1.0)) < 0.05, f"rotation x +1 degree: {curve(st, spine, 0)['key_values']}")
    await ed.call("undo", scope="project")
    st = await state(ed)
    ed.check(abs(curve(st, root, 1)["key_values"][1] - y_before) < 1e-4 and abs(curve(st, spine, 0)["key_values"][0] - rx_before) < 0.05,
             "one undo step for the mixed drag")
    # visibility and framing
    st = await hook(ed, curve_visible={"track": root, "component": 0, "visible": False})
    ed.check(not curve(st, root, 0)["visible"] and curve(st, root, 1)["visible"], "x hidden, y shown")
    st = await hook(ed, select=[{"track": root, "time": 0.5}], frame_curves="selected")
    low, high = st["curve_range"]
    y = curve(st, root, 1)["key_values"][1]
    ed.check(low <= y <= high, f"F frames the selected key: {st['curve_range']} around {y}")
    await hook(ed, curve_visible={"track": root, "component": 0, "visible": True})


async def test_record_screenshot_in_curves(ed: Editor):
    st = await hook(ed, entity="Hero", frame=15, record=True)
    ed.check(st["recording"], "recording in the curves view")
    await ed.call("focus_window", window="inspector")
    await shot(ed, "04-record-curves-inspector")
    st = await hook(ed, record=False, preview=False, view="dopesheet")
    ed.check(not st["recording"] and st["view"] == "dopesheet", "back to the dopesheet, Record off")
    ed.check(close(await transform(ed, "Hero"), S["rest_hero"]), "restored")


async def test_real_gizmo_drag_records(ed: Editor):
    """The Scene view's real mouse drag of the Move gizmo (xdotool on the harness' Xvfb) with Record
    on: SceneRender's drag path keys Hero's position at the playhead, no scene step, one undo step."""
    await ed.call("save_scene", scene="Main Scene")
    st = await hook(ed, entity="Hero", frame=20, record=True)
    ed.check(st["recording"], "Record on")
    before_keys = keys_of(st, "", "position")
    position = await transform(ed, "Hero")
    await ed.call("select_entities", entities=["Hero"])
    await ed.call("set_scene_camera", position=[position[0], position[1] + 1.5, position[2] + 5.0], target=position)
    await focus_x(ed, "scene")
    await ed.call("wait", frames=10)
    # the Move gizmo's centre sits on Hero, in the middle of the Scene view (the camera's target)
    cx, cy = (300 + 1238) // 2, (100 + 605) // 2
    run_x(ed, "xdotool", "mousemove", str(cx), str(cy))
    await ed.call("wait", frames=6)
    run_x(ed, "xdotool", "mousedown", "1")
    await ed.call("wait", frames=3)
    for step in range(1, 6):
        run_x(ed, "xdotool", "mousemove", str(cx + 16 * step), str(cy))
        await ed.call("wait", frames=3)
    run_x(ed, "xdotool", "mouseup", "1")
    await ed.call("wait", frames=8)
    st = await state(ed)
    keys = keys_of(st, "", "position")
    moved = await transform(ed, "Hero")
    ed.check(abs(moved[0] - position[0]) > 0.05, f"the drag moved Hero: {position} -> {moved}")
    ed.check(round(20 / 30, 4) in keys and len(keys) == len(set(before_keys) | {round(20 / 30, 4)}), f"keyed at frame 20: {before_keys} -> {keys}")
    ed.check(close(row(await hook(ed, frame=20), "", "position")["value"], moved, 1e-3), "the key holds the dragged position")
    ed.check(not await modified(ed), "no scene step")
    await ed.call("undo", scope="project")
    ed.check_eq(keys_of(await state(ed), "", "position"), before_keys, "one undo step for the whole drag")
    st = await hook(ed, record=False, preview=False)
    ed.check(close(await transform(ed, "Hero"), S["rest_hero"], 1e-3), "restored")


async def test_curves_view_large_clip_responsive(ed: Editor):
    """I3: the curves view of a 60 x 600 key clip draws in under 30 ms per frame."""
    paths = [p for p in (await hook(ed, list_paths=True))["paths"] if p.startswith("Hero/")][:20]
    tracks = []
    for i, path in enumerate(paths):
        for channel in ("position", "rotation", "scale"):
            keys = []
            for k in range(600):
                t = round(k / 60, 6)
                wave = math.sin(k / 20 + i)
                if channel == "rotation":
                    keys.append([t, euler_xyzw(30 * wave, 10 * wave, 0)])
                elif channel == "scale":
                    keys.append([t, [1 + 0.1 * wave, 1, 1]])
                else:
                    keys.append([t, [wave, 0, 0]])
            tracks.append({"path": path, "channel": channel, "keys": keys})
    clip = {"format": "jasperengine.anim", "version": 1, "frame_rate": 60, "wrap": "loop", "tracks": tracks, "events": []}
    (ed.project / "assets/anim/bigcurves.anim").write_text(json.dumps(clip) + "\n")
    st = await hook(ed, clip="assets/anim/bigcurves.anim", root="Hero", brief=True, view="curves", frame_curves="all")
    ed.check_eq(st["view"], "curves", "the curves view")
    await ed.call("wait", seconds=1.0)
    st = await hook(ed, brief=True)
    ed.check(st["draw_ms"] < 30.0, f"the curves view draws in {st['draw_ms']:.1f} ms (moving average)")
    await shot(ed, "05-curves-large-clip")
    st = await hook(ed, view="dopesheet", clip=CLIP, root="Hero")


async def test_blend_shape_weights_save_and_reload(ed: Editor):
    """I1: a Blend Shapes weight edited on a plain model and on a prefab instance survives save +
    reopen (morphWeights[i] is a Catalog property; the instance's edit is an override)."""
    (ed.project / "assets/models").mkdir(parents=True, exist_ok=True)
    shutil.copy(MORPH_MODEL, ed.project / "assets/models/skinned_morph.gltf")
    blob = (await ed.call("create_entity", type="model", name="Blob", model="assets/models/skinned_morph.gltf"))["entity_id"]
    await ed.call("wait", frames=6)
    mesh_path = None
    for candidate in ("Blob", "Blob/Strip"):
        r = await ed.call_raw("focus_window", {"window": "animation", "animation": {"morph": {"entities": [candidate], "index": 0, "value": 0.6}}})
        if not r.is_error:
            mesh_path = candidate
            break
    ed.check(mesh_path is not None, "the model's mesh with blend shapes")
    ed.check(abs(await morph_weight(ed, mesh_path) - 0.6) < 1e-4, "edited")
    made = await ed.call("create_prefab", entity=blob, path="prefabs/Blob.prefab")
    ed.check(made["prefabs"], f"prefab made: {made}")
    inst = await ed.call("instantiate_prefab", prefab="prefabs/Blob.prefab", position=[3, 0, 0], name="Blob B")
    await ed.call("wait", frames=6)
    instance_mesh = mesh_path.replace("Blob", "Blob B", 1)
    r = await ed.call_raw("focus_window", {"window": "animation", "animation": {"morph": {"entities": [instance_mesh], "index": 0, "value": 0.9}}})
    ed.check(not r.is_error, f"the instance's mesh: {r.text if r.is_error else ''}")
    ed.check(abs(await morph_weight(ed, instance_mesh) - 0.9) < 1e-4, "the instance edited")
    await ed.call("save_scene", scene="Main Scene")
    await ed.call("create_scene", name="Spare", type="3d")
    await ed.call("close_scene", scene="Main Scene")
    await ed.call("open_scene", scene="Main Scene.scene")
    await ed.call("wait", frames=10)
    ed.check(abs(await morph_weight(ed, mesh_path) - 0.6) < 1e-4, f"the plain model's weight after reopening: {await morph_weight(ed, mesh_path)}")
    ed.check(abs(await morph_weight(ed, instance_mesh) - 0.9) < 1e-4, f"the instance's weight after reopening: {await morph_weight(ed, instance_mesh)}")
