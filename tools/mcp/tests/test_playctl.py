"""Fixed-step play control and standalone process adapters; no game project is required."""

import asyncio
import shutil
import stat
from pathlib import Path

from harness import Editor, skip

EDITOR = {"create": True, "name": "Play Clock"}


async def clock(ed: Editor) -> dict:
    return (await ed.call("get_play_state"))["time"]


def make_script(path: Path, body: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


async def test_auto_pause_freezes_between_calls(ed: Editor):
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60, time_scale=1)
    try:
        before = await clock(ed)
        ed.check(before["held"], before)
        # Elapsed wall time is the contract here: idle time must not advance the simulation.
        await asyncio.sleep(0.1)
        await ed.call_raw("screenshot", {"view": "game", "max_width": 160})
        after = await clock(ed)
        ed.check_eq(after["frames"], before["frames"])
        ed.check_eq(after["game_time"], before["game_time"])
        dt = await ed.call("eval_lua", code="return Engine.fixedDeltatime")
        ed.check(abs(dt["values"][0] - 1 / 60) < 1e-9, dt)
        await ed.call("play", action="stop")
        await ed.call("play", action="start")
        kept = await clock(ed)
        ed.check(kept["held"] and kept["auto_pause"] and kept["fixed_fps"] == 60,
                 "MCP clock settings survive Stop and apply to the next session")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0, time_scale=1)


async def test_wait_advances_exact_frames(ed: Editor):
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60, time_scale=1)
    try:
        before = await clock(ed)
        waited = await ed.call("wait", frames=6)
        ed.check_eq(waited["frames"], 6)
        ed.check(abs(waited["game_seconds"] - 0.1) < 1e-6, waited)
        ed.check_eq((await clock(ed))["frames"] - before["frames"], 6)
        timed = await ed.call("wait", game_seconds=0.05)
        ed.check_eq(timed["frames"], 3)
        stepped = await ed.call("play", action="step", frames=2)
        ed.check_eq(stepped["stepped_frames"], 2)
        ed.check(stepped["time"]["held"], "step restores the auto-pause hold")
        until = await ed.call("wait", until="Engine.frameCount % 10 == 3")
        ed.check_eq(until["reason"], "condition")
        ed.check_eq((await ed.call("eval_lua", code="return Engine.frameCount % 10"))["values"], [3])
        capped = await ed.call_raw("wait", {"until": "false", "game_seconds": 0.1})
        ed.check(capped.is_error and capped.structured["reason"] == "game_seconds", capped.text)
        ed.check_eq(capped.structured["frames"], 6, "an unmet condition stops at its game-time cap")
        await ed.call("play", action="set", time_scale=0.5)
        scaled = await ed.call("wait", frames=6)
        ed.check(abs(scaled["game_seconds"] - 0.05) < 1e-6, scaled)
        cut = await ed.call("wait", frames=2, input=[
            {"type": "key", "key": "up", "action": "down"},
            {"type": "wait", "frames": 100},
            {"type": "key", "key": "up", "action": "up"}])
        ed.check(cut["input"]["cut_short"], cut)
        ed.check_eq((await ed.call("eval_lua", code="return Input.isKeyPressed(Input.KEY_UP)"))["values"], [False],
                    "cutting an input sequence short releases its held keys")
        await ed.check_error("play", action="set", time_scale=-1)
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0, time_scale=1)


async def test_run_build_flags_lua_errors(ed: Editor):
    bin_dir = ed.scratch.root / "fake-players"
    broken = make_script(bin_dir / "broken", (
        'echo "Scene loaded"\n'
        'echo "(ERROR): Lua init() failed for \'PlayerProbe\': scripts/PlayerProbe.lua:12: boom"\n'
        'echo "(WARN): something odd"\n'
        'sleep 30\n'))
    raw = await ed.call_raw("run_build", {"path": str(broken), "seconds": 0.5, "screenshot": False})
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    ed.check_eq(r["error_count"], 1, f"errors: {r}")
    ed.check("Lua init() failed" in r["errors"][0], f"errors: {r['errors']}")
    ed.check_eq(r["warning_count"], 1, "warnings")
    ed.check(not r["crashed"], "a script error is not a crash")
    ed.check("ERROR" in raw.text, f"text: {raw.text}")
    await ed.check_error("run_build", {"path": str(broken), "input": [{"type": "gamepad", "button": "a"}]})
    await ed.check_error("run_build", {"path": str(broken), "input": [{"type": "key", "key": "nokey"}]},
                         contains="unknown key")


async def test_run_build_input_reaches_window(ed: Editor):
    xev = shutil.which("xev")
    if not xev:
        skip("xev not installed (stands in for a player window)")
    bin_dir = ed.scratch.root / "fake-players"
    player = make_script(bin_dir / "xev-player", f"exec {xev} -geometry 400x300+0+0\n")
    raw = await ed.call_raw("run_build", {
        "path": str(player),
        "input_delay_seconds": 0.1,
        "input": [
            {"type": "mouse", "action": "click", "x": 0.5, "y": 0.5},
            {"type": "key", "key": "space"},
            {"type": "text", "text": "Hi"},
            {"type": "screenshot"},
            {"type": "mouse", "action": "move", "x": 10, "y": 20},
        ],
        "screenshots_at": [0.05],
        "max_width": 400,
    })
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    print("[test] xev run:", raw.text[:600])
    inp = r["input"]
    ed.check_eq(inp["actions_done"], inp["actions_total"], f"input: {inp}")
    ed.check("error" not in inp, f"input error: {inp}")
    log = "\n".join(r["log"]) + "\n" + Path(r["log_file"]).read_text()
    ed.check("ButtonPress" in log and "ButtonRelease" in log, "the click reached the window")
    ed.check("(keysym 0x20, space)" in log, "space reached the window")
    ed.check("(keysym 0x48, H)" in log or "(keysym 0x68, h)" in log, "typed text reached the window")
    ed.check("MotionNotify" in log, "pointer motion")
    ed.check_eq(len(raw.images), 2, f"two screenshots: {r.get('screenshots')}")
    ed.check_eq({s["from"] for s in r["screenshots"]}, {"screenshots_at", "input"}, "both capture schedules return images")
    for i, (data, _) in enumerate(raw.images):
        (ed.scratch.root / f"player-input-{i}.png").write_bytes(data)
