"""Script lifecycle parity (script-lifecycle 12): the same scripted scene logs the same lifecycle in
editor Play and in an exported Linux player, a scene reload included.

The Desktop build compiles the engine from source (~20-60 min at 3 jobs on this host): set
JASPER_SLOW_BUILD=1 to run it.
"""

import asyncio
import os
import time
from pathlib import Path

from harness import Editor, skip

EDITOR = {"create": True, "name": "Lifecycle Export"}
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

SCRIPT = """local Parity = {}
function Parity:init() print("parity init " .. self.gameObject.name) end
function Parity:onEnable() print("parity enable " .. self.gameObject.name) end
function Parity:start()
    print("parity start " .. self.gameObject.name)
    if not parityReloaded and self.gameObject.name == "Parent" then
        parityReloaded = true
        SceneManager.loadScene(SceneManager.currentSceneName)
    end
end
function Parity:onDisable() print("parity disable " .. self.gameObject.name) end
function Parity:onDestroy() print("parity destroy " .. self.gameObject.name) end
return Parity
"""

# both reach this point: two loads of the scene, the first unloaded in between
EXPECTED = [
    "parity init Parent", "parity init Child", "parity enable Parent", "parity enable Child",
    "parity start Parent", "parity start Child",
    "parity disable Parent", "parity disable Child", "parity destroy Parent", "parity destroy Child",
    "parity init Parent", "parity init Child", "parity enable Parent", "parity enable Child",
    "parity start Parent", "parity start Child",
]


def parity_lines(lines) -> list[str]:
    out = []
    for line in lines:
        at = line.find("parity ")
        if at >= 0:
            out.append(line[at:].strip())
    return out


async def test_play_and_export_log_the_same_lifecycle(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    parent = (await ed.call("create_entity", name="Parent", type="empty"))["entity_id"]
    child = (await ed.call("create_entity", name="Child", type="empty", parent=parent))["entity_id"]
    await ed.call("create_script", kind="lua", name="Parity", content=SCRIPT, entity=parent)
    await ed.call("attach_script", entity=child, path="Parity.lua")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])
    await ed.call("set_project_settings", values={
        "graphics.runtime_shaders": False,       # smaller, faster player build
        "preferences.build_jobs": 3,
        "player.window_width": 640,
        "player.window_height": 480,
    })

    await ed.console_since(None)
    await ed.call("eval_lua", code="parityReloaded = nil", allow_edit_mode=True)
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=4)
        play = parity_lines(e["message"] for e in await ed.console_since())
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    ed.check_eq(play[:len(EXPECTED)], EXPECTED, "editor Play")

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
    ed.check_eq(exported[:len(EXPECTED)], EXPECTED, "exported player")
    ed.check_eq(exported[:len(EXPECTED)], play[:len(EXPECTED)], "editor Play and the export agree")
