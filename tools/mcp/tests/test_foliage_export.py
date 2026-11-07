"""T5 AC29, AC39: foliage in a built game. A prebuilt player with no runtime shader compiler draws what
the export collected, grass and trees, from the cooked scene and its tree file."""

from __future__ import annotations

import os

import build_cache
from harness import Editor, skip

import test_foliage as common

CI = True
EDITOR = {"create": True, "name": "Foliage Export"}
PREBUILT_PLAYERS = ["glcore-release"]

TIMEOUT = 1500


async def test_prebuilt_lite_player_draws_foliage(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (the run builds its prebuilt runtime once)")
    sid, terrain = await common.terrain_scene(ed, "Exported")
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {
        "foliageLayers": [{"density": 2.0, "meshPath": "assets/blade.obj", "drawDistance": 60, "maxSlope": 90}]}})
    await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[
        {"op": "scatter_by_rules", "target": {"detail_layer": 0}, "density": 2.0, "seed": 1}])
    await ed.call("edit_terrain", scene=sid, entity=terrain, space="local",
                  ops=[{"op": "place_trees", "prototype": 0, "points": [[0, 0], [8, 0], [-8, 0]]}])
    camera = (await ed.call("create_entity", scene=sid, name="View", type="camera", position=[0, 20, 30]))["entity_id"]
    await ed.call("set_properties", scene=sid, entity=camera, values={"camera": {"target": [0, 8, 0]}})
    await ed.call("save_scene", scene=sid)
    await ed.call("set_project_settings", values={"graphics": {"runtime_shaders": False}})

    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-foliage"
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    job = r if r["status"] != "running" else await ed.call("get_job", job_id=r["job_id"], wait_seconds=TIMEOUT)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
    ed.check(list(out.rglob("*.jetrees")) or list(out.rglob("*.jpak")), "the tree file ships")

    raw = await ed.call_raw("run_build", {"after_ready": True, "fixed_fps": 60, "quit_after": 120})
    ed.check(not raw.is_error, raw.text)
    ed.check(not raw.value["crashed"], f"the player crashed: {raw.value}")
    ed.check(raw.images, f"the player's own screenshot: {raw.text[:2000]}")
    import pngdiff
    width, height, rgba = pngdiff.decode(raw.images[0][0])
    background = rgba[0:3]
    differing = sum(1 for at in range(0, width * height * 4, 4)
                    if max(abs(rgba[at + c] - background[c]) for c in range(3)) > 24)
    ed.check(differing > width * height / 50, f"the frame shows more than the sky: {differing} pixels")
