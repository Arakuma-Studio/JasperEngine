"""FBX imports never block the tool server, and a pending import is no failure (S98 REQ-32, REQ-33,
AC25, AC26). A project whose scene places three FBX models is copied without its import cache (a
fresh clone): opening the scene queues the three imports on the import queue's Background job, each
held 5 s (JASPER_TEST_FBX_IMPORT_DELAY_MS). Meanwhile editor_status, get_scene_tree and read_console
answer, a model created from a fourth FBX waits for its import without holding the other calls, the
models appear once their imports land, and the Console has one "Importing N models" line and no
model load error. editor_status lists the import job with done/total.

The model is editor/tests/fixtures/rigged_cube.fbx, the gtest FbxImportTest's handwritten ASCII FBX.
"""

from __future__ import annotations

import asyncio
import shutil
import struct
import tempfile
import time
import zlib
from pathlib import Path

from harness import Editor, over_budget

CI = True
EDITOR = {"create": True}
TIMEOUT = 900
REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "editor" / "tests" / "fixtures" / "rigged_cube.fbx"
DELAY_ENV = "JASPER_TEST_FBX_IMPORT_DELAY_MS"
SCENE = "scenes/Imports.scene"
MODELS = ("A", "B", "C")
S: dict = {}


def write_png(path: Path, size: int = 4) -> None:
    """A small grey checker (the texture the FBX links), written without any image library"""
    rows = b""
    for y in range(size):
        rows += b"\x00" + b"".join(bytes([255 if (x + y) % 2 else 64] * 3) for x in range(size))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


async def wait_until(ed: Editor, what: str, probe, timeout: float = 120.0):
    deadline = time.monotonic() + timeout
    while True:
        value = await probe()
        if value:
            return value
        if time.monotonic() > deadline:
            ed.fail(f"timed out waiting for {what}")
        await asyncio.sleep(0.2)


async def project_copy() -> Path:
    """A copy of a project whose scene places three FBX models, without the import and cook caches"""
    if "source" not in S:
        S["work"] = Path(tempfile.mkdtemp(prefix="import-blocking-"))
        async with Editor(create=True, name="Import Blocking", label="import-source") as ed:
            summary = await ed.call("get_project_summary")
            assets = summary["assets_dir"].rstrip("/") or "."
            S["models"] = f"{assets}/models" if assets != "." else "models"
            folder = ed.project / S["models"]
            for name in ("a", "b", "c", "d"):
                folder.mkdir(parents=True, exist_ok=True)
                (folder / f"{name}.fbx").write_bytes(FIXTURE.read_bytes())
            write_png(folder / "textures" / "checker.png")
            await ed.call("sync_files")
            sid = (await ed.call("create_scene", name="Imports", type="3d", path=SCENE))["scene"]["id"]
            for name in MODELS:
                await ed.call("create_entity", scene=sid, type="model", name=name, model=f"{S['models']}/{name.lower()}.fbx",
                              select=False)
            await ed.call("save_scene", scene=sid)
            await ed.call("close_scene", scene=sid)
            await ed.call("save_project")
            S["source"] = S["work"] / "source"
            shutil.copytree(ed.project, S["source"], ignore=shutil.ignore_patterns("editor.lock"))
        for cache in ("imported", "cache"):
            shutil.rmtree(S["source"] / ".jasperengine" / cache, ignore_errors=True)
    S["copies"] = S.get("copies", 0) + 1
    copy = S["work"] / f"copy{S['copies']}"
    shutil.copytree(S["source"], copy)
    return copy


def import_job(status: dict) -> dict | None:
    return next((job for job in status["jobs"] if job["kind"] == "model_import"), None)


async def models_loaded(ed: Editor, sid) -> bool:
    for name in MODELS:
        info = await ed.call("get_model_info", scene=sid, entity=name)
        if info["morph_targets"] != ["Bulge"]:
            return False
    return True


async def test_tools_answer_during_import():
    """AC25. Red before the change (the delay hook alone): create_entity converts the FBX on the main
    thread, so the calls behind it answer only once it is done, and no import job is listed"""
    project = await project_copy()
    async with Editor(project=str(project), label="import-delayed", env={DELAY_ENV: "5000"}) as ed:
        started = time.monotonic()
        sid = (await ed.call("open_scene", scene=SCENE))["scene"]["id"]
        took = {"open_scene": time.monotonic() - started}
        # a model whose FBX was never imported: the call waits for the import, the others do not wait for it
        create = asyncio.ensure_future(ed.call("create_entity", scene=sid, type="model", name="D",
                                               model=f"{S['models']}/d.fbx", select=False, timeout=300))
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

        await wait_until(ed, "the scene's models to appear once imported", lambda: models_loaded(ed, sid))
        created = await create
        ed.check("entity" in created, f"{created!r}")
        info = await ed.call("get_model_info", scene=sid, entity="D")
        ed.check(not info["loading"] and info["morph_targets"] == ["Bulge"], f"the created model loaded: {info!r}")
        ed.check(over_budget("tool calls during a 20 s FBX import", took, 1.0) == {}, "over budget")


async def test_pending_import_logs_no_error():
    """AC26. Red before the change: each model logs "Model file not found" and "Model load failed,
    not retrying" before its import lands, and no import job is listed"""
    project = await project_copy()
    async with Editor(project=str(project), label="import-fresh", env={DELAY_ENV: "1000"}) as ed:
        cursor = (await ed.call("read_console", limit=1)).get("next_cursor")
        sid = (await ed.call("open_scene", scene=SCENE))["scene"]["id"]
        jobs = []

        async def loaded():
            job = import_job(await ed.call("editor_status"))
            if job:
                jobs.append(job)
            return await models_loaded(ed, sid)

        await wait_until(ed, "the scene's models to appear once imported", loaded)

        args = {"since": cursor} if cursor is not None else {}
        problems = (await ed.call("read_console", only_problems=True, limit=1000, **args))["entries"]
        for entry in problems:
            ed.check("Model file not found" not in entry["message"] and "not retrying" not in entry["message"],
                     f"a pending import is no failure: {entry['message']}")
        lines = (await ed.call("read_console", regex=r"Importing \d+ models? \(first open\)", limit=1000, **args))["entries"]
        ed.check_eq(len(lines), 1, f"one line for the batch: {[e['message'] for e in lines]}")
        ed.check_in(f"Importing {len(MODELS)} models (first open): they appear when ready", lines[0]["message"] if lines else "")
        ed.check(jobs, "editor_status listed the model_import job while it ran")
        ed.check(all(0 <= job["done"] <= job["total"] for job in jobs), f"done/total: {jobs}")
        ed.check_eq(max((job["total"] for job in jobs), default=0), len(MODELS), f"the job counts the scene's imports: {jobs}")
