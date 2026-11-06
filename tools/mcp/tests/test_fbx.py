"""FBX import (ufbx): an .fbx in the assets is converted into a .glb in .jasperengine/imported and
loads as a glTF while the scene keeps its .fbx path. import_files converts what it copies,
get_asset_info summarizes the conversion, set_model loads it (mesh, two bones, an animation,
a blend shape), a saved scene records which import its model parts came from, an edit of the
FBX reloads the scene's model, and an export ships each FBX as its cook in je-cooked/models, under
its .fbx path (S53 R12: the .fbx and its .fbx.glb import stay behind).

The model is editor/tests/fixtures/rigged_cube.fbx, the gtest FbxImportTest's handwritten ASCII FBX.
"""

from __future__ import annotations

import asyncio
import json
import shutil
import struct
import time
import zlib
from pathlib import Path

import yaml

from harness import Editor, find_key

EDITOR = {"create": True, "name": "FBX Test"}
CI = True
REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "editor" / "tests" / "fixtures" / "rigged_cube.fbx"
GLTF = REPO / "tools" / "fixtures" / "shader-library-export" / "assets" / "models" / "box.gltf"
S: dict = {}


def write_png(path: Path, size: int = 4) -> None:
    """A small grey checker, written without any image library"""
    rows = b""
    for y in range(size):
        rows += b"\x00" + b"".join(bytes([255 if (x + y) % 2 else 64] * 3) for x in range(size))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


async def wait_until(ed: Editor, what: str, probe, timeout: float = 30.0):
    deadline = time.monotonic() + timeout
    while True:
        value = await probe()
        if value:
            return value
        if time.monotonic() > deadline:
            ed.fail(f"timed out waiting for {what}")
        await asyncio.sleep(0.5)


async def test_import_files_converts_the_fbx(ed: Editor):
    ed.require_tools("import_files", "get_project_summary")
    summary = await ed.call("get_project_summary")
    assets = summary["assets_dir"].rstrip("/") or "."
    S["assets"] = ed.project / assets
    S["models"] = f"{assets}/models" if assets != "." else "models"
    S["fbx"] = f"{S['models']}/rigged_cube.fbx"

    incoming = ed.scratch.root / "incoming"
    incoming.mkdir(parents=True, exist_ok=True)
    (incoming / "rigged_cube.fbx").write_bytes(FIXTURE.read_bytes())
    write_png(incoming / "textures" / "checker.png")

    r = await ed.call("import_files", sources=[str(incoming / "rigged_cube.fbx"), str(incoming / "textures")],
                      to_dir=S["models"])
    ed.check_eq(r.get("fbx_imports"), [S["fbx"]], f"{r!r}")
    ed.check("fbx_import_failed" not in r, f"{r!r}")
    S["glb"] = ed.project / ".jasperengine" / "imported" / "models" / "rigged_cube.fbx.glb"
    ed.check(S["glb"].is_file(), f"no import at {S['glb']}")
    ed.check(S["glb"].read_bytes()[:4] == b"glTF", "the import is a binary glTF")
    ed.check(not (S["assets"] / "models" / "rigged_cube.fbx.glb").exists(), "the import stays out of the assets")


async def test_asset_info_summarizes_the_import(ed: Editor):
    info = await ed.call("get_asset_info", path=S["fbx"])
    ed.check_eq(info["type"], "model")
    ed.check_eq(info["fbx_import"]["state"], "current", f"{info!r}")
    ed.check(info["fbx_import"]["path"].endswith("rigged_cube.fbx.glb"), f"{info!r}")
    model = info["model"]
    ed.check_eq(len(model["meshes"]), 1, f"{model!r}")
    ed.check_eq(model["skins"], 1)
    ed.check_eq([a["name"] for a in model["animations"]], ["Take 001"])
    ed.check_eq(model["materials"], ["Checker"])
    ed.check_eq(model["images"], ["textures/checker.png"], "the texture is linked, relative to the FBX")


async def test_asset_info_does_not_convert(ed: Editor):
    # read only: an FBX nothing loaded yet is reported, not converted. In a folder of its own,
    # which the Project window does not list: its thumbnail would convert it.
    fresh = S["assets"] / "unlisted" / "fresh.fbx"
    fresh.parent.mkdir(parents=True, exist_ok=True)
    fresh.write_bytes(FIXTURE.read_bytes())
    rel = fresh.relative_to(ed.project).as_posix()
    info = await ed.call("get_asset_info", path=rel)
    ed.check_eq(info["fbx_import"]["state"], "missing", f"{info!r}")
    ed.check_in("Not converted yet", info.get("error", ""))
    glb = ed.project / ".jasperengine" / "imported" / "unlisted" / "fresh.fbx.glb"
    ed.check(not glb.exists(), "get_asset_info wrote no conversion")
    # (with its asset identity's .meta sidecar, S04)
    shutil.rmtree(fresh.parent)


async def test_set_model_loads_the_fbx(ed: Editor):
    ed.require_tools("set_model", "create_scene")
    made = await ed.call("create_scene", name="FbxWorld", type="3d")
    S["scene"] = made["scene"]["id"]
    r = await ed.call("set_model", scene=S["scene"], create={"name": "Rig"}, path=S["fbx"])
    S["rig"] = find_key(r["model"], "id", "entity_id")
    ed.check_eq(r["file"], S["fbx"], "the model keeps its .fbx path")
    ed.check(not r["loading"], "loaded")
    ed.check(len(r["meshes"]) >= 1, f"{r!r}")
    ed.check_eq(r["bone_count"], 2, f"{r!r}")
    ed.check_eq(len(r["animations"]), 1, f"{r!r}")
    ed.check_eq(r["morph_targets"], ["Bulge"], f"{r!r}")


async def test_gltf_models_still_load(ed: Editor):
    # the glTF path the FBX import shares (MeshSystem::loadGLTFFile)
    (S["assets"] / "models" / "box.gltf").write_bytes(GLTF.read_bytes())
    await ed.call("sync_files", paths=[f"{S['models']}/box.gltf"])
    r = await ed.call("set_model", scene=S["scene"], create={"name": "Box"}, path=f"{S['models']}/box.gltf")
    ed.check(not r["loading"] and len(r["meshes"]) >= 1, f"{r!r}")
    S["box"] = find_key(r["model"], "id", "entity_id")


async def test_saved_scene_records_the_import(ed: Editor):
    saved = await ed.call("save_scene", scene=S["scene"], path="scenes/FbxWorld")
    S["path"] = saved["scene"]["path"]
    data = yaml.safe_load((ed.project / S["path"]).read_text())
    rel = S["fbx"][len(S["assets"].relative_to(ed.project).as_posix()) + 1:] if S["fbx"].startswith(
        S["assets"].relative_to(ed.project).as_posix() + "/") else S["fbx"]
    imports = data.get("imports") or {}
    ed.check(rel in imports, f"scene imports {imports!r} has no {rel}")
    S["import_id"] = imports.get(rel)


async def test_fbx_edit_reloads_the_model(ed: Editor):
    # an edit undone before the FBX changes can still be redone after the reload
    await ed.call("rename_entity", scene=S["scene"], entity=S["box"], name="Crate")
    await ed.call("undo", scene=S["scene"])
    fbx = ed.project / S["fbx"]
    fbx.write_text(fbx.read_text().replace("SubDeformer::Bulge", "SubDeformer::Swell"))
    before = S["glb"].stat().st_mtime_ns

    async def renamed():
        info = await ed.call("get_model_info", scene=S["scene"], entity=S["rig"])
        return info if info["morph_targets"] == ["Swell"] and not info["loading"] else None

    info = await wait_until(ed, "the scene's model to reload the edited FBX", renamed)
    ed.check_eq(info["bone_count"], 2)
    ed.check(S["glb"].stat().st_mtime_ns != before, "reimported")

    r = await ed.call("redo", scene=S["scene"])
    box = await ed.call("get_entity", scene=S["scene"], entity=S["box"], detail="summary")
    ed.check_eq(box["name"], "Crate", f"the redo step survived the automatic reload: {r!r}")
    info = await ed.call("get_model_info", scene=S["scene"], entity=S["rig"])
    ed.check_eq(info["morph_targets"], ["Swell"], "the redo leaves the reloaded model alone")

    await ed.call("save_scene", scene=S["scene"])
    data = yaml.safe_load((ed.project / S["path"]).read_text())
    ids = list((data.get("imports") or {}).values())
    ed.check(ids and ids[0] != S["import_id"], f"the saved import id follows the edit: {ids!r}")


async def test_play_imports_every_fbx(ed: Editor):
    # scripts can load any FBX by path, so Play converts those no scene uses too
    (S["assets"] / "models" / "scripted.fbx").write_bytes(FIXTURE.read_bytes())
    await ed.call("play", action="start", scene=S["scene"], timeout_seconds=120)
    try:
        info = await ed.call("get_model_info", scene=S["scene"], entity=S["rig"])
        ed.check_eq(info["bone_count"], 2, f"the model plays: {info!r}")
    finally:
        await ed.call("play", action="stop")
    ed.check((S["glb"].parent / "scripted.fbx.glb").is_file(), "Play imported the FBX no scene uses")
    (S["assets"] / "models" / "scripted.fbx").unlink()


async def test_source_export_ships_the_imports(ed: Editor):
    ed.require_tools("build_project", "edit_build_scenes")
    # an FBX no scene uses still ships: scripts can load any of them by path
    (S["assets"] / "models" / "unused.fbx").write_bytes(FIXTURE.read_bytes())
    await ed.call("sync_files", paths=[f"{S['models']}/unused.fbx"])
    await ed.call("edit_build_scenes", action="add", scene=S["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=S["path"])

    out = ed.scratch.root / "export-fbx"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"{r!r}")
    assets = out / "project" / "assets"
    if not assets.is_dir():
        assets = next(p for p in out.rglob("assets") if p.is_dir())
    shipped = assets / "models"
    cooked = assets / "je-cooked" / "models"
    models = json.loads((cooked / "index.json").read_text())["models"]
    for fbx in ("models/rigged_cube.fbx", "models/unused.fbx"):
        ed.check(fbx in models, f"{fbx} ships cooked: {sorted(models)}")
        for family, entry in models.get(fbx, {}).items():
            ed.check((cooked / entry["file"]).read_bytes()[:4] == b"glTF", f"{fbx}: its {family} cook is a binary glTF")
    for name in ("rigged_cube.fbx", "unused.fbx", "rigged_cube.fbx.glb", "unused.fbx.glb"):
        ed.check(not (shipped / name).exists(), f"{name} is not shipped: the cook takes its place")
    # its linked texture ships cooked (S14 R9: a glTF's external image loads its cook; a Linux
    # export ships the desktop family, the source stays out)
    index = json.loads((assets / "je-cooked" / "desktop" / "index.json").read_text())["textures"]
    ed.check("models/textures/checker.png" in index, f"its linked texture ships cooked: {sorted(index)}")
    ed.check((assets / "je-cooked" / "desktop" / index["models/textures/checker.png"]["file"]).is_file(),
             "its cooked file ships")
    ed.check(not (shipped / "textures" / "checker.png").exists(), "the cooked source itself is not shipped")
    sources = [p for p in out.rglob("*.cpp") if "rigged_cube.fbx" in p.read_text(errors="ignore")]
    ed.check(sources, "the scene source loads the model by its .fbx path")


async def test_an_asset_at_the_import_path_fails_the_export(ed: Editor):
    clash = S["assets"] / "models" / "unused.fbx.glb"
    clash.write_bytes(b"glTF")
    try:
        await ed.call("sync_files", paths=[f"{S['models']}/unused.fbx.glb"])
        msg = await ed.call("build_project", mode="source", output_dir=str(ed.scratch.root / "export-fbx-clash"),
                            platform="linux", timeout_seconds=900, expect_error=True)
        ed.check_in("models/unused.fbx.glb would replace the import of models/unused.fbx", msg)
    finally:
        clash.unlink()
        await ed.call("sync_files", paths=[f"{S['models']}/unused.fbx.glb"])
