"""One game, built both ways, draws the same (docs/specs/S98-editor-tooling-gaps.md REQ-1, REQ-52; AC2).

A Compile build reads the cooked scenes and prefabs and game.json a prebuilt build ships (S98 D1):
the same Lua-only scene (lit meshes, a sprite, a prefab instance, a Lua script that moves a cube),
built with the run's prebuilt glcore runtime and compiled from source, gives the same frame
(pngdiff --tolerance 8 --max-fraction 0.002, as S30 AC5). Both builds are real: set
JASPER_SLOW_BUILD=1 (it compiles the engine, through ccache: harness.compile_env).

The settings a Compile build's generated startup code once applied come from game.json and the cooked
scene now: a Lua probe prints what both games run with (project settings changed from their defaults,
and the scene's Physics 2D sub-steps and Canvas Scaler, which the cooked scene gained in format 1.7),
and both print the values the editor set.
"""

from __future__ import annotations

import asyncio
import os
import time
from pathlib import Path

import build_cache
import harness
import pngdiff
from harness import Editor, build_jobs, skip

PREBUILT_PLAYERS = ["glcore-release"]
CI_EXCLUDED = ("long-running (it compiles a real Desktop player); in the full suite (MCP_SUITE=all) on the build servers")
EDITOR = {"create": True, "name": "Build Parity",
          "env": {**harness.compile_env("test_build_parity"), **build_cache.player_env("glcore", "release")}}
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

MOVER = """local Mover = {}
function Mover:init() self.gameObject.position = Vector3(1.5, 0.5, 0) end
return Mover
"""

# what the game runs with, one log line: the settings the generated init() and create_<scene>() set
PROBE = """local Probe = {}
function Probe:init()
  local scene = self.scene
  print(string.format("PARITY canvas=%dx%d ppu=%d frameRate=%d vsync=%d foliage=%.3f subSteps=%d",
    Engine.canvasWidth, Engine.canvasHeight, Engine.getPixelsPerUnit(), Engine.targetFrameRate, Engine.vSyncCount,
    Engine.foliageDistanceScale, scene.physics2DSubSteps))
end
return Probe
"""
EXPECTED_PROBE = "PARITY canvas=800x450 ppu=32 frameRate=50 vsync=0 foliage=0.500 subSteps=9"


async def wait_job(ed: Editor, job: dict) -> dict:
    deadline = time.monotonic() + TIMEOUT
    while job.get("status") == "running":
        await asyncio.sleep(10)
        job = await ed.call("get_job", job_id=job["job_id"], log_lines=10)
        if time.monotonic() > deadline:
            ed.fail(f"build still running: {job}")
    return job


async def frame(ed: Editor, label: str) -> tuple:
    raw = await ed.call_raw("run_build", {"after_ready": True, "fixed_fps": 60, "quit_after": 60})
    ed.check(not raw.is_error and raw.images, f"{label}: {raw.text[:1500]}")
    ed.check(not raw.value["crashed"], f"{label}: the player crashed: {raw.value}")
    png = raw.images[0][0]
    (ed.scratch.root / f"parity-{label}.png").write_bytes(png)
    log_file = Path(raw.value["log_file"]) if raw.value.get("log_file") else None
    if log_file and log_file.is_file():
        log = log_file.read_text(errors="replace")
    else:
        log = "\n".join(raw.value["log"]) if isinstance(raw.value.get("log"), list) else str(raw.value.get("log", ""))
    probe = [line[line.index("PARITY "):].strip() for line in log.splitlines() if "PARITY " in line]
    ed.check(probe, f"{label}: the probe printed nothing: {log[-1500:]}")
    return pngdiff.decode(png), probe[0]


async def test_compile_and_prebuilt_match(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build a game with the prebuilt runtime and compile it (compiles the engine)")
    sid = (await ed.call("create_scene", name="Parity", type="3D", path="scenes/Parity.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="directional_light", name="Sun", rotation={"euler": [-50, 30, 0]}, select=False)
    await ed.call("create_entity", scene=sid, type="camera", name="Camera", position=[0, 1, 6], select=False)
    red = (await ed.call("create_entity", scene=sid, type="cube", name="Red", position=[-1.5, 0, 0], select=False))["entity_id"]
    await ed.call("set_properties", scene=sid, entity=red, values={"mesh": {"submeshes[0].material.baseColorFactor": [1, 0, 0, 1]}})
    mover = (await ed.call("create_entity", scene=sid, type="sphere", name="Moved", select=False))["entity_id"]
    await ed.call("create_script", kind="lua", name="Mover", content=MOVER, entity=mover)
    crate = (await ed.call("create_entity", scene=sid, type="cylinder", name="Crate", position=[0, -1, 0], select=False))["entity_id"]
    await ed.call("create_prefab", scene=sid, entity=crate, path="prefabs/Crate.prefab")
    probe = (await ed.call("create_entity", scene=sid, type="empty", name="Probe", select=False))["entity_id"]
    # a 2D body: a Compile build has the Box2D module only when the game uses it (S34), and without
    # it no scene has 2D sub-steps to set
    await ed.call("create_entity", scene=sid, type="rigidbody2d", name="Body2D", position=[0, -20, 0], select=False)
    await ed.call("create_script", kind="lua", name="Probe", content=PROBE, entity=probe)
    await ed.call("set_scene_settings", scene=sid, settings={"physics_2d_sub_steps": 9,
                  "canvas_scaler": {"mode": "scale_with_screen_size", "reference": [1280, 720]}})
    await ed.call("save_scene", scene=sid)
    await ed.call("edit_build_scenes", action="add", scene=sid)
    await ed.call("edit_build_scenes", action="set_start", scene=sid)
    await ed.call("set_project_settings", values={"player.window_width": 640, "player.window_height": 360, "graphics.runtime_shaders": False,
                                                  "graphics.canvas_width": 800, "graphics.canvas_height": 450, "graphics.pixels_per_unit": 32,
                                                  "player.target_frame_rate": 50, "quality.vsync": False,
                                                  "quality.foliage_distance_desktop": 0.5})

    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "parity-prebuilt"
    job = await wait_job(ed, await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=30))
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
    prebuilt, prebuilt_probe = await frame(ed, "prebuilt")

    out = ed.scratch.root / "parity-compile"
    job = await wait_job(ed, await ed.call("build_project", mode="desktop", output_dir=str(out), method="compile", build_jobs=build_jobs(),
                                           timeout_seconds=30))
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq(job["result"]["method_used"], "compile", f"{job}")
    compiled, compiled_probe = await frame(ed, "compile")
    ed.check_eq(prebuilt_probe, EXPECTED_PROBE, "the prebuilt game runs with the project's and the scene's settings")
    ed.check_eq(compiled_probe, prebuilt_probe, "the compiled game runs with the settings the prebuilt one does")

    diff = pngdiff.stats(prebuilt, compiled, premultiplied=True, tolerance=8)
    print(f"[test] prebuilt vs compile: {diff}")
    ed.check(diff is not None, f"the same size: {prebuilt[:2]} / {compiled[:2]}")
    ed.check(diff["fraction_over"] <= 0.002, f"the compiled game draws what the prebuilt one does: {diff}")
