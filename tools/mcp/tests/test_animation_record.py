"""The Animation window's Record mode in the real editor (animation clips spec 4.4, 9.R5/R5b/R6), on the pose
fixture project (Hero: hero_tpose.gltf, Mixamo bones), driven by REAL X input (xdotool on the harness' Xvfb,
widgets found through the UI probe, Editor(ui_probe=True)) and the public MCP tools. The record sources
(gizmo, Inspector rows, blend shapes, lattice drags), the multi-selection rule, MCP set_properties never
being recorded and the curves view are the editor unit test editor/tests/AnimationWindowClipTest.cpp.

Covered here: R toggling Record on the focused window (with the preview it starts, or keeping the preview
that was on), a real mouse drag of the Move gizmo in the Scene view keying Hero's position at the playhead
with no scene step and one undo step, real Ctrl+Z / Ctrl+Y from the Scene view going to the project history
while that recorded key is the newest step (R5b), leaving Record restoring the scene; Blend Shapes weights
(morphWeights[i], a Catalog property) of a plain model and of a prefab instance surviving save + reopen,
and a weight set to 0 on purpose staying 0 over a file default.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 900

CLIP = "assets/anim/rec.anim"
MORPH_MODEL = harness.REPO / "tools" / "fixtures" / "runtime-shaders" / "assets" / "skinned_morph.gltf"

S: dict = {}


# ---------------------------------------------------------------------------- helpers

def xdo(ed: Editor, *args: str) -> str:
    if not shutil.which("xdotool"):
        # agents review M5: these tests carry the window's real-input coverage (no editor test hook):
        # a host without xdotool fails them instead of skipping quietly
        raise harness.TestFailure("xdotool is not installed: the Animation window's real-input tests need it with the "
                                  "harness' Xvfb (install xdotool, e.g. apt install xdotool)")
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout


async def focus_x(ed: Editor, window: str) -> None:
    """`window` focused in ImGui and the editor's X window focused for xdotool."""
    await ed.call("focus_window", window=window)
    ids = xdo(ed, "search", "--onlyvisible", "--name", "JasperEngine").split()
    ed.check(ids, "the editor's X window")
    xdo(ed, "windowfocus", "--sync", ids[-1])
    await ed.call("wait", frames=6)


async def key(ed: Editor, combo: str) -> None:
    *modifiers, last = combo.split("+")
    for modifier in modifiers:
        xdo(ed, "keydown", "--clearmodifiers", modifier)
    xdo(ed, "keydown", last)
    xdo(ed, "keyup", last)
    for modifier in reversed(modifiers):
        xdo(ed, "keyup", modifier)
    await ed.call("wait", frames=8)


async def modified(ed: Editor) -> bool:
    scenes = await ed.call("list_scenes")
    main = next(s for s in harness.find_key(scenes, "scenes") if s["name"] == "Main Scene")
    return bool(main["modified"])


async def transform(ed: Editor, entity: str, what: str = "position") -> list:
    value = harness.find_key(await ed.call("get_entity", entity=entity, detail="full"), what)
    if isinstance(value, dict):
        value = value.get("value", value)
        value = value.get("quat", value) if isinstance(value, dict) else value
    return [float(v) for v in value]


def close(a, b, tol=1e-3) -> bool:
    return a is not None and b is not None and len(a) == len(b) and all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))


def clip_track(ed: Editor) -> dict:
    data = json.loads((ed.project / CLIP).read_text())
    return next(t for t in data["tracks"] if t["path"] == "" and t["channel"] == "position")


def clip_keys(ed: Editor) -> list:
    return [round(k[0], 4) for k in clip_track(ed)["keys"]]


async def toggles(ed: Editor) -> tuple[bool, bool]:
    """(Record on, Preview on) as the window's toolbar shows them (its highlighted buttons)."""
    await ed.call("wait", frames=4)
    items = ed.ui_items()
    record = "clip.record.on" in items
    preview = "clip.preview.on" in items
    ed.check(record or "clip.record.off" in items, f"the Record button is drawn: {sorted(k for k in items if k.startswith('clip.'))}")
    return record, preview


async def morph_weight(ed: Editor, entity: str, index: int = 0) -> float:
    """MeshComponent morphWeights[index] (a Catalog property) of a GameObject path."""
    got = await ed.call("get_entity", entity=entity, components=["mesh"], properties=[f"morphWeights[{index}]"])
    value = harness.find_key(got, f"morphWeights[{index}]")
    if isinstance(value, dict):
        value = value.get("value", value)
    return float(value)


async def set_morph_weight(ed: Editor, entity: str, value: float, index: int = 0) -> bool:
    r = await ed.call_raw("set_properties", {"entity": entity, "values": {"mesh": {f"morphWeights[{index}]": value}}})
    return not r.is_error


# ---------------------------------------------------------------------------- setup

async def test_setup_clip(ed: Editor):
    ed.require_tools("focus_window", "write_file", "add_component", "select_entities", "preview_animation", "set_scene_camera")
    await ed.call("save_scene", scene="Main Scene")
    S["rest_hero"] = await transform(ed, "Hero")
    clip = {"format": "jasperengine.anim", "version": 1, "frame_rate": 30, "wrap": "loop",
            "tracks": [{"path": "", "channel": "position", "keys": [[0, S["rest_hero"]]]}], "events": []}
    await ed.call("write_file", path=CLIP, content=json.dumps(clip, indent=2) + "\n")
    await ed.call("sync_files")
    await ed.call("add_component", entity="Hero", component="animationplayer", values={"animationplayer": {"clips": [{"clip": CLIP}]}})
    await ed.call("save_scene", scene="Main Scene")
    await ed.call("select_entities", entities=["Hero"])
    await focus_x(ed, "animation")
    ed.check(await ed.ui_rect("clip.rowLabel.0"), f"Hero's clip shows: {sorted(ed.ui_items())}")
    ed.check_eq(await toggles(ed), (False, False), "Record and Preview off")


# ---------------------------------------------------------------------------- Record

async def test_record_shortcut_toggles_record_and_its_preview(ed: Editor):
    await focus_x(ed, "animation")
    await key(ed, "r")
    ed.check_eq(await toggles(ed), (True, True), "R starts Record (and the preview)")
    await key(ed, "r")
    ed.check_eq(await toggles(ed), (False, False), "and stops it with the preview it started")
    ed.check(close(await transform(ed, "Hero"), S["rest_hero"]), "Hero as it was")
    ed.check(not await modified(ed), "the scene as saved")


async def test_real_gizmo_drag_records(ed: Editor):
    """The Scene view's real mouse drag of the Move gizmo with Record on: SceneRender's drag path keys Hero's
    position at the playhead, no scene step, one undo step."""
    await ed.call("preview_animation", animation="Hero", action="seek", time=20 / 30)
    await focus_x(ed, "animation")
    await key(ed, "r")
    ed.check_eq(await toggles(ed), (True, True), "Record on (the preview was on already)")
    before = clip_keys(ed)
    position = await transform(ed, "Hero")
    await ed.call("select_entities", entities=["Hero"])
    await ed.call("set_scene_camera", position=[position[0], position[1] + 1.5, position[2] + 5.0], target=position)
    await focus_x(ed, "scene")
    view = await ed.ui_rect("scene.view")
    ed.check(view, f"the Scene view is drawn: {sorted(ed.ui_items())}")
    # the Move gizmo's centre sits on Hero, in the middle of the Scene view (the camera's target)
    cx, cy = int((view[0] + view[2]) / 2), int((view[1] + view[3]) / 2)
    xdo(ed, "mousemove", str(cx), str(cy))
    await ed.call("wait", frames=6)
    xdo(ed, "mousedown", "1")
    await ed.call("wait", frames=3)
    for step in range(1, 6):
        xdo(ed, "mousemove", str(cx + 16 * step), str(cy))
        await ed.call("wait", frames=3)
    xdo(ed, "mouseup", "1")
    await ed.call("wait", frames=8)
    keys = clip_keys(ed)
    moved = await transform(ed, "Hero")
    ed.check(abs(moved[0] - position[0]) > 0.05, f"the drag moved Hero: {position} -> {moved}")
    ed.check(round(20 / 30, 4) in keys and len(keys) == len(set(before) | {round(20 / 30, 4)}), f"keyed at frame 20: {before} -> {keys}")
    value = next(k[1] for k in clip_track(ed)["keys"] if abs(k[0] - 20 / 30) < 1e-4)
    ed.check(close(value, moved, 1e-3), f"the key holds the dragged position: {value} vs {moved}")
    ed.check(not await modified(ed), "no scene step")
    S["recorded"] = keys
    S["before_record"] = before
    await ed.call("undo", scope="project")
    ed.check_eq(clip_keys(ed), before, "one undo step for the whole drag")
    await ed.call("redo", scope="project")
    ed.check_eq(clip_keys(ed), keys, "redo")


async def test_record_undo_from_the_scene_view(ed: Editor):
    """R5b: with the Scene view focused, real Ctrl+Z goes to the project history while a recorded key is the
    newest step; Record never takes a scene step's undo."""
    await focus_x(ed, "scene")
    await key(ed, "ctrl+z")
    ed.check_eq(clip_keys(ed), S["before_record"], "Ctrl+Z in the Scene view undid the recorded key")
    ed.check(not await modified(ed), "the scene was not touched")
    await focus_x(ed, "scene")
    await key(ed, "ctrl+y")
    ed.check_eq(clip_keys(ed), S["recorded"], "Ctrl+Y redid it")


async def test_leaving_record_restores_the_scene(ed: Editor):
    await focus_x(ed, "animation")
    await key(ed, "r")
    ed.check_eq(await toggles(ed), (False, True), "R left Record; the preview it found on stays (Unity)")
    await ed.call("preview_animation", animation="Hero", action="stop")
    ed.check_eq(await toggles(ed), (False, False), "the preview off")
    ed.check(close(await transform(ed, "Hero"), S["rest_hero"]), "Hero back to how it was")
    ed.check(not await modified(ed), "the scene as saved")
    await ed.screenshot(view="editor", max_width=0)


# ---------------------------------------------------------------------------- blend shape weights

async def test_blend_shape_weights_save_and_reload(ed: Editor):
    """I1: a Blend Shapes weight edited on a plain model and on a prefab instance survives save + reopen
    (morphWeights[i] is a Catalog property; the instance's edit is an override)."""
    (ed.project / "assets/models").mkdir(parents=True, exist_ok=True)
    shutil.copy(MORPH_MODEL, ed.project / "assets/models/skinned_morph.gltf")
    await ed.call("sync_files")
    blob = (await ed.call("create_entity", type="model", name="Blob", model="assets/models/skinned_morph.gltf"))["entity_id"]
    await ed.call("wait", frames=6)
    mesh_path = None
    for candidate in ("Blob", "Blob/Strip"):
        if await set_morph_weight(ed, candidate, 0.6):
            mesh_path = candidate
            break
    ed.check(mesh_path is not None, "the model's mesh with blend shapes")
    ed.check(abs(await morph_weight(ed, mesh_path) - 0.6) < 1e-4, "edited")
    made = await ed.call("create_prefab", entity=blob, path="prefabs/Blob.prefab")
    ed.check(made["prefabs"], f"prefab made: {made}")
    await ed.call("instantiate_prefab", prefab="prefabs/Blob.prefab", position=[3, 0, 0], name="Blob B")
    await ed.call("wait", frames=6)
    instance_mesh = mesh_path.replace("Blob", "Blob B", 1)
    ed.check(await set_morph_weight(ed, instance_mesh, 0.9), "the instance's mesh")
    ed.check(abs(await morph_weight(ed, instance_mesh) - 0.9) < 1e-4, "the instance edited")
    await ed.call("save_scene", scene="Main Scene")
    await ed.call("create_scene", name="Spare", type="3d")
    await ed.call("close_scene", scene="Main Scene")
    await ed.call("open_scene", scene="Main Scene.scene")
    await ed.call("wait", frames=10)
    ed.check(abs(await morph_weight(ed, mesh_path) - 0.6) < 1e-4, f"the plain model's weight after reopening: {await morph_weight(ed, mesh_path)}")
    ed.check(abs(await morph_weight(ed, instance_mesh) - 0.9) < 1e-4, f"the instance's weight after reopening: {await morph_weight(ed, instance_mesh)}")


async def test_zero_blend_shape_weights_survive_a_reload(ed: Editor):
    """A model whose file has a default weight (0.5): a new one gets it; set to 0 on purpose, saved and
    reopened it stays 0 (not the file default again)."""
    gltf = json.loads(MORPH_MODEL.read_text())
    gltf["meshes"][0]["weights"] = [0.5]
    (ed.project / "assets/models/morph_default.gltf").write_text(json.dumps(gltf))
    await ed.call("sync_files")
    await ed.call("create_entity", type="model", name="Dflt", model="assets/models/morph_default.gltf")
    await ed.call("wait", frames=6)
    mesh_path = None
    for candidate in ("Dflt", "Dflt/Strip"):
        r = await ed.call_raw("get_entity", {"entity": candidate, "components": ["mesh"], "properties": ["morphWeights[0]"]})
        if not r.is_error and harness.find_key(r.structured, "morphWeights[0]") is not None:
            mesh_path = candidate
            break
    ed.check(mesh_path is not None, "the model's mesh with blend shapes")
    ed.check(abs(await morph_weight(ed, mesh_path) - 0.5) < 1e-4, f"a new model has the file's default: {await morph_weight(ed, mesh_path)}")
    ed.check(await set_morph_weight(ed, mesh_path, 0.0), "set to 0")
    ed.check(abs(await morph_weight(ed, mesh_path)) < 1e-4, "set to 0")
    await ed.call("save_scene", scene="Main Scene")
    await ed.call("close_scene", scene="Main Scene")
    await ed.call("open_scene", scene="Main Scene.scene")
    await ed.call("wait", frames=10)
    ed.check(abs(await morph_weight(ed, mesh_path)) < 1e-4, f"the saved 0 after reopening, not the file's 0.5: {await morph_weight(ed, mesh_path)}")
    await ed.call("delete_entities", entities=["Dflt"])
    await ed.call("save_scene", scene="Main Scene")
