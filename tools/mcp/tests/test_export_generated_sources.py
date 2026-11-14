"""An export and the editor's cooked cache (S30, S98 REQ-2): a change the editor picks up while a
build exports (a prefab a closed scene uses, edited by another program: the asset database
invalidates that scene's cooked copy) is cooked again once the export has finished, and the export
writes nothing into the editor's generated folder.

The export is held before it reads the scenes by the editor's test hook JASPER_TEST_EXPORT_HOLD=<path>
(it creates <path>.held and waits while <path> exists), so nothing here depends on timing.
"""

from __future__ import annotations

import asyncio
import os
import tempfile
from pathlib import Path

from harness import Editor
from scene_history import SCENE_INPUTS, History, two_level_prefab

TIMEOUT = 1800
CI = True


def generated_files(project: Path) -> dict[str, bytes]:
    root = project / ".jasperengine" / "generated"
    if not root.is_dir():
        return {}
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*") if p.is_file()}


def cooked_scene(project: Path, scene_id: int) -> Path:
    return project / ".jasperengine" / "cooked" / "scenes" / f"{scene_id}.jscn"


async def source_export(ed: Editor, out: Path, **args):
    return await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"], allow_missing_assets=True,
                         overwrite=True, timeout_seconds=900, **args)


async def wait_file(path: Path, seconds: float) -> bool:
    for _ in range(int(seconds / 0.05)):
        if path.exists():
            return True
        await asyncio.sleep(0.05)
    return path.exists()


async def test_no_regeneration_while_an_export_runs():
    """An export held before it reads the scenes; a prefab a closed scene uses edited on disk
    meanwhile, and read-only calls let frames run. Once released the export succeeds without
    touching the generated folder, and after it the closed scene is cooked again"""
    hold = Path(tempfile.gettempdir()) / f"jasper-export-hold-{os.getpid()}"
    held = Path(str(hold) + ".held")
    try:
        async with Editor(project=str(SCENE_INPUTS), label="export-hold", env={"JASPER_TEST_EXPORT_HOLD": str(hold)}) as ed:
            h = await History.create(ed, "Closed", "closed")
            pa = await two_level_prefab(h, "Pa", "cube", "sphere")
            await ed.call("instantiate_prefab", scene=h.sid, prefab=pa, name="I1")
            await h.save()
            await ed.call("save_project")
            # the first export; the project's build exists after the save
            first = await source_export(ed, ed.scratch.root / "export-1")
            ed.check_eq(first["status"], "succeeded", f"first export: {first}")
            ed.check((ed.project / "CMakeLists.txt").exists(), "the save created the project's build")
            ed.check(cooked_scene(ed.project, h.sid).is_file(), "the save cooked the scene for the project's build")
            await ed.call("close_scene", scene=h.sid)
            generated = generated_files(ed.project)

            hold.write_text("hold\n")
            held.unlink(missing_ok=True)
            out = ed.scratch.root / "export-2"
            job = await source_export(ed, out, wait=False)
            job_id = job.get("job_id") or job.get("id")
            ed.check(job_id, f"build_project with wait false gives a job: {job}")
            ed.check(await wait_file(held, 300), "the export reached the hold")

            # another program edits the prefab: the watcher's batch, the database's walk and the
            # closed scene's cooked copy invalidated land in the frames the read-only calls run
            prefab_file = ed.project / pa
            prefab_file.write_bytes(prefab_file.read_bytes().replace(b"Pa Child", b"Pa Kid"))
            for _ in range(20):
                await asyncio.sleep(0.2)
                status = await ed.call_raw("editor_status")
                ed.check(not status.is_error, status.text[:300])

            hold.unlink()
            done = None
            for _ in range(1800):
                done = await ed.call("get_job", job_id=job_id)
                if done.get("status") not in ("running", "queued", "pending"):
                    break
                await asyncio.sleep(0.25)
            ed.check_eq(done.get("status"), "succeeded", f"the held export finishes: {done}")
            ed.check_eq(sorted(generated_files(ed.project)), sorted(generated), "the export writes nothing into the generated folder")
            for _ in range(30):
                await ed.call("wait", frames=5)
                if cooked_scene(ed.project, h.sid).is_file():
                    break
            ed.check(cooked_scene(ed.project, h.sid).is_file(), "after the export the closed scene is cooked again")
            console = await ed.call("read_console", contains="cooked", limit=50)
            problems = [e["message"] for e in console.get("entries", []) if "Failed" in e.get("message", "")]
            ed.check_eq(problems, [], "no failure around the cooked cache")
    finally:
        hold.unlink(missing_ok=True)
        held.unlink(missing_ok=True)
