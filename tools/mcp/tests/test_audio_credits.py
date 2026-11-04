"""S36 AC10: export credits. A project with a CC0 essential, a CC-BY clip, a generated placeholder, an
`unknown` clip and a clip without provenance exports licenses/THIRD-PARTY-AUDIO.txt with the right sections,
W-AUDIO-LICENCE and W-AUDIO-PLACEHOLDER in the build result; audio.credits_file false writes none.

The source export (mode source) runs the Compile path's copyLicenses here. The Prebuilt path (stageNotices) needs a
player runtime: JASPER_SLOW_BUILD=1 runs it. Oracle: the text, read back; the manifest for the essential's URL."""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

import build_cache
from harness import Editor, skip

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import audio_refs as ar  # noqa: E402

CI = True
TIMEOUT = 1800
RATE = 48000
REPO = Path(__file__).resolve().parents[3]
# a new project's assets folder is the project's
TICK = "credits/ui/tick.ogg"
BY = "sfx/by.wav"
GENERATED = "sfx/placeholder.wav"
UNKNOWN = "sfx/mystery.wav"
OWN = "sfx/own.wav"
FILE = "THIRD-PARTY-AUDIO.txt"


async def setup_project(ed: Editor) -> None:
    await ed.call("import_files", sources=["essentials:ui/tick"], to_dir="credits")
    for rel in (BY, UNKNOWN, OWN):
        ar.write_wav(ed.project / rel, [ar.sine(800.0, 0.2, RATE, -12.0)], RATE, ar.PCM16)
    await ed.call("sync_files", paths=[BY, UNKNOWN, OWN])
    await ed.call("set_audio_import_settings", paths=[BY],
                  provenance={"licence": "CC-BY-4.0", "licence_url": "https://creativecommons.org/licenses/by/4.0/", "author": "Ana",
                              "attribution": "Sound by Ana", "source": "freesound", "url": "https://example.org/by"})
    await ed.call("set_audio_import_settings", paths=[UNKNOWN], provenance={"licence": "unknown", "source": "user"})
    await ed.call("generate_sound", path=GENERATED, preset="jump", seed=7, image="none")


async def export(ed: Editor, name: str, **args) -> tuple[Path, dict]:
    status = await ed.call("get_build_info")
    if not status.get("scenes_in_build"):
        await ed.call("create_entity", name="Box", type="box")
        s = await ed.call("save_project", untitled_scene_dir="scenes")
        await ed.call("edit_build_scenes", action="set_start", scene=s["scenes_in_build"][0]["id"])
    out = ed.scratch.root / name
    r = await ed.call("build_project", mode="source", output_dir=str(out), **args)
    deadline = time.monotonic() + TIMEOUT
    while r.get("status") == "running" and time.monotonic() < deadline:
        r = await ed.call("get_job", job_id=r.get("job_id") or r["id"])
    ed.check_eq(r["status"], "succeeded", f"{r}")
    return out, r


def warnings_of(job: dict) -> dict:
    return {w["code"]: w["message"] for w in job["result"].get("warnings", [])}


def check_text(ed: Editor, text: str) -> None:
    manifest = json.loads((REPO / "editor/resources/sound-library/essentials/manifest.json").read_text())
    tick = next(c for c in manifest["clips"] if c["id"] == "ui/tick")
    ed.check("CC0-1.0" in text and "CC-BY-4.0" in text, f"a section per licence:\n{text}")
    ed.check(tick["url"] in text and "credits/ui/tick.ogg" in text, f"Kenney's clip, grouped by its pack:\n{text}")
    ed.check("Kenney pack: " + tick["url"] in text, f"the pack's line:\n{text}")
    ed.check("sfx/by.wav | Ana | https://example.org/by | CC-BY-4.0 https://creativecommons.org/licenses/by/4.0/ | Attribution: Sound by Ana" in text, f"the BY line:\n{text}")
    ed.check("Placeholder sounds (generate_sound)\n  sfx/placeholder.wav" in text, f"the placeholder:\n{text}")
    ed.check("Unknown licence" in text and "sfx/mystery.wav" in text, f"the unknown clip:\n{text}")
    ed.check("sfx/own.wav" not in text, f"a clip without provenance is not listed:\n{text}")


async def test_compile_path_credits_and_warnings():
    async with Editor(create=True, name="Credits") as ed:
        await setup_project(ed)
        out, job = await export(ed, "credits")
        files = list(out.rglob(FILE))
        ed.check_eq(len(files), 1, "one file")
        ed.check_eq(files[0].parent.name, "licenses", "in licenses/")
        check_text(ed, files[0].read_text())
        warnings = warnings_of(job)
        ed.check("sfx/mystery.wav" in warnings.get("W-AUDIO-LICENCE", ""), f"W-AUDIO-LICENCE names the path: {warnings}")
        ed.check("sfx/placeholder.wav" in warnings.get("W-AUDIO-PLACEHOLDER", ""), f"W-AUDIO-PLACEHOLDER names the path: {warnings}")

        await ed.call("set_project_settings", values={"audio.credits_file": False})
        off, job = await export(ed, "credits-off")
        ed.check(not list(off.rglob(FILE)), "audio.credits_file false writes none")
        ed.check_in("W-AUDIO-LICENCE", warnings_of(job), "the warnings stay")


async def test_a_project_without_provenance_has_no_file():
    async with Editor(create=True, name="No Credits") as ed:
        ar.write_wav(ed.project / OWN, [ar.sine(800.0, 0.2, RATE, -12.0)], RATE, ar.PCM16)
        await ed.call("sync_files", paths=[OWN])
        out, job = await export(ed, "no-credits")
        ed.check(not list(out.rglob(FILE)), "no provenance: no file")
        ed.check_eq(set(warnings_of(job)) & {"W-AUDIO-LICENCE", "W-AUDIO-PLACEHOLDER"}, set(), "no warning")


async def test_prebuilt_path_credits():
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build with a prebuilt player runtime")
    env = build_cache.player_env("glcore", "development")
    async with Editor(create=True, name="Credits Prebuilt", env=env) as ed:
        await build_cache.player_runtime(ed, "glcore", "development")
        await setup_project(ed)
        await ed.call("create_entity", name="Box", type="box")
        s = await ed.call("save_project", untitled_scene_dir="scenes")
        await ed.call("edit_build_scenes", action="set_start", scene=s["scenes_in_build"][0]["id"])
        await ed.call("set_project_settings", values={"graphics.runtime_shaders": False, "player.development_build": True})
        out = ed.scratch.root / "credits-prebuilt"
        r = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=900)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        files = list(out.rglob(FILE))
        ed.check_eq(len(files), 1, "the prebuilt build has the file")
        check_text(ed, files[0].read_text())
