"""S39 AC7 (MCP half): analyze_audio's `music` block on loops and music clips, and the tempo keys of
set_audio_import_settings. Passing `fix` writes the `.meta` block, after which `agrees` is true; nothing is
written before; the grid is cached beside the analysis.

Oracles: the click tracks' own parameters (audio_refs.click_track), the files on disk (PyYAML)."""

from __future__ import annotations

import sys
from pathlib import Path

import yaml

from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import audio_refs as ar  # noqa: E402

CI = True
EDITOR = {"create": True}
TIMEOUT = 600
RATE = 48000
LOOP = "assets/music/loop.wav"


async def clip(ed: Editor, path: str, samples: list, settings: dict | None = None) -> str:
    """a fresh clip: tests share one project"""
    for stale in (ed.project / path, ed.project / (path + ".meta")):
        stale.unlink(missing_ok=True)
    ar.write_wav(ed.project / path, [samples], RATE, ar.PCM16)
    await ed.call("sync_files", paths=[path])
    if settings:
        await ed.call("set_audio_import_settings", paths=[path], settings=settings)
    return path


def music_block(ed: Editor, path: str):
    root = yaml.safe_load((ed.project / (path + ".meta")).read_text()) or {}
    return ((root.get("import") or {}).get("audioImporter") or {}).get("music")


async def test_fix_writes_the_block_and_then_agrees(ed: Editor):
    await clip(ed, LOOP, ar.click_track(120.0, 0.25, 4, 8.0, RATE), {"type": "music"})
    meta = ed.project / (LOOP + ".meta")
    before = meta.read_text()
    r = await ed.call("analyze_audio", paths=[LOOP], image="none")
    m = r["items"][0]["music"]
    ed.check(abs(m["bpm"] - 120.0) <= 0.1, f"bpm {m['bpm']}")
    ed.check(abs(m["first_beat_ms"] - 250.0) <= 5.0, f"first beat {m['first_beat_ms']}")
    ed.check_eq(m["beats_per_bar_guess"], 4, "4/4")
    ed.check(m["confident"], f"a click track is clear: {m}")
    ed.check_eq(m["loop_beats"], 16.0, "8 s at 120 bpm")
    ed.check_eq(len(m["candidates"]), 2, "the detection and its alternative")
    ed.check_eq(m["current"], None, "no block yet")
    ed.check_eq(m["agrees"], False, "nothing to agree with")
    ed.check_eq(m["fix"], {"music": {"bpm": m["bpm"], "beats_per_bar": 4, "first_beat_ms": m["first_beat_ms"]}}, "fix is the settings to pass")
    ed.check_eq(meta.read_text(), before, "analyze_audio writes no .meta")
    grids = list((ed.project / ".jasperengine" / "cache" / "audio").glob("*-grid-v1.json"))
    ed.check_eq(len(grids), 1, f"one cached grid: {grids}")

    s = await ed.call("set_audio_import_settings", paths=[LOOP], settings=m["fix"])
    ed.check_eq(music_block(ed, LOOP), m["fix"]["music"], "the block as fix gave it")
    ed.check_eq(s["items"][0]["resolved"]["first_beat_frame"], round(m["first_beat_ms"] * RATE / 1000), "resolved to frames")
    again = (await ed.call("analyze_audio", paths=[LOOP], image="none"))["items"][0]["music"]
    ed.check_eq(again["current"], m["fix"]["music"], "current reads the block")
    ed.check_eq(again["agrees"], True, "the block agrees with the detection")
    ed.check_eq(len(list((ed.project / ".jasperengine" / "cache" / "audio").glob("*-grid-v1.json"))), 1, "the same bytes, the same grid")

    await ed.call("undo", scope="project")
    ed.check_eq(music_block(ed, LOOP), None, "one undo step removes it")


async def test_bad_music_keys_are_refused_with_the_fix(ed: Editor):
    await clip(ed, LOOP, ar.click_track(120.0, 0.0, 4, 6.0, RATE), {"type": "music"})
    r = await ed.call("set_audio_import_settings", paths=[LOOP], settings={"music": {"bpm": 900}}, expect_error=True)
    ed.check_in("music.bpm must be within 20 .. 400", r, "the range is named")
    r = await ed.call("set_audio_import_settings", paths=[LOOP], settings={"music": {"tempo": 120}}, expect_error=True)
    ed.check_in("bpm, beats_per_bar, first_beat_ms", r, "the keys are named")
    ed.check_eq(music_block(ed, LOOP), None, "nothing written")


async def test_only_loops_and_music_get_music(ed: Editor):
    short = "assets/music/short.wav"
    await clip(ed, short, ar.click_track(120.0, 0.0, 4, 2.0, RATE), {"type": "music"})
    shot = "assets/sfx/shot.wav"
    await clip(ed, shot, ar.sine(1000.0, 0.3, RATE, -12.0))
    r = await ed.call("analyze_audio", paths=[short, shot], image="none")
    m = r["items"][0]["music"]
    ed.check_eq(m["bpm"], None, "too short to judge")
    ed.check_eq([n["id"] for n in m["notes"]], ["too_short"], "and it says so")
    ed.check_eq(m["fix"], None, "nothing to write")
    ed.check("music" not in r["items"][1], "a one-shot has no tempo")
