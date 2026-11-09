"""S81 Title Art export (docs/specs/S81-title-art-runtime.md R32, Deviation 23): a Glyph Atlas title's
prebaked atlas ships with a Desktop build and serves the player's first value.

Scene Hud (UI): a counter "1234" as a Glyph Atlas Title Art Renderer with Prebake Initial State, and a
Lua probe on it that prints its state once it is ready and quits. Built with the run's prebuilt glcore
player: the build holds the atlas manifest (<title>.glyphs-<style>.json) and its page PNGs beside the
title, the manifest's cells cover the counter's digits at each place, and the player, run headless,
shows the prebaked initial state at once and, on a new value of the same places, logs that the atlas
showed its first string after 0 cell renders, with the cells restored from the build's prebake.

Set JASPER_SLOW_BUILD=1 to run it: it uses the run's prebuilt glcore runtime (tools/mcp/build_cache.py).
"""

import asyncio
import json
import os
import re
import subprocess
import time
from pathlib import Path

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
function Probe:report(what, t)
    print(string.format("glyphprobe %s frames=%d ready=%s pending=%s text=%s", what, self.frames, tostring(t.ready), t.pending, t.text))
end
function Probe:onUpdate(dt)
    self.frames = self.frames + 1
    local t = TitleArt(self.scene, self.entity)
    if self.frames >= 900 then
        self:report("timeout", t)
        Engine.quit(0)
    elseif not self.changed and t.ready then
        -- the prebaked image of the initial state is up at once; a new value (every digit of it
        -- at a place the prebake covers) composes from the shipped atlas
        self:report("first", t)
        t.text = "4321"
        self.changed = true
    elseif self.changed and t.ready and t.text == "4321" then
        self:report("second", t)
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


def build_text(job: dict) -> str:
    """What a build said: its warnings and its log."""
    result = job.get("result") or {}
    log = Path(result["log_file"]).read_text(errors="replace") if result.get("log_file") else ""
    return json.dumps(result.get("warnings", [])) + log


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
    shipped = sorted(p.relative_to(out).as_posix() for p in out.rglob("*.titleart*"))
    said = [line for line in build_text(job).splitlines() if "itle Art" in line or "titleart" in line or "Prebak" in line]
    ed.check(len(manifests) == 1, f"one prebaked atlas manifest in {out}: {manifests}; {shipped}; {said}")
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
    ed.check("glyphprobe first" in log and "ready=true pending= text=1234" in log, f"the initial value is ready: {log[-2000:]}")
    frames = re.search(r"glyphprobe first frames=(\d+)", log)
    ed.check(frames is not None and int(frames.group(1)) <= 3, f"R11: the prebaked initial state shows at once: {log[-2000:]}")
    ed.check("glyphprobe second" in log and "text=4321" in log, f"the new value is ready: {log[-2000:]}")
    first = FIRST.search(log)
    ed.check(first is not None, f"the atlas reports its first string: {log[-3000:]}")
    if first:
        ed.check_eq(int(first.group(1)), 0, "the player's first composed value renders no glyph cell: the build's prebake holds them")
        ed.check(int(first.group(2)) >= 4, f"cells restored from the prebake: {first.group(0)}")
