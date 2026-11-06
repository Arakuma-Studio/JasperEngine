"""S36 AC3: the defect measurements and notes of analyze_audio and get_asset_info.

Every expected value comes from outside the editor: tools/mcp/audio_defects_ref.py (the definitions
in numpy, run on soundfile's decode, a second decoder beside SoLoud's), the synthetic clips it writes,
Python's `wave` for first samples, and the cache files' bytes. Levels +/- 0.1 dB on WAV, +/- 0.5 dB
on Ogg; times +/- 1 ms; flags equal except within 1 dB of a threshold, where they are not asserted."""

from __future__ import annotations

import asyncio
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
import time
import wave
from pathlib import Path

import yaml

from harness import Editor, skip

CI = True
EDITOR = {"create": True}

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_refs as ar  # noqa: E402
from test_audio_analysis import write_session  # noqa: E402

TIMEOUT = 1800
RATE = 48000
DIR = "assets/defects"
REPO = Path(__file__).resolve().parents[3]
ESSENTIALS = REPO / "editor/resources/sound-library/essentials"
JUMP = REPO / "tools/fixtures/audio/assets/audio/jump.wav"
REF = Path(ar.__file__).resolve().parent / "audio_defects_ref.py"
ANALYZER_VERSION = 1

NEW_NOTES = ("edge_click", "clicks", "crackle", "truncated_tail", "silence_gap", "harsh_resonance", "aliasing", "trim_cut")
EDGE_MAX_DB = -40.0
# the click-safe fades by kind; the fade-in a note proposes is shortened to keep the attack
# (expected_fade_in), and a cut tail's fade-out, 20 ms, fixes the end's click too
FADE_IN = {"transient": 0.5, "glitch": 0.5, "tonal": 2, "swell": 2, "riser": 2, "speech": 2, "loop": 2}
FADE_OUT = {"transient": 5, "glitch": 5, "tonal": 12, "swell": 12, "riser": 12, "speech": 12, "loop": 0}
TAIL_FIX_MS = 20
STATE: dict = {}


# -- the oracle -------------------------------------------------------------------------------------


async def ref(request: dict):
    def run():
        return subprocess.run([ar._uv(), "run", "--quiet", "--no-project", "--with", "numpy", "--with", "soundfile",
                               "python", str(REF)], input=json.dumps(request), capture_output=True, text=True, timeout=1200)

    out = await asyncio.to_thread(run)
    if out.returncode != 0:
        raise RuntimeError(f"audio_defects_ref {request['op']} failed: {out.stderr[-2000:]}")
    return json.loads(out.stdout.strip().splitlines()[-1])


async def oracle(ed: Editor, jobs: list) -> list:
    """[(project path, region or None)] -> the oracle's {defects, frames, rate, pitch, envelope_peak_ms}"""
    return await ref({"op": "defects", "items": [{"path": str(ed.project / p), "region": r or {}} for p, r in jobs]})


async def synth(ed: Editor) -> None:
    if STATE.get("synth"):
        return
    STATE["synth"] = await ref({"op": "synth", "dir": str(ed.project / DIR)})


def clip(name: str) -> str:
    return f"{DIR}/{name}.wav"


# -- tool helpers -----------------------------------------------------------------------------------


async def analyze(ed: Editor, paths: list, **args) -> list:
    r = await ed.call("analyze_audio", paths=list(paths), image="none", **args)
    ed.check(not r["errors"], f"analyze_audio errors: {r['errors']!r}")
    return r["items"]


async def item_of(ed: Editor, path: str) -> dict:
    return (await analyze(ed, [path]))[0]


def ids(item: dict) -> list:
    return [n["id"] for n in item["notes"]]


def new_ids(item: dict) -> list:
    return [i for i in ids(item) if i in NEW_NOTES]


def note(item: dict, nid: str) -> dict:
    found = [n for n in item["notes"] if n["id"] == nid]
    return found[0] if found else {}


def write_sidecar(ed: Editor, path: str, text: str) -> None:
    ar.write_import(ed.project / path, text)


def near(ed: Editor, actual, expected, tolerance: float, what: str) -> None:
    if expected is None:
        ed.check(actual is None, f"{what}: {actual!r}, expected null")
        return
    ed.check(actual is not None and abs(actual - expected) <= tolerance + 1e-9, f"{what}: {actual!r}, expected {expected} +/- {tolerance}")


def check_edges(ed: Editor, edges: dict, expected: dict, tolerance: float, what: str) -> None:
    """levels within tolerance (both null when 0) where they are audible; flags equal except within
    1 dB of EDGE_MAX_DB"""
    for side in ("start", "end"):
        level, want = edges[f"{side}_db"], expected[f"{side}_db"]
        if want is None or want > -50.0:
            near(ed, level, want, tolerance, f"{what} {side}_db")
        if want is None or abs(want - EDGE_MAX_DB) >= 1.0:
            ed.check_eq(edges[f"{side}_click"], expected[f"{side}_click"], f"{what} {side}_click (oracle level {want})")


def sha1(path: Path) -> str:
    return hashlib.sha1(path.read_bytes()).hexdigest()


def cache_file(ed: Editor, path: str) -> Path:
    return ed.project / ".jasperengine/cache/audio" / f"{sha1(ed.project / path)}-v{ANALYZER_VERSION}.json"


async def fades_supported(ed: Editor) -> bool:
    """the fade keys resolve (import.fade_in_frames)"""
    if "fades" not in STATE:
        await synth(ed)
        probe = f"{DIR}/fade_probe.wav"
        shutil.copyfile(ed.project / clip("start_step"), ed.project / probe)
        write_sidecar(ed, probe, "fade_in_ms: 2\n")
        item = await item_of(ed, probe)
        STATE["fades"] = isinstance(item.get("import"), dict) and "fade_in_frames" in item["import"]
    return STATE["fades"]


def expected_fade_in(edges: dict, kind: str, rate: float):
    """spec 5.5: the fade_in_ms a note proposes from a defects' edges (the oracle's, or the item's
    own where the oracle is checked elsewhere): the kind's click-safe length, or fewer frames when
    edges.safe_fade_in_frames (the longest that keeps the attack) is shorter; 2 frames when none is"""
    kind_ms = FADE_IN[kind]
    kind_frames = ar._round_half_away(kind_ms * rate / 1000.0)
    safe = edges["safe_fade_in_frames"]
    frames = min(safe, kind_frames) if safe > 0 else ar.FADE_IN_FLOOR_FRAMES
    return kind_ms if frames >= kind_frames else round(ar._round_half_away(frames * 1000.0 / rate / 0.001) * 0.001, 3)


def edge_text(start_db=None, end_db=None, kind="transient", fade_in=None, fade_out=None) -> str:
    parts = []
    fade_in = FADE_IN[kind] if fade_in is None else fade_in
    fade_out = FADE_OUT[kind] if fade_out is None else fade_out
    if start_db is not None:
        when = "when it starts" if kind == "loop" else "every time it plays"
        parts.append(f"starts at {start_db:.1f} dB re its peak: clicks {when}; fade_in_ms {fade_in:g} in its import settings fixes it")
    if end_db is not None:
        tail = " fixes it" if start_db is not None else " in its import settings fixes it"
        parts.append(f"ends at {end_db:.1f} dB re its peak: clicks when it stops; fade_out_ms {fade_out:g}{tail}")
    return "; ".join(parts)


# -- 3.1 evidence ------------------------------------------------------------------------------------


async def test_evidence(ed: Editor):
    """The facts the design rests on: jump.wav starts at 9,263 of a 9,496 peak; a whole-cycle 5 kHz
    sine ends at -4.3 dB of its peak; ui/back's largest second differences recur every 337 or 338
    samples (its 131 Hz pulse wave)."""
    with wave.open(str(JUMP), "rb") as w:
        raw = w.readframes(w.getnframes())
    samples = [int.from_bytes(raw[i:i + 2], "little", signed=True) for i in range(0, len(raw), 2)]
    ed.check_eq((samples[0], max(abs(s) for s in samples)), (9263, 9496), "jump.wav's first sample and peak")
    await synth(ed)
    five = ar.read_wav(ed.project / clip("whole_cycles_5k")).channels[0]
    near(ed, ar.amplitude_to_db(abs(five[-1]) / ar.peak(five)), -4.3, 0.05, "a whole-cycle 5 kHz sine's last sample re its peak")
    back = await ref({"op": "second_differences", "path": str(ESSENTIALS / "ui/back.ogg"), "count": 7})
    steps = [b - a for a, b in zip(back["positions"], back["positions"][1:])]
    ed.check_eq(back["rate"], 44100, "ui/back is 44.1 kHz")
    ed.check(all(s in (337, 338) for s in steps), f"ui/back's second-difference peaks every 337-338 samples: {back['positions']}")


# -- D-1 edges ----------------------------------------------------------------------------------------


async def test_d1_edges(ed: Editor):
    ed.require_tools("analyze_audio", "import_files", "write_file")
    await synth(ed)
    names = ["start_step", "end_step", "whole_cycles_1k", "whole_cycles_5k"]
    shutil.copyfile(JUMP, ed.project / DIR / "jump.wav")
    shutil.copyfile(str(JUMP) + ".meta", ed.project / DIR / "jump.wav.meta")
    # test_audio_review_editor's clean 1 kHz sine (it starts at 0 and ends one sample before a zero crossing)
    ar.write_wav(ed.project / DIR / "clean_sine.wav", [ar.sine(1000.0, 1.0, RATE, -12.0)], RATE, ar.FLOAT32)
    await ed.call("import_files", sources=["essentials:ui/tick"], to_dir="assets/d1")
    tick = "assets/d1/ui/tick.ogg"
    await strip_fades(ed, [tick])
    paths = [clip(n) for n in names] + [f"{DIR}/jump.wav", f"{DIR}/clean_sine.wav", tick]
    items = await analyze(ed, paths)
    expected = await oracle(ed, [(p, None) for p in paths])
    for path, item, want in zip(paths, items, expected):
        tolerance = 0.5 if path.endswith(".ogg") else 0.1
        check_edges(ed, item["defects"]["edges"], want["defects"]["edges"], tolerance, path)
    for path, item, want in zip(paths, items, expected):
        # the longest fade-in the attack allows: the same frames on a WAV, within a frame
        # of soundfile's decode on an Ogg
        safe, wsafe = item["defects"]["edges"]["safe_fade_in_frames"], want["defects"]["edges"]["safe_fade_in_frames"]
        ed.check(abs(safe - wsafe) <= (1 if path.endswith(".ogg") else 0), f"{path}: safe_fade_in_frames {safe}, the oracle's {wsafe}")
    by = dict(zip(paths, items))
    want = dict(zip(paths, expected))
    start = by[clip("start_step")]
    fade = expected_fade_in(want[clip("start_step")]["defects"]["edges"], start["kind"], RATE)
    ed.check(fade < FADE_IN[start["kind"]], f"start_step starts at its peak: a shorter fade-in than {FADE_IN[start['kind']]} ms ({fade})")
    ed.check_eq(note(start, "edge_click").get("fix"), {"fade_in_ms": fade}, "start_step's fix")
    ed.check_eq(note(start, "edge_click").get("text"), edge_text(start_db=start["defects"]["edges"]["start_db"], kind=start["kind"], fade_in=fade),
                "start_step's text")
    end = by[clip("end_step")]
    ed.check_eq(note(end, "edge_click").get("fix"), {"fade_out_ms": FADE_OUT[end["kind"]]}, "end_step's fix")
    ed.check_eq(note(end, "edge_click").get("text"), edge_text(end_db=end["defects"]["edges"]["end_db"], kind=end["kind"]), "end_step's text")
    jump = by[f"{DIR}/jump.wav"]
    jump_rate = want[f"{DIR}/jump.wav"]["rate"]
    ed.check_eq((jump["kind"], note(jump, "edge_click").get("fix")),
                ("tonal", {"fade_in_ms": expected_fade_in(want[f"{DIR}/jump.wav"]["defects"]["edges"], "tonal", jump_rate)}),
                "jump.wav: tonal, the fade-in its attack allows")
    ed.check(note(jump, "edge_click").get("text", "").startswith("starts at -0.2 dB re its peak"), f"jump.wav: {jump['notes']}")
    t = by[tick]
    # ui/tick holds a quarter of its energy in its first half millisecond: 0.5 ms took 1.3 LU of it
    #, so its fix is a few frames (the item's own safe frames, checked against the oracle above)
    tick_fade = expected_fade_in(t["defects"]["edges"], "transient", 44100)
    ed.check_eq((t["kind"], note(t, "edge_click").get("fix")), ("transient", {"fade_in_ms": tick_fade}), f"ui/tick: {t['notes']}")
    ed.check(tick_fade <= 0.1, f"ui/tick's fade-in keeps its attack: {tick_fade} ms")
    near(ed, t["defects"]["edges"]["start_db"], -1.0, 0.5, "ui/tick starts at -1.0 dB re its peak")
    for name in ("whole_cycles_1k", "whole_cycles_5k"):
        ed.check_eq(new_ids(by[clip(name)]), [], f"{name}: no new note")
    ed.check_eq(ids(by[f"{DIR}/clean_sine.wav"]), [], "the clean 1 kHz sine of test_audio_review_editor has no notes")
    # summaries keep each note's headline
    line = (await ed.call_raw("analyze_audio", {"paths": [clip("start_step")], "image": "none"})).text
    ed.check(f"starts at {start['defects']['edges']['start_db']:.1f} dB re its peak" in line and "clicks every time" not in line,
             f"the summary line: {line[:400]}")


async def strip_fades(ed: Editor, paths: list) -> None:
    """reset fade_in_ms / fade_out_ms (the essentials' import defaults), so each clip plays as the
    whole file"""
    await ed.call("set_audio_import_settings", paths=paths, settings={"fade_in_ms": None, "fade_out_ms": None})


# -- D-2 clicks, D-3 gaps, D-4 tails ----------------------------------------------------------------


async def test_d2_d3_d4_clicks_gaps_tails(ed: Editor):
    await synth(ed)
    names = ["dropout", "spike", "pulse_131", "square_100", "gap", "tail_cut", "tail_cut_fast_fade", "tail_cut_fade_20",
             "tail_long_fade", "linear_decay"]
    paths = [clip(n) for n in names]
    items = dict(zip(names, await analyze(ed, paths)))
    expected = dict(zip(names, await oracle(ed, [(p, None) for p in paths])))
    for name in ("dropout", "spike"):
        c, want = items[name]["defects"]["clicks"], expected[name]["defects"]["clicks"]
        ed.check_eq((c["count"], want["count"]), (1, 1), f"D-2 {name}: one click (oracle {want})")
        near(ed, c["times_s"][0], want["times_s"][0], 0.001, f"D-2 {name}: its time")
        ed.check_in("clicks", ids(items[name]), f"D-2 {name}: the note")
        ed.check(note(items[name], "clicks")["text"].startswith(f"1 click inside the sound at {c['times_s'][0]:.3f} s: a discontinuity"),
                 f"D-2 {name}: {note(items[name], 'clicks')}")
    near(ed, items["dropout"]["defects"]["clicks"]["times_s"][0], 0.2005, 0.001, "D-2 dropout at 0.2005 s")
    for name in ("pulse_131", "square_100"):
        ed.check_eq((items[name]["defects"]["clicks"]["count"], expected[name]["defects"]["clicks"]["count"]), (0, 0), f"D-2 {name}: no click")
    back = (await analyze(ed, ["essentials:ui/back"]))[0]
    ed.check_eq((back["defects"]["clicks"]["count"], "clicks" in ids(back)), (0, False), "D-2 essentials:ui/back: its pulse edges are no clicks")

    g = items["gap"]["defects"]["gaps"]
    ed.check_eq((g["count"], g["longest_ms"], g["times_s"]), (1, 5.0, [0.2]), "D-3 gap: one, 5.0 ms at 0.200 s")
    ed.check_eq(g, expected["gap"]["defects"]["gaps"], "D-3 gap: the oracle's")
    ed.check_eq(note(items["gap"], "silence_gap").get("text"), "1 gap of digital silence inside the sound (5.0 ms at 0.200 s): it drops out",
                "D-3 the text")
    every = await analyze(ed, ["essentials:*"])
    ed.check_eq([i["path"] for i in every if i["defects"]["gaps"]["count"]], [], "D-3 no essential has a gap")

    for name in ("tail_cut", "tail_cut_fast_fade"):
        tail, want = items[name]["defects"]["tail"], expected[name]["defects"]["tail"]
        ed.check(want["cut_db"] is not None, f"D-4 {name}: the oracle finds the cut: {want}")
        near(ed, tail["cut_db"], want["cut_db"], 0.5, f"D-4 {name} cut_db")
        ed.check_eq(tail["how"], want["how"], f"D-4 {name} how")
        near(ed, tail["slope_db_per_s"], want["slope_db_per_s"], abs(want["slope_db_per_s"]) * 0.1, f"D-4 {name} slope")
        near(ed, tail["release_ms"], want["release_ms"], 1.0, f"D-4 {name} release_ms")
        ed.check_in("truncated_tail", ids(items[name]), f"D-4 {name}: the note")
        ed.check_eq(note(items[name], "truncated_tail").get("fix"), {"fade_out_ms": TAIL_FIX_MS}, f"D-4 {name}: the fix")
    ed.check_eq((items["tail_cut"]["defects"]["tail"]["how"], items["tail_cut_fast_fade"]["defects"]["tail"]["how"]), ("stop", "fade"),
                "D-4 a stop and a fast final fade (5 ms)")
    text = note(items["tail_cut"], "truncated_tail")["text"]
    ed.check(text.startswith(f"tail cut at {items['tail_cut']['defects']['tail']['cut_db']:.1f} dB re its peak while still decaying (") and
             text.endswith(f"dB/s): it ends abruptly; fade_out_ms {TAIL_FIX_MS} in its import settings fixes it"), f"D-4 the stop text: {text}")
    text = note(items["tail_cut_fast_fade"], "truncated_tail")["text"]
    release = items["tail_cut_fast_fade"]["defects"]["tail"]["release_ms"]
    ed.check(text.startswith(f"ends in a {release:.0f} ms fade at ") and text.endswith(f"dB/s): it sounds cut; fade_out_ms {TAIL_FIX_MS} in its import settings fixes it"),
             f"D-4 the fade text: {text}")
    # a 20 ms fade baked into the file is the fix's own release: no cut
    for name in ("tail_cut_fade_20", "tail_long_fade", "linear_decay"):
        ed.check_eq((items[name]["defects"]["tail"]["cut_db"], expected[name]["defects"]["tail"]["cut_db"]), (None, None), f"D-4 {name}: no cut")
        ed.check_eq(new_ids(items[name]), [], f"D-4 {name}: none of the new notes")


# -- D-5 resonance and aliasing -----------------------------------------------------------------------


async def test_d5_resonance_and_aliasing(ed: Editor):
    await synth(ed)
    shutil.copyfile(ed.project / clip("noise_ring"), ed.project / clip("ring_impact"))
    write_sidecar(ed, clip("noise_ring"), "type: whoosh\n")
    write_sidecar(ed, clip("noise"), "type: whoosh\n")
    write_sidecar(ed, clip("ring_impact"), "type: impact\n")
    names = ["noise_ring", "noise", "ring_impact", "saw_naive", "saw_blep", "long_tone"]
    paths = [clip(n) for n in names]
    items = dict(zip(names, await analyze(ed, paths)))
    expected = dict(zip(names, await oracle(ed, [(p, None) for p in paths])))
    ring = items["noise_ring"]
    ed.check_eq((ring["kind"], "harsh_resonance" in ids(ring)), ("swell", True), f"D-5 noise_ring as a whoosh: {ring['notes']}")
    near(ed, ring["defects"]["resonance"]["hz"], 3000.0, 90.0, "D-5 the ring at 3 kHz +/- 3%")
    near(ed, ring["defects"]["resonance"]["score_db"], expected["noise_ring"]["defects"]["resonance"]["score_db"], 0.5, "D-5 its score")
    ed.check(note(ring, "harsh_resonance")["text"].startswith(f"narrow peak +{ring['defects']['resonance']['prominence_db']:.1f} dB at 3,0"),
             f"D-5 the text: {note(ring, 'harsh_resonance')}")
    ed.check_eq(new_ids(items["noise"]), [], f"D-5 the noise alone: {items['noise']['notes']}")
    ed.check(items["noise"]["defects"]["resonance"]["score_db"] < 15, f"D-5 the noise scores low: {items['noise']['defects']['resonance']}")
    ed.check_eq((items["ring_impact"]["kind"], new_ids(items["ring_impact"])), ("transient", []), "D-5 the ringing noise typed impact")
    naive, blep = items["saw_naive"], items["saw_blep"]
    ed.check_in("aliasing", ids(naive), f"D-5 the naive saw: {naive['notes']}")
    ed.check("fold back (information): naive synthesis" in note(naive, "aliasing")["text"], "D-5 an information note")
    near(ed, naive["defects"]["aliasing"]["fold"], expected["saw_naive"]["defects"]["aliasing"]["fold"], 0.02, "D-5 the naive saw's fold")
    ed.check("aliasing" not in ids(blep) and blep["defects"]["aliasing"]["fold"] <= 0.05, f"D-5 the band-limited saw: {blep['defects']}")
    tone = items["long_tone"]
    ed.check_eq((tone["defects"]["resonance"], tone["defects"]["aliasing"]), (None, None), "D-5 a 12 s tone: resonance and aliasing null")
    ed.check(not {"harsh_resonance", "aliasing"} & set(ids(tone)), f"D-5 the 12 s tone: {tone['notes']}")


# -- D-6 gating --------------------------------------------------------------------------------------

# spec 2.3.2, as its table reads (kind -> notes allowed; designed-end types lose truncated_tail)
ALLOWED = {
    "transient": {"edge_click", "clicks", "truncated_tail", "silence_gap", "aliasing", "trim_cut"},
    "tonal": {"edge_click", "clicks", "truncated_tail", "silence_gap", "aliasing", "trim_cut"},
    "swell": set(NEW_NOTES),
    "riser": set(NEW_NOTES),
    "speech": {"edge_click", "clicks", "truncated_tail", "silence_gap", "harsh_resonance", "trim_cut"},
    "glitch": {"aliasing", "trim_cut"},
    "loop": {"edge_click", "clicks", "silence_gap", "trim_cut"},
}
GATING_TYPES = {"impact": "transient", "chime": "tonal", "whoosh": "swell", "riser": "riser", "voice": "speech",
                "glitch": "glitch", "ambience": "loop", "ui": "tonal", "jump": "tonal"}


def triggered(defects: dict) -> dict:
    """note id -> what the measurements say before gating (edge sides separately)"""
    out = {"edge_start": defects["edges"]["start_click"], "edge_end": defects["edges"]["end_click"],
           "clicks": defects["clicks"]["count"] > 0, "truncated_tail": bool(defects["tail"] and defects["tail"]["cut_db"] is not None),
           "silence_gap": defects["gaps"]["count"] > 0,
           "harsh_resonance": bool(defects["resonance"] and (defects["resonance"]["score_db"] or 0) > 15),
           "aliasing": bool(defects["aliasing"] and (defects["aliasing"]["fold"] or 0) > 0.05)}
    return out


def gated(defects: dict, kind: str, type_name: str) -> list:
    t = triggered(defects)
    allowed = set(ALLOWED[kind])
    if type_name in ("glitch", "ui", "zap", "pickup", "jump", "stinger"):
        allowed.discard("truncated_tail")
    out = []
    edge = (t["edge_start"] or (t["edge_end"] and kind != "loop")) and "edge_click" in allowed
    if edge:
        out.append("edge_click")
    for nid in ("clicks", "truncated_tail", "silence_gap", "harsh_resonance", "aliasing"):
        if t[nid] and nid in allowed:
            out.append(nid)
    return out


async def test_d6_gating(ed: Editor):
    await synth(ed)
    path = clip("gating")
    first = await item_of(ed, path)
    t = triggered(first["defects"])
    ed.check(all(t[k] for k in ("edge_start", "edge_end", "clicks", "truncated_tail", "silence_gap", "harsh_resonance")),
             f"D-6 the gating file has every defect but aliasing: {first['defects']}")
    cache = cache_file(ed, path)
    stamp = cache.stat().st_mtime_ns
    for type_name, kind in GATING_TYPES.items():
        write_sidecar(ed, path, f"type: {type_name}\n")
        item = await item_of(ed, path)
        ed.check_eq((item["kind"], new_ids(item)), (kind, gated(item["defects"], kind, type_name)), f"D-6 typed {type_name}")
        if kind == "loop":
            ed.check(not note(item, "edge_click").get("fix", {}).get("fade_out_ms"), f"D-6 a loop's edge_click has only its start half: {item['notes']}")
            ed.check_eq(note(item, "edge_click").get("fix"), {"fade_in_ms": expected_fade_in(item["defects"]["edges"], "loop", RATE)},
                        "D-6 the loop's fix")
    ed.check_eq(cache.stat().st_mtime_ns, stamp, "D-6 no new analysis for a type change (the cache file is untouched)")
    # aliasing by kind on the naive saw
    saw = clip("saw_naive")
    for type_name in ("chime", "voice", "ambience", "glitch", "whoosh"):
        write_sidecar(ed, saw, f"type: {type_name}\n")
        item = await item_of(ed, saw)
        ed.check_eq("aliasing" in ids(item), type_name in ("chime", "glitch", "whoosh"), f"D-6 the naive saw typed {type_name}: {ids(item)}")
    write_sidecar(ed, saw, "")
    # a capture range of start_step: none of the new notes
    steps = ar.read_wav(ed.project / clip("start_step")).channels[0]
    write_session(ed.project, "20261017-120000-01", [steps, steps])
    capture = (await ed.call("analyze_audio", capture="20261017-120000-01", image="none"))["items"][0]
    ed.check_eq(([i for i in ids(capture) if i in NEW_NOTES], "defects" in capture), ([], False), f"D-6 a capture: {capture['notes']}")


# -- D-7 the earshot fixtures ------------------------------------------------------------------------

# spec 2.13's tables: id -> (notes expected, notes not asserted); every other new note must be absent
EARSHOT = {
    "ui/tick": ({"edge_click"}, set()),
    "ui/error": ({"edge_click"}, set()),
    "shoot/laser_small": ({"edge_click"}, set()),
    "ui/click": ({"edge_click", "truncated_tail"}, set()),
    "door/scifi_close": (set(), set()),
    "door/scifi_open": (set(), set()),
    "explosion/crunch": (set(), set()),
    "footstep/concrete_1": ({"edge_click", "truncated_tail"}, set()),
    "footstep/concrete_2": ({"edge_click", "truncated_tail"}, set()),
    "impact/light": ({"truncated_tail"}, {"edge_click"}),
    "jump/phase": (set(), set()),
    # its 50 ms fade-out baked into the file is a release, not a cut
    "shoot/laser_retro": (set(), set()),
    "ui/back": (set(), set()),
    "ui/close": (set(), set()),
    "ui/confirm": (set(), set()),
    "ui/pop": ({"truncated_tail"}, {"edge_click"}),
    "stinger/pizzicato": ({"edge_click"}, {"aliasing"}),
    "ui/glitch": (set(), set()),
    "ui/chime": ({"edge_click"}, set()),
    "ui/hover": ({"edge_click"}, set()),
    "ui/select": ({"edge_click"}, set()),
    "impact/metal": ({"truncated_tail"}, set()),
}
# the side of each edge_click (spec 2.10's table)
EDGE_SIDES = {"ui/tick": "start", "ui/error": "start", "shoot/laser_small": "both", "ui/click": "start", "footstep/concrete_1": "end",
              "footstep/concrete_2": "end", "stinger/pizzicato": "end", "ui/chime": "start", "ui/hover": "end", "ui/select": "start"}


async def test_d7_earshot_fixtures(ed: Editor):
    clips = json.loads((ESSENTIALS / "manifest.json").read_text())["clips"]
    await ed.call("import_files", sources=["essentials:*"], to_dir="assets/ess")
    paths = [f"assets/ess/{c['path']}" for c in clips]
    await strip_fades(ed, paths)
    items = []
    for at in range(0, len(paths), 20):
        items += await analyze(ed, paths[at:at + 20])
    expected = await oracle(ed, [(p, None) for p in paths])
    for c, item, want in zip(clips, items, expected):
        what = c["id"]
        wanted, free = EARSHOT.get(what, (set(), set()))
        got = set(new_ids(item))
        ed.check_eq(got - free, wanted, f"D-7 {what} ({item['kind']}, {item.get('type')}): {item['notes']}")
        check_edges(ed, item["defects"]["edges"], want["defects"]["edges"], 0.5, f"D-7 {what}")
        tail, wtail = item["defects"]["tail"], want["defects"]["tail"]
        if tail and wtail and tail["cut_db"] is not None and wtail["cut_db"] is not None:
            near(ed, tail["cut_db"], wtail["cut_db"], 0.5, f"D-7 {what} tail cut_db")
        if "truncated_tail" in got:
            ed.check_eq(note(item, "truncated_tail").get("fix"), {"fade_out_ms": TAIL_FIX_MS}, f"D-7 {what}: the tail's fix")
        if what in EDGE_SIDES:
            e = item["defects"]["edges"]
            side = EDGE_SIDES[what]
            ed.check_eq((e["start_click"], e["end_click"]), (side in ("start", "both"), side in ("end", "both")), f"D-7 {what}'s clicking side")
        print(f"[defects] {what}: edges {item['defects']['edges']} tail {tail} aliasing {item['defects']['aliasing']} notes {sorted(got)}")
    pizz = next(i for c, i in zip(clips, items) if c["id"] == "stinger/pizzicato")
    print(f"[defects] stinger/pizzicato aliasing (recorded): {pizz['defects']['aliasing']}")
    shutil.copyfile(JUMP, ed.project / "assets/ess/jump.wav")
    shutil.copyfile(str(JUMP) + ".meta", ed.project / "assets/ess/jump.wav.meta")
    jump = await item_of(ed, "assets/ess/jump.wav")
    ed.check_eq((new_ids(jump), "dc_offset" in ids(jump)), (["edge_click"], True), f"D-7 jump.wav: {jump['notes']}")


# -- D-8 played regions, D-11 get_asset_info ------------------------------------------------------


async def test_d8_d11_played_region(ed: Editor):
    ed.require_tools("analyze_audio", "write_file", "get_asset_info")
    await synth(ed)
    path = clip("trim_into")
    write_sidecar(ed, path, "trim_head_ms: 50\n")
    item = await item_of(ed, path)
    region = item["defects"]["region"]
    ed.check_eq((region["start_frame"], region["fade_in_frames"]), (2400, 0), "D-8 the region starts at the 50 ms trim")
    want = (await oracle(ed, [(path, {"start_frame": 2400})]))[0]["defects"]
    near(ed, item["defects"]["cuts"]["head_db"], want["cuts"]["head_db"], 0.1, "D-8 the cut level")
    near(ed, item["defects"]["cuts"]["head_db"], -9.0, 0.1, "D-8 the trim cuts at -9 dB")
    check_edges(ed, item["defects"]["edges"], want["edges"], 0.1, "D-8 trim_into")
    ed.check(set(new_ids(item)) >= {"trim_cut", "edge_click"}, f"D-8 trim_cut and edge_click: {item['notes']}")
    fade = expected_fade_in(want["edges"], item["kind"], RATE)
    ed.check_eq(note(item, "trim_cut"), {"id": "trim_cut", "text": f"trim_head_ms cuts into the sound at -9.0 dB re its peak: it starts mid-sound; "
                                                                    f"add fade_in_ms {fade:g}", "fix": {"fade_in_ms": fade}}, "D-8 the trim_cut note")
    played_cache = list(cache_file(ed, path).parent.glob(f"{sha1(ed.project / path)}-i*-v{ANALYZER_VERSION}.json"))
    ed.check_eq(len(played_cache), 1, "D-8 the clip as it plays has its cache file")
    ed.check_eq(json.loads(played_cache[0].read_text())["played"]["start_frame"], 2400, "D-8 the cache holds the played region")
    info = await ed.call("get_asset_info", path=path)
    texts = [n["text"] for n in item["notes"]]
    ed.check_eq(info["audio"]["analysis"]["notes"], texts, "D-11 get_asset_info's notes are the played region's")

    # a gain alone is a new region; levels re the peak do not move
    write_sidecar(ed, clip("start_step"), "gain_db: -6\n")
    gained = await item_of(ed, clip("start_step"))
    write_sidecar(ed, clip("start_step"), "")
    plain = await item_of(ed, clip("start_step"))
    ed.check_eq(gained["defects"]["region"]["gain_db"], -6.0, "D-8 the gain is the region's")
    ed.check_eq(gained["defects"]["edges"], plain["defects"]["edges"], "D-8 edges re the peak are the same at -6 dB")
    gained_cache = list(cache_file(ed, clip("start_step")).parent.glob(f"{sha1(ed.project / clip('start_step'))}-i*-v{ANALYZER_VERSION}.json"))
    ed.check_eq(len(gained_cache), 1, "D-8 the gained region is cached")

    ed.check(await fades_supported(ed), "fade_in_ms resolves")
    write_sidecar(ed, path, "trim_head_ms: 50\nfade_in_ms: 2\n")
    faded = await item_of(ed, path)
    ed.check_eq((faded["defects"]["region"]["fade_in_frames"], faded["import"]["fade_in_frames"]), (96, 96), "D-8 96 frames of fade")
    ed.check("edge_click" not in ids(faded), f"D-8 the fade removes edge_click: {faded['notes']}")
    ed.check_eq(note(faded, "trim_cut"), {"id": "trim_cut", "text": "trim_head_ms cuts into the sound at -9.0 dB re its peak: it starts "
                                                                    "mid-sound; the fade hides the click"}, "D-8 the fade hides the click")
    ed.check_eq(faded["defects"]["edges"]["start_db"], None, "D-8 the faded start is exactly 0")
    info = await ed.call("get_asset_info", path=path)
    ed.check_eq(info["audio"]["analysis"]["notes"], [n["text"] for n in faded["notes"]], "D-11 the faded notes")
    ed.check_eq(info["audio"]["import"].get("fade_in_ms"), 2, "D-11 audio.import shows fade_in_ms")


# -- D-9 auto trims -----------------------------------------------------------------------------------


async def test_d9_auto_trims(ed: Editor):
    await synth(ed)
    step, rise = clip("silence_then_step"), clip("silence_then_rise")
    write_sidecar(ed, step, "trim_head_ms: auto\n")
    write_sidecar(ed, rise, "trim_head_ms: auto\n")
    items = await analyze(ed, [step, rise])
    s, r = items
    ed.check_eq(s["defects"]["region"]["start_frame"], 4800, "D-9 the auto trim cuts the 100 ms of zeros")
    want_step = (await oracle(ed, [(step, {"start_frame": s["defects"]["region"]["start_frame"]})]))[0]["defects"]
    ed.check_eq(note(s, "edge_click").get("fix"), {"fade_in_ms": expected_fade_in(want_step["edges"], s["kind"], RATE)},
                f"D-9 a jump from silence clicks: {s['notes']}")
    ed.check_eq(new_ids(r), [], f"D-9 a 10 ms rise: none of the new notes: {r['notes']} {r['defects']}")
    want = await oracle(ed, [(rise, {"start_frame": r["defects"]["region"]["start_frame"]})])
    near(ed, r["defects"]["cuts"]["head_db"], want[0]["defects"]["cuts"]["head_db"], 0.1, "D-9 the rise's cut level")


# -- D-10 the cache ------------------------------------------------------------------------------------


async def test_d10_cache(ed: Editor):
    await synth(ed)
    path = clip("tail_cut")
    digest = sha1(ed.project / path)
    old = ed.project / ".jasperengine/cache/audio" / f"{digest}-v{ANALYZER_VERSION - 1}.json"
    old.parent.mkdir(parents=True, exist_ok=True)
    old.write_text(json.dumps({"analyzer_version": ANALYZER_VERSION - 1, "sha1": digest, "format": {}, "stale": True}))
    item = await item_of(ed, path)
    cache = cache_file(ed, path)
    stored = json.loads(cache.read_text())
    ed.check_eq((stored["analyzer_version"], stored["sha1"]), (ANALYZER_VERSION, digest), f"D-10 the v{ANALYZER_VERSION} cache file")
    ed.check(isinstance(stored["defects"], dict) and "envelope_peak_ms" in stored["onset"], "D-10 defects and onset.envelope_peak_ms are cached")
    ed.check(old.exists() and json.loads(old.read_text()).get("stale"), f"D-10 the v{ANALYZER_VERSION - 1} file is left as it was")
    ed.check("stale" not in item, f"D-10 the v{ANALYZER_VERSION - 1} file is ignored")
    want = (await oracle(ed, [(path, None)]))[0]
    near(ed, stored["onset"]["envelope_peak_ms"], want["envelope_peak_ms"], 1.0, "D-10 envelope_peak_ms against the oracle's 20 ms RMS peak")
