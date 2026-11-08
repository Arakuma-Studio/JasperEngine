"""S81 Title Art export (docs/specs/S81-title-art-runtime.md R32, Deviation 23): a Glyph Atlas title's
prebaked atlas ships with a Desktop build and serves the player's first value.

Scene Hud (UI): a counter "1234" as a Glyph Atlas Title Art Renderer with Prebake Initial State, and a
Lua probe on it that prints its state once it is ready and quits. Built with the run's prebuilt glcore
player: the build holds the atlas manifest (<title>.glyphs-<style>.json) and its page PNGs beside the
title, the manifest's cells cover the counter's digits at each place, and the player, run headless,
logs that the atlas showed its first string after 0 cell renders, with the cells restored from the
build's prebake.

Set JASPER_SLOW_BUILD=1 to run it: it uses the run's prebuilt glcore runtime (tools/mcp/build_cache.py).
"""

import asyncio
import json
import os
import re
import subprocess
import time

import build_cache
from harness import REPO, Editor, skip

PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Title Export", "env": build_cache.player_env("glcore", "release")}
CI = True

TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

DOC = "assets/titles/score.titleart"

PROBE = """local Probe = {}
function Probe:start()
    self.frames = 0
    RegisterEngineEvent(self, 'onUpdate')
end
function Probe:onUpdate(dt)
    self.frames = self.frames + 1
    local t = TitleArt(self.scene, self.entity)
    if t.ready or self.frames >= 600 then
        print(string.format("glyphprobe frames=%d ready=%s pending=%s text=%s", self.frames, tostring(t.ready), t.pending, t.text))
        Engine.quit(0)
    end
end
return Probe
"""

FIRST = re.compile(r"Title Art 'TitleArt/Score': the glyph atlas showed its first string after (\d+) cell renders \((\d+) cells from")


async def wait_job(ed: Editor, job: dict, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while job.get("status") == "running":
        if time.monotonic() > deadline:
            ed.fail(f"job {job['job_id']} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)
        job = await ed.call("get_job", job_id=job["job_id"], log_lines=10)
    return job


async def test_glyph_atlas_prebake_ships_and_serves_the_first_value(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (the run builds its prebuilt runtime once)")
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("edit_title", path=DOC, create={"text": "1234", "canvas": [640, 200]}, bake="never")
    await ed.call("export_title", path=DOC)
    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/Hud")
    scene = made["scene"]["id"]
    await ed.call("create_ui", scene=scene, root={"type": "image", "name": "Score", "layout": {"anchor": "center", "size": [640, 200]}})
    await ed.call("add_component", scene=scene, entity="Score", component="titleart")
    await ed.call("set_properties", scene=scene, entity="Score",
                  values={"titleart": {"title": DOC, "renderMode": "glyph_atlas", "prebakeInitialState": True}})
    await ed.call("create_script", kind="lua", name="GlyphProbe", content=PROBE, entity="Score", scene=scene)
    saved = await ed.call("save_scene", scene=scene)
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scene"]["path"])

    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-title-glyphs"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    job = await wait_job(ed, job, TIMEOUT - 600)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq(job["result"]["method_used"], "prebuilt", f"{job}")
    ed.check("titleart" in job["result"].get("modules", []), f"the build ships the titleart module: {job['result']}")

    # the shipped atlas: its manifest beside the title, and the pages it names
    manifests = sorted(out.rglob("score.titleart.glyphs-*.json"))
    ed.check(len(manifests) == 1, f"one prebaked atlas manifest in {out}: {manifests}")
    manifest = json.loads(manifests[0].read_text())
    pages = sorted(manifests[0].parent.glob(manifests[0].stem + "-p*.png"))
    ed.check(pages, f"the atlas pages beside {manifests[0].name}: {sorted(p.name for p in manifests[0].parent.iterdir())}")
    for page in pages:
        ed.check(page.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", f"{page.name} is a PNG")
    cells = manifest.get("cells", [])
    ed.check(len(cells) >= 10, f"the digits at each of the counter's places: {len(cells)} cells ({sorted(manifest)})")
    ed.check(len(cells) <= 1024, f"at most 1,024 cells: {len(cells)}")
    ed.check(len(manifest.get("pages", [])) == len(pages) or "pages" not in manifest,
             f"the manifest names the pages that shipped: {manifest.get('pages')} vs {[p.name for p in pages]}")

    # the player, as a person runs it: the first value comes from the prebake
    run = await asyncio.to_thread(
        subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], "--headless", "--fixed-fps", "60",
                         "--quit-after", "900"],
        cwd=out, capture_output=True, text=True, timeout=900)
    log = run.stdout + run.stderr
    (ed.scratch.root / "title-glyphs-player.log").write_text(log)
    ed.check_eq(run.returncode, 0, f"the player quits: {log[-2000:]}")
    ed.check("glyphprobe" in log and "ready=true" in log, f"the counter is ready in the player: {log[-2000:]}")
    ed.check("text=1234" in log, f"it shows its initial value: {log[-2000:]}")
    first = FIRST.search(log)
    ed.check(first is not None, f"the atlas reports its first string: {log[-3000:]}")
    if first:
        ed.check_eq(int(first.group(1)), 0, "the first value renders no glyph cell: the build's prebake holds them")
        ed.check(int(first.group(2)) >= 4, f"cells restored from the prebake: {first.group(0)}")
