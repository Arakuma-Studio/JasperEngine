"""An export and the editor's regeneration of the generated sources never run at once (S30).

A change the editor picks up while a build exports (a prefab a closed scene uses, edited by another
program: the asset database invalidates that scene's generated source) must not make the main
thread write .jasperengine/generated while the export thread clears and copies it ("Failed to clear
generated directory: Directory not empty"); the change is regenerated once the export has finished.

The export is held at its most exposed point, right after it cleared the folder, by the editor's
test hook JASPER_TEST_EXPORT_HOLD=<path> (it creates <path>.held and waits while <path> exists), so
nothing here depends on timing.
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


def generated_files(project: Path) -> list[str]:
    root = project / ".jasperengine" / "generated"
    if not root.is_dir():
        return []
    return sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file())


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
    """An export held right after its clear; a prefab a closed scene uses edited on disk meanwhile,
    and read-only calls let frames run. The generated folder stays as the export left it; once
    released the export succeeds, and after it the closed scene's source is in the folder again"""
    hold = Path(tempfile.gettempdir()) / f"jasper-export-hold-{os.getpid()}"
    held = Path(str(hold) + ".held")
    try:
        async with Editor(project=str(SCENE_INPUTS), label="export-hold", env={"JASPER_TEST_EXPORT_HOLD": str(hold)}) as ed:
            h = await History.create(ed, "Closed", "closed")
            pa = await two_level_prefab(h, "Pa", "cube", "sphere")
            await ed.call("instantiate_prefab", scene=h.sid, prefab=pa, name="I1")
            await h.save()
            await ed.call("save_project")
            # the first export creates the build (the regeneration waits for its CMakeLists.txt)
            first = await source_export(ed, ed.scratch.root / "export-1")
            ed.check_eq(first["status"], "succeeded", f"first export: {first}")
            ed.check((ed.project / "CMakeLists.txt").exists(), "the first export created the project's build")
            await ed.call("close_scene", scene=h.sid)

            hold.write_text("hold\n")
            held.unlink(missing_ok=True)
            out = ed.scratch.root / "export-2"
            job = await source_export(ed, out, wait=False)
            job_id = job.get("job_id") or job.get("id")
            ed.check(job_id, f"build_project with wait false gives a job: {job}")
            ed.check(await wait_file(held, 300), "the export reached the hold (right after its clear)")
            ed.check_eq(generated_files(ed.project), [], "the export cleared the generated folder")

            # another program edits the prefab: the watcher's batch, the database's walk and the
            # closed scene's source invalidated land in the frames the read-only calls run
            prefab_file = ed.project / pa
            prefab_file.write_bytes(prefab_file.read_bytes().replace(b"Pa Child", b"Pa Kid"))
            for _ in range(20):
                await asyncio.sleep(0.2)
                status = await ed.call_raw("editor_status")
                ed.check(not status.is_error, status.text[:300])
            ed.check_eq(generated_files(ed.project), [], "nothing regenerated the folder while the export holds it")

            hold.unlink()
            done = None
            for _ in range(1800):
                done = await ed.call("get_job", job_id=job_id)
                if done.get("status") not in ("running", "queued", "pending"):
                    break
                await asyncio.sleep(0.25)
            ed.check_eq(done.get("status"), "succeeded", f"the held export finishes: {done}")
            for _ in range(30):
                await ed.call("wait", frames=5)
                if any(name.endswith(".cpp") and "Closed" in name for name in generated_files(ed.project)):
                    break
            files = generated_files(ed.project)
            ed.check(any("Closed" in name for name in files), f"after the export the closed scene's source is generated again: {files}")
            console = await ed.call("read_console", contains="generated", limit=50)
            problems = [e["message"] for e in console.get("entries", []) if "Failed" in e.get("message", "")]
            ed.check_eq(problems, [], "no failure around the generated folder")
    finally:
        hold.unlink(missing_ok=True)
        held.unlink(missing_ok=True)
