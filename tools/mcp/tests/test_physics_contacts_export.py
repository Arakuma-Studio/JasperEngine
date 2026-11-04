"""Contact callbacks parity (physics-contacts acceptance 6): a ball landing on a floor (T1) and a
ball falling through a trigger (T3) log the same callbacks in editor Play and in an exported
Linux player.

The Desktop build compiles the engine from source (~20-60 min at 3 jobs on this host): set
JASPER_SLOW_BUILD=1 to run it.
"""

import asyncio
import os
import time
from pathlib import Path

from harness import Editor, skip

EDITOR = {"create": True, "name": "Contacts Export"}
CI_EXCLUDED = ("long-running (about 50 minutes on a loaded server: its test builds and runs a real Desktop player); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

SCRIPT = """local Parity = {}
function Parity:onCollisionEnter(c) print("parity collision enter " .. self.gameObject.name .. " " .. c.gameObject.name) end
function Parity:onCollisionExit(c) print("parity collision exit " .. self.gameObject.name .. " " .. c.gameObject.name) end
function Parity:onTriggerEnter(o) print("parity trigger enter " .. self.gameObject.name .. " " .. o.gameObject.name) end
function Parity:onTriggerExit(o) print("parity trigger exit " .. self.gameObject.name .. " " .. o.gameObject.name) end
return Parity
"""


def parity_lines(lines) -> list[str]:
    out = []
    for line in lines:
        at = line.find("parity ")
        if at >= 0:
            out.append(line[at:].strip())
    return sorted(out)


async def test_play_and_export_log_the_same_callbacks(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    sid = (await ed.call("create_scene", name="ContactsParity", type="3d"))["scene"]["id"]
    floor = (await ed.call("create_entity", scene=sid, name="Floor", type="cube", position=[0, -0.5, 0], scale=[20, 1, 20]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=floor, body_type="static", add=[{"shape": "box"}])
    lander = (await ed.call("create_entity", scene=sid, name="Lander", type="cube", position=[-3, 2, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=lander, body_type="dynamic", add=[{"shape": "sphere", "radius": 0.5, "fit": False}])
    zone = (await ed.call("create_entity", scene=sid, name="Zone", type="cube", position=[3, 3, 0], scale=[2, 2, 2]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=zone, body_type="static", add=[{"shape": "box"}], body={"sensor": True})
    faller = (await ed.call("create_entity", scene=sid, name="Faller", type="cube", position=[3, 6, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=faller, body_type="dynamic", add=[{"shape": "sphere", "radius": 0.3, "fit": False}],
                  body={"gravityFactor": 1})
    await ed.call("create_script", kind="lua", name="Parity", content=SCRIPT, entity=lander)
    for e in (floor, zone, faller):
        await ed.call("attach_script", entity=e, path="Parity.lua")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    scene_id = next(s["id"] for s in saved["scenes_in_build"] if s["name"] == "ContactsParity")
    await ed.call("edit_build_scenes", action="set_start", scene=scene_id)
    await ed.call("set_project_settings", values={
        "graphics.runtime_shaders": False,
        "preferences.build_jobs": 3,
        "player.window_width": 640,
        "player.window_height": 480,
    })

    await ed.console_since(None)
    await ed.call("play", action="start", scene=sid, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=180)
        play = parity_lines(e["message"] for e in await ed.console_since())
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    # the faller crosses the trigger (both sides Enter and Exit), then lands with the lander
    for expected in ("parity collision enter Floor Lander", "parity collision enter Lander Floor",
                     "parity trigger enter Zone Faller", "parity trigger enter Faller Zone",
                     "parity trigger exit Zone Faller", "parity trigger exit Faller Zone"):
        ed.check(expected in play, f"editor Play has '{expected}': {play}")

    out = ed.scratch.root / "export-desktop"
    t0 = time.monotonic()
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=3, timeout_seconds=30)
    job = r
    while job["status"] == "running":
        await asyncio.sleep(15)
        job = await ed.call("get_job", job_id=r["job_id"], log_lines=10)
        if time.monotonic() - t0 > TIMEOUT:
            ed.fail(f"build still running: {job}")
    ed.check_eq(job["status"], "succeeded", f"{job}")
    exe = Path(job["result"]["executable"])
    raw = await ed.call_raw("run_build", {"seconds": 6, "screenshot": False})
    ed.check(not raw.is_error, raw.text)
    log = raw.value["log"]
    exported = parity_lines(log if isinstance(log, list) else str(log).splitlines())
    print("[test] exported player:", exe, exported)
    ed.check_eq(exported, play, "editor Play and the export log the same callbacks")
