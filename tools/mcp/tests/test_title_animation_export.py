"""S85 AC11 (docs/specs/S85-title-animation.md R13, R16, R31): a Baked animated title in a prebuilt
Linux player. A script samples the animation at 0, 0.25 and 0.8 s (TitleArt:sample) and the player
screenshots the frame (--screenshot, headless, --fixed-fps 30); each capture equals the editor's
render_title {animation: {as: "baked"}} at that time (24/255 on all but 1 % of the pixels: the
build ships cooked, lossy textures). The .titleart is not in the content manifest and the player stays on the lite2d variant
(S81 R59). The player's Baked title costs +0 draws against an Image twin in the editor's Play.

Set JASPER_SLOW_BUILD=1 to run it (the run builds its prebuilt runtime once).
"""

import asyncio
import json
import os
import subprocess
from pathlib import Path

import build_cache
import pngdiff
from harness import Editor, skip

REPO = Path(__file__).resolve().parents[3]
CI = True
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Title Animation Export", "env": build_cache.player_env("glcore", "release")}
TIMEOUT = 1800

DOC = "assets/titles/anim.titleart"
TIMES = [0.0, 0.25, 0.8]
# the build ships the flipbooks cooked (je-cooked/, a lossy block format): edges vary a little from
# the editor's render (measured: worst 60/255 on 0.8 % of the pixels); the golden tests' tolerance covers that
TOLERANCE = 24
MAX_FRACTION = 0.01

SAMPLER = """local Sampler = { properties = {} }
function Sampler:init()
    self.title = TitleArt(self.scene, self.entity)
    self.time = tonumber(os.getenv("TITLE_SAMPLE_TIME") or "0")
    self.scene:setBackgroundColor(0, 0, 0, 1)
    RegisterEngineEvent(self, "onUpdate")
    self.frames = 0
end
function Sampler:onUpdate(dt)
    self.frames = self.frames + 1
    -- the title's own size, frozen at the sample time, every frame until the player quits
    self.title:setNativeSize()
    self.title:sample("Intro", self.time)
    if self.frames % 20 == 0 then Log.print("[anim] sampled " .. self.time) end
end
return Sampler
"""


async def wait_job(ed: Editor, job: dict, timeout: float) -> dict:
    deadline = asyncio.get_event_loop().time() + timeout
    while job["status"] == "running":
        if asyncio.get_event_loop().time() > deadline:
            ed.fail(f"job {job['job_id']} still running: {job}")
        await asyncio.sleep(2)
        job = await ed.call("get_job", job_id=job["job_id"], log_lines=10)
    return job


async def test_a_baked_animation_plays_in_the_exported_player(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real prebuilt player (the run builds its runtime once)")
    await ed.call("tool_groups", enable=["title_art", "animation"])
    # the game's canvas is the window: the title draws at its native pixels
    await ed.call("set_project_settings", values={"title_art.auto_bake": False, "graphics.canvas_width": 640, "graphics.canvas_height": 360})
    await ed.call("edit_title", path=DOC, create={"text": "WIN", "canvas": [640, 360]}, bake="never", ops=[
        {"op": "add_rim", "layer": "Title"},
        {"op": "add_animation", "name": "Intro", "duration": 1.0},
        {"op": "set_track", "animation": "Intro", "layer": "Title", "field": "rim#0.width", "keys": [[0, 2], [0.5, 24], [1, 2]]},
    ])
    bake = await ed.call("export_title", path=DOC)
    ed.check(bake.get("ok"), f"exported: {bake}")

    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/Hud")
    sid = made["scene"]["id"]
    await ed.call("create_ui", scene=sid, root={"type": "image", "name": "Logo", "layout": {"anchor": "center", "size": [640, 360]}})
    await ed.call("add_component", scene=sid, entity="Logo", component="titleart")
    await ed.call("set_properties", scene=sid, entity="Logo", values={"titleart": {"title": DOC, "playAutomatically": False}})
    await ed.call("create_script", kind="lua", name="Sampler", content=SAMPLER, entity="Logo", scene=sid)
    saved = await ed.call("save_scene", scene=sid)
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scene"]["path"])

    # the editor's Baked renders at the sample times
    editor_frames = {}
    for t in TIMES:
        save = f"renders/editor_{t:.2f}.png"
        await ed.call("render_title", path=DOC, save_path=save, backgrounds=["black"], image="none",
                      options={"animation": {"name": "Intro", "time": t, "as": "baked"}})
        editor_frames[t] = pngdiff.decode((ed.project / save).read_bytes())

    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-title-anim"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=900)
    job = await wait_job(ed, job, TIMEOUT - 300)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq(job["result"]["player_id"], runtime.name, f"{job}")
    ed.check_eq(job["result"]["method_used"], "prebuilt", f"{job}")
    # S81 R59: a Baked title needs no titleart module: nothing in the project asks for it, and the
    # build names the lite2d player as the one that fits (only the full player is installed here)
    ed.check("titleart" not in job["result"].get("module_reasons", {}), f"a Baked title adds no module: {job['result']}")
    variant = [w["message"] for w in job["result"].get("warnings", []) if w.get("code") == "W-PLAYER-VARIANT"]
    ed.check(len(variant) == 1 and "lite2d" in variant[0] and "titleart" not in variant[0], f"the variant that fits is lite2d, without titleart: {variant}")
    manifest = json.loads(next(out.rglob("content-manifest.json")).read_text())
    listed = [e if isinstance(e, str) else e.get("path", "") for e in manifest.get("files", manifest.get("assets", []))]
    ed.check(not any(p.endswith(".titleart") for p in listed), f"the .titleart is not shipped content: {listed[:30]}")

    # the player: one run per sample time
    bad = []
    captures = {}
    for t in TIMES:
        shot = ed.scratch.root / f"player_{t:.2f}.png"
        env = dict(os.environ)
        env["TITLE_SAMPLE_TIME"] = str(t)
        run = await asyncio.to_thread(
            subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], "--headless", "--fixed-fps", "30", "--after-ready",
                             "--quit-after", "120", "--resolution", "640x360", "--screenshot", str(shot), "--fail-on-error"],
            cwd=out, capture_output=True, text=True, timeout=600, env=env)
        log = run.stdout + run.stderr
        (ed.scratch.root / f"player_{t:.2f}.log").write_text(log)
        ed.check_eq(run.returncode, 0, f"the player ran and logged no error: {log[-2000:]}")
        ed.check("[anim] sampled" in log, f"the script sampled the animation: {log[-2000:]}")
        captures[t] = pngdiff.decode(shot.read_bytes())
        fraction, offset, worst = pngdiff.centre_match(captures[t], editor_frames[t], TOLERANCE)
        print(f"t={t}: {fraction:.5f} of the pixels differ by more than {TOLERANCE}/255 at offset {offset} (worst {worst}), capture {captures[t][:2]}, editor {editor_frames[t][:2]}")
        if fraction > MAX_FRACTION:
            bad.append(f"t={t}: {fraction:.4f} over tolerance at {offset} (worst {worst})")
    ed.check(not bad, "; ".join(bad))
    # red control: the capture differs from another time's editor frame, so the comparison tells poses apart
    fraction, _, _ = pngdiff.centre_match(captures[0.25], editor_frames[0.8], TOLERANCE)
    ed.check(fraction > MAX_FRACTION, f"a capture does not match the editor at another time: {fraction:.5f}")


async def test_a_baked_title_costs_no_extra_draws(ed: Editor):
    """AC11's +0 draws: in Play, a Baked title draws as one Image draw, the same as a plain Image twin."""
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    path = "assets/titles/stat.titleart"
    await ed.call("edit_title", path=path, create={"text": "STAT", "canvas": [640, 360]}, bake="never", ops=[
        {"op": "add_animation", "name": "Intro", "duration": 1.0},
        {"op": "set_track", "animation": "Intro", "field": "opacity", "keys": [[0, 0], [1, 1]]},
    ])
    await ed.call("export_title", path=path)
    made = await ed.call("create_scene", name="Stats", type="UI", path="scenes/Stats")
    sid = made["scene"]["id"]
    await ed.call("create_ui", scene=sid, root={"type": "image", "name": "Twin", "layout": {"anchor": "center", "size": [320, 180]}})
    await ed.call("save_scene", scene=sid)
    await ed.call("play", action="start", scene=sid)
    await ed.call("wait", frames=10)
    twin = await ed.call("get_render_stats")
    await ed.call("play", action="stop")
    await ed.call("add_component", scene=sid, entity="Twin", component="titleart")
    await ed.call("set_properties", scene=sid, entity="Twin", values={"titleart": {"title": path, "playAutomatically": True}})
    await ed.call("save_scene", scene=sid)
    await ed.call("play", action="start", scene=sid)
    await ed.call("wait", frames=20)
    title = await ed.call("get_render_stats")
    await ed.call("play", action="stop")
    ed.check_eq(title.get("draw_calls"), twin.get("draw_calls"), f"the Baked title draws like its Image twin: {title} vs {twin}")
