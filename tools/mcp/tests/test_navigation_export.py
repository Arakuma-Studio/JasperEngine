"""A baked navmesh ships in a Desktop build and works there (docs/specs/S19-navigation.md AC27, as
docs/specs/S74-navigation-fixes.md R28 / AC29 asks).

Scene NavExport (3D): a 40 x 40 floor with a NavMesh Surface, a static block, and a walker with a
NavMesh Agent and a Lua script that sends it 12 m away and prints a marker from
onDestinationReached. Baked and saved, it is built with the run's prebuilt glcore player and its
content packed (player.pack_native_resources): `tools/jpak.py list` shows the .navmesh in the pack,
and the player, run headless, logs the marker (the agent walked the shipped navmesh). Then the
block moves: the next build's preflight warns that the surface is out of date, and so does
`jasperengine-editor check` (finding tool "navigation").

Set JASPER_SLOW_BUILD=1 to run it: it uses the run's prebuilt glcore runtime (tools/mcp/build_cache.py).
"""

import asyncio
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import build_cache
import harness
from harness import REPO, Editor, skip

PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Navigation Export", "env": build_cache.player_env("glcore", "release")}
CI = True

TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

WALKER = """local Walker = {}
function Walker:start()
    self.frames = 0
    RegisterEngineEvent(self, 'onUpdate')
end
function Walker:onUpdate(dt)
    self.frames = self.frames + 1
    if self.frames == 10 then
        local agent = NavMeshAgent(self.scene, self.entity)
        agent.speed = 6
        agent.acceleration = 20
        agent.onDestinationReached = function(a)
            print("navmarker arrived " .. tostring(a.pathStatus == NavMeshPathStatus.Complete))
        end
        print("navmarker sent " .. tostring(agent:setDestination(Vector3(-15, 0, -3))) .. " " .. tostring(agent.isOnNavMesh))
    end
end
return Walker
"""


async def wait_job(ed: Editor, job: dict, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while job.get("status") == "running":
        if time.monotonic() > deadline:
            ed.fail(f"job {job['job_id']} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)
        job = await ed.call("get_job", job_id=job["job_id"], log_lines=10)
    return job


def build_text(job: dict) -> str:
    """What a build said: its message, its warnings and its log."""
    result = job.get("result") or {}
    log = Path(result["log_file"]).read_text(errors="replace") if result.get("log_file") else ""
    return str(job.get("message", "")) + json.dumps(result.get("warnings", [])) + log


async def test_desktop_export_ships_navmesh(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (the run builds its prebuilt runtime once)")
    made = await ed.call("create_scene", name="NavExport", type="3D", path="scenes/NavExport")
    scene = made["scene"]["id"]
    floor = (await ed.call("create_entity", scene=scene, type="plane", name="Floor", scale=[4, 1, 4]))["entity_id"]
    block = (await ed.call("create_entity", scene=scene, type="cube", name="Block", position=[0, 1, 0], scale=[2, 2, 2]))["entity_id"]
    await ed.call("add_component", scene=scene, entity=floor, component="navmeshsurface")
    walker = (await ed.call("create_entity", scene=scene, type="capsule", name="Walker", position=[-15, 1, -15]))["entity_id"]
    await ed.call("add_component", scene=scene, entity=walker, component="navmeshagent",
                  values={"navmeshagent": {"baseOffset": 1.0}})
    await ed.call("create_script", kind="lua", name="Walker", content=WALKER, entity=walker)
    baked = await ed.call("bake_navigation", scene=scene, mode="full")
    ed.check(baked["polys"] > 0, f"{baked}")
    saved = await ed.call("save_scene", scene=scene)
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scene"]["path"])
    asset = (await ed.call("get_navigation_info", scene=scene))["surfaces"][0]["asset"]
    ed.check(asset.endswith(".navmesh"), f"the surface's .navmesh: {asset}")
    await ed.call("set_project_settings", values={"player.pack_native_resources": True})

    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-navigation"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    job = await wait_job(ed, job, TIMEOUT - 600)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
    ed.check("navigation" in job["result"].get("modules", []), f"the build ships the navigation module: {job['result']}")
    ed.check("is out of date" not in build_text(job), "a fresh bake: no stale-navmesh warning")

    packs = sorted(out.rglob("content.jpak"))
    ed.check(packs, f"a content pack in {out}: {sorted(p.name for p in out.iterdir())}")
    listed = subprocess.run([sys.executable, "-I", str(REPO / "tools" / "jpak.py"), "list", str(packs[0])],
                            capture_output=True, text=True, timeout=120)
    ed.check_eq(listed.returncode, 0, listed.stderr)
    ed.check(Path(asset).name in listed.stdout and ".navmesh" in listed.stdout,
             f"the pack holds {Path(asset).name}: {listed.stdout[:3000]}")

    # the player itself, as a person runs it: its agent walks the shipped navmesh
    run = await asyncio.to_thread(
        subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], "--headless", "--fixed-fps", "60",
                         "--after-ready", "--quit-after", "900"],
        cwd=out, capture_output=True, text=True, timeout=900)
    log = run.stdout + run.stderr
    (ed.scratch.root / "navigation-player.log").write_text(log)
    ed.check_eq(run.returncode, 0, f"the player quits after its frames: {log[-2000:]}")
    ed.check("navmarker sent true true" in log, f"the agent is on the shipped navmesh: {log[-2000:]}")
    ed.check("navmarker arrived true" in log, f"onDestinationReached fired in the player: {log[-2000:]}")

    # static geometry moved after the bake: the build's preflight and the project check warn
    await ed.call("set_transform", scene=scene, entity=block, position=[6, 1, 6])
    await ed.call("save_scene", scene=scene)
    stale = await ed.call("build_project", mode="desktop", output_dir=str(ed.scratch.root / "export-navigation-stale"),
                          method="prebuilt", timeout_seconds=600)
    stale = await wait_job(ed, stale, 1200)
    ed.check_eq(stale["status"], "succeeded", f"a stale navmesh warns, it does not fail the build: {stale}")
    text = build_text(stale)
    ed.check("NavMesh Surface 'Floor' is out of date" in text, f"the preflight warns: {text[-3000:]}")

    tmp = Path(tempfile.mkdtemp(prefix="jasper-navcheck-"))
    try:
        copy = tmp / "project"
        shutil.copytree(ed.project, copy, symlinks=True, ignore=shutil.ignore_patterns(".git", "build", ".jasperengine"))
        checked = await asyncio.to_thread(
            subprocess.run, harness.editor_command() + ["check", "--project", str(copy), "--shaders", "--format", "json"],
            cwd=str(copy), capture_output=True, text=True, timeout=600)
        data = json.loads(checked.stdout)
        found = [f for f in data["findings"] if f["tool"] == "navigation"]
        ed.check(any("'Floor' is out of date" in f["message"] for f in found),
                 f"check reports the stale surface (tool navigation): {data['findings']} {checked.stderr[-1000:]}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
