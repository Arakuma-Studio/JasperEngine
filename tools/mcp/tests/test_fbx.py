"""FBX import (ufbx): an .fbx in the assets is converted into a .glb in .jasperengine/imported and
loads as a glTF while the scene keeps its .fbx path. import_files converts what it copies,
get_asset_info summarizes the conversion, set_model loads it (mesh, two bones, an animation,
a blend shape), a saved scene records which import its model parts came from, an edit of the
FBX reloads the scene's model, and an export ships each FBX as its cook in je-cooked/models, under
its .fbx path (S53 R12: the .fbx and its .fbx.glb import stay behind). A fresh clone's FBX imports
run in the background and never block the tool server (S98 REQ-32, REQ-33).

The model is editor/tests/fixtures/rigged_cube.fbx, the gtest FbxImportTest's handwritten ASCII FBX.
"""

from __future__ import annotations

import asyncio
import json
import shutil
import struct
import tempfile
import time
import zlib
from pathlib import Path

import yaml

from harness import Editor, find_key, over_budget

EDITOR = {"create": True, "name": "FBX Test"}
CI = True
# the background-import tests start three editors of their own
TIMEOUT = 900
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
    # a source export carries the scenes cooked, as the prebuilt player reads them (S98 WP-1)
    scenes = list((out / "project" / "content" / "scenes").glob("*.jscn"))
    ed.check(any(b"models/rigged_cube.fbx" in p.read_bytes() for p in scenes),
             f"the cooked scene loads the model by its .fbx path: {[p.name for p in scenes]}")


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

# ---- FBX imports never block the tool server (S98 REQ-32, REQ-33, AC25, AC26) ----
# A project whose scene places three FBX models is copied without its import cache (a fresh clone):
# opening the scene queues the three imports on the import queue's Background job, each held 5 s
# (JASPER_TEST_FBX_IMPORT_DELAY_MS). Meanwhile editor_status, get_scene_tree and read_console answer,
# a model created from a fourth FBX waits for its import without holding the other calls, the
# models appear once their imports land, and the Console has one "Importing N models" line and no
# model load error. editor_status lists the import job with done/total.

DELAY_ENV = "JASPER_TEST_FBX_IMPORT_DELAY_MS"
IMPORT_SCENE = "scenes/Imports.scene"
IMPORT_MODELS = ("A", "B", "C")
IMPORTS: dict = {}


async def import_project_copy() -> Path:
    """A copy of a project whose scene places three FBX models, without the import and cook caches"""
    if "source" not in IMPORTS:
        IMPORTS["work"] = Path(tempfile.mkdtemp(prefix="import-blocking-"))
        async with Editor(create=True, name="Import Blocking", label="import-source") as ed:
            summary = await ed.call("get_project_summary")
            assets = summary["assets_dir"].rstrip("/") or "."
            IMPORTS["models"] = f"{assets}/models" if assets != "." else "models"
            folder = ed.project / IMPORTS["models"]
            for name in ("a", "b", "c", "d"):
                folder.mkdir(parents=True, exist_ok=True)
                (folder / f"{name}.fbx").write_bytes(FIXTURE.read_bytes())
            write_png(folder / "textures" / "checker.png")
            await ed.call("sync_files")
            sid = (await ed.call("create_scene", name="Imports", type="3d", path=IMPORT_SCENE))["scene"]["id"]
            for name in IMPORT_MODELS:
                await ed.call("create_entity", scene=sid, type="model", name=name, model=f"{IMPORTS['models']}/{name.lower()}.fbx",
                              select=False)
            await ed.call("save_scene", scene=sid)
            await ed.call("close_scene", scene=sid)
            await ed.call("save_project")
            IMPORTS["source"] = IMPORTS["work"] / "source"
            shutil.copytree(ed.project, IMPORTS["source"], ignore=shutil.ignore_patterns("editor.lock"))
        for cache in ("imported", "cache"):
            shutil.rmtree(IMPORTS["source"] / ".jasperengine" / cache, ignore_errors=True)
    IMPORTS["copies"] = IMPORTS.get("copies", 0) + 1
    copy = IMPORTS["work"] / f"copy{IMPORTS['copies']}"
    shutil.copytree(IMPORTS["source"], copy)
    return copy


def import_job(status: dict) -> dict | None:
    return next((job for job in status["jobs"] if job["kind"] == "model_import"), None)


async def import_models_loaded(ed: Editor, sid) -> bool:
    for name in IMPORT_MODELS:
        info = await ed.call("get_model_info", scene=sid, entity=name)
        if info["morph_targets"] != ["Bulge"]:
            return False
    return True


async def test_tools_answer_during_import():
    """AC25. Red before the change (the delay hook alone): create_entity converts the FBX on the main
    thread, so the calls behind it answer only once it is done, and no import job is listed"""
    project = await import_project_copy()
    async with Editor(project=str(project), label="import-delayed", env={DELAY_ENV: "5000"}) as ed:
        started = time.monotonic()
        sid = (await ed.call("open_scene", scene=IMPORT_SCENE))["scene"]["id"]
        took = {"open_scene": time.monotonic() - started}
        # a model whose FBX was never imported: the call waits for the import, the others do not wait for it
        create = asyncio.ensure_future(ed.call("create_entity", scene=sid, type="model", name="D",
                                               model=f"{IMPORTS['models']}/d.fbx", select=False, timeout=300))
        await asyncio.sleep(0.5)
        for tool, args in (("editor_status", {}), ("get_scene_tree", {"scene": sid}), ("read_console", {"limit": 20})):
            started = time.monotonic()
            await ed.call(tool, **args)
            took[tool] = time.monotonic() - started
            ed.check(not create.done(), f"{tool} answered while create_entity waits for its FBX's import")

        raw = await ed.call_raw("editor_status")
        job = import_job(raw.value)
        ed.check(job is not None and job["done"] < job["total"], f"the import job runs: {raw.value['jobs']}")
        ed.check_in("Importing models:", raw.text, "the status text names the import")

        await wait_until(ed, "the scene's models to appear once imported", lambda: import_models_loaded(ed, sid), timeout=120.0)
        created = await create
        ed.check("entity" in created, f"{created!r}")
        info = await ed.call("get_model_info", scene=sid, entity="D")
        ed.check(not info["loading"] and info["morph_targets"] == ["Bulge"], f"the created model loaded: {info!r}")
        ed.check(over_budget("tool calls during a 20 s FBX import", took, 1.0) == {}, "over budget")


async def test_pending_import_logs_no_error():
    """AC26. Red before the change: each model logs "Model file not found" and "Model load failed,
    not retrying" before its import lands, and no import job is listed"""
    project = await import_project_copy()
    async with Editor(project=str(project), label="import-fresh", env={DELAY_ENV: "1000"}) as ed:
        cursor = (await ed.call("read_console", limit=1)).get("next_cursor")
        sid = (await ed.call("open_scene", scene=IMPORT_SCENE))["scene"]["id"]
        jobs = []

        async def loaded():
            job = import_job(await ed.call("editor_status"))
            if job:
                jobs.append(job)
            return await import_models_loaded(ed, sid)

        await wait_until(ed, "the scene's models to appear once imported", loaded, timeout=120.0)

        args = {"since": cursor} if cursor is not None else {}
        problems = (await ed.call("read_console", only_problems=True, limit=1000, **args))["entries"]
        for entry in problems:
            ed.check("Model file not found" not in entry["message"] and "not retrying" not in entry["message"],
                     f"a pending import is no failure: {entry['message']}")
        lines = (await ed.call("read_console", regex=r"Importing \d+ models? \(first open\)", limit=1000, **args))["entries"]
        # the scene's models in one batch, one line; the model cook queue's import of the unused d.fbx
        # joins it, or starts a batch of its own when it comes after the line was written
        messages = [e["message"] for e in lines]
        scene_batch = [m for m in messages if any(f"Importing {n} models (first open): they appear when ready" in m
                                                  for n in (len(IMPORT_MODELS), len(IMPORT_MODELS) + 1))]
        ed.check_eq(len(scene_batch), 1, f"one line for the scene's batch: {messages}")
        ed.check(len(messages) == 1 or (len(messages) == 2 and f"Importing {len(IMPORT_MODELS)} models" in scene_batch[0]
                                        and any("Importing 1 model (first open)" in m for m in messages)),
                 f"at most d.fbx's own line besides it: {messages}")
        ed.check(jobs, "editor_status listed the model_import job while it ran")
        ed.check(all(0 <= job["done"] <= job["total"] for job in jobs), f"done/total: {jobs}")
        # (the model cook queue's import of the unused d.fbx can join the batch)
        ed.check(max((job["total"] for job in jobs), default=0) >= len(IMPORT_MODELS), f"the job counts the scene's imports: {jobs}")
