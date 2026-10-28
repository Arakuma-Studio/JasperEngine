"""The textures of a model in a scene (bumpy_box.gltf from tools/fixtures/runtime-shaders, its
normal map embedded): the scene stores them by the model's path in the assets directory, never by
this machine's absolute path, and the project copied to another folder (the harness opens a copy)
draws the model with its textures as the original did. Scenes saved with absolute ids are migrated
when they load (MeshSystem::migrateModelTextureId, the gtest suite ModelTextureId).

The model embeds its image, so its texture is always named by the model: an external image
(normal.png next to the .gltf) is loaded from its cook once the editor has cooked it (S14,
MeshSystem::loadGLTFTexture), and is then named by its own assets path, depending on whether the
background cook landed before the model loaded. The last test checks that case.
"""

from __future__ import annotations

import base64
import json
import shutil

import harness
import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Model Texture Paths"}
TIMEOUT = 600

FIXTURE = harness.REPO / "tools" / "fixtures" / "runtime-shaders" / "assets"
SCENE = "scenes/Models.scene"
NORMAL = "submeshes[0].material.normalTexture"
DISPLAY = {"show_grid_3d": False, "hide_light_icons": True, "hide_selection_outline": True,
           "hide_camera_view": True, "hide_sound_icons": True}
S: dict = {}


async def shot(ed: Editor, scene, label: str) -> tuple:
    """The Scene view (480 px wide) and the size it was scaled from"""
    # nothing selected: no transform gizmo over the model in either editor
    await ed.call("select_entities", scene=scene, entities=[])
    await ed.call("set_scene_camera", scene=scene, position=[2.2, 1.6, 2.6], target=[0, 0, 0], display=DISPLAY)
    await ed.call("wait", frames=6)
    r = await ed.call_raw("screenshot", {"view": "scene", "scene": scene, "max_width": 480}, timeout=300)
    ed.check(not r.is_error and r.images, f"no scene screenshot: {r.text[:300]}")
    (ed.scratch.root / f"model-{label}.png").write_bytes(r.images[0][0])
    size = (r.structured or {}).get("source_width"), (r.structured or {}).get("source_height")
    return pngdiff.decode(r.images[0][0]), size


async def normal_texture(ed: Editor, scene, entity) -> dict:
    got = await ed.call("get_entity", scene=scene, entity=entity, components=["mesh"], properties=[NORMAL], detail="values")
    return next(iter(got["components"].values()))[NORMAL]


def embedded_model() -> str:
    """bumpy_box.gltf with normal.png inside it, as a data URI (an embedded image is never cooked)"""
    gltf = json.loads((FIXTURE / "bumpy_box.gltf").read_text())
    png = base64.b64encode((FIXTURE / "normal.png").read_bytes()).decode()
    gltf["images"] = [{"uri": "data:image/png;base64," + png}]
    return json.dumps(gltf)


async def test_scene_stores_no_absolute_path(ed: Editor):
    models = ed.project / "assets" / "models"
    models.mkdir(parents=True, exist_ok=True)
    (models / "bumpy_box.gltf").write_text(embedded_model())
    made = await ed.call("create_scene", name="Models", type="3D", path=SCENE)
    scene = made["scene"]["id"]
    await ed.call("set_model", scene=scene, create={"name": "Box"}, path="assets/models/bumpy_box.gltf")
    await ed.call("save_scene", scene=scene)
    texture = await normal_texture(ed, scene, "Box")
    # the model's path in the assets directory (this project's is its root)
    S["id"] = texture.get("id", "")
    ed.check(S["id"].endswith("models/bumpy_box.gltf|gltf-image|0|s0") and not S["id"].startswith("/"),
             f"the model's normal map is named by the model's assets path: {texture}")
    text = (ed.project / SCENE).read_text()
    root = str(ed.project.resolve())
    ed.check(S["id"] in text and root not in text and str(ed.scratch.root) not in text, f"the scene names no absolute path:\n{text}")
    original, _ = await shot(ed, scene, "original")
    # the comparison below sees a model drawn without its normal map
    await ed.call("set_properties", scene=scene, entity="Box", values={"mesh": {NORMAL: None}})
    flat, _ = await shot(ed, scene, "without-normal-map")
    same, fraction, _ = pngdiff.compare(original, flat, 8, 0.002)
    S["without"] = fraction
    print(f"     [model] without its normal map {fraction * 100:.2f}% of the pixels differ")
    ed.check(not same, f"the normal map shows in the Scene view ({fraction * 100:.2f}% of the pixels differ without it)")
    await ed.call("undo")
    await ed.call("save_scene", scene=scene)
    # the reference: the scene as saved, drawn by the editor that saved it
    S["shot"], S["size"] = await shot(ed, scene, "saved")


async def test_moved_project_loads_the_model_textures(ed: Editor):
    # the harness copies the project into a scratch folder of its own: the project moved
    async with Editor(project=ed.project) as moved:
        ed.check(moved.project.resolve() != ed.project.resolve(), "the copy is elsewhere")
        opened = await moved.call("open_scene", scene=SCENE)
        scene = opened["scene"]["id"]
        await moved.call("get_model_info", scene=scene, entity="Box")
        texture = await normal_texture(moved, scene, "Box")
        moved.check_eq(texture.get("id"), S["id"], "the moved project names the same texture")
        image, size = await shot(moved, scene, "moved")
        # A project the editor opens can get a Scene view a pixel taller or shorter than the one
        # it created (944x488 then 944x489: the dock layout it saved, read back): another aspect
        # ratio moves every edge by a fraction of a pixel, about 0.5% of the pixels past the
        # channel tolerance. The same size compares tightly; another one still must be far
        # closer than the model without its normal map (16% here).
        limit = 0.002 if size == S["size"] else min(0.02, S["without"] / 4)
        same, fraction, diff = pngdiff.compare(S["shot"], image, 8, limit)
        (ed.scratch.root / "model-moved.diff.png").write_bytes(diff or b"")
        print(f"     [model] moved: {fraction * 100:.2f}% of the pixels differ from the original "
              f"(Scene view {size[0]}x{size[1]}, original {S['size'][0]}x{S['size'][1]}; limit {limit * 100:.1f}%)")
        moved.check(same, f"the moved project draws the model as the original did ({fraction * 100:.2f}% of the pixels differ, "
                          f"limit {limit * 100:.1f}%; {S['without'] * 100:.2f}% without the normal map)")


async def test_cooked_external_image_is_named_by_its_path(ed: Editor):
    models = ed.project / "assets" / "external"
    models.mkdir(parents=True, exist_ok=True)
    for name in ("bumpy_box.gltf", "normal.png"):
        shutil.copy(FIXTURE / name, models / name)
    # the cook first (the queue starts with the project; until then reimport_textures refuses)
    for _ in range(200):
        cooked = await ed.call_raw("reimport_textures", {"paths": ["assets/external/normal.png"], "platforms": ["desktop"]})
        if not cooked.is_error:
            break
        await ed.call("wait", frames=5)
    ed.check(not cooked.is_error, f"the external normal map is cooked: {cooked.text[:300]}")
    made = await ed.call("create_scene", name="External", type="3D", path="scenes/External.scene")
    scene = made["scene"]["id"]
    await ed.call("set_model", scene=scene, create={"name": "Box"}, path="assets/external/bumpy_box.gltf")
    await ed.call("save_scene", scene=scene)
    texture = await normal_texture(ed, scene, "Box")
    ed.check_eq(texture.get("path"), "assets/external/normal.png", f"the cooked image is named by its assets path: {texture}")
    text = (ed.project / "scenes/External.scene").read_text()
    ed.check(str(ed.project.resolve()) not in text and str(ed.scratch.root) not in text, f"the scene names no absolute path:\n{text}")
