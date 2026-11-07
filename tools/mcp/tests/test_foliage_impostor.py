"""T5 AC15, AC35, AC38: Bake Impostor, by op: a job over frames, atlases on the prototype, a silhouette
like the mesh's, out of date when the mesh changes, refused in Play."""

from __future__ import annotations

import asyncio
import sys
import time
from pathlib import Path


from harness import Editor

sys.path.insert(0, str(Path(__file__).parent))
import test_foliage as common  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Foliage Impostor"}


async def wait_job(ed: Editor, job_id: str, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id)
        if job["status"] != "running" or time.monotonic() > deadline:
            return job
        await asyncio.sleep(0.25)


async def bake(ed: Editor, sid: int, terrain: int, **extra) -> dict:
    started = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "bake_impostors", "views": 8, "resolution": 128, **extra}],
                            wait=False)
    # a loaded host's software renderer bakes slowly: the job's own state, not a wall-clock guess
    done = await wait_job(ed, started["job_id"], 900)
    ed.check_eq(done.get("status"), "succeeded", f"the bake: {done}")
    return done


def coverage(png: bytes) -> tuple[set, tuple[float, float, float]]:
    import pngdiff
    width, height, rgba = pngdiff.decode(png)
    background = rgba[0:3]
    pixels = set()
    totals = [0, 0, 0]
    for y in range(height):
        for x in range(width):
            at = (y * width + x) * 4
            if max(abs(rgba[at + c] - background[c]) for c in range(3)) > 24:
                pixels.add((x, y))
                for c in range(3):
                    totals[c] += rgba[at + c]
    count = max(len(pixels), 1)
    return pixels, (totals[0] / count, totals[1] / count, totals[2] / count)


async def test_bake_op_job_and_refuses_in_play(ed: Editor):
    sid, terrain = await common.terrain_scene(ed, "Bake")
    started = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "bake_impostors", "views": 4, "resolution": 128}], wait=False)
    ed.check("job_id" in started, f"{started}")
    job = await wait_job(ed, started["job_id"], 120)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    got = await ed.call("sample_terrain", scene=sid, entity=terrain, foliage=True, grid=0)
    ed.check_eq(got["foliage"]["trees"][0]["impostor_state"], "baked", f"{got['foliage']}")

    await ed.check_error("edit_terrain", {"scene": sid, "entity": terrain, "ops": [
        {"op": "bake_impostors"}, {"op": "clear_foliage", "target": "trees"}]}, contains="alone")

    await ed.call("save_scene", scene=sid)
    await ed.call("play", action="start", scene=sid, fixed_fps=60)
    try:
        await ed.check_error("edit_terrain", {"scene": sid, "entity": terrain, "ops": [{"op": "bake_impostors"}]}, contains="Play")
    finally:
        await ed.call("play", action="stop")


async def test_bake_goes_on_in_an_idle_editor(ed: Editor):
    sid, terrain = await common.terrain_scene(ed, "IdleBake")
    started = await ed.call("edit_terrain", scene=sid, entity=terrain, ops=[{"op": "bake_impostors", "views": 4, "resolution": 128}], wait=False)
    # A bake renders a view a frame; with no call to wake it, the editor must keep framing by itself
    quiet = 60.0
    await asyncio.sleep(quiet)
    job = await wait_job(ed, started["job_id"], 900)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check(job["elapsed_seconds"] < quiet, f"the bake finished while nothing called the editor: {job}")


async def test_panel_state_goes_out_of_date(ed: Editor):
    sid, terrain = await common.terrain_scene(ed, "OutOfDate")
    await bake(ed, sid, terrain)
    got = await ed.call("sample_terrain", scene=sid, entity=terrain, foliage=True, grid=0)
    ed.check_eq(got["foliage"]["trees"][0]["impostor_state"], "baked", f"{got['foliage']}")
    (ed.project / "assets" / "tree.obj").write_text(common.CROSS + "v 0 9 0\nf 1//1 2//1 9//1\n")
    got = await ed.call("sample_terrain", scene=sid, entity=terrain, foliage=True, grid=0)
    ed.check_eq(got["foliage"]["trees"][0]["impostor_state"], "out_of_date", f"{got['foliage']}")


async def test_silhouette_matches_mesh(ed: Editor):
    sid, terrain = await common.terrain_scene(ed, "Silhouette")
    await bake(ed, sid, terrain)
    await ed.call("edit_terrain", scene=sid, entity=terrain, space="local", ops=[{"op": "place_trees", "prototype": 0, "points": [[0, 0]]}])
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {"billboardStart": 5000, "fadeLength": 0.1}})
    await ed.call("set_scene_camera", scene=sid, position=[20, 15, 20], target=[0, 3, 0])
    await ed.call("wait", seconds=60, frames=30)
    mesh = await ed.screenshot("scene", scene=sid)
    await ed.call("set_properties", scene=sid, entity=terrain, values={"terrain": {"billboardStart": 0.5, "fadeLength": 0.1, "maxMeshTrees": 0}})
    await ed.call("wait", seconds=60, frames=30)
    impostor = await ed.screenshot("scene", scene=sid)
    a, colour_a = coverage(mesh)
    b, colour_b = coverage(impostor)
    union = len(a | b)
    iou = len(a & b) / union if union else 0.0
    ed.check(iou >= 0.85, f"coverage IoU {iou:.3f}")
    ed.check(max(abs(colour_a[c] - colour_b[c]) for c in range(3)) <= 20, f"mean colours {colour_a} {colour_b}")
