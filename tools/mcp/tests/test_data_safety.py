"""Data safety in a running editor (docs/data-safety.md, docs/specs/S03-data-safety.md).

- test_project_lock (AC6): a second `jasperengine-editor mcp --headless -p` on the project another
  editor has open exits with code 3 and project_locked on stderr; with --ignore-lock it starts on
  that same project.
- test_refused_editor_opens_nothing: that refusal exits with 3 without opening the temporary
  project every editor shares, whether another editor holds that project's lock (it used to
  segfault, the Inspector drawing a frame with no scene) or it does not exist (not made).
- test_no_project_open_keeps_drawing: a project that does not open and a temporary project another
  editor holds: no project, no scene, and an editor that stays up and answers.
- test_crash_recovery (AC11): an edit, the Auto Save backup it makes (JE_AUTOSAVE_INTERVAL_TEST=1:
  every 2 s), kill -9, a new editor: open_scene says a backup is there and keeps it; recovery=restore
  brings the edit back, modified; save_scene deletes the backup. A second crash with
  recovery=discard: the saved state, and no backup left.
- test_mcp_degraded_save (AC16): a scene with an unknown entity type and an unknown key below
  submeshes[0] (dropped.scene): save_scene fails with degraded_document listing both, the file
  untouched; get_load_report gives the same; allow_data_loss=true writes it; save_scene all=true
  saves the clean scenes (unknown data kept) and reports the refused one; no modal ever blocks.
"""

from __future__ import annotations

import asyncio
import fcntl
import json
import os
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

import harness
from harness import Editor, Scratch, TestFailure

CI = True
TIMEOUT = 900

FIXTURES = harness.REPO / "editor" / "tests" / "fixtures" / "data-safety"


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def lock_owner(project: Path) -> dict:
    return json.loads((project / ".jasperengine" / "user" / "editor.lock").read_text())


def backup_of(project: Path, scene: str) -> Path:
    return project / ".jasperengine" / "backup" / "scenes" / scene


async def wait_for(cond, seconds: float, what: str) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if cond():
            return
        await asyncio.sleep(0.5)
    fail(f"timed out waiting for {what}")


def second_instance(project: Path, extra: list[str], stdin: str = "",
                    env_extra: dict[str, str] | None = None) -> tuple[subprocess.CompletedProcess, Path]:
    """`<editor> mcp --headless --project <project>` (not a copy: the same directory)"""
    root = Path(tempfile.mkdtemp(prefix="jasper-lock-"))
    (root / "home").mkdir()
    scratch = Scratch(root=root, project=project, home=root / "home", stderr_log=root / "editor.stderr.log")
    argv, env, cwd = harness.launch_spec(scratch, extra_args=extra, extra_env=env_extra)
    try:
        run = subprocess.run(argv, cwd=str(cwd), env=env, input=stdin, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired as e:
        err = e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
        fail(f"{' '.join(argv)} did not exit in 120 s; stderr:\n{err[-3000:]}")
    return run, root


async def test_project_lock():
    async with Editor(create=True, name="Lock Test") as first:
        await first.call("get_project_summary")  # the project is made after initialize
        project = first.project
        owner = lock_owner(project)
        check(owner.get("pid", 0) > 0 and owner.get("editor_version") is not None, f"lock owner info: {owner}")

        run, root = second_instance(project, [])
        try:
            check(run.returncode == 3, f"a second editor on a locked project exits with 3, got {run.returncode}:\n{run.stderr[-2000:]}")
            check("project_locked" in run.stderr, "stderr names project_locked:\n" + run.stderr[-2000:])
            check(f"PID {owner['pid']}" in run.stderr, "the message names the owner's pid:\n" + run.stderr[-2000:])
        finally:
            shutil.rmtree(root, ignore_errors=True)

        # --ignore-lock: it starts on the same project (initialize answered, then stdin closes)
        hello = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "lock-test", "version": "1"}}}) + "\n"
        run, root = second_instance(project, ["--ignore-lock"], stdin=hello)
        try:
            check(run.returncode == 0, f"--ignore-lock starts the editor, got {run.returncode}:\n{run.stderr[-2000:]}")
            check('"id":1' in run.stdout.replace(" ", "") and "serverInfo" in run.stdout, "it answered initialize:\n" + run.stdout[:2000])
            check("[mcp] project_locked" not in run.stderr, "no project_locked:\n" + run.stderr[-2000:])
        finally:
            shutil.rmtree(root, ignore_errors=True)

        # the first editor still holds its lock and works
        await first.call("get_project_summary")
        check(lock_owner(project)["pid"] == owner["pid"], "the first editor keeps the lock")


TEMP_PROJECT = "MyJasperEngineProject"  # App::loadStartupProject's fallback, under the temp directory


def hold_temp_project(temp_dir: Path, project_yaml: str):
    """The temporary project as another live editor has it: there, and its lock (flock) held.
    Close the returned file to release it."""
    user = temp_dir / TEMP_PROJECT / ".jasperengine" / "user"
    user.mkdir(parents=True)
    (temp_dir / TEMP_PROJECT / "project.yaml").write_text(project_yaml)
    held = open(user / "editor.lock", "w")
    fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
    return held


async def test_refused_editor_opens_nothing():
    async with Editor(create=True, name="Lock Test") as first:
        await first.call("get_project_summary")
        project = first.project
        owner = lock_owner(project)
        temp_root = Path(tempfile.mkdtemp(prefix="jasper-lock-tmp-"))
        try:
            # held by another editor (another refused one, under load): the crash
            busy = temp_root / "busy"
            busy.mkdir()
            held = hold_temp_project(busy, (project / "project.yaml").read_text())
            try:
                run, root = second_instance(project, [], env_extra={"TMPDIR": str(busy)})
                shutil.rmtree(root, ignore_errors=True)
            finally:
                held.close()
            check(run.returncode == 3, f"refused, temporary project held: exit 3, got {run.returncode}:\n{run.stderr[-2000:]}")
            check(f"PID {owner['pid']}" in run.stderr, "the message names the owner's pid, not the temporary project's:\n" + run.stderr[-2000:])

            # nothing there: nothing made
            empty = temp_root / "empty"
            empty.mkdir()
            run, root = second_instance(project, [], env_extra={"TMPDIR": str(empty)})
            shutil.rmtree(root, ignore_errors=True)
            check(run.returncode == 3, f"refused, no temporary project: exit 3, got {run.returncode}:\n{run.stderr[-2000:]}")
            check(f"PID {owner['pid']}" in run.stderr, "the message names the owner's pid:\n" + run.stderr[-2000:])
            check(not (empty / TEMP_PROJECT).exists(), "a refused editor makes no temporary project")
        finally:
            shutil.rmtree(temp_root, ignore_errors=True)
        await first.call("get_project_summary")


async def test_no_project_open_keeps_drawing():
    work = Path(tempfile.mkdtemp(prefix="jasper-noscene-"))
    try:
        broken = work / "broken"
        broken.mkdir()
        (broken / "project.yaml").write_text("{ this is not a project: [\n")
        temp_dir = work / "tmp"
        temp_dir.mkdir()
        held = hold_temp_project(temp_dir, "name: held\n")
        try:
            async with Editor(project=broken, env={"TMPDIR": str(temp_dir)}) as ed:
                # each answer is after frames with the Inspector and the other windows drawn
                for _ in range(5):
                    await ed.call("editor_status")
                    await ed.call("wait", frames=2)
                summary = await ed.call("get_project_summary")
                check(not summary.get("scenes"), f"no scene is open: {summary}")
                stderr = ed.stderr_text()
                check("no project is open" in stderr, "stderr says no project is open:\n" + stderr[-2000:])
                await ed.call("editor_status")
        finally:
            held.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)


async def crash(ed: Editor) -> None:
    """kill -9 the editor (its pid from the project lock): no clean-up runs"""
    pid = lock_owner(ed.project)["pid"]
    os.kill(pid, signal.SIGKILL)
    ed.keep = True  # its scratch holds the project the next editor copies
    await ed.close()


async def make_backup(ed: Editor, scene: str, entity: str) -> None:
    await ed.call("open_scene", scene=scene)
    await ed.call("create_entity", name=entity, type="empty")
    await wait_for(lambda: backup_of(ed.project, scene).exists() and (backup_of(ed.project, scene).parent / (scene + ".json")).exists(),
                   60, f"the Auto Save backup of {scene}")
    sidecar = json.loads((backup_of(ed.project, scene).parent / (scene + ".json")).read_text())
    check(sidecar["source"] == scene and sidecar["base_sha1"] and sidecar["pid"] == lock_owner(ed.project)["pid"], f"sidecar: {sidecar}")
    check(entity in backup_of(ed.project, scene).read_text(), "the backup holds the edit")
    # never the scene file itself
    check(entity not in (ed.project / scene).read_text(), "Auto Save does not write the scene")


async def has_entity(ed: Editor, scene: str, name: str) -> bool:
    found = await ed.call("find_entities", scene=scene, name=name)
    items = found.get("entities", found) if isinstance(found, dict) else found
    return bool(items)


async def test_crash_recovery():
    os.environ["JE_AUTOSAVE_INTERVAL_TEST"] = "1"
    try:
        async with Editor(create=True, name="Recovery Test") as ed:
            await ed.call("create_scene", name="Level", path="Level.scene")
            await ed.call("save_scene", scene="Level")
            await make_backup(ed, "Level.scene", "Survivor")
            project = ed.project
            await crash(ed)

        async with Editor(project=project) as ed:
            check(backup_of(ed.project, "Level.scene").exists(), "the crash left the backup")
            opened = await ed.call("open_scene", scene="Level.scene")
            rec = opened["recovery"]
            check(rec["available"] and rec["status"] == "usable" and rec["applied"] == "keep", f"keep: {rec}")
            check(rec["autosaved_at"], "autosaved_at")
            check(not await has_entity(ed, "Level", "Survivor"), "kept: the saved file is open")
            check(backup_of(ed.project, "Level.scene").exists(), "kept: the backup stays")
            listed = await ed.call("list_scene_backups")
            check(any(b["source"] == "Level.scene" and b["status"] == "usable" for b in listed["backups"]), f"listed: {listed}")

            opened = await ed.call("open_scene", scene="Level.scene", recovery="restore")
            check(opened["recovery"]["applied"] == "restore", f"restore: {opened['recovery']}")
            check(await has_entity(ed, "Level", "Survivor"), "restored: the edit is back")
            check(opened["scene"]["modified"], "restored: modified")
            check(opened["scene"]["path"] == "Level.scene", "restored: the document keeps its path")

            await ed.call("save_scene", scene="Level")
            check(not backup_of(ed.project, "Level.scene").exists(), "save deletes the backup")
            check("Survivor" in (ed.project / "Level.scene").read_text(), "saved")

            # second crash, discarded this time
            await make_backup(ed, "Level.scene", "Doomed")
            project = ed.project
            await crash(ed)

        async with Editor(project=project) as ed:
            opened = await ed.call("open_scene", scene="Level.scene", recovery="discard")
            check(opened["recovery"]["available"] and opened["recovery"]["applied"] == "discard", f"discard: {opened['recovery']}")
            check(not await has_entity(ed, "Level", "Doomed"), "discarded: the saved state")
            check(await has_entity(ed, "Level", "Survivor"), "discarded: what was saved is there")
            check(not backup_of(ed.project, "Level.scene").exists(), "discarded: no backup")
            check(not opened["scene"]["modified"], "discarded: not modified")
    finally:
        os.environ.pop("JE_AUTOSAVE_INTERVAL_TEST", None)


EDITOR = {"create": True, "name": "Degraded Save"}


async def test_mcp_degraded_save(ed: Editor):
    await ed.call("get_project_summary")  # the project is made after initialize
    for name in ("dropped.scene", "future.scene"):
        shutil.copy(FIXTURES / name, ed.project / name)
    shutil.copy(FIXTURES / "dropped.scene", ed.project / "dropped2.scene")

    opened = await ed.call("open_scene", scene="dropped.scene")
    # the asset database gives the copied file its identity (S04: a guid line, nothing else),
    # when it imports the file the watcher reported
    deadline = time.monotonic() + 60
    while not (ed.project / "dropped.scene").read_bytes().startswith(b"guid: "):
        check(time.monotonic() < deadline, "the asset database gave the copied scene no identity within 60 s")
        await ed.call("editor_status")
        await asyncio.sleep(0.2)
    original = (ed.project / "dropped.scene").read_bytes()
    lines = original.decode().splitlines(True)
    check(lines[0].startswith("guid: ") and "".join(lines[1:]).encode() == (FIXTURES / "dropped.scene").read_bytes(),
          f"the scene has its S04 identity and is otherwise the copied file: {lines[:3]}")
    report = opened["load_report"]
    check(report["dropped"] == 2 and not report["newer_format"], f"load_report: {report}")
    check(opened["scene"]["degraded"]["dropped"] == 2, f"degraded: {opened['scene']['degraded']}")

    refused = await ed.call_raw("save_scene", {"scene": "dropped.scene"})
    check(refused.is_error, "save_scene refuses a degraded scene")
    data = refused.structured or {}
    check(data.get("code") == "degraded_document", f"code: {data}")
    check(len(data.get("dropped", [])) == 2, f"two dropped issues: {data}")
    kinds = sorted(i["kind"] for i in data["dropped"])
    check(kinds == ["unknown_entity_type", "unknown_field"], f"kinds: {kinds}")
    check("allow_data_loss=true" in data.get("hint", ""), "the hint")
    check((ed.project / "dropped.scene").read_bytes() == original, "nothing written")

    full = await ed.call("get_load_report", scene="dropped.scene")
    check(full["dropped"] == 2, f"get_load_report: {full}")
    nested = [i for i in full["issues"] if i["kind"] == "unknown_field"]
    check(nested and nested[0]["field"] == "submeshes[0].glint" and nested[0]["component"] == "mesh" and nested[0]["line"] > 0,
          f"the nested key: {nested}")
    check(any(i["kind"] == "unknown_entity_type" and i.get("value") == "Hologram" for i in full["issues"]), "the entity type")

    saved = await ed.call("save_scene", scene="dropped.scene", allow_data_loss=True)
    check(saved["degraded"]["dropped"] == 0, f"after Save Anyway: {saved['degraded']}")
    text = (ed.project / "dropped.scene").read_text()
    check("Hologram" not in text and "glint" not in text and text.startswith("formatVersion: 8"), "written without them")

    # Save All: the clean scenes are written (unknown data kept), the degraded one refused
    await ed.call("open_scene", scene="future.scene")
    await ed.call("create_entity", scene="future.scene", name="Added", type="empty")
    await ed.call("open_scene", scene="dropped2.scene")
    await ed.call("create_entity", scene="dropped2.scene", name="Added", type="empty")
    all_saved = await ed.call_raw("save_scene", {"all": True})
    check(all_saved.is_error, "all=true fails when one is refused")
    result = all_saved.structured or {}
    check(result.get("code") == "degraded_document", f"all: {result}")
    check([r["document"] for r in result["refused"]] == ["dropped2.scene"], f"refused: {result['refused']}")
    check(any(s["name"] == "Future" for s in result["saved"]), f"saved: {result['saved']}")
    future = (ed.project / "future.scene").read_text()
    for kept in ("jumpable:", "areaSize:", "type: area", "position: [a, b]", "stance: crouched", "lighting2:", "Added"):
        check(kept in future, f"future.scene keeps {kept!r}:\n{future[:3000]}")

    # no modal is waiting: the editor answers at once
    await ed.call("get_project_summary")
