"""Dynamic resolution wiring (docs/specs/S13-post-fx.md R8, R9, R16, AC17): in Play the controller drives
the main camera's render scale from the frame cost. set_post_processing_debug feeds it a GPU time:
40 ms at a 60 fps target reaches the min scale within 2 s, 5 ms returns it to the max within 10 s;
force_render_scale 0.5 pins the scale the uber pass's source is drawn at (frame stats: the bloom chain's
prefilter, a quarter of that source, halves).
Edit mode never changes the scale, and Play's end releases the pins. Off by default."""

from __future__ import annotations

import os
import re
import time

from harness import Editor

# read at editor start: this file runs in its own process
os.environ["JASPERENGINE_READY_SIGNAL"] = "1"
os.environ["JASPERENGINE_FRAME_STATS"] = "all"

CI = True
EDITOR = {"create": True, "name": "Dynamic Resolution"}
TIMEOUT = 900

# the bloom chain's prefilter target is a quarter of the uber pass's source (the scene at the applied scale)
PREFILTER = re.compile(r"Frame stats \(frame \d+\): .*pass list: .*?bloom prefilter (\d+)x(\d+)")


async def status(ed: Editor, sid: int) -> dict:
    return await ed.call("get_post_processing_status", scene=sid)


async def wait_for(ed: Editor, sid: int, what: str, ok, seconds: float) -> tuple[dict, float]:
    """Polls the status (frames keep running) until ok(status), at most `seconds` of wall time."""
    start = time.monotonic()
    got = await status(ed, sid)
    while not ok(got) and time.monotonic() - start < seconds:
        await ed.call("wait", frames=3)
        got = await status(ed, sid)
    elapsed = time.monotonic() - start
    ed.check(ok(got), f"{what} within {seconds} s (after {elapsed:.2f} s): scene {got['scene_id']}, playing {got['playing']}, "
                      f"{got['dynamic_resolution']}")
    return got, elapsed


def source_sizes(ed: Editor, start: int) -> list[tuple[int, int]]:
    log = ed.scratch.root / "editor.log"
    lines = log.read_text(errors="replace").splitlines()[start:] if log.exists() else []
    return [(int(m.group(1)), int(m.group(2))) for m in (PREFILTER.search(line) for line in lines) if m]


def log_length(ed: Editor) -> int:
    log = ed.scratch.root / "editor.log"
    return len(log.read_text(errors="replace").splitlines()) if log.exists() else 0


async def test_dynamic_resolution_in_play(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "set_scene_settings", "play", "wait", "get_post_processing_status",
                     "set_post_processing_debug")
    # the project's start scene: Play runs it (a scene made here would not be the one playing)
    scenes = (await ed.call("list_scenes"))["scenes"]
    start = [s for s in scenes if s.get("start_scene")]
    ed.check(start, f"a start scene: {scenes}")
    sid = start[0]["id"]
    await ed.call("select_scene", scene=sid)
    await ed.call("create_entity", scene=sid, name="Ball", type="sphere")

    # off by default: nothing runs, the settings' scale applies
    got = await status(ed, sid)
    ed.check_eq(got["dynamic_resolution"]["enabled"], False)
    ed.check_eq(got["render_scale_applied"], 1.0)

    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": {
        "dynamic_resolution_enabled": True, "dynamic_resolution_min_scale": 0.6, "dynamic_resolution_target_fps": 60,
        "bloom_enabled": True}})
    got = await status(ed, sid)
    ed.check(got["uber"] is True and "Bloom" in got["uber_features"], f"the uber pass at scale 1: {got['uber_features']}")

    # edit mode: never runs, never changes the scale (a pin is held for Play)
    await ed.call("set_post_processing_debug", scene=sid, fake_gpu_ms=40)
    for _ in range(5):
        await ed.call("wait", frames=10)
        got = await status(ed, sid)
        ed.check(got["dynamic_resolution"]["running"] is False and got["dynamic_resolution"]["idle"] == "edit mode" and
                 got["render_scale_applied"] == 1.0,
                 f"edit mode keeps scale 1: {got['dynamic_resolution']}")
    await ed.call("set_post_processing_debug", scene=sid, fake_gpu_ms=None)

    await ed.call("play", action="start", scene=sid)
    try:
        await ed.call("wait", frames=30)
        got, _ = await wait_for(ed, sid, "running", lambda s: s["dynamic_resolution"]["running"], 5)
        ed.check_eq(got["dynamic_resolution"]["max_scale"], 1.0)
        ed.check(abs(got["dynamic_resolution"]["target_ms"] - 1000 / 60) < 0.01, f"target {got['dynamic_resolution']}")

        # a heavy GPU (40 ms at a 16.7 ms target): the GPU law drops to the min scale fast
        await ed.call("set_post_processing_debug", scene=sid, fake_gpu_ms=40)
        got, elapsed = await wait_for(ed, sid, "min scale",
                                      lambda s: abs(s["dynamic_resolution"]["scale"] - 0.6) < 1e-3, 2.0)
        ed.check_eq(got["dynamic_resolution"]["source"], "gpu")
        ed.check(got["dynamic_resolution"]["saturated_low"] is True, f"still over the target at the min: {got['dynamic_resolution']}")
        print(f"40 ms: min scale after {elapsed:.2f} s, {got['dynamic_resolution']['accepted_changes']} changes")

        # a light GPU (5 ms): back to the max, slowly
        await ed.call("set_post_processing_debug", scene=sid, fake_gpu_ms=5)
        got, elapsed = await wait_for(ed, sid, "max scale",
                                      lambda s: abs(s["dynamic_resolution"]["scale"] - 1.0) < 1e-3, 10.0)
        print(f"5 ms: max scale after {elapsed:.2f} s, {got['dynamic_resolution']['accepted_changes']} changes")
        ed.check(got["dynamic_resolution"]["accepted_changes"] <= 30, f"rare changes: {got['dynamic_resolution']}")

        # a pinned scale: the uber pass's source is drawn at half the size
        await ed.call("set_post_processing_debug", scene=sid, fake_gpu_ms=None)
        await ed.call("wait", frames=10)
        start = log_length(ed)
        full = source_sizes(ed, start - 400 if start > 400 else 0)
        await ed.call("set_post_processing_debug", scene=sid, force_render_scale=0.5)
        await ed.call("wait", frames=20)
        got = await status(ed, sid)
        ed.check_eq(got["render_scale_applied"], 0.5)
        ed.check(got["dynamic_resolution"]["overridden"] is True, "pinned")
        half = []
        deadline = time.time() + 20
        while time.time() < deadline and not half:
            half = source_sizes(ed, start)[-5:]
            if not half:
                await ed.call("wait", frames=10)
        ed.check(full and half, f"frame stats show the bloom prefilter ({len(full)}, {len(half)} lines)")
        if full and half:
            (fw, fh), (hw, hh) = full[-1], half[-1]
            ed.check(abs(hw - fw / 2) <= 1 and abs(hh - fh / 2) <= 1, f"the uber source halves (its quarter: {fw}x{fh} -> {hw}x{hh})")
    finally:
        await ed.call("play", action="stop")

    # stopped: the pins are released, edit mode draws at the settings' scale
    await ed.call("wait", frames=10)
    got = await status(ed, sid)
    ed.check(got["render_scale_applied"] == 1.0 and got["dynamic_resolution"]["overridden"] is False,
             f"Play's end releases the pin: {got}")
    ed.check(got["debug"]["force_render_scale"] is None and got["debug"]["fake_gpu_ms"] is None, f"pins cleared: {got['debug']}")

    # a pin needs a number or null
    await ed.check_error("set_post_processing_debug", scene=sid, force_render_scale=0.25, contains="0.5")
    await ed.check_error("set_post_processing_debug", scene=sid, fake_gpu_ms="fast", contains="number")
