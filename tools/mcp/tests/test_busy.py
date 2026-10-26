"""The server's busy policy (McpServer::busyRefusal): jobs that hold part of the editor
exclusively refuse the calls that would race them, with the job id and how to wait.

- export / build player (JobBusy::Export): edits, Play, script builds, other builds and project
  switches are refused; reads, get_job, cancel_job, selection and batch (per call) are not
- C++ script build (JobBusy::ScriptCompile): Play, builds and project switches are refused;
  edits are not
"""

import asyncio
import os
import shutil
import subprocess
import time

from harness import Editor, skip

# the harness owns the Xvfb so xdotool can press keys in the editor window
CI = True
EDITOR = {"create": True, "name": "Busy Test", "xvfb": "harness"}
TIMEOUT = 1800
JOBS = 3

_PREPARED = None


async def wait_job(ed: Editor, job_id: str, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=5)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s")
        await asyncio.sleep(1.0)


async def refused(ed: Editor, tool: str, job_id: str, **args) -> None:
    r = await ed.call_raw(tool, args)
    ed.check(r.is_error, f"{tool} should be refused while {job_id} runs: {r.text[:300]}")
    ed.check(job_id in r.text, f"{tool}'s refusal should name {job_id}: {r.text[:300]}")
    ed.check("get_job" in r.text, f"{tool}'s refusal should say how to wait: {r.text[:300]}")
    busy = (r.structured or {}).get("busy") or {}
    ed.check(busy.get("job_id") == job_id, f"{tool}: structured busy info: {r.structured!r}")


async def human_play_attempt(ed: Editor, job_id: str) -> None:
    """A human pressing F5 (Play) while the job runs: Play must not start; the editor shows
    a Cannot Play alert naming the job (when the key reaches the window)."""
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        print("     (no xdotool / harness Xvfb: human Play not tried)")
        return
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    def xdo(*args):
        return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout.split()
    wins = xdo("search", "--name", ".")
    if not wins:
        print("     (no editor window found: human Play not tried)")
        return
    xdo("windowfocus", "--sync", wins[-1])
    await ed.call("get_job", job_id=job_id)
    xdo("key", "F5")
    dialog = None
    for _ in range(20):
        st = await ed.call("editor_status")
        ed.check(not st["playing"], f"a human Play started during {job_id}")
        dialog = st.get("dialog")
        if dialog:
            break
        await asyncio.sleep(0.25)
    if not dialog:
        print("     (F5 did not reach the editor window: no alert seen)")
        return
    ed.check_in("Cannot Play", dialog["title"])
    ed.check_in(job_id, dialog["message"])
    await ed.call("respond_dialog", choice="ok")


async def prepare(ed: Editor):
    global _PREPARED
    if _PREPARED is ed:
        return
    await ed.call("create_entity", name="Box", type="box")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    scene = saved["scenes_in_build"][0]
    await ed.call("edit_build_scenes", action="set_start", scene=scene["id"])
    await ed.call("set_project_settings", values={"graphics.runtime_shaders": False, "preferences.build_jobs": JOBS})
    _PREPARED = ed


async def test_export_job_refusals(ed: Editor):
    await prepare(ed)
    info = await ed.call("get_build_info")
    if not info["desktop"]["available"]:
        skip(f"no desktop build tools: {info['desktop'].get('missing_tools')}")
    out = ed.scratch.root / "busy-export"
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), wait=False, build_jobs=JOBS)
    job_id = r["job_id"]
    ed.check_eq(r.get("holds"), "export", f"the job says what it holds: {r!r}")
    try:
        # reads and controls work
        await ed.call("editor_status")
        await ed.call("find_entities", limit=5)
        await ed.call("get_job", job_id=job_id)
        await ed.call("select_entities", entities=["Box"])
        # edits, Play, other builds, script builds and project switches are refused
        await refused(ed, "create_entity", job_id, name="Nope", type="empty")
        await refused(ed, "set_transform", job_id, entity="Box", position=[1, 2, 3])
        await refused(ed, "undo", job_id)
        await refused(ed, "play", job_id, action="start")
        await refused(ed, "compile_scripts", job_id)
        await refused(ed, "set_project_settings", job_id, values={"quality.vsync": True})
        await refused(ed, "create_project", job_id, path=str(ed.scratch.root / "not-now"))
        msg = await ed.call("build_project", mode="source", output_dir=str(ed.scratch.root / "other"), expect_error=True)
        ed.check_in(job_id, msg)
        # inside a batch: per call
        b = await ed.batch([("editor_status", {}), ("create_entity", {"name": "Nope2", "type": "empty"})],
                           stop_on_error=False)
        ed.check_eq(b["failed"], 1, f"batch: {b!r}")
        ed.check(job_id in b["results"][1]["text"], f"nested refusal names the job: {b['results'][1]}")
        await human_play_attempt(ed, job_id)
    finally:
        await ed.call("cancel_job", job_id=job_id)
        job = await wait_job(ed, job_id, 300)
    ed.check_in(job["status"], ("cancelled", "failed", "succeeded"))
    # free again
    made = await ed.call("create_entity", name="AfterBuild", type="empty")
    await ed.call("delete_entities", entities=[made["entity_id"]])


async def test_script_compile_job_refusals(ed: Editor):
    await prepare(ed)
    if not (shutil.which("cmake") and (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++"))):
        skip("no C++ toolchain (cmake + compiler) on PATH for the editor")
    await ed.call("create_script", kind="cpp_script", name="BusySpinner", entity="Box")
    r = await ed.call("compile_scripts", wait=False)
    job_id = r.get("job_id")
    if not job_id or r.get("status") != "running":
        skip(f"compile finished at once: {r!r}")
    try:
        # edits are fine while scripts build; Play and builds are not
        made = await ed.call("create_entity", name="DuringCompile", type="empty")
        await ed.call("delete_entities", entities=[made["entity_id"]])
        await refused(ed, "play", job_id, action="start")
        await refused(ed, "create_project", job_id, path=str(ed.scratch.root / "not-now-2"))
        msg = await ed.call("build_project", mode="source", output_dir=str(ed.scratch.root / "other2"), expect_error=True)
        ed.check_in(job_id, msg)
        # a second compile joins the running one instead of racing it
        again = await ed.call("compile_scripts", wait=False)
        ed.check_eq(again.get("job_id"), job_id, "second compile_scripts joins the running job")
    finally:
        await ed.call("cancel_job", job_id=job_id)
        await wait_job(ed, job_id, 600)
    st = await ed.call("editor_status")
    ed.check_eq(st["jobs"], [], "no running job left")
