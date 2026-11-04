"""A Compile build of a project that reaches none of the optional modules (docs/specs/S34-player-size.md R6): the
engine compiles with every module off, and that game runs. The editor's engine has every module, so only such a build
shows a source that got a header through a module's code (S72's header cut broke this path).
tools/check-physics-off.sh all-off is the quick gate of the same thing; this is the user's path.

Set JASPER_SLOW_BUILD=1 to run it: it compiles the engine.
"""

import asyncio
import os
from pathlib import Path

import harness
from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "No Modules", "env": harness.compile_env("test_compile_without_modules")}
SWITCHES = ("PHYSICS_2D", "PHYSICS_3D", "NAVIGATION", "RUNTIME_SHADERS", "HTMLUI", "VIDEO")

PLAIN_SCRIPT = """local Plain = {}
function Plain:start()
    print("nomodules started")
end
return Plain
"""


async def test_a_compile_build_that_needs_no_module_builds_and_runs(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to compile the engine with every module off and run the game")
    await ed.call("create_entity", name="Plain", type="empty")
    await ed.call("create_script", kind="lua", name="Plain", content=PLAIN_SCRIPT, entity="Plain")
    # (the one module a project keeps unless its settings say otherwise)
    await ed.call("set_navigation_settings", include_in_builds=False)
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])

    out = ed.scratch.root / "export-plain"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="compile", build_jobs=harness.build_jobs(), timeout_seconds=30)
    while job["status"] == "running":
        await asyncio.sleep(10)
        job = await ed.call("get_job", job_id=job["job_id"])
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq(job["result"]["method_used"], "compile", f"{job}")
    ed.check_eq(job["result"]["modules"], [], f"{job['result'].get('module_reasons')}")
    # the compile tree really had them off: what the exporter wrote into the engine's CMake
    root = Path(EDITOR["env"]["XDG_CACHE_HOME"]) / "jasperengine" / "compile-builds"
    written = [text for text in (p.read_text(errors="replace") for p in root.rglob("CMakeLists.txt")) if "set(JASPERENGINE_WINDOW_TITLE " in text]
    ed.check(written and all(f"set(JASPERENGINE_{switch} OFF)" in written[0] for switch in SWITCHES), f"{len(written)} CMakeLists.txt with the project's settings")

    raw = await ed.call_raw("run_build", {"after_ready": True, "fixed_fps": 60, "quit_after": 30, "screenshot": False})
    ed.check(not raw.is_error, raw.text)
    ed.check_eq(raw.value.get("exit_code"), 0, f"{raw.value}")
    log = "\n".join(raw.value["log"]) if isinstance(raw.value["log"], list) else str(raw.value["log"])
    ed.check("nomodules started" in log, f"the script ran in a game without the modules: {log[-1500:]}")
