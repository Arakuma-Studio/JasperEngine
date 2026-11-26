"""String Tables ship in exported games (docs/specs/game-ui-localization.md R6, R21, D5, AC14).

A project with a String Table (ui.menu.play: Play, de Spielen, ja プレイ) and a Lua script that prints
Localization.get('ui.menu.play') at start is built:

- prebuilt (the run's glcore runtime) and run headless with LANG=de_DE.UTF-8: the game prints
  Spielen, its build holds l10n/ui.de.strbin and the manifest;
- again with localization.languages [en, ja]: the de file is left out and the build warns
  W-L10N-LANGUAGE-DROPPED;
- compiled (the generated main loads l10n/manifest.json, a path only a compiled game has) and run
  with LANG=de_DE.UTF-8: Spielen.

Set JASPER_SLOW_BUILD=1 to run them (docs/testing.md, "Compiled and prebuilt game builds").
"""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import build_cache
import harness
from harness import REPO, Editor, skip

PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Localization Export",
          "env": {**build_cache.player_env("glcore", "release"), **harness.compile_env("test_localization_export")}}
CI = True
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

TABLE = "assets/l10n/ui.stringtable"
SCRIPT = """local Shout = {}
function Shout:start()
    Log.print("[l10n-export] " .. Localization.get("ui.menu.play") .. " " .. Localization.language)
end
return Shout
"""


def slow():
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run real Desktop players")


async def wait_job(ed: Editor, job: dict, timeout: float, poll: float = 5.0) -> dict:
    deadline = time.monotonic() + timeout
    while job.get("status") == "running":
        if time.monotonic() > deadline:
            ed.fail(f"job {job['job_id']} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)
        job = await ed.call("get_job", job_id=job["job_id"], log_lines=10)
    return job


def build_text(job: dict) -> str:
    result = job.get("result") or {}
    log = Path(result["log_file"]).read_text(errors="replace") if result.get("log_file") else ""
    return str(job.get("message", "")) + json.dumps(result.get("warnings", [])) + log


def shipped_files(out: Path) -> str:
    """Every file name of the build, the content packs' entries included."""
    names = [p.as_posix() for p in out.rglob("*") if p.is_file()]
    for pack in out.rglob("content.jpak"):
        listed = subprocess.run([sys.executable, "-I", str(REPO / "tools" / "jpak.py"), "list", str(pack)],
                                capture_output=True, text=True, timeout=120)
        names.append(listed.stdout)
    return "\n".join(names)


async def run_player(ed: Editor, executable: str, cwd: Path, name: str) -> str:
    env = dict(os.environ, LANG="de_DE.UTF-8", LANGUAGE="de_DE:de", LC_ALL="")
    run = await asyncio.to_thread(
        subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), executable, "--headless", "--fixed-fps", "60", "--after-ready",
                         "--quit-after", "30"],
        cwd=cwd, capture_output=True, text=True, timeout=900, env=env)
    log = run.stdout + run.stderr
    (ed.scratch.root / f"{name}.log").write_text(log)
    ed.check_eq(run.returncode, 0, f"the player quits after its frames: {log[-2000:]}")
    return log


async def setup(ed: Editor) -> None:
    await ed.call("get_project_summary")
    (ed.project / "assets").mkdir(exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
    await ed.call("edit_string_table", path=TABLE, create={}, ops=[
        {"op": "set", "key": "menu.play", "text": "Play"},
        {"op": "add_language", "language": "de"},
        {"op": "add_language", "language": "ja"},
        {"op": "set", "key": "menu.play", "language": "de", "text": "Spielen"},
        {"op": "set", "key": "menu.play", "language": "ja", "text": "プレイ"}])
    made = await ed.call("create_scene", name="Shout", type="2D", path="scenes/Shout")
    scene = made["scene"]["id"]
    holder = (await ed.call("create_entity", scene=scene, type="empty", name="Shout"))["entity_id"]
    await ed.call("create_script", kind="lua", name="Shout", content=SCRIPT, scene=scene, entity=holder)
    saved = await ed.call("save_scene", scene=scene)
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scene"]["path"])


async def test_export_ships_cooked_tables(ed: Editor):
    slow()
    await setup(ed)
    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-l10n"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    job = await wait_job(ed, job, TIMEOUT - 600)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
    files = shipped_files(out)
    for name in ("l10n/manifest.json", "l10n/ui.en.strbin", "l10n/ui.de.strbin", "l10n/ui.ja.strbin"):
        ed.check(name in files, f"the build ships {name}: {files[:3000]}")
    ed.check(".stringtable" not in files and ".translation" not in files, "the JSON sources stay out of the build")
    log = await run_player(ed, job["result"]["executable"], out, "prebuilt-player")
    ed.check("[l10n-export] Spielen de" in log, f"the game shows the German text under LANG=de_DE: {log[-2000:]}")

    # a language outside localization.languages is left out, with its warning
    await ed.call("set_project_settings", values={"localization.languages": ["en", "ja"]})
    narrow = ed.scratch.root / "export-l10n-narrow"
    job = await ed.call("build_project", mode="desktop", output_dir=str(narrow), method="prebuilt", timeout_seconds=600)
    job = await wait_job(ed, job, TIMEOUT - 600)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    files = shipped_files(narrow)
    ed.check("l10n/ui.de.strbin" not in files and "l10n/ui.ja.strbin" in files, f"de left out, ja shipped: {files[:3000]}")
    ed.check("W-L10N-LANGUAGE-DROPPED" in build_text(job), f"the build warns: {build_text(job)[-2000:]}")
    log = await run_player(ed, job["result"]["executable"], narrow, "prebuilt-player-narrow")
    ed.check("[l10n-export] Play en" in log, f"without de the game falls back to the default: {log[-2000:]}")
    await ed.call("set_project_settings", values={"localization.languages": []})


async def test_compiled_game_loads_the_manifest(ed: Editor):
    slow()
    out = ed.scratch.root / "export-l10n-compiled"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="compile",
                        build_jobs=harness.build_jobs(), timeout_seconds=30)
    job = await wait_job(ed, job, TIMEOUT, poll=15.0)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq(job["result"]["method_used"], "compile", f"{job}")
    log = await run_player(ed, job["result"]["executable"], out, "compiled-player")
    ed.check("[l10n-export] Spielen de" in log, f"the compiled game shows the German text: {log[-2000:]}")


if __name__ == "__main__":
    harness.main(globals())
