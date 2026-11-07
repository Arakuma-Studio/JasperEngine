"""A prebuilt player plays Sound Events as data (docs/specs/S37-sound-events.md R24, 6.6; AC11): the event ships as
an asset with no generated code, the player's picks equal the editor's for the same scene and seed, a trigger
plays, a missing clip is a build warning, and content-manifest.json lists soundlayer. The picks are read from the
voice lines of JASPERENGINE_AUDIO_TRACE until S35's player capture lands.

Set JASPER_SLOW_BUILD=1 to run it: it uses the run's prebuilt glcore runtime (build_cache).
"""

import asyncio
import json
import os
import subprocess
from pathlib import Path

import build_cache
from audio_refs import TRACE_ENV, voice_lines, write_tone
from harness import Editor, skip

REPO = Path(__file__).resolve().parents[3]
CI = True
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Events Prebuilt",
          "env": {**build_cache.player_env("glcore", "release"), **TRACE_ENV}}

EVENT = "events/steps.soundevent"
TRIGGERED = "events/land.soundevent"
SEED = 5

SCRIPT = """local Plays = {}
function Plays:start()
    self.frames = 0
    RegisterEngineEvent(self, 'onUpdate')
end
function Plays:onUpdate(dt)
    self.frames = self.frames + 1
    if self.frames % 5 == 0 and self.frames <= 40 then
        Audio.play("events/steps.soundevent")
    end
end
return Plays
"""


def event_picks(lines: list[dict], event: str = EVENT) -> list[int]:
    return [l["variation"] for l in lines if l["type"] == "voice_start" and l.get("event_path") == event]


def landed(lines: list[dict]) -> list[str]:
    return [l.get("trigger", "") for l in lines if l["type"] == "voice_start" and l.get("event_path") == TRIGGERED]


async def test_a_prebuilt_player_plays_the_editors_picks(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real player (the run builds its prebuilt runtime once)")
    for i in range(4):
        write_tone(ed.project / f"sounds/c{i}.wav", seconds=0.1, frequency=300 + 200 * i)
    await ed.call("sync_files")
    await ed.call("set_project_settings", values={"audio.seed": SEED})
    await ed.call("tool_groups", enable=["audio"])
    await ed.call("edit_sound_event", path=EVENT, create=True, values={"mode": "random", "clips": [f"sounds/c{i}.wav" for i in range(4)]})
    # a second event whose clip is gone: a warning of the build, naming both
    await ed.call("edit_sound_event", path=TRIGGERED, create=True, values={"mode": "random", "clips": ["sounds/c0.wav", "sounds/c1.wav"]})
    await ed.call("edit_sound_event", path="events/broken.soundevent", create=True, values={"mode": "shuffle", "clips": ["sounds/gone.wav"]})
    await ed.call("create_entity", name="Plays", type="empty")
    await ed.call("create_script", kind="lua", name="Plays", content=SCRIPT, entity="Plays")
    # the trigger: a box lands on a floor and plays its Sound Event
    await ed.call("create_entity", type="cube", name="Floor", position=[0, -0.5, 0], scale=[30, 1, 30])
    await ed.call("edit_colliders", entity="Floor", body_type="static", add=[{"shape": "box"}])
    await ed.call("create_entity", type="cube", name="Box", position=[0, 1.5, 0])
    await ed.call("edit_colliders", entity="Box", body_type="dynamic", add=[{"shape": "box"}])
    await ed.call("add_component", entity="Box", component="audiosource")
    await ed.call("set_properties", entity="Box", values={"audiosource": {"event": TRIGGERED, "playOn": ["collision_3d"]}})
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])

    await ed.console_since(None)
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=119)
        editor_lines = voice_lines(await ed.console_since(limit=500))
        editor_picks = event_picks(editor_lines)
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    ed.check_eq(len(editor_picks), 8, f"{editor_picks}")
    ed.check_eq(landed(editor_lines)[:1], ["collision_3d"], f"the box landed in Play: {editor_lines}")

    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-events"
    await ed.console_since(limit=500)
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    while job["status"] == "running":
        await asyncio.sleep(2)
        job = await ed.call("get_job", job_id=job["job_id"])
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq(job["result"]["player_id"], runtime.name, f"{job}")

    warnings = [e["message"] for e in await ed.console_since(limit=500)]
    ed.check(any("broken.soundevent" in m and "gone.wav" in m for m in warnings), f"the missing clip is a build warning: {warnings[-20:]}")
    manifest = json.loads(next(out.rglob("content-manifest.json")).read_text())
    ed.check("soundlayer" in manifest["modules"], f"{manifest['modules']}")
    ed.check(list(out.rglob("steps.soundevent")), "the event ships as an asset")
    ed.check_eq(json.loads((out / "game.json").read_text())["audio"]["seed"], SEED, "audio.seed in game.json")

    env = dict(os.environ)
    env.update(TRACE_ENV)
    run = await asyncio.to_thread(
        subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], "--headless", "--fixed-fps", "60",
                         "--after-ready", "--quit-after", "120"],
        cwd=out, capture_output=True, text=True, timeout=600, env=env)
    log = run.stdout + run.stderr
    ed.check_eq(run.returncode, 0, f"the player quits after its frames: {log[-2000:]}")
    entries = [{"message": line} for line in log.splitlines()]
    player_lines = voice_lines(entries)
    ed.check_eq(event_picks(player_lines), editor_picks, f"the player's picks are the editor's: {log[-1500:]}")
    ed.check_eq(event_picks(player_lines, TRIGGERED), event_picks(editor_lines, TRIGGERED), "the trigger's picks too")
    ed.check_eq(landed(player_lines)[:1], ["collision_3d"], f"the trigger plays in the player: {log[-1500:]}")
