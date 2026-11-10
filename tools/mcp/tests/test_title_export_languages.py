"""S87 AC10 (docs/specs/S87-title-localisation-interchange.md R24-R26, R64): a title's language sets in
a prebuilt Linux player. The shipped source sidecar lists the languages that ship; with
localization.languages [en, de], ja is dropped from the build (its set absent, the shipped table
without it, the project's sidecar unchanged) with W-TITLE-LANGUAGE-DROPPED, and the player run with
LANGUAGE=de shows the German bake.

Set JASPER_SLOW_BUILD=1 to run it (the run builds its prebuilt runtime once).
"""

import asyncio
import json
import os
import subprocess
from pathlib import Path

import build_cache
import pngdiff
from harness import Editor, skip

REPO = Path(__file__).resolve().parents[3]
CI = True
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Title Export Languages", "env": build_cache.player_env("glcore", "release")}
TIMEOUT = 1800

DOC = "assets/titles/lang.titleart"
# the build ships the bakes cooked (je-cooked/, a lossy block format), so edges vary a little from the
# PNG (measured: at most 11/255 on 4 % of the pixels); the golden tests' tolerance covers that
TOLERANCE = 24
MAX_FRACTION = 0.01

SHOWS = """local Shows = { properties = {} }
function Shows:init()
    self.title = TitleArt(self.scene, self.entity)
    self.scene:setBackgroundColor(0, 0, 0, 1)
    RegisterEngineEvent(self, "onUpdate")
end
function Shows:onUpdate(dt)
    self.title:setNativeSize()
    if self.title:isLanguageReady() then Log.print("[lang] ready " .. self.title:getResolvedLanguage()) end
end
return Shows
"""


async def wait_job(ed: Editor, job: dict, timeout: float) -> dict:
    deadline = asyncio.get_event_loop().time() + timeout
    while job["status"] == "running":
        if asyncio.get_event_loop().time() > deadline:
            ed.fail(f"job {job['job_id']} still running: {job}")
        await asyncio.sleep(2)
        job = await ed.call("get_job", job_id=job["job_id"], log_lines=10)
    return job


async def test_languages_ship_and_the_player_shows_german(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real prebuilt player (the run builds its runtime once)")
    await ed.call("tool_groups", enable=["title_art"])
    # the game's canvas is the window: the title draws at its native pixels
    await ed.call("set_project_settings", values={"title_art.auto_bake": False, "graphics.canvas_width": 640, "graphics.canvas_height": 360})
    await ed.call("edit_title", path=DOC, create={"text": "BIG WIN", "canvas": [640, 360]}, bake="never", ops=[
        {"op": "add_language", "code": "de", "source": "en"},
        {"op": "add_language", "code": "ja"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GROSSER GEWINN"},
        {"op": "set_translation", "layer": "Title", "language": "ja", "text": "大当たり"},
    ])
    await ed.call("export_title", path=DOC)
    source = ed.project / (DOC + ".bake.json")
    table = json.loads(source.read_text())["languages"]
    ed.check_eq(sorted(table["sets"]), ["de", "ja"], f"the bake's table: {table}")

    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/Hud")
    sid = made["scene"]["id"]
    await ed.call("create_ui", scene=sid, root={"type": "image", "name": "Logo", "layout": {"anchor": "center", "size": [640, 360]}})
    await ed.call("add_component", scene=sid, entity="Logo", component="titleart")
    await ed.call("set_properties", scene=sid, entity="Logo", values={"titleart": {"title": DOC}})
    await ed.call("create_script", kind="lua", name="Shows", content=SHOWS, entity="Logo", scene=sid)
    saved = await ed.call("save_scene", scene=sid)
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scene"]["path"])
    await ed.call("set_project_settings", values={"localization.languages": ["en", "de"]})

    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-title-languages"
    await ed.console_since(limit=500)
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=900)
    job = await wait_job(ed, job, TIMEOUT - 300)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq(job["result"]["player_id"], runtime.name, f"{job}")

    warnings = json.dumps(job["result"].get("warnings", [])) + " ".join(e["message"] for e in await ed.console_since(limit=500))
    ed.check("W-TITLE-LANGUAGE-DROPPED" in warnings and "ja" in warnings, f"the dropped language is a warning: {warnings[-1500:]}")

    shipped = next(out.rglob("lang.titleart.bake.json"))
    table = json.loads(shipped.read_text())["languages"]
    ed.check_eq(sorted(table["sets"]), ["de"], f"the shipped table lists only what ships: {table}")
    ed.check_eq(table["source"], "en", f"{table}")
    files = {p.name for p in shipped.parent.iterdir()}
    # the set's image ships cooked (je-cooked/), its sidecar beside the document; the player run below draws it
    ed.check("lang.titleart.de.bake.json" in files, f"the German set ships: {sorted(files)}")
    ed.check(not any(".ja." in f for f in files), f"the Japanese set is left out: {sorted(files)}")
    manifest = next(out.rglob("content-manifest.json")).read_text()
    ed.check("lang.titleart.ja" not in manifest, "the build's manifest names no Japanese file")
    project_table = json.loads(source.read_text())["languages"]
    ed.check_eq(sorted(project_table["sets"]), ["de", "ja"], "the project's sidecar is unchanged")

    # the player, its language from the environment
    env = dict(os.environ)
    env["LANGUAGE"] = "de"
    env["LANG"] = "de_DE.UTF-8"
    env.pop("LC_ALL", None)
    env.pop("LC_MESSAGES", None)
    shot = ed.scratch.root / "player_de.png"
    run = await asyncio.to_thread(
        subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], "--headless", "--fixed-fps", "30", "--after-ready",
                         "--quit-after", "60", "--resolution", "640x360", "--screenshot", str(shot), "--fail-on-error"],
        cwd=out, capture_output=True, text=True, timeout=600, env=env)
    log = run.stdout + run.stderr
    (ed.scratch.root / "player_de.log").write_text(log)
    ed.check_eq(run.returncode, 0, f"the player ran and logged no error: {log[-2000:]}")
    ed.check("[lang] ready de" in log, f"the player resolved German: {log[-2000:]}")

    capture = pngdiff.decode(shot.read_bytes())
    german = pngdiff.decode((ed.project / "assets/titles/lang.titleart.de.png").read_bytes())
    fraction, offset, worst = pngdiff.centre_match(capture, german, TOLERANCE)
    print(f"the German capture against the German bake: {fraction:.5f} over {TOLERANCE}/255 at {offset} (worst {worst})")
    ed.check(fraction <= MAX_FRACTION, f"the German bake drew: {fraction:.4f} over at {offset} (worst {worst}), capture {capture[:2]} bake {german[:2]}")
    # red control: the English bake does not match the capture
    english = pngdiff.decode((ed.project / "assets/titles/lang.titleart.png").read_bytes())
    wrong, _, _ = pngdiff.centre_match(capture, english, TOLERANCE)
    ed.check(wrong > MAX_FRACTION, f"the comparison tells the languages apart: the English bake differs by {wrong:.4f}")
