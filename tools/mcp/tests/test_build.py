"""W5 build tools: get_build_info, build_project (jobs, cancel), generate_shaders, run_build.

Starts from a brand-new project (`mcp --create`): saves its scene, sets the start scene, exports
the C++ source project, cancels a Desktop build, generates shaders and checks run_build's process
handling with stand-in "players" (shell scripts).

The full Desktop build (compiles the engine from source with cmake + g++, ~20-60 min at 3 jobs on
this host) and running the real player are slow: set JASPER_SLOW_BUILD=1 to run them.
"""

import asyncio
import os
import stat
import time
from pathlib import Path

from harness import Editor, find_key, skip

EDITOR = {"create": True, "name": "Build Test"}
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))
JOBS = 3   # the host is shared: never more


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


def make_script(path: Path, body: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


async def test_build_info(ed: Editor):
    info = await ed.call("get_build_info")
    ed.check_eq(info["host_platform"], "linux")
    ed.check(info["desktop"]["available"], f"cmake/g++ missing on the host? {info['desktop']['missing_tools']}")
    ed.check_in("opengl", info["desktop"]["graphics_apis"])
    ed.check_eq(info["scenes"], [], "the new project's scene is untitled")
    ed.check(info["running_job"] is None, "no build running")


async def test_prepare_project(ed: Editor):
    # nothing to build yet: the scene is untitled
    msg = await ed.call("build_project", mode="source", output_dir=str(ed.scratch.root / "never"), expect_error=True)
    ed.check_in("save", msg.lower())

    if ed.has_tool("create_entity"):
        await ed.call("create_entity", name="Box", type="box")
    s = await ed.call("save_project", untitled_scene_dir="scenes")
    ed.check_eq(len(s["scenes_in_build"]), 1)
    scene = s["scenes_in_build"][0]
    r = await ed.call("edit_build_scenes", action="set_start", scene=scene["id"])
    ed.check(r["scenes_in_build"][0]["start"], "start scene")
    await ed.call("set_project_settings", values={
        "graphics.runtime_shaders": False,       # smaller, faster player build
        "preferences.build_jobs": JOBS,
        "player.window_width": 640,
        "player.window_height": 480,
    })
    info = await ed.call("get_build_info", include_shaders=True)
    ed.check_eq(len(info["scenes"]), 1)
    ed.check(info["shader_count"] > 0, "the scene needs shaders")
    ed.check(all(s["key"].startswith("0x") and s["name"] for s in info["shaders"]), "shader keys and names")


async def test_refusals(ed: Editor):
    out = str(ed.scratch.root / "x")
    await ed.call("build_project", mode="console", output_dir=out, expect_error=True)
    await ed.call("build_project", mode="source", output_dir="relative/dir", expect_error=True)
    msg = await ed.call("build_project", mode="source", output_dir=str(ed.project / "Builds"), expect_error=True)
    ed.check_in("inside the project", msg)
    msg = await ed.call("build_project", mode="desktop", output_dir=out, graphics_api="d3d11", expect_error=True)
    ed.check_in("opengl", msg)
    await ed.call("build_project", mode="source", output_dir=out, backends=["glide"], expect_error=True)
    await ed.call("build_project", mode="desktop", output_dir=out, build_type="Fast", expect_error=True)
    info = await ed.call("get_build_info")
    if not info["web"]["available"]:
        msg = await ed.call("build_project", mode="web", output_dir=out, expect_error=True)
        ed.check_in("Emscripten", msg)


async def test_source_export(ed: Editor):
    out = ed.scratch.root / "export-source"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    result = r["result"]
    ed.check_eq(Path(result["output_dir"]).resolve(), out.resolve())
    ed.check((out / "CMakeLists.txt").exists(), "CMakeLists.txt exported")
    ed.check_in("CMakeLists.txt", result["artifacts"])
    ed.check("how_to_build" in result, "how_to_build")
    ed.check(Path(result["log_file"]).exists(), "build log file")
    ed.check_eq(result["errors"], [])

    # a non-empty directory needs overwrite
    msg = await ed.call("build_project", mode="source", output_dir=str(out), expect_error=True)
    ed.check_in("overwrite", msg)
    r = await ed.call("build_project", mode="source", output_dir=str(out), overwrite=True,
                      backends=["opengl", "opengles"], timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded")

    # the remembered output dir is the default next time
    info = await ed.call("get_build_info")
    ed.check_eq(Path(info["source"]["output_dir"]).resolve(), out.resolve())
    ed.check_eq(sorted(info["source"]["backends"]), ["opengl", "opengles"])
    ed.check_eq(info["last_build"]["status"], "succeeded")


async def test_cancel_desktop_build(ed: Editor):
    out = ed.scratch.root / "export-cancel"
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), wait=False, build_jobs=JOBS)
    job_id = r["job_id"]
    ed.check_eq(r["status"], "running")

    # one build at a time
    msg = await ed.call("build_project", mode="source", output_dir=str(ed.scratch.root / "other"), expect_error=True)
    ed.check_in(job_id, msg)
    info = await ed.call("get_build_info")
    ed.check_eq(info["running_job"], job_id)

    await asyncio.sleep(2)
    await ed.call("cancel_job", job_id=job_id)
    job = await wait_job(ed, job_id, 180, poll=1.0)
    ed.check_eq(job["status"], "cancelled", f"{job}")
    # the editor is still responsive and free to build again
    await ed.call("editor_status")
    ed.check((await ed.call("get_build_info"))["running_job"] is None, "no build running after cancel")


async def test_generate_shaders(ed: Editor):
    out = ed.scratch.root / "shaders"
    r = await ed.call("generate_shaders", output_dir=str(out), backends=["opengl"], format="binary", timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    files = [p for p in out.rglob("*") if p.is_file()]
    ed.check(files, "shader files written")
    await ed.call("generate_shaders", output_dir="rel", expect_error=True)
    await ed.call("generate_shaders", output_dir=str(out), format="text", expect_error=True)


async def test_run_build_process_handling(ed: Editor):
    bin_dir = ed.scratch.root / "fake-players"
    crasher = make_script(bin_dir / "crasher", 'echo "starting $1"\necho "Segmentation fault" >&2\nexit 3\n')
    r = await ed.call("run_build", path=str(crasher), seconds=5, args=["--level", "2"], screenshot=False)
    ed.check_eq(r["exit_code"], 3)
    ed.check(r["exited_early"], "exited early")
    ed.check(r["crashed"], "crash detected")
    ed.check_in("Segmentation fault", r["fatal_lines"])
    ed.check_in("starting --level", " ".join(r["log"]))
    ed.check(Path(r["log_file"]).read_text().count("starting") == 1, "log file")

    sleeper = make_script(bin_dir / "sleeper", 'echo "pwd=$(pwd)"\nwhile true; do echo tick; sleep 0.5; done\n')
    t0 = time.monotonic()
    raw = await ed.call_raw("run_build", {"path": str(sleeper), "seconds": 2, "screenshot_after_seconds": 1})
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    ed.check(time.monotonic() - t0 < 15, "stopped in time")
    ed.check(r["stopped_by_run_build"], "stopped by run_build")
    ed.check(not r["crashed"], "a stopped player is not a crash")
    ed.check_in(f"pwd={bin_dir}", r["log"], "runs in its own directory")
    # no window of its own: the whole screen (the editor) is captured
    ed.check(raw.images, "screenshot image")
    ed.check_in("whole screen", r["screenshot"]["note"])
    data, mime = raw.images[0]
    ed.check_eq(mime, "image/png")
    ed.check(data[:8] == b"\x89PNG\r\n\x1a\n", "PNG data")

    await ed.call("run_build", path=str(bin_dir / "missing"), expect_error=True)
    await ed.call("run_build", path="relative", expect_error=True)
    msg = await ed.call("run_build", path=str(bin_dir), expect_error=True)   # two executables there
    ed.check_in("No player executable", msg)


async def test_desktop_build_and_run(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    out = ed.scratch.root / "export-desktop"
    t0 = time.monotonic()
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=JOBS, timeout_seconds=30)
    job_id = r["job_id"]
    job = r if r["status"] != "running" else await wait_job(ed, job_id, TIMEOUT - 300, poll=15.0)
    print(f"[test] desktop build: {job['status']} in {time.monotonic() - t0:.0f}s")
    ed.check_eq(job["status"], "succeeded", f"{job}")
    exe = Path(job["result"]["executable"])
    ed.check(exe.exists(), f"player at {exe}")

    raw = await ed.call_raw("run_build", {"seconds": 8, "screenshot_after_seconds": 6})
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    print("[test] run_build:", raw.text[:1500])
    ed.check_eq(Path(r["executable"]).resolve(), exe.resolve())
    ed.check(not r["crashed"], f"the player crashed: {r['fatal_lines']} {r['log'][-20:]}")
    ed.check(r.get("stopped_by_run_build"), "still running when stopped")
    ed.check(raw.images, "screenshot")
    ed.check_in("player window", r["screenshot"]["note"])
    shot = ed.scratch.root / "player.png"
    shot.write_bytes(raw.images[0][0])
    print(f"[test] player screenshot: {shot}")

    # compile errors come back as file:line: a broken source in a script directory
    (ed.project / "cpp").mkdir(exist_ok=True)
    (ed.project / "cpp" / "Broken.cpp").write_text("int broken() {\n    return undefined_symbol_xyz;\n}\n")
    await ed.call("set_project_settings", values={"scripting.script_dirs": ["cpp"]})
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=JOBS, timeout_seconds=30)
    job = r if r["status"] != "running" else await wait_job(ed, r["job_id"], 3600, poll=10.0)
    ed.check_eq(job["status"], "failed", f"{job}")
    errors = job["result"]["errors"]
    ed.check(errors, f"no parsed errors: {job}")
    first = errors[0]
    ed.check_in("undefined_symbol_xyz", first["message"])
    ed.check_eq(first["line"], 2)
    ed.check_eq(first.get("project_file"), "cpp/Broken.cpp")
    (ed.project / "cpp" / "Broken.cpp").unlink()
    await ed.call("set_project_settings", values={"scripting.script_dirs": []})
