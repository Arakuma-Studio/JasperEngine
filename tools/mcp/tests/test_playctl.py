"""Deterministic play-mode time control and driving built players.

play's game clock (time_scale, fixed_fps, auto_pause) against the TappyPlane fixture (a 2D Lua
game: GameObject "Game" carries the GameController script; "ready" → space → "playing" → it falls
without flaps and crashes → "dead"), then run_build: Lua errors in a player's log, input into a
player window (xev stands in for a player), and, with JASPER_SLOW_BUILD=1, a real TappyPlane
Desktop player driven with input + screenshots. Tests run in order and share one editor.
"""

import asyncio
import os
import shutil
import stat
import time
from pathlib import Path

from harness import Editor, skip

# the slow Desktop build compiles the engine (~20-60 min at 3 jobs)
TIMEOUT = 7800 if os.environ.get("JASPER_SLOW_BUILD", "") not in ("", "0") else 900
JOBS = 3   # the host is shared: never more


async def game(ed: Editor, field: str = "state"):
    v = await ed.call("eval_lua", code=f"return script('Game').{field}")
    return v["values"][0]


async def clock(ed: Editor) -> dict:
    return (await ed.call("get_play_state"))["time"]


def make_script(path: Path, body: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


# -- play-mode clock ---------------------------------------------------------------


async def test_auto_pause_freezes_between_calls(ed: Editor):
    started = await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"start: {started}")
    print(f"[test] play start: {started.get('startup_seconds')} s, clock {started['time']}")
    t = started["time"]
    ed.check(t["auto_pause"] and t["fixed_fps"] == 60, f"clock settings: {t}")

    before = await clock(ed)
    ed.check(before["held"], f"auto_pause: held between calls: {before}")
    await asyncio.sleep(1.5)
    after = await clock(ed)
    ed.check_eq(after["frames"], before["frames"], "frames advanced while no call ran")
    ed.check_eq(after["game_time"], before["game_time"], "game time advanced while no call ran")
    ed.check_eq(await game(ed), "ready", "the game waits")
    v = await ed.call("eval_lua", code="return Engine.gameTime, Engine.frameCount, Engine.fixedDeltatime")
    await asyncio.sleep(0.5)
    v2 = await ed.call("eval_lua", code="return Engine.gameTime, Engine.frameCount")
    ed.check_eq(v2["values"], v["values"][:2], "Engine.gameTime / frameCount frozen")
    ed.check(abs(v["values"][2] - 1 / 60) < 1e-9, f"Engine.fixedDeltatime: {v['values']}")

    # a screenshot of the held game: the frame it stopped at
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 400})
    ed.check(not shot.is_error and shot.images, shot.text)
    ed.check(not shot.structured.get("flat_colour"), f"held frame is blank: {shot.text}")
    ed.check_eq((await clock(ed))["frames"], after["frames"], "a screenshot does not advance the game")


async def test_wait_advances_exact_frames(ed: Editor):
    c0 = await clock(ed)
    w = await ed.call("wait", frames=30)
    ed.check_eq(w["reason"], "frames", f"wait: {w}")
    ed.check_eq(w["frames"], 30, "frames waited")
    ed.check(abs(w["game_seconds"] - 0.5) < 1e-6, f"30 frames at 1/60 s: {w['game_seconds']}")
    c1 = await clock(ed)
    ed.check_eq(c1["frames"] - c0["frames"], 30, f"session frames: {c0} -> {c1}")
    ed.check(c1["held"], "held again after the wait")
    await asyncio.sleep(0.5)
    ed.check_eq((await clock(ed))["frames"], c1["frames"], "nothing ran after the wait")

    g = await ed.call("wait", game_seconds=0.25)
    ed.check_eq(g["reason"], "game_seconds", f"game_seconds: {g}")
    ed.check_eq(g["frames"], 15, "0.25 s at 60 fps")

    stepped = await ed.call("play", action="step", frames=7)
    ed.check_eq(stepped["stepped_frames"], 7, f"step: {stepped}")
    ed.check_eq(stepped["state"], "playing", "auto_pause steps without pausing")
    ed.check(stepped["time"]["held"], "held after the step")

    # until: the condition is checked between frames, and the game stops right there
    u = await ed.call("wait", until="Engine.frameCount % 10 == 3", timeout_seconds=30)
    ed.check_eq(u["reason"], "condition", f"until: {u}")
    v = await ed.call("eval_lua", code="return Engine.frameCount % 10")
    ed.check_eq(v["values"], [3], "held on the frame the condition became true")


async def test_time_scale_halves_game_time(ed: Editor):
    s = await ed.call("play", action="set", time_scale=0.5)
    ed.check_eq(s["time"]["time_scale"], 0.5, f"set: {s}")
    v = await ed.call("eval_lua", code="return Engine.timeScale")
    ed.check_eq(v["values"], [0.5], "Engine.timeScale")
    w = await ed.call("wait", frames=60)
    ed.check(abs(w["game_seconds"] - 0.5) < 1e-6, f"60 frames x 1/60 x 0.5: {w['game_seconds']}")

    # real time (no fixed step, no auto_pause): game time runs at half the wall clock
    await ed.call("play", action="set", fixed_fps=0, auto_pause=False)
    w = await ed.call("wait", seconds=2)
    ratio = w["game_seconds"] / w["elapsed_seconds"]
    ed.check(0.3 < ratio < 0.6, f"time_scale 0.5 in real time: {w['game_seconds']} game s in {w['elapsed_seconds']} s")
    ed.check(not w["time"]["held"], "not held without auto_pause")
    await ed.call("play", action="set", time_scale=1, fixed_fps=60, auto_pause=True)
    await ed.check_error("play", {"action": "set", "time_scale": -1})


async def test_send_input_and_wait_compose(ed: Editor):
    ed.check_eq(await game(ed), "ready", "ready")
    # the flap starts a run; the game freezes as soon as the input is done
    r = await ed.call("send_input", events=[{"type": "key", "key": "space", "hold_frames": 1}], frames_after=1)
    ed.check_eq(await game(ed), "playing", f"space starts the run: {r}")
    held = await clock(ed)
    ed.check(held["held"], "held after send_input")
    await asyncio.sleep(1.0)
    ed.check_eq(await game(ed), "playing", "a frozen plane can't crash while the agent thinks")

    # a flap whenever the plane sinks below its spawn height keeps it flying
    spawn = await game(ed, "spawnY")
    flaps = 0
    for _ in range(12):
        w = await ed.call("wait", until=f"script('Game').planeY < {spawn} - 40 or script('Game').state ~= 'playing'",
                          timeout_seconds=60)
        ed.check_eq(w["reason"], "condition", f"wait: {w}")
        ed.check_eq(await game(ed), "playing", f"the plane crashed after {flaps} flap(s)")
        w = await ed.call("wait", input=[{"type": "key", "key": "space", "hold_frames": 1}], frames=6)
        ed.check_eq(w["input"]["steps_done"], 2, f"input ran: {w}")
        flaps += 1
        if (await clock(ed))["game_time"] > held["game_time"] + 2.5:
            break
    ed.check_eq(await game(ed), "playing", "kept alive by flaps")
    ed.check((await clock(ed))["game_time"] > held["game_time"] + 1.5, "flew for a while")

    # without flaps it falls and crashes: wait until dead, in game time
    w = await ed.call("wait", until="script('Game').state == 'dead'", timeout_seconds=60)
    ed.check_eq(w["reason"], "condition", f"falls without flaps: {w}")
    ed.check(w["game_seconds"] < 2.0, f"crashed within 2 game seconds: {w['game_seconds']}")

    # input cut short by the wait's condition: what it pressed is released
    await ed.call("wait", game_seconds=0.6)
    w = await ed.call("wait", input=[{"type": "key", "key": "up", "action": "down"}, {"type": "wait", "frames": 100},
                                     {"type": "key", "key": "up", "action": "up"}], frames=5)
    ed.check(w["input"]["cut_short"], f"cut short: {w['input']}")
    ed.check(not w.get("held_input", {}).get("keys"), f"released: {w.get('held_input')}")
    await ed.call("play", action="stop")


async def test_settings_stay_and_restart(ed: Editor):
    st = await ed.call("get_play_state")
    ed.check(st["time"]["auto_pause"] and st["time"]["fixed_fps"] == 60, f"settings kept after stop: {st['time']}")
    started = await ed.call("play", action="start")
    ed.check(started["time"]["auto_pause"], "kept for the next session")
    c0 = await clock(ed)
    await asyncio.sleep(1.0)
    ed.check_eq((await clock(ed))["frames"], c0["frames"], "the new session is held too")
    await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    st = await ed.call("get_play_state")
    ed.check(not st["time"]["auto_pause"] and st["time"]["fixed_fps"] is None, f"reset: {st['time']}")


# -- built players -------------------------------------------------------------------


async def test_run_build_flags_lua_errors(ed: Editor):
    bin_dir = ed.scratch.root / "fake-players"
    broken = make_script(bin_dir / "broken", (
        'echo "Scene loaded"\n'
        'echo "(ERROR): Lua init() failed for \'GameController\': scripts/GameController.lua:12: boom"\n'
        'echo "(WARN): something odd"\n'
        'sleep 30\n'))
    raw = await ed.call_raw("run_build", {"path": str(broken), "seconds": 2, "screenshot": False})
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
        "input_delay_seconds": 0.5,
        "input": [
            {"type": "mouse", "action": "click", "x": 0.5, "y": 0.5},
            {"type": "key", "key": "space"},
            {"type": "text", "text": "Hi"},
            {"type": "screenshot"},
            {"type": "mouse", "action": "move", "x": 10, "y": 20},
        ],
        "screenshots_at": [0.3],
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
    ed.check_eq([s["from"] for s in r["screenshots"]], ["screenshots_at", "input"], f"order: {r['screenshots']}")


async def test_desktop_player_driven(ed: Editor):
    prebuilt = os.environ.get("JASPER_PLAYER", "")   # a TappyPlane player built earlier: skips the build
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0") and not prebuilt:
        skip("set JASPER_SLOW_BUILD=1 to build TappyPlane as a Desktop player and drive it (compiles the engine), "
             "or JASPER_PLAYER=<its executable>")
    if prebuilt:
        await drive_player(ed, {"path": prebuilt})
        return
    out = ed.scratch.root / "export-desktop"
    t0 = time.monotonic()
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=JOBS, timeout_seconds=30)
    job = r
    while job["status"] == "running":
        if time.monotonic() - t0 > 7000:
            ed.fail(f"desktop build still running: {job}")
        await asyncio.sleep(15)
        job = await ed.call("get_job", job_id=r["job_id"], log_lines=5)
    print(f"[test] desktop build: {job['status']} in {time.monotonic() - t0:.0f}s")
    ed.check_eq(job["status"], "succeeded", f"{job}")
    await drive_player(ed, {})


async def drive_player(ed: Editor, args: dict):
    raw = await ed.call_raw("run_build", {
        **args,
        "input_delay_seconds": 3,
        "input": [
            {"type": "screenshot"},
            {"type": "key", "key": "space"},
            {"type": "wait", "ms": 250},
            {"type": "key", "key": "space"},
            {"type": "wait", "ms": 150},
            {"type": "screenshot"},
            {"type": "wait", "ms": 2500},
            {"type": "screenshot"},
        ],
        "max_width": 640,
    })
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    print("[test] run_build:", raw.text[:1500])
    ed.check(r["log_lines"] > 0, "the player's log was captured")
    ed.check_eq(r["error_count"], 0, f"player errors: {r['errors']}")
    ed.check(not r["crashed"], f"crashed: {r['fatal_lines']}")
    ed.check_eq(r["input"]["actions_done"], r["input"]["actions_total"], f"input: {r['input']}")
    ed.check_eq(len(raw.images), 3, f"screenshots: {r.get('screenshots')}")
    for i, (data, _mime) in enumerate(raw.images):
        shot = ed.scratch.root / f"player-{i}.png"
        shot.write_bytes(data)
        print(f"[test] player screenshot {i}: {shot}")
    ed.check(raw.images[0][0] != raw.images[1][0], "the input changed the picture")
    for shot in r["screenshots"]:
        ed.check("player window" in shot.get("note", ""), f"not the player window: {shot}")
