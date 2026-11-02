"""A model's images reach its meshes whatever else loads the same file meanwhile (BF15).

The Project window makes a thumbnail of a model file that appears or changes (the file watcher's
event): a preview of the model at 256 px, loaded on the thumbnail strand and drawn and freed on
the main thread, while a scene may be loading the same file. The preview keyed the model's
embedded images as the scene's model does, so it shared the scene's pooled pixels (its own GPU
upload freed them), replaced the generator that makes them again with one bound to the preview's
model (gone with the preview), and its teardown let the GPU texture go. A mesh of the scene
that made its GPU texture after that found no pixels and no way to make them: it drew the
stand-in, white, for good, and nothing bound the image later. test_khronos_models met it in
about 2% of its runs under load: SpecularTest's label plates white, 5.25% of the pixels off the
golden.

JASPER_MESH_LOAD_HOLD_FILE holds the first load of the scene's meshes (of a model loaded in the
background) while that file exists, so the preview comes between the scene's model load and its
meshes' binding every time.
"""

from __future__ import annotations

import math
import shutil
import sys
import tempfile
import time
import uuid
from pathlib import Path

import pngdiff
from harness import Editor

sys.path.insert(0, str(Path(__file__).parent))
from test_materials_pbr import (GOLDEN_DIR, GOLDEN_SIZE, all_ready, camera_at, entities_with,  # noqa: E402
                                framing, model_file, new_scene, scene_camera, shot, studio_sky, sun)

CI = True
HOLD = Path(tempfile.gettempdir()) / f"jasper-mesh-hold-{uuid.uuid4().hex}"
EDITOR = {"create": True, "name": "Model Texture Bind", "env": {"JASPER_MESH_LOAD_HOLD_FILE": str(HOLD)}}
TIMEOUT = 1800
MODEL = "SpecularTest"
# another model, whose thumbnail runs the thumbnail strand's render queue again: the cleanups a
# preview queued there (the release of the pixels its GPU upload took) run with the next batch
OTHER = "MetalRoughSpheresNoTextures"


def thumbnails(ed: Editor) -> list:
    folder = ed.project / ".jasperengine" / "thumbs"
    return sorted(folder.glob("*.thumb.png")) if folder.is_dir() else []


async def make_thumbnail(ed: Editor, path: str) -> None:
    """The Project window's thumbnail of the model, made now: view_asset makes a missing one and
    answers with it once it is there (each call waits up to 15 s); its preview was loaded, drawn
    and freed (on the main thread, right after its draw) by then"""
    deadline = time.monotonic() + 600
    while True:
        r = await ed.call_raw("view_asset", {"path": path}, timeout=300)
        if not r.is_error and r.images:
            break
        ed.check(time.monotonic() < deadline, f"the thumbnail of {path} was made: {r.text[:300]}")
        await ed.call("wait", frames=10, timeout_seconds=900)
    await ed.call("wait", frames=5, timeout_seconds=900)


async def test_a_preview_of_the_model_leaves_the_scene_its_images(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "view_asset", "screenshot", "get_material_variant")
    HOLD.unlink(missing_ok=True)
    source = model_file(MODEL)
    position, target = framing(source)
    await new_scene(ed, f"Khronos{MODEL}")
    await ed.console_since("last", limit=1000)  # (moves the cursor past what came before)
    await studio_sky(ed)
    await ed.call("set_properties", entity=await sun(ed), values={"light": {"direction": [-0.35, -0.45, -1], "intensity": 3.0}})

    # the model in the project, and the thumbnail its file event asks for made before the scene
    # loads it (only the preview below comes between the scene's load and its binding)
    shutil.copytree(source.parent, ed.project / "models" / MODEL, dirs_exist_ok=True)
    rel = f"models/{MODEL}/{source.name}"
    other = model_file(OTHER)
    shutil.copytree(other.parent, ed.project / "models" / OTHER, dirs_exist_ok=True)
    other_rel = f"models/{OTHER}/{other.name}"
    await ed.call("sync_files")
    await make_thumbnail(ed, rel)
    await make_thumbnail(ed, other_rel)

    HOLD.write_text("held")
    try:
        await ed.call("create_entity", name=MODEL, type="model", model=rel)
        await camera_at(ed, position, target)
        distance = math.dist(position, target)
        await ed.call("set_properties", entity=await scene_camera(ed),
                      values={"camera": {"nearClip": distance * 0.01, "farClip": distance * 20}})
        # the model loaded (in the background): its meshes are there, none bound yet
        meshes = []
        deadline = time.monotonic() + 600
        while len(meshes) < 2 and time.monotonic() < deadline:
            await ed.call("wait", frames=10, timeout_seconds=900)
            meshes = await entities_with(ed, "MeshComponent")
        ed.check(len(meshes) >= 2, f"the model's meshes: {meshes}")
        # (the ones with submeshes: the model's root holds none)
        states = {}
        for eid in meshes:
            r = await ed.call_raw("get_material_variant", {"entity": eid, "submesh": 0})
            if not r.is_error:
                states[eid] = r.value["state"]
        meshes = list(states)
        ed.check(states and not any(s["loaded"] for s in states.values()),
                 f"the scene's meshes wait for the hold: {list(states.values())[:3]}")

        # a thumbnail of the same file now (the file watcher asks for one when the file changes),
        # then another one, which runs the cleanups the first queued (its upload's pixel release)
        for old in thumbnails(ed):
            old.unlink()
        await make_thumbnail(ed, rel)
        await make_thumbnail(ed, other_rel)
    finally:
        HOLD.unlink(missing_ok=True)

    await ed.call("wait", frames=60, timeout_seconds=900)
    problems = await all_ready(ed, meshes)
    ed.check(not [p for p in problems if p.get("failed")], f"variants failed: {problems[:3]!r}")
    img = await shot(ed, frames=30)
    small = pngdiff.downscale(img, *GOLDEN_SIZE)
    golden = GOLDEN_DIR / f"{MODEL}.png"
    ok, fraction, diff = pngdiff.compare(small, pngdiff.load(golden))
    if not ok:
        (ed.scratch.root / f"{MODEL}.actual.png").write_bytes(pngdiff.encode(small))
        (ed.scratch.root / f"{MODEL}.diff.png").write_bytes(diff or b"")
        ed.keep = True
    ed.check(ok, f"{MODEL} after a preview of its file: {fraction * 100:.2f}% of the pixels differ from {golden} "
                 f"(white label plates: the scene's mesh bound a stand-in for its base colour)")
    errors = [line for line in await ed.console_since("last", limit=1000) if line.get("level") == "error"]
    ed.check(not errors, f"errors: {errors[:3]}")
