"""S36 M8: layered presets in families, and the variety report. The oracles read the written WAVs with Python's own
arithmetic: Goertzel and a zero-crossing count for pitch, the energy above a frequency for brightness, the frames for
length, and the sha1 of the bytes for determinism."""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path

import yaml

from harness import Editor

CI = True
EDITOR = {"create": True}

sys.path.insert(0, str(Path(__file__).parent))
import audio_refs as ar  # noqa: E402

TIMEOUT = 600
FAMILIES = {
    "ui_click": ["tink", "snap", "mouse", "glass", "soft"],
    "swoosh": ["fast", "big", "thin"],
    "impact": ["wood", "metal", "stone", "soft", "heavy"],
    "pop": ["bubble", "cork", "soft"],
}
TYPES = {"ui_click": "click", "swoosh": "whoosh", "impact": "impact", "pop": "pop"}


async def gen(ed: Editor, path: str, preset: str, **args):
    return await ed.call_raw("generate_sound", {"path": f"assets/design/{path}.wav", "preset": preset, "image": "none", **args}, timeout=TIMEOUT)


def wav(ed: Editor, path: str):
    return ar.read_wav(ed.project / f"assets/design/{path}.wav")


def sha1(ed: Editor, path: str) -> str:
    return hashlib.sha1((ed.project / f"assets/design/{path}.wav").read_bytes()).hexdigest()


def meta(ed: Editor, path: str) -> dict:
    return yaml.safe_load((ed.project / f"assets/design/{path}.wav.meta").read_text())


def bright_share(x: list, rate: int, above: float = 2000.0) -> float:
    """The share of the energy above a frequency: 1 minus a one-pole low-pass's share of it."""
    a = 1.0 - 2.718281828 ** (-6.283185307 * above / rate)
    low = 0.0
    total = 0.0
    high = 0.0
    for s in x:
        low += a * (s - low)
        total += s * s
        high += (s - low) * (s - low)
    return high / total if total > 0 else 0.0


def crossings_hz(x: list, rate: int) -> float:
    """The rate of upward zero crossings of the loudest half of a tone."""
    peak = max(abs(s) for s in x)
    start = next(i for i, s in enumerate(x) if abs(s) >= 0.5 * peak)
    end = len(x) - next(i for i, s in enumerate(reversed(x)) if abs(s) >= 0.5 * peak)
    ups = [i for i in range(start + 1, end) if x[i - 1] < 0.0 <= x[i]]
    return (len(ups) - 1) * rate / (ups[-1] - ups[0]) if len(ups) > 1 else 0.0


async def test_every_family_renders_to_its_type_with_a_clean_file(ed: Editor):
    for preset, families in FAMILIES.items():
        for family in families:
            name = f"{preset}_{family}"
            reply = await gen(ed, name, preset, family=family, seed=7)
            ed.check(not reply.is_error, f"{name}: {reply.text}")
            data = reply.value
            f = data["files"][0]
            ed.check(data["type"] == TYPES[preset] and f["family"] == family, f"{name} reply: {f}")
            w = wav(ed, name)
            ed.check(w.rate == 48000 and len(w.channels) == 1, f"{name}: {w.rate} Hz {len(w.channels)} channels")
            # the fades of every render
            ed.check(w.channels[0][0] == 0.0 and w.channels[0][-1] == 0.0, f"{name}: first and last samples are 0")
            m = meta(ed, name)
            g = m["provenance"]["generator"]
            ed.check(g["preset"] == preset and g["family"] == family and g["seed"] == 7, f"{name} generator: {g}")
            ed.check(m["provenance"]["licence"] == "generated", f"{name} licence")
            notes = [n["id"] for n in f["analysis"].get("notes", [])]
            ed.check("edge_click" not in notes and "clicks" not in notes, f"{name} notes: {notes}")


async def test_the_default_family_is_the_first(ed: Editor):
    await gen(ed, "default", "impact", seed=3)
    await gen(ed, "wood", "impact", seed=3, family="wood")
    ed.check(sha1(ed, "default") == sha1(ed, "wood"), "impact's default family is wood")
    ed.check(meta(ed, "default")["provenance"]["generator"]["family"] == "wood", "the sidecar names it")


async def test_determinism_and_what_a_seed_varies(ed: Editor):
    await gen(ed, "a", "impact", seed=11, family="metal")
    await gen(ed, "b", "impact", seed=11, family="metal")
    await gen(ed, "c", "impact", seed=12, family="metal")
    await gen(ed, "d", "impact", seed=11, family="soft")
    ed.check(sha1(ed, "a") == sha1(ed, "b"), "the same request gives the same bytes")
    ed.check(sha1(ed, "a") != sha1(ed, "c"), "another seed differs")
    ed.check(sha1(ed, "a") != sha1(ed, "d"), "another family differs")


async def test_families_keep_their_character(ed: Editor):
    # R: the character a family names, measured outside the editor
    await gen(ed, "metal", "impact", seed=5, family="metal")
    await gen(ed, "soft", "impact", seed=5, family="soft")
    metal = wav(ed, "metal")
    soft = wav(ed, "soft")
    ed.check(bright_share(metal.channels[0], 48000) > 2.0 * bright_share(soft.channels[0], 48000), "metal is brighter than soft")
    ed.check(metal.frames > soft.frames, f"metal rings longer: {metal.frames} against {soft.frames}")


async def test_the_six_params(ed: Editor):
    # a fixed force: the draw is still made, so the other draws of the seed stay
    await gen(ed, "tone_0", "pop", seed=9, family="soft", params={"pitch_st": 0, "force": 0})
    await gen(ed, "tone_12", "pop", seed=9, family="soft", params={"pitch_st": 12, "force": 0})
    low = crossings_hz(wav(ed, "tone_0").channels[0], 48000)
    high = crossings_hz(wav(ed, "tone_12").channels[0], 48000)
    ed.check(1.9 < high / low < 2.1, f"pitch_st 12 doubles the frequency: {low:.0f} -> {high:.0f} Hz")

    await gen(ed, "len_1", "swoosh", seed=9, family="fast", params={"length": 1})
    await gen(ed, "len_2", "swoosh", seed=9, family="fast", params={"length": 2})
    ratio = wav(ed, "len_2").frames / wav(ed, "len_1").frames
    ed.check(1.8 < ratio < 2.2, f"length 2 doubles a swoosh: {ratio:.2f}")

    await gen(ed, "bright_0", "impact", seed=9, family="wood", params={"brightness": -1})
    await gen(ed, "bright_1", "impact", seed=9, family="wood", params={"brightness": 1})
    ed.check(bright_share(wav(ed, "bright_1").channels[0], 48000) > bright_share(wav(ed, "bright_0").channels[0], 48000), "brightness brightens")

    await gen(ed, "force_0", "impact", seed=9, family="stone", params={"force": -1})
    await gen(ed, "force_1", "impact", seed=9, family="stone", params={"force": 1})
    ed.check(bright_share(wav(ed, "force_1").channels[0], 48000) > bright_share(wav(ed, "force_0").channels[0], 48000), "a harder hit is brighter")

    await gen(ed, "room_0", "ui_click", seed=9, family="tink", params={"room": -1})
    await gen(ed, "room_1", "ui_click", seed=9, family="tink", params={"room": 1})
    ed.check(wav(ed, "room_1").frames > wav(ed, "room_0").frames, "a room adds a tail")

    await gen(ed, "drive_0", "impact", seed=9, family="wood", params={"drive": -1, "force": 0})
    await gen(ed, "drive_1", "impact", seed=9, family="wood", params={"drive": 1, "force": 0})
    ed.check(sha1(ed, "drive_0") != sha1(ed, "drive_1"), "drive changes the sound")


async def test_refusals_name_what_to_do(ed: Editor):
    text = await ed.call("generate_sound", timeout=TIMEOUT, path="assets/design/x.wav", preset="impact", family="oak", image="none", expect_error=True)
    ed.check("unknown family 'oak' for impact: wood, metal, stone, soft, heavy" in text, text)
    text = await ed.call("generate_sound", timeout=TIMEOUT, path="assets/design/x.wav", preset="click", family="wood", image="none", expect_error=True)
    ed.check("family applies to ui_click, swoosh, impact and pop" in text, text)
    text = await ed.call("generate_sound", timeout=TIMEOUT, path="assets/design/x.wav", preset="impact", params={"length": 9}, image="none", expect_error=True)
    ed.check("length" in text, text)
    ed.check(not (ed.project / "assets/design/x.wav").exists(), "a refusal writes nothing")


async def test_the_variety_report(ed: Editor):
    reply = await gen(ed, "takes", "impact", seed=21, family="wood", variations=8)
    data = reply.value
    variety = data["variety"]
    ed.check(variety["pairs"] == 28, f"8 variations are 28 pairs: {variety}")
    d = variety["distance"]
    ed.check(d["min"] <= d["mean"] <= d["max"], f"distances: {d}")
    a, b, nearest = variety["nearest"]
    ed.check(1 <= a < b <= 8 and nearest == d["min"], f"nearest: {variety['nearest']}")
    ed.check(variety["verdict"] in ("one_palette", "wide", "near_duplicates"), variety["verdict"])
    ed.check(variety["verdict"] == "near_duplicates" or not variety["near_duplicates"], f"verdict against list: {variety}")
    ed.check("8 variations of impact/wood" in variety["text"], variety["text"])
    ed.check(variety["text"] in reply.text, "the text carries it")
    # the first-generation presets get it too, without a family in its words
    reply = await gen(ed, "older", "click", seed=21, variations=3)
    variety = reply.value["variety"]
    ed.check(variety["pairs"] == 3 and "3 variations of click:" in variety["text"], variety["text"])
    # what the seeds of each family measure: the report tells a take from a repeat, so a family's usual seeds are not
    # near-duplicates (at NEAR_DUPLICATE_DISTANCE, for an event's clips, 15 of the 16 were)
    verdicts = []
    for preset, families in FAMILIES.items():
        for family in families:
            reply = await gen(ed, f"var_{preset}_{family}", preset, seed=1, family=family, variations=8)
            v = reply.value["variety"]
            verdicts.append(v["verdict"])
            print(f"[variety] {preset}/{family}: {v['verdict']} {v['distance']} near {len(v['near_duplicates'])}")
    ed.check(verdicts.count("near_duplicates") <= len(verdicts) // 2, f"verdicts: {verdicts}")
    # one file has none
    reply = await gen(ed, "alone", "impact", seed=21)
    ed.check("variety" not in reply.value, "one file has no variety")
