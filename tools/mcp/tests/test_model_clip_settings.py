"""A model's clip settings (S77 R27-R29, R53, AC13): edit_animation on "model#Walk" with wrap and
root_motion writes the model's <model>.clips sidecar as one project undo step; get_asset_info and
get_animation read it back; Play loops a Walk set to Loop where it stopped before; undo puts the
sidecar's bytes back; an edit of the file on disk reloads; no sidecar is today's clip; unknown keys
survive; a source export ships the sidecar. With JASPER_SLOW_BUILD=1 a prebuilt Linux player plays
it looping too (red without the sidecar). Runs on a scratch copy of tools/fixtures/pose-animation.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import subprocess
from pathlib import Path

import build_cache
from harness import Editor, REPO, skip

EDITOR = {"project": REPO / "tools" / "fixtures" / "pose-animation", "env": build_cache.player_env("glcore", "release")}
CI = True
TIMEOUT = 1200
PREBUILT_PLAYERS = ["glcore-release"]

MODEL = "assets/models/hero_animated.gltf"
WALK = MODEL + "#Walk"
SIDECAR = MODEL + ".clips"

# what the player of Hero is doing past the end of the 1 s Walk
PROBE = """local ClipProbe = {}
function ClipProbe:init()
    self.t = 0
    self.done = false
    RegisterEngineEvent(self, "onPostUpdate")
end
function ClipProbe:onPostUpdate()
    self.t = self.t + Engine.deltatime
    if self.done or self.t < 1.6 then return end
    self.done = true
    local player = GameObject(self.scene, self.entity):getAnimationPlayer()
    print(string.format("clipprobe playing=%s", tostring(player:isPlaying())))
end
return ClipProbe
"""
PROBE_LINE = re.compile(r"clipprobe playing=(true|false)")


def sidecar(ed: Editor) -> Path:
    return ed.project / SIDECAR


def walk_of(info: dict) -> dict:
    return next(a for a in info["model"]["animations"] if a["name"] == "Walk")


async def probe_in_play(ed: Editor) -> str:
    await ed.console_since(None)
    await ed.call("play", action="start", fixed_fps=60)
    try:
        await ed.call("wait", game_seconds=2.0)
        lines = " ".join(str(e.get("message")) for e in await ed.console_since("last", limit=500))
    finally:
        await ed.call("play", action="stop", fixed_fps=0)
    found = PROBE_LINE.findall(lines)
    ed.check(found, f"the probe printed: {lines[-800:]}")
    return found[-1] if found else ""


async def test_wrap_writes_the_sidecar_and_play_loops(ed: Editor):
    before = await ed.call("get_animation", clip=WALK, max_keys=100)
    ed.check_eq(before["wrap"], "once", "the importer's wrap")
    ed.check_eq(walk_of(await ed.call("get_asset_info", path=MODEL))["wrap"], "once", "get_asset_info's")
    ed.check(walk_of(await ed.call("get_asset_info", path=MODEL))["root_motion"] is None, "no root_motion block")

    await ed.call("add_component", entity="Hero", component="animationplayer",
                  values={"animationplayer": {"clips": [{"clip": "models/hero_animated.gltf#Walk"}], "playAutomatically": True}})
    await ed.call("create_script", kind="lua", name="ClipProbe", content=PROBE, entity="Hero")
    ed.check_eq(await probe_in_play(ed), "false", "Once: the player stopped at the end")

    r = await ed.call("edit_animation", clip=WALK, wrap="loop")
    ed.check(r["changed"] and r["wrap"] == "loop", f"{r}")
    ed.check_eq(r["settings_file"], SIDECAR, f"{r}")
    ed.check_eq(json.loads(sidecar(ed).read_text()), {"version": 1, "clips": {"Walk": {"wrap": "loop"}}}, "the sidecar")
    ed.check_eq((await ed.call("get_animation", clip=WALK, max_keys=1))["wrap"], "loop", "the clip plays looping")
    ed.check_eq(walk_of(await ed.call("get_asset_info", path=MODEL))["wrap"], "loop", "get_asset_info reads it")
    again = await ed.call("edit_animation", clip=WALK, wrap="loop")
    ed.check(not again["changed"], f"the same value writes nothing: {again}")
    ed.check_eq(await probe_in_play(ed), "true", "Loop: the player plays on past the end")

    # one undo step: no sidecar again, and the clip is today's byte for byte
    await ed.call("undo", scope="project")
    ed.check(not sidecar(ed).exists(), "undo removes the sidecar it made")
    after = await ed.call("get_animation", clip=WALK, max_keys=100)
    for got in (before, after):
        got.pop("stats", None)
        got.pop("users", None)  # Hero's player came between the two
    ed.check_eq(after, before, "no sidecar: the clip as before")


async def test_unknown_keys_survive_and_undo_restores_bytes(ed: Editor):
    text = '{\n  "version": 1,\n  "clips": {"Idle": {"wrap": "ping_pong", "note": "by hand"}},\n  "tool": {"x": 1}\n}\n'
    sidecar(ed).write_text(text)
    await ed.call("sync_files", paths=[SIDECAR])
    ed.check_eq((await ed.call("get_animation", clip=MODEL + "#Idle", max_keys=1))["wrap"], "ping_pong", "an edit on disk reloads")

    r = await ed.call("edit_animation", clip=WALK, root_motion={"bone": "mixamorig:Hips", "bake_position_xz": True})
    ed.check_eq(r["root_motion"], {"bone": "mixamorig:Hips", "bake_rotation": False, "bake_position_y": True, "bake_position_xz": True}, f"{r}")
    written = json.loads(sidecar(ed).read_text())
    ed.check_eq(written["tool"], {"x": 1}, "a top-level key it does not know")
    ed.check_eq(written["clips"]["Idle"], {"wrap": "ping_pong", "note": "by hand"}, "another clip's entry and its unknown key")
    ed.check_eq(written["clips"]["Walk"]["root_motion"]["bone"], "mixamorig:Hips", f"{written}")
    walk = walk_of(await ed.call("get_asset_info", path=MODEL))
    ed.check_eq(walk["root_motion"], r["root_motion"], f"get_asset_info: {walk}")
    ed.check_eq(walk["wrap"], "once", "root_motion alone leaves wrap")

    await ed.call("undo", scope="project")
    ed.check_eq(sidecar(ed).read_text(), text, "undo puts the bytes back")
    sidecar(ed).unlink()
    await ed.call("sync_files", paths=[SIDECAR])
    ed.check_eq((await ed.call("get_animation", clip=MODEL + "#Idle", max_keys=1))["wrap"], "once", "a deleted sidecar reloads too")


async def test_a_models_other_fields_keep_todays_errors(ed: Editor):
    r = await ed.call_raw("edit_animation", {"clip": WALK, "tracks": [{"path": "x", "channel": "position", "keys": []}]})
    ed.check(r.is_error and "is a model clip (read-only)" in r.text, r.text)
    r = await ed.call_raw("edit_animation", {"clip": WALK, "wrap": "loop", "events": []})
    ed.check(r.is_error and "is a model clip (read-only)" in r.text, f"wrap with another field: {r.text}")
    r = await ed.call_raw("edit_animation", {"clip": MODEL + "#Run", "wrap": "loop"})
    ed.check(r.is_error and "Run" in r.text, r.text)
    r = await ed.call_raw("edit_animation", {"clip": WALK, "wrap": "default"})
    ed.check(r.is_error and "ping_pong" in r.text, r.text)
    r = await ed.call_raw("edit_animation", {"clip": WALK, "root_motion": {"bake_y": True}})
    ed.check(r.is_error and "bake_position_y" in r.text, r.text)
    ed.check(not sidecar(ed).exists(), "a refused call writes nothing")
    r = await ed.call_raw("edit_animation", {"clip": MODEL + "#4", "wrap": "clamp_forever"})
    ed.check(not r.is_error and r.structured["clip"] == MODEL + "#Wave 2", f"an index names the clip: {r.text}")
    await ed.call("undo", scope="project")
    # root_motion in scene mode is a clip-file setting
    r = await ed.call_raw("edit_animation", {"target": "Hero", "root_motion": {}})
    ed.check(r.is_error and "root_motion" in r.text, r.text)


async def test_an_anim_takes_root_motion(ed: Editor):
    clip = "assets/anim/s74_root.anim"
    await ed.call("edit_animation", clip=clip, create={}, tracks=[
        {"path": "mixamorig:Hips", "channel": "position", "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 1, "value": [0, 0, 1]}]}])
    await ed.call("edit_animation", clip=clip, root_motion={"bake_rotation": True})
    block = json.loads((ed.project / clip).read_text())["root_motion"]
    ed.check_eq(block, {"bone": "", "bake_rotation": True, "bake_position_y": True, "bake_position_xz": False}, "the .anim's block")


async def test_exports_ship_the_sidecar(ed: Editor):
    await ed.call("edit_animation", clip=WALK, wrap="loop")
    await ed.call("save_scene", scene="Main Scene")
    out = ed.scratch.root / "export-source"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=900)
    while r.get("status") == "running":
        await asyncio.sleep(2)
        r = await ed.call("get_job", job_id=r["job_id"])
    shipped = out / "project" / "assets" / "models" / "hero_animated.gltf.clips"
    ed.check(shipped.is_file(), f"the sidecar ships beside the model: {r}")
    ed.check_eq(shipped.read_text(), sidecar(ed).read_text(), "as written")


async def test_a_linux_player_plays_the_sidecar(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real player (the run builds its prebuilt runtime once)")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])
    runtime = await build_cache.player_runtime(ed, "glcore", "release")

    async def run_export(name: str) -> str:
        out = ed.scratch.root / name
        job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
        while job["status"] == "running":
            await asyncio.sleep(2)
            job = await ed.call("get_job", job_id=job["job_id"])
        ed.check_eq(job["status"], "succeeded", f"{job}")
        ed.check_eq(job["result"]["player_id"], runtime.name, f"{job}")
        run = await asyncio.to_thread(
            subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], "--headless", "--fixed-fps", "60",
                             "--after-ready", "--quit-after", "150"],
            cwd=out, capture_output=True, text=True, timeout=600)
        log = run.stdout + run.stderr
        found = PROBE_LINE.findall(log)
        ed.check(found, f"the probe printed in the player: {log[-1500:]}")
        return found[-1] if found else ""

    ed.check_eq(await run_export("export-loop"), "true", "the exported Walk loops")
    sidecar(ed).unlink()
    await ed.call("sync_files", paths=[SIDECAR])
    ed.check_eq(await run_export("export-once"), "false", "without the sidecar it stops (the red control)")


async def test_the_sidecar_moves_with_its_model(ed: Editor):
    await ed.call("edit_animation", clip=MODEL + "#Idle", wrap="ping_pong")
    text = sidecar(ed).read_text()
    r = await ed.call_raw("move_asset", {"path": MODEL, "to": "assets/models/hero_moved.gltf"})
    ed.check(not r.is_error, r.text[:500])
    moved = ed.project / "assets/models/hero_moved.gltf.clips"
    ed.check(moved.is_file() and not sidecar(ed).exists(), "the .clips file goes with the model")
    ed.check_eq(moved.read_text(), text, "unchanged")
    ed.check_eq((await ed.call("get_animation", clip="assets/models/hero_moved.gltf#Idle", max_keys=1))["wrap"], "ping_pong",
                "the moved model's clip keeps its setting")
