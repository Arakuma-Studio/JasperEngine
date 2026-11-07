"""S36 M10: generate_sound like, a placeholder fitted to a reference of any licence. The reference's only trace in the
result is its measurements: the written file is a render of the preset (a generated licence), and provenance.reference
records where it came from."""

from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

import yaml

from harness import Editor

CI = True
EDITOR = {"create": True}

sys.path.insert(0, str(Path(__file__).parent))
import audio_refs as ar  # noqa: E402

TIMEOUT = 600
RATE = 48000


async def gen(ed: Editor, path: str, preset: str, **args):
    args.setdefault("image", "none")
    return await ed.call_raw("generate_sound", {"path": f"assets/fit/{path}.wav", "preset": preset, **args}, timeout=TIMEOUT)


async def refusal(ed: Editor, preset: str, like: str, needle: str, **args):
    text = await ed.call("generate_sound", timeout=TIMEOUT, path="assets/fit/refused.wav", preset=preset, like=like, image="none", expect_error=True, **args)
    ed.check(needle in text, f"like {like} with {preset}: {text}")
    ed.check(not (ed.project / "assets/fit/refused.wav").exists(), "a refusal writes nothing")


async def test_a_fit_finds_its_own_family(ed: Editor):
    await gen(ed, "reference", "impact", seed=4, family="metal")
    # the reference's own seed: its family at the defaults is the reference itself, as the written file measures it
    reply = await ed.call_raw("generate_sound", {"path": "assets/fit/fitted.wav", "preset": "impact", "like": "assets/fit/reference.wav", "seed": 4}, timeout=TIMEOUT)
    data = reply.value
    fit = data["fit"]
    ed.check(fit["family"] == "metal", f"the family a metal hit fits best: {fit}")
    ed.check(fit["distance"] <= fit["start_distance"], f"the search does not end worse: {fit}")
    ed.check(fit["grade"] == "close" and fit["distance"] < 0.1, f"the search finds the reference's own render: {fit}")
    ed.check(0 < fit["renders"] <= 64, f"bounded: {fit['renders']}")
    other = await gen(ed, "other_seed", "impact", like="assets/fit/reference.wav")
    ed.check_eq(other.value["fit"]["family"], "metal", "another seed finds the family too")
    ed.check(other.value["fit"]["distance"] <= other.value["fit"]["start_distance"], f"the search does not end worse: {other.value['fit']}")
    ed.check(set(fit["params"]) == {"pitch_st", "length", "brightness", "room", "drive"}, f"the space: {fit['params']}")
    ed.check(fit["reference"]["path"] == "assets/fit/reference.wav", fit["reference"])
    text = reply.text
    ed.check("fit to assets/fit/reference.wav" in text and "renders" in text, text)
    ed.check("not that it sounds right" in text, "the text says what the distance is not")

    # the compare image
    width, height = struct.unpack(">II", reply.images[0][0][16:24])
    ed.check_eq((width, height), (768, 248), "the compare image")
    ed.check_eq(data["image"]["mode"], "compare", "the default image of one fitted file")

    m = yaml.safe_load((ed.project / "assets/fit/fitted.wav.meta").read_text())
    p = m["provenance"]
    ed.check(p["licence"] == "generated" and p["source"] == "generate_sound", p)
    ed.check(p["reference"]["path"] == "assets/fit/reference.wav" and p["generator"]["family"] == "metal", p)


async def test_a_fit_is_reproducible(ed: Editor):
    await gen(ed, "ref_r", "thump", seed=8)
    await gen(ed, "r1", "thump", like="assets/fit/ref_r.wav")
    await gen(ed, "r2", "thump", like="assets/fit/ref_r.wav")
    one = hashlib.sha1((ed.project / "assets/fit/r1.wav").read_bytes()).hexdigest()
    two = hashlib.sha1((ed.project / "assets/fit/r2.wav").read_bytes()).hexdigest()
    ed.check(one == two, "the fit is a pure function of the reference, the preset and the seed")
    reply = await gen(ed, "r3", "thump", like="assets/fit/ref_r.wav", variations=2)
    data = reply.value
    ed.check(data["files"][0]["seed"] == 1, f"like: the seed defaults to 1: {data['files'][0]['seed']}")
    ed.check("variety" in data and "fit" in data, "variations keep their report and the fit")


async def test_the_search_improves_on_the_defaults(ed: Editor):
    # a recipe far from its defaults, from a low thump: 200 Hz less and much longer
    await gen(ed, "ref_far", "thump", seed=2, params={"freq_hz": 140, "decay_ms": 700, "length_ms": 1600, "click": 0})
    reply = await gen(ed, "far", "thump", like="assets/fit/ref_far.wav", seed=2)
    fit = reply.value["fit"]
    ed.check(fit["distance"] < 0.5 * fit["start_distance"], f"the search earns its renders: {fit}")


async def test_a_reference_of_any_licence_is_recorded(ed: Editor):
    # user decision 6: a fit copies no audio, so a licence is no reason to refuse: it is recorded
    path = ed.project / "assets/fit/cc_by.wav"
    path.parent.mkdir(parents=True, exist_ok=True)
    ar.write_wav(path, [ar.linear_burst(800.0, RATE, 0.002, 0.2)], RATE)
    ar.write_meta_section(path, "provenance", ["  version: 1", "  licence: CC-BY-4.0", "  author: Someone", "  url: https://example.com/hit"])
    await ed.call("sync_files")
    reply = await gen(ed, "from_by", "impact", like="assets/fit/cc_by.wav", family="wood")
    data = reply.value
    ed.check(data["licence"] == "generated", data["licence"])
    ref = yaml.safe_load((ed.project / "assets/fit/from_by.wav.meta").read_text())["provenance"]["reference"]
    ed.check(ref == {"path": "assets/fit/cc_by.wav", "licence": "CC-BY-4.0", "author": "Someone", "url": "https://example.com/hit"}, ref)
    ed.check(data["fit"]["family"] == "wood", "a family given stays")


async def test_a_builtin_reference(ed: Editor):
    reply = await gen(ed, "from_builtin", "impact", like="essentials:impact/light")
    fit = reply.value["fit"]
    ed.check(fit["reference"]["path"] == "essentials:impact/light", fit["reference"])
    ed.check(fit["reference"]["licence"] == "CC0-1.0", fit["reference"])


async def test_refusals(ed: Editor):
    await gen(ed, "ok", "click", seed=1)
    like = "assets/fit/ok.wav"
    await refusal(ed, "coin", like, "like fits a recipe or a layered preset")
    await refusal(ed, "random", like, "like fits a recipe or a layered preset")
    await refusal(ed, "click", "assets/fit/none.wav", "like")
    await refusal(ed, "click", like, "leave params out", params={"freq_hz": 900})

    silent = ed.project / "assets/fit/silent.wav"
    ar.write_wav(silent, [ar.silence(0.3, RATE)], RATE)
    long = ed.project / "assets/fit/long.wav"
    ar.write_wav(long, [ar.white_noise(9.0, RATE)], RATE)
    ar.write_meta_section(long, "import", ["  audioImporter:", "    version: 1", "    type: impact"])
    loop = ed.project / "assets/fit/loop.wav"
    ar.write_wav(loop, [ar.sine(220.0, 1.0, RATE, -12.0)], RATE)
    ar.write_meta_section(loop, "import", ["  audioImporter:", "    version: 1", "    type: ambience"])
    await ed.call("sync_files")
    await refusal(ed, "click", "assets/fit/silent.wav", "silent")
    await refusal(ed, "click", "assets/fit/long.wav", "longer than the 8 s")
    await refusal(ed, "click", "assets/fit/loop.wav", "a fit shapes a one-shot sound")


async def test_one_undo_step(ed: Editor):
    await gen(ed, "ref_u", "pluck", seed=5)
    await gen(ed, "undone", "pluck", like="assets/fit/ref_u.wav")
    ed.check((ed.project / "assets/fit/undone.wav").exists(), "written")
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / "assets/fit/undone.wav").exists() and not (ed.project / "assets/fit/undone.wav.meta").exists(), "undo removes both")
