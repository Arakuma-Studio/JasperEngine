"""An export and the editor's cooked cache (S30, S98 REQ-2): a change the editor picks up while a
build exports (a prefab a closed scene uses, edited by another program: the asset database
invalidates that scene's cooked copy) is cooked again once the export has finished, and the export
writes nothing into the editor's generated folder.

The export is held before it reads the scenes by the editor's test hook JASPER_TEST_EXPORT_HOLD=<path>
(it creates <path>.held and waits while <path> exists), so nothing here depends on timing.

What a build writes (S98 REQ-4, REQ-5; AC5, AC6):
- A build writes nothing in the project but the editor's .jasperengine/: every other file hashes
  the same after a source export with an open scene and a Prefab Mode stage both modified; each is
  named by W-SCENE-UNSAVED, and the cooked scene and prefab hold what the editor shows.
- Two source exports of the AllComponents fixture into fresh folders are byte-identical.
The same holds for prebuilt builds: with JASPER_SLOW_BUILD=1 both tests build with the run's prebuilt
glcore runtime too (tools/mcp/build_cache.py; docs/testing.md "Compiled and prebuilt game builds").
"""

from __future__ import annotations

import asyncio
import hashlib
import os
import tempfile
from pathlib import Path

import build_cache
from harness import Editor
from scene_history import SCENE_INPUTS, History, two_level_prefab

TIMEOUT = 1800
CI = True
REPO = Path(__file__).resolve().parents[3]
PREBUILT_PLAYERS = ["glcore-release"]


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


# ---- what a build writes (S98 REQ-4, REQ-5) ----


def slow() -> bool:
    return os.environ.get("JASPER_SLOW_BUILD", "") not in ("", "0")


def hashes(root: Path, skip_internal: bool = True) -> dict[str, str]:
    """{relative path: sha256} of every file below root (the editor's .jasperengine/ left out)"""
    out = {}
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if not path.is_file() or (skip_internal and rel.parts and rel.parts[0] == ".jasperengine") or path.name == "editor.lock":
            continue
        out[rel.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


def changed(before: dict[str, str], after: dict[str, str]) -> list[str]:
    return sorted(name for name in set(before) | set(after) if before.get(name) != after.get(name))


def warning_messages(job: dict, code: str) -> list[str]:
    return [w.get("message", "") for w in job.get("result", {}).get("warnings", []) if isinstance(w, dict) and w.get("code") == code]


async def export_build(ed: Editor, out: Path, **args) -> dict:
    r = await ed.call("build_project", output_dir=str(out), allow_missing_assets=True, timeout_seconds=900, **args)
    ed.check_eq(r["status"], "succeeded", f"{str(r)[:2000]}")
    return r


async def test_export_writes_no_source():
    """A source export (and with JASPER_SLOW_BUILD=1 a prebuilt one) of a project with an open scene and a
    Prefab Mode stage both modified writes no project file; each is named by W-SCENE-UNSAVED"""
    async with Editor(create=True, name="Export Sources", label="export-no-source", env=build_cache.player_env("glcore", "release")) as ed:
        sid = (await ed.call("create_scene", name="Level", type="3D", path="scenes/Level.scene"))["scene"]["id"]
        await ed.call("create_entity", scene=sid, type="cube", name="Saved Cube", select=False)
        crate = (await ed.call("create_entity", scene=sid, type="sphere", name="Crate", select=False))["entity_id"]
        await ed.call("create_prefab", scene=sid, entity=crate, path="prefabs/Crate.prefab")
        await ed.call("save_scene", scene=sid)
        await ed.call("edit_build_scenes", action="add", scene=sid)
        await ed.call("edit_build_scenes", action="set_start", scene=sid)
        await ed.call("save_project")

        # the open scene modified, and the prefab's Prefab Mode with an edit not saved yet
        await ed.call("create_entity", scene=sid, type="cube", name="Unsaved Cube", select=False)
        stage = (await ed.call("prefab_mode", action="open", prefab="prefabs/Crate.prefab", auto_save=False, in_context=False))
        stage_id = stage.get("scene_id") or stage.get("scene", {}).get("id")
        ed.check(stage_id, f"the stage's scene: {stage}")
        await ed.call("create_entity", scene=stage_id, parent="Crate", type="cube", name="Stage Lid", select=False)
        builds = [("source", dict(mode="source", platform="linux", backends=["opengl"]))]
        if slow():
            await build_cache.player_runtime(ed, "glcore", "release")
            builds.insert(0, ("prebuilt", dict(mode="desktop", method="prebuilt")))
        # build_project remembers the graphics API it is given in project.yaml (Build Settings does the
        # same): a first build of each kind takes that, the builds checked change no setting
        for label, args in builds:
            await export_build(ed, ed.scratch.root / f"settings-{label}", **args)
        before = hashes(ed.project)
        for label, args in builds:
            out = ed.scratch.root / f"no-source-{label}"
            job = await export_build(ed, out, **args)
            ed.check_eq(changed(before, hashes(ed.project)), [], f"{label}: the build wrote no project file")
            unsaved = warning_messages(job, "W-SCENE-UNSAVED")
            ed.check_eq(len(unsaved), 2, f"{label}: the scene and the prefab are named: {unsaved}")
            ed.check(any("Level" in m for m in unsaved) and any("Crate.prefab" in m for m in unsaved), f"{label}: {unsaved}")
            content = out / "project" / "content" if label == "source" else out
            ed.check_in(b"Unsaved Cube", (content / "scenes" / f"{sid}.jscn").read_bytes(), f"{label}: the cooked scene is what the editor shows")
            ed.check(any(b"Stage Lid" in p.read_bytes() for p in (content / "prefabs").glob("*.jpfb")),
                     f"{label}: the cooked prefab is what Prefab Mode shows")

        # the editor still holds the edits, and the files are as they were saved
        ed.check(b"Unsaved Cube" not in (ed.project / "scenes" / "Level.scene").read_bytes(), "the scene's file stays as saved")
        ed.check(b"Stage Lid" not in (ed.project / "prefabs" / "Crate.prefab").read_bytes(), "the prefab's file stays as saved")
        await ed.call("prefab_mode", action="close", unsaved="discard")


async def test_export_is_deterministic():
    """Two builds of the AllComponents fixture (every component type) into fresh folders: the same bytes"""
    async with Editor(project=str(REPO / "tools" / "fixtures" / "reflection-parity"), label="deterministic",
                      env=build_cache.player_env("glcore", "release")) as ed:
        scenes = (await ed.call("list_scenes"))["scenes"]
        ed.check(scenes, "the fixture has its scene")
        builds = [("source", dict(mode="source", platform="linux", backends=["opengl"]))]
        if slow():
            await build_cache.player_runtime(ed, "glcore", "release")
            builds.append(("prebuilt", dict(mode="desktop", method="prebuilt")))
        for label, args in builds:
            first, second = ed.scratch.root / f"{label}-1", ed.scratch.root / f"{label}-2"
            await export_build(ed, first, **args)
            await export_build(ed, second, **args)
            a, b = hashes(first, skip_internal=False), hashes(second, skip_internal=False)
            # the build's own log names its folder; the rest is the game
            stamps = {name for name in a if name.endswith(".log")}
            differ = [name for name in changed(a, b) if name not in stamps]
            ed.check_eq(differ, [], f"{label}: two builds of an unchanged project give the same bytes")
            ed.check(any(name.endswith(".jscn") for name in a), f"{label}: the cooked scene is compared: {len(a)} files")
