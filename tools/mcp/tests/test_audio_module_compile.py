"""A Compile build of a project without the sound layer's features leaves the module out (docs/specs/S37-sound-events.md
6.6; AC11): a plain AudioSource and AudioSource:play in Lua need nothing from it, the engine compiles with
JASPERENGINE_SOUND_LAYER off, and that player still plays a clip.

Set JASPER_SLOW_BUILD=1 to run it: it compiles the engine.
"""

import asyncio
import os
from pathlib import Path

import harness
from audio_refs import write_tone
from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "Plain Audio", "env": harness.compile_env("test_audio_module_compile")}

PLAIN_SCRIPT = """local Plain = {}
function Plain:start()
    local sound = AudioSource(self.scene, self.entity)
    sound:play()
    print("plainclip playing=" .. tostring(sound:isPlaying()))
end
return Plain
"""


async def test_a_compile_build_without_the_features_leaves_the_module_out(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to compile the engine without the module and run the player")
    write_tone(ed.project / "sounds/plain.wav", seconds=0.2)
    await ed.call("sync_files")
    await ed.call("create_entity", name="Plain", type="audio_source")
    await ed.call("set_properties", entity="Plain", values={"audiosource": {"filename": "sounds/plain.wav"}})
    await ed.call("create_script", kind="lua", name="PlainClip", content=PLAIN_SCRIPT, entity="Plain")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])

    out = ed.scratch.root / "export-plain"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="compile", build_jobs=harness.build_jobs(), timeout_seconds=30)
    while job["status"] == "running":
        await asyncio.sleep(10)
        job = await ed.call("get_job", job_id=job["job_id"])
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq(job["result"]["method_used"], "compile", f"{job}")
    ed.check("soundlayer" not in job["result"]["modules"], f"{job['result']['module_reasons']}")
    caches = [p.read_text(errors="replace") for p in (Path(EDITOR["env"]["XDG_CACHE_HOME"]) / "jasperengine" / "compile-builds").rglob("CMakeCache.txt")]
    ed.check(any("JASPERENGINE_SOUND_LAYER:BOOL=OFF" in c for c in caches), f"the compile tree: {len(caches)} caches")

    raw = await ed.call_raw("run_build", {"after_ready": True, "fixed_fps": 60, "quit_after": 30, "screenshot": False})
    ed.check(not raw.is_error, raw.text)
    ed.check_eq(raw.value.get("exit_code"), 0, f"{raw.value}")
    log = "\n".join(raw.value["log"]) if isinstance(raw.value["log"], list) else str(raw.value["log"])
    ed.check("plainclip playing=true" in log, f"a plain clip plays without the module: {log[-1500:]}")
