"""S36 M9: edit_sound_event slice cuts a multi-take file into a shuffle Sound Event. The source here is built from known
takes, so the oracle knows where every slice must start and what samples it holds; the WAVs are read with audio_refs,
the event with json, the sidecars with PyYAML."""

from __future__ import annotations

import json
import math
import struct
import sys
from pathlib import Path

import yaml

from harness import Editor

CI = True
EDITOR = {"create": True}

sys.path.insert(0, str(Path(__file__).parent))
import audio_refs as ar  # noqa: E402

RATE = 48000
TIMES = [0.25, 0.75, 1.25, 1.75]
SECONDS = 2.3
TIMEOUT = 300


def take(freq: float, seconds: float = 0.25) -> list:
    return [0.7 * math.sin(2 * math.pi * freq * i / RATE) * math.exp(-25.0 * i / RATE) for i in range(int(seconds * RATE))]


def source(ed: Editor, name: str = "takes", licence: bool = True) -> str:
    x = [0.0] * int(SECONDS * RATE)
    for n, at in enumerate(TIMES):
        burst = take(500.0 + 200.0 * n)
        start = int(at * RATE)
        x[start:start + len(burst)] = burst
    path = ed.project / f"assets/slice/{name}.wav"
    path.parent.mkdir(parents=True, exist_ok=True)
    ar.write_wav(path, [x], RATE)
    lines = ["  version: 1", "  licence: CC0-1.0", "  author: Test"] if licence else []
    if lines:
        ar.write_meta_section(path, "provenance", lines)
    ar.write_meta_section(path, "import", ["  audioImporter:", "    version: 1", "    type: footstep"])
    return f"assets/slice/{name}.wav"


async def slice_call(ed: Editor, event: str, **slice_args):
    return await ed.call_raw("edit_sound_event", {"path": f"assets/slice/{event}.soundevent", "create": True, "slice": slice_args}, timeout=TIMEOUT)


async def test_preview_writes_nothing(ed: Editor):
    rel = source(ed, "preview")
    await ed.call("sync_files")
    r = await slice_call(ed, "p", source=rel, preview=True)
    ed.check(not r.is_error, r.text)
    slices = r.value["slices"]
    ed.check_eq(len(slices), len(TIMES), f"one slice per take: {slices}")
    for s, at in zip(slices, TIMES):
        ed.check(abs(s["start_s"] - at) < 0.012, f"slice at {at}: {s}")
    width, height = struct.unpack(">II", r.images[0][0][16:24])
    ed.check_eq((width, height), (768, 184), "the slices image")
    ed.check(not (ed.project / "assets/slice/p.soundevent").exists(), "preview writes no event")
    ed.check(not (ed.project / "assets/slice/p_01.wav").exists(), "preview writes no clip")


async def test_slices_become_a_shuffle_event_in_one_undo_step(ed: Editor):
    rel = source(ed, "whole")
    await ed.call("sync_files")
    before = (ed.project / rel).read_bytes()
    r = await slice_call(ed, "steps", source=rel)
    ed.check(not r.is_error, r.text)
    ed.check_eq(r.value["event"], "assets/slice/steps.soundevent", "the reply names the event")
    files = [ed.project / f"assets/slice/steps_{n:02d}.wav" for n in range(1, 5)]
    source_samples = ar.read_wav(ed.project / rel).channels[0]
    for n, (f, s) in enumerate(zip(files, r.value["slices"])):
        ed.check(f.exists(), f"{f.name}")
        ed.check_eq(s["path"], f"assets/slice/{f.name}", "the slice's path")
        w = ar.read_wav(f)
        start = round(s["start_s"] * RATE)
        ed.check(all(abs(a - b) < 2.0 / 32767 for a, b in zip(w.channels[0], source_samples[start:start + w.frames])), f"{f.name} holds the source's samples, unfaded")
        m = yaml.safe_load(Path(str(f) + ".meta").read_text())
        p = m["provenance"]
        ed.check_eq(p["licence"], "CC0-1.0", "the source's licence, verbatim")
        ed.check_eq(p["sliced_from"]["path"], rel, "sliced_from")
        ed.check_eq(p["sliced_from"]["end_frame"] - p["sliced_from"]["start_frame"], w.frames, "the frames recorded")
        ed.check_eq(m["import"]["audioImporter"]["type"], "footstep", "the source's type")
    event = json.loads((ed.project / "assets/slice/steps.soundevent").read_text())
    ed.check_eq(event["mode"], "shuffle", "the mode")
    ed.check_eq(event.get("type"), "footstep", "the type")
    ed.check_eq(len(event["clips"]), 4, "every slice is a clip")
    for c, f in zip(event["clips"], files):
        guid = yaml.safe_load(Path(str(f) + ".meta").read_text())["guid"]
        ed.check_eq(c["clip"]["guid"], guid, "the clip by its GUID")
    ed.check_eq((ed.project / rel).read_bytes(), before, "the source is not changed")
    ed.check_eq(r.value["problems"], [], "no problems")

    await ed.call("undo", scope="project")
    ed.check(not (ed.project / "assets/slice/steps.soundevent").exists(), "undo removes the event")
    for f in files:
        ed.check(not f.exists() and not Path(str(f) + ".meta").exists(), f"undo removes {f.name} and its meta")


async def test_pick_and_variation(ed: Editor):
    rel = source(ed, "pick")
    await ed.call("sync_files")
    r = await slice_call(ed, "chosen", source=rel, pick=[2, 4], variation=True)
    ed.check(not r.is_error, r.text)
    ed.check_eq([s["n"] for s in r.value["slices"]], [2, 4], "the picked slices")
    ed.check((ed.project / "assets/slice/chosen_02.wav").exists() and not (ed.project / "assets/slice/chosen_03.wav").exists(), "numbered from 01 in pick order")
    event = json.loads((ed.project / "assets/slice/chosen.soundevent").read_text())
    ed.check_eq(event["pitch_semitones"], [-1, 1], "variation's pitch")
    ed.check_eq(event["volume_db"], [-1.5, 0], "variation's volume")
    r = await ed.call_raw("edit_sound_event", {"path": "assets/slice/heard.soundevent", "create": True, "audition": 2, "slice": {"source": rel}}, timeout=TIMEOUT)
    ed.check(not r.is_error and len(r.value["renditions"]) == 2, f"audition renders the new event: {r.text}")


async def test_a_source_without_licence_is_a_problem(ed: Editor):
    rel = source(ed, "nolicence", licence=False)
    await ed.call("sync_files")
    r = await slice_call(ed, "unknown", source=rel)
    ed.check(not r.is_error, r.text)
    ed.check("source has no licence: the slices' licence is unknown" in r.value["problems"], r.value["problems"])


async def test_refusals_write_nothing(ed: Editor):
    rel = source(ed, "refused")
    await ed.call("sync_files")
    err = await ed.call("edit_sound_event", path="assets/slice/r.soundevent", create=True, slice={"source": rel, "gate_db": 0}, expect_error=True)
    ed.check("gate_db" in err and "Nothing was written" in err, err)
    err = await ed.call("edit_sound_event", path="assets/slice/r.soundevent", create=True, slice={"source": rel, "pick": [9]}, expect_error=True)
    ed.check("pick 9" in err, err)
    err = await ed.call("edit_sound_event", path="assets/slice/r.soundevent", create=True, slice={"source": rel, "nonsense": 1}, expect_error=True)
    ed.check("slice.nonsense" in err, err)
    err = await ed.call("edit_sound_event", path="assets/slice/r.soundevent", create=True, values={"loop": True}, slice={"source": rel}, expect_error=True)
    ed.check("a second call" in err, err)
    err = await ed.call("edit_sound_event", path="assets/slice/r.soundevent", slice={"source": rel}, expect_error=True)
    ed.check("create: true" in err or "does not exist" in err, err)
    err = await ed.call("edit_sound_event", path="assets/slice/r.soundevent", create=True, slice={"source": "assets/slice/none.wav"}, expect_error=True)
    ed.check("slice.source" in err, err)
    (ed.project / "assets/slice/taken_02.wav").write_bytes(b"x")
    await ed.call("sync_files")
    err = await ed.call("edit_sound_event", path="assets/slice/taken.soundevent", create=True, slice={"source": rel}, expect_error=True)
    ed.check("taken_02.wav exists" in err, err)
    ed.check(not (ed.project / "assets/slice/taken_01.wav").exists(), "a target that exists refuses the whole write")
    ed.check(not (ed.project / "assets/slice/r.soundevent").exists(), "a refusal writes no event")
