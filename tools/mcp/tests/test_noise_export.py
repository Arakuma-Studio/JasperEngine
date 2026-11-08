"""What a game ships of noise (T2 R43-R45; AC35): a project that only bakes ships no .noisegraph and does not need the
noise module; a project whose Lua uses NoiseGraph ships its graphs, needs the module, and its prebuilt player evaluates
the graph to the bits the editor gives.

Set JASPER_SLOW_BUILD=1 to run it: it uses the run's prebuilt glcore runtime (build_cache). The variant choice itself
(lite3d for a baked project, core for a Noise user) and the ship decision are NoiseExport.* in the editor tests, which build no player.
"""

import asyncio
import json
import os
import shutil
import subprocess
from pathlib import Path

import build_cache
from harness import Editor, skip

REPO = Path(__file__).resolve().parents[3]
CI = True
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Noise Export", "env": build_cache.player_env("glcore", "release")}

GRAPH = "graphs/Plains.noisegraph"

SCRIPT = """local Sampler = {}
function Sampler:start()
    local graph = NoiseGraph.load("graphs/Plains.noisegraph")
    local grid = graph:sampleGrid{x = 0, z = 0, width = 64, depth = 64, step = 1 / 64, seed = 7}
    print("noise-export grid " .. grid.hash)
end
return Sampler
"""


async def export(ed: Editor, name: str):
    out = ed.scratch.root / name
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    while job["status"] == "running":
        await asyncio.sleep(2)
        job = await ed.call("get_job", job_id=job["job_id"])
    ed.check_eq(job["status"], "succeeded", f"{job}")
    return out, job


async def test_only_runtime_use_ships_graphs(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real player (the run builds its prebuilt runtime once)")
    # a created project's assets are its root, where a game reads "graphs/..." from
    (ed.project / "graphs").mkdir(parents=True, exist_ok=True)
    shutil.copy(REPO / "editor" / "resources" / "noisegraphs" / "plains.noisegraph", ed.project / GRAPH)
    await ed.call("sync_files")
    await ed.call("create_entity", name="Holder", type="empty")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])
    await build_cache.player_runtime(ed, "glcore", "release")

    # Baked: the graph is a recipe for the editor, so it stays home
    baked, job = await export(ed, "export-baked")
    ed.check("noise" not in job["result"]["modules"], f"a graph file is no evidence: {job['result'].get('module_reasons')}")
    ed.check_eq(list(baked.rglob("*.noisegraph")), [], "no .noisegraph in a baked game's payload")

    # Used at run time: every graph ships, and the player evaluates it
    await ed.call("create_script", kind="lua", name="Sampler", content=SCRIPT, entity="Holder")
    await ed.call("save_project")
    expected = (await ed.call("preview_noise_graph", path=GRAPH, size=64, seed=7, images="none"))["outputs"]["height"]
    used, job = await export(ed, "export-used")
    ed.check("noise" in job["result"]["modules"], f"a Lua NoiseGraph user needs the module: {job['result'].get('module_reasons')}")
    ed.check(list(used.rglob("Plains.noisegraph")), "the graph a script loads ships")
    manifest = json.loads(next(used.rglob("content-manifest.json")).read_text())
    ed.check("noise" in manifest["modules"], f"{manifest['modules']}")

    run = await asyncio.to_thread(
        subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], "--headless", "--fixed-fps", "60",
                         "--after-ready", "--quit-after", "30"],
        cwd=used, capture_output=True, text=True, timeout=600)
    log = run.stdout + run.stderr
    ed.check_eq(run.returncode, 0, f"the player quits after its frames: {log[-2000:]}")
    ed.check(f"noise-export grid {expected}" in log, f"the player's grid is the editor's ({expected}): {log[-1500:]}")
