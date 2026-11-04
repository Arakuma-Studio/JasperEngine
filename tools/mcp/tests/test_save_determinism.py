"""A scene saves (and generates) the same whether or not it was drawn, or its models loaded (S03).

- A Text's UI texture is the font atlas the UISystem builds when it first draws: a file holds only
  its authored sampler settings, never the atlas id of a run;
- a scene saves the same whether or not the Scene view made its icons (Sprites in its registry when
  it draws a light, a camera or an Audio Source), and holds no maxValues (components have no
  capacities since mesh-memory);
- a Mesh's (or Points') transparent under Auto Transparency is what a load starts from, not the
  value the renderer derives once it drew a blended submesh;
- a save before a model of the scene finished loading writes what a save after it writes (the
  external buffers the load gives its meshes);
- a Missing Prefab instance's data re-saves in the style the file had (an added GameObject's
  empty sequences);
- the generated code of a scene's Default Camera (the editor's, sized to the editor's view) does
  not depend on the editor window.

Oracles: the files on disk (bytes, PyYAML) and the exported sources.
"""

from __future__ import annotations

import re
import shutil
import tempfile
import time
from pathlib import Path

import yaml

from harness import Editor
from scene_history import FIXTURES, SCENE_INPUTS, History, first_difference, two_level_prefab

TIMEOUT = 1800
CI = True
EDITOR = {"project": str(SCENE_INPUTS)}
# box.gltf: one mesh of four attributes and indices, five buffer views
BOX_SOURCE = FIXTURES / "shader-library-export" / "assets" / "models" / "box.gltf"
BOX = "assets/models/box.gltf"
DELAY_ENV = "JASPER_TEST_MODEL_LOAD_DELAY_MS"
TEXT_PATH = "Atlas Text.scene"
ATLAS_ID = re.compile(r"\|\d+#\d+$")


def components_of(scene: dict, name: str) -> dict:
    for entity in scene["entities"]:
        if entity.get("name") == name:
            return entity["components"]
    raise AssertionError(f"no {name} in the scene file")


def has_max_values(data: bytes) -> bool:
    return "maxValues" in yaml.safe_load(data)


async def modified(ed: Editor, sid) -> bool:
    for entry in (await ed.call("list_scenes"))["scenes"]:
        if entry["id"] == sid:
            return entry["modified"]
    ed.fail(f"no scene {sid}")


async def draw(ed: Editor, sid, view: str = "scene") -> bytes:
    image = b""
    for _ in range(2):
        shot = await ed.call_raw("screenshot", {"view": view, "scene": sid, "max_width": 320})
        ed.check(not shot.is_error and shot.images, f"the {view} view draws: {shot.text[:300]}")
        image = shot.images[0][0]
    return image


async def source_export(ed: Editor, name: str) -> Path:
    out = ed.scratch.root / name
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"], allow_missing_assets=True,
                      overwrite=True, timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"source export: {str(r)[:2000]}")
    return out


def generated_sources(root: Path) -> dict:
    """{relative path: text} of the generated C++ of a source export"""
    project = root / "project"
    sources = {p.relative_to(root).as_posix(): p.read_text(errors="replace")
               for p in sorted(project.rglob("*")) if p.is_file() and p.suffix in (".cpp", ".h")}
    assert any(rel.endswith("/main.cpp") for rel in sources), f"no generated main.cpp under {project}"
    return sources


# -- a Text's font atlas ------------------------------------------------------------------------------------


def text_ui(data: bytes) -> dict:
    return components_of(yaml.safe_load(data), "Hint")["ui"]


def with_atlas_block(text: str, block: list) -> str:
    """the Text's `texture: ~` line replaced by `block` (lines under texture:), as an older editor saved it"""
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line.strip() == "name: Hint":
            for j in range(i, len(lines)):
                if lines[j].strip() == "texture: ~":
                    pad = lines[j][:len(lines[j]) - len(lines[j].lstrip())]
                    lines[j:j + 1] = [pad + "texture:"] + [pad + "  " + b for b in block]
                    return "\n".join(lines)
    raise AssertionError("no texture: ~ under the Hint GameObject")


async def atlas_id(ed: Editor, sid) -> str:
    ui = (await ed.call("get_entity", scene=sid, entity="Hint", components=["ui"], properties=["texture"]))["components"]["ui"]
    texture = ui.get("texture") or {}
    return texture.get("id", "") if isinstance(texture, dict) else ""


async def test_text_atlas_is_not_saved(ed: Editor):
    """A UI scene with a Text, drawn once (its geometry is then computed and saved, as in every scene a
    user saved), re-saved with and without its atlas built: the same bytes, the texture `~`. A file an
    older editor saved after drawing (an atlas id of its run) loads, draws and re-saves as the scene
    without it; an authored sampler setting of the atlas is kept; the generated code names no atlas"""
    path = ed.project / TEXT_PATH
    sid = (await ed.call("create_scene", name="Atlas Text", type="UI", path=TEXT_PATH))["scene"]["id"]
    await ed.call("create_ui", scene=sid, root={"type": "text", "name": "Hint", "text": "Atlas", "font_size": 30})
    await draw(ed, sid, "game")
    await ed.call("save_scene", scene=sid)
    drawn_first = path.read_bytes()
    built = await atlas_id(ed, sid)
    ed.check(ATLAS_ID.search(built), f"drawing built the Text's atlas: {built!r}")
    ed.check_eq(text_ui(drawn_first).get("texture"), None, "after the atlas is built the texture saves as none")

    # opened and saved in one batch: no frame in between, so the atlas is not built yet
    await ed.call("close_scene", scene=sid)
    made = await ed.call("batch", calls=[{"tool": "open_scene", "arguments": {"scene": TEXT_PATH}},
                                         {"tool": "save_scene", "arguments": {"scene": TEXT_PATH}}])
    ed.check_eq(made.get("failed"), 0, f"batch: {str(made)[:500]}")
    undrawn = path.read_bytes()
    ed.check(undrawn == drawn_first, f"the scene re-saves byte for byte whether or not the Text was drawn: {first_difference(drawn_first, undrawn)}")
    shot = await draw(ed, sid, "game")
    ed.check(not await modified(ed, sid), "drawing does not modify the scene")
    await ed.call("save_scene", scene=sid)
    ed.check(path.read_bytes() == drawn_first, f"drawn again: {first_difference(drawn_first, path.read_bytes())}")

    # a file an older editor saved after drawing: the atlas id of that run
    await ed.call("close_scene", scene=sid)
    old_block = ["textureType: 2D", "source: id", "id: font|30#97", "minFilter: linear", "magFilter: linear",
                 "wrapU: repeat", "wrapV: repeat", "releaseDataAfterLoad: false"]
    path.write_text(with_atlas_block(drawn_first.decode(), old_block))
    await ed.call("open_scene", scene=TEXT_PATH)
    old_shot = await draw(ed, sid, "game")
    ed.check_eq(old_shot, shot, "the old file draws as the scene without the atlas id")
    ed.check(not await modified(ed, sid), "the old file is not modified by loading and drawing it")
    own = await atlas_id(ed, sid)
    ed.check(ATLAS_ID.search(own) and own != "font|30#97", f"the Text built its own atlas: {own!r}")
    await ed.call("save_scene", scene=sid)
    ed.check(path.read_bytes() == drawn_first, f"the old file re-saves as the scene without the atlas id: "
                                               f"{first_difference(drawn_first, path.read_bytes())}")

    # an authored sampler setting of the atlas (the Inspector's texture settings) is kept
    await ed.call("close_scene", scene=sid)
    path.write_text(with_atlas_block(drawn_first.decode(), ["textureType: 2D", "source: id", "id: font|30#2", "minFilter: nearest",
                                                            "magFilter: nearest", "wrapU: clamp_to_edge", "wrapV: repeat",
                                                            "releaseDataAfterLoad: false"]))
    await ed.call("open_scene", scene=TEXT_PATH)
    await draw(ed, sid, "game")
    await ed.call("save_scene", scene=sid)
    sampler_save = path.read_bytes()
    ed.check_eq(text_ui(sampler_save).get("texture"), {"minFilter": "nearest", "magFilter": "nearest", "wrapU": "clamp_to_edge", "wrapV": "repeat"},
                "the sampler settings are kept, without the atlas id")
    await ed.call("close_scene", scene=sid)
    await ed.call("open_scene", scene=TEXT_PATH)
    await ed.call("save_scene", scene=sid)
    ed.check(path.read_bytes() == sampler_save, f"the sampler-only texture re-saves byte for byte: {first_difference(sampler_save, path.read_bytes())}")
    sources = generated_sources(await source_export(ed, "export-text"))
    text = "\n".join(sources.values())
    ed.check("font|" not in text, "the generated code names no font atlas")
    ed.check("ui.texture.setMinFilter(TextureFilter::NEAREST);" in text, "the generated code sets the authored sampler")


# -- the Scene view's icons and derived transparency ----------------------------------------------------------


async def test_scene_saves_the_same_with_or_without_the_editor_icons(ed: Editor):
    """A 3D scene with a Directional Light, a Camera and an Audio Source and no Mesh, drawn once and
    saved, then reopened and saved in one batch (no frame: the Scene view has not made its icons,
    Sprites of one submesh, in the scene's registry), then drawn and saved again: the same bytes each
    time, without a maxValues block, and drawing does not modify it"""
    rel = "Icons.scene"
    path = ed.project / rel
    sid = (await ed.call("create_scene", name="Icons", type="3D", path=rel))["scene"]["id"]
    for kind, name in (("directional_light", "Sun"), ("camera", "Eye")):
        await ed.call("create_entity", scene=sid, type=kind, name=name, select=False)
    await ed.call("create_entity", scene=sid, type="empty", components=["audiosource"], name="Voice", select=False)
    await draw(ed, sid)
    await ed.call("save_scene", scene=sid)
    first = path.read_bytes()
    await ed.call("close_scene", scene=sid)
    r = await ed.call("batch", calls=[{"tool": "open_scene", "arguments": {"scene": rel}},
                                      {"tool": "save_scene", "arguments": {"scene": rel}}])
    ed.check_eq(r.get("failed"), 0, f"batch: {[x for x in r['results'] if not x.get('ok')]}")
    undrawn = path.read_bytes()
    await draw(ed, sid)
    ed.check(not await modified(ed, sid), "drawing does not modify the scene")
    await ed.call("save_scene", scene=sid)
    drawn = path.read_bytes()
    got = [has_max_values(data) for data in (first, undrawn, drawn)]
    ed.check_eq(got, [False, False, False], "a new scene holds no maxValues (drawn, undrawn, drawn)")
    ed.check(undrawn == first and drawn == first,
             f"the scene saves the same whether or not the Scene view drew it: {first_difference(first, undrawn)}\n{first_difference(first, drawn)}")


async def test_derived_transparency_is_saved_as_a_load_starts_it(ed: Editor):
    """A cube (autoTransparency on, the default) drawn and saved; its material made blended and the
    scene saved in one batch (no frame: the mesh is not reloaded yet); then drawn (the renderer
    reloads it and makes it transparent) and saved again: the same bytes, with transparent false,
    the value every load starts from (the renderer's own is MeshComponent::derivedTransparent)"""
    rel = "Glass.scene"
    path = ed.project / rel
    sid = (await ed.call("create_scene", name="Glass", type="3D", path=rel))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="cube", name="Box", select=False)
    await draw(ed, sid)
    await ed.call("save_scene", scene=sid)
    r = await ed.call("batch", calls=[
        {"tool": "set_properties", "arguments": {"scene": sid, "entity": "Box", "values": {"mesh": {"submeshes[0].material.blendMode": "alpha"}}}},
        {"tool": "save_scene", "arguments": {"scene": sid}}])
    ed.check_eq(r.get("failed"), 0, f"batch: {[x for x in r['results'] if not x.get('ok')]}")
    blended = path.read_bytes()
    await draw(ed, sid)
    ed.check(not await modified(ed, sid), "drawing does not modify the scene")
    await ed.call("save_scene", scene=sid)
    box = components_of(yaml.safe_load(path.read_bytes()), "Box")["mesh"]
    ed.check_eq((box.get("transparent", False), box.get("autoTransparency", True)), (False, True), "saved as a load starts it (false)")
    ed.check(path.read_bytes() == blended, f"the same bytes drawn or not: {first_difference(blended, path.read_bytes())}")


# -- model loads ------------------------------------------------------------------------------------------


class delayed_loads:
    """JASPER_TEST_MODEL_LOAD_DELAY_MS for the editors started inside"""

    def __init__(self, ms: int):
        self.ms = ms

    def env(self) -> dict:
        return {DELAY_ENV: str(self.ms)}


async def test_a_save_before_the_model_loads_writes_what_one_after_it_writes():
    """A scene of a box model, drawn and saved. Closed, then opened and saved in
    one batch, so no frame has run its load: the same bytes. Then drawn and saved: the same. In an
    editor whose model parses are held on their worker for 4 s (JASPER_TEST_MODEL_LOAD_DELAY_MS),
    opened, a few frames, saved while the load still runs (the save waits for it): the same bytes,
    and the scene keeps drawing"""
    problems = []

    def expect(ok: bool, message: str) -> None:
        if not ok:
            print(f"[model-load] FAILS: {message}")
            problems.append(message)

    rel = "Load.scene"
    work = Path(tempfile.mkdtemp(prefix="model-load-"))
    try:
        async with Editor(project=str(SCENE_INPUTS), label="model-load") as ed:
            shutil.copy(BOX_SOURCE, ed.project / BOX)
            path = ed.project / rel
            sid = (await ed.call("create_scene", name="Load", type="3D", path=rel))["scene"]["id"]
            await ed.call("create_entity", scene=sid, type="model", name="Box", model=BOX, select=False)
            await ed.call("wait", frames=5)
            await draw(ed, sid)
            await ed.call("save_scene", scene=sid)
            first = path.read_bytes()
            await ed.call("save_project")
            await ed.call("close_scene", scene=sid)

            r = await ed.call("batch", calls=[{"tool": "open_scene", "arguments": {"scene": rel}},
                                              {"tool": "save_scene", "arguments": {"scene": rel}}])
            ed.check_eq(r.get("failed"), 0, f"batch: {[x for x in r['results'] if not x.get('ok')]}")
            unloaded = path.read_bytes()
            expect(unloaded == first, f"a save before the load equals one after it:\n{first_difference(first, unloaded)}")
            await draw(ed, sid)
            await ed.call("save_scene", scene=sid)
            drawn = path.read_bytes()
            expect(drawn == first, f"drawn again: the same bytes:\n{first_difference(first, drawn)}")
            project_copy = work / "project"
            shutil.copytree(ed.project, project_copy, ignore=shutil.ignore_patterns("editor.lock"))

        async with Editor(project=str(project_copy), label="model-load-delayed", env=delayed_loads(4000).env()) as ed:
            path = ed.project / rel
            # opened now, so its load starts now (the project may have opened it at start)
            await ed.call("create_scene", name="Holder", type="2D")
            for scene in (await ed.call("list_scenes"))["scenes"]:
                if scene.get("path") == rel and scene.get("opened"):
                    await ed.call("close_scene", scene=scene["id"], unsaved="discard")
            sid = (await ed.call("open_scene", scene=rel))["scene"]["id"]
            await ed.call("wait", frames=5)
            t0 = time.monotonic()
            await ed.call("save_scene", scene=sid)
            took = time.monotonic() - t0
            pending = path.read_bytes()
            print(f"[model-load] saved with the load held back in {took:.2f} s")
            expect(pending == first, f"a save while the load runs equals one after it:\n{first_difference(first, pending)}")
            await draw(ed, sid)
            await ed.call("save_scene", scene=sid)
            expect(path.read_bytes() == first, f"after the load: the same bytes:\n{first_difference(first, path.read_bytes())}")

    finally:
        shutil.rmtree(work, ignore_errors=True)
    assert not problems, "\n".join(problems)


# -- a Missing Prefab's data -------------------------------------------------------------------------------


async def test_added_gameobject_of_a_missing_prefab_resaves_alike(ed: Editor):
    """A cube added below an instance's child (an added GameObject override), the scene saved; opened
    with the prefab gone (the instance data kept as read), saved: the bytes of the first, the cube's
    empty sequences in the one style"""
    h = await History.create(ed, "Missing Data", "missing")
    pa = await two_level_prefab(h, "Pa", "cube", "sphere")
    await ed.call("instantiate_prefab", scene=h.sid, prefab=pa, name="I1")
    await h.entity("cube", "Added", parent="I1/Pa Child")
    await h.entity("cube", "Tail")
    before = await h.save()
    await ed.call("save_project")
    await ed.call("close_scene", scene=h.sid)
    prefab_file = ed.project / pa
    kept = prefab_file.read_bytes()
    prefab_file.unlink()
    await ed.call("sync_files")
    h.sid = (await ed.call("open_scene", scene=h.rel))["scene"]["id"]
    info = await ed.call("get_prefab_info", scene=h.sid, entity="I1", usage=False)
    ed.check(info.get("missing_prefab"), f"the instance is a Missing Prefab: {info}")
    missing = await h.save()
    prefab_file.write_bytes(kept)
    ed.check(missing == before, f"the Missing Prefab's scene as before:\n{first_difference(before, missing)}")


# -- the Default Camera's generated code ---------------------------------------------------------------------


async def test_default_camera_code_does_not_follow_the_editor_view(ed: Editor):
    """A 3D scene without a camera of its own (the editor's Default Camera, which follows the Scene
    view's size): two source exports with the Scene view split between them generate the same
    sources (the game sizes that camera to its own canvas)"""
    rel = "No Camera.scene"
    sid = (await ed.call("create_scene", name="No Camera", type="3D", path=rel))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="cube", name="Box", select=False)
    await ed.call("select_scene", scene=sid)
    await draw(ed, sid)
    first = generated_sources(await source_export(ed, "camera-1"))
    await ed.call("set_scene_view_layout", scene=sid, layout="split")
    await draw(ed, sid)
    second = generated_sources(await source_export(ed, "camera-2"))
    await ed.call("set_scene_view_layout", scene=sid, layout="single")
    differ = {}
    for name, text in first.items():
        if second.get(name) != text:
            a, b = text.splitlines(), (second.get(name) or "").splitlines()
            differ[name] = [f"{x!r} / {y!r}" for x, y in zip(a, b) if x != y][:6]
    ed.check_eq(sorted(first), sorted(second), "the same generated files")
    ed.check_eq(differ, {}, "two exports with another editor view size generate the same sources")
