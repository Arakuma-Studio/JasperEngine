"""S36 AC6: generate_sound and its .meta. Every render fades in and out, so its first and last samples
are 0, and a seed reproduces its bytes.

The oracles are outside the editor: Python's wave module for the samples of the written WAVs and the
raised-cosine weights computed here, pyloudnorm's momentary max and sample peaks of the written WAVs, the
true peak by BS.1770-4 Annex 2's FIR in numpy (and scipy's 4x resampling as a sanity bound), all in one
`uv run --with numpy --with pyloudnorm --with scipy` subprocess, a Python xorshift32 for the preset
`random` picks, the RIFF chunks read here, and the metas read with PyYAML."""

from __future__ import annotations

import asyncio
import hashlib
import json
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

import yaml

from harness import Editor, editor_command

CI = True
EDITOR = {"create": True}

sys.path.insert(0, str(Path(__file__).parent))
import audio_refs as ar  # noqa: E402
from test_audio_analysis import check_image, near, note_ids  # noqa: E402
from test_runtime_regressions import decode_png  # noqa: E402

TIMEOUT = 1500
DIR = "assets/gen"

# spec 2.10: each preset's type
PRESET_TYPES = {
    "coin": "pickup", "laser": "shoot", "explosion": "explosion", "powerup": "pickup", "hurt": "hurt",
    "jump": "jump", "blip": "ui", "click": "click", "pluck": "ui", "bell": "chime", "whoosh": "whoosh",
    "riser": "riser", "thump": "thump",
}
PRESETS = list(PRESET_TYPES)
SFXR = {"coin", "laser", "explosion", "powerup", "hurt", "jump", "blip"}
# spec 2.9 (kind, LUFS-M target); click, explosion, hurt and shoot as lowered in spec 5.4
TYPES = {
    "pickup": ("tonal", -18.0), "shoot": ("transient", -21.0), "explosion": ("transient", -18.0),
    "hurt": ("transient", -24.0), "jump": ("tonal", -18.0), "ui": ("tonal", -22.0), "click": ("transient", -32.0),
    "chime": ("tonal", -20.0), "whoosh": ("swell", -18.0), "riser": ("riser", -16.0), "thump": ("transient", -16.0),
}


def xorshift32_pick(seed: int) -> str:
    """The preset `random` stands for (spec 2.10): the first xorshift32 output of the seed (0 maps
    to 0x9E3779B9), modulo 13, in spec 2.10's order."""
    x = seed if seed else 0x9E3779B9
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    return PRESETS[x % 13]


def sha1(path: Path) -> str:
    return hashlib.sha1(path.read_bytes()).hexdigest()


def chunks(path: Path) -> list:
    """(id, size) of every RIFF chunk."""
    raw = path.read_bytes()
    assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE", path
    out, pos = [], 12
    while pos + 8 <= len(raw):
        cid, size = struct.unpack("<4sI", raw[pos:pos + 8])
        out.append((cid.decode("latin-1"), size))
        pos += 8 + size + (size & 1)
    return out


_ORACLE = r"""
import contextlib, io, json, sys
import numpy as np
from scipy.signal import resample_poly
here = sys.argv[1]
paths = json.loads(sys.argv[2])
sys.path.insert(0, here)
import audio_refs
code = compile(audio_refs._PYLOUDNORM, "audio_refs._PYLOUDNORM", "exec")
# ITU-R BS.1770-4 Annex 2, as published
BS1770_FIR = [
    [0.0017089843750, 0.0109863281250, -0.0196533203125, 0.0332031250000, -0.0594482421875, 0.1373291015625,
     0.9721679687500, -0.1022949218750, 0.0476074218750, -0.0266113281250, 0.0148925781250, -0.0083007812500],
    [-0.0291748046875, 0.0292968750000, -0.0517578125000, 0.0891113281250, -0.1665039062500, 0.4650878906250,
     0.7797851562500, -0.2003173828125, 0.1015625000000, -0.0582275390625, 0.0330810546875, -0.0189208984375],
    [-0.0189208984375, 0.0330810546875, -0.0582275390625, 0.1015625000000, -0.2003173828125, 0.7797851562500,
     0.4650878906250, -0.1665039062500, 0.0891113281250, -0.0517578125000, 0.0292968750000, -0.0291748046875],
    [-0.0083007812500, 0.0148925781250, -0.0266113281250, 0.0476074218750, -0.1022949218750, 0.9721679687500,
     0.1373291015625, -0.0594482421875, 0.0332031250000, -0.0196533203125, 0.0109863281250, 0.0017089843750],
]
out = []
for path in paths:
    wav = audio_refs.read_wav(path)
    sys.argv = ["pyloudnorm", here, path, "file"]
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        exec(code, {"__name__": "__main__"})
    momentary = json.loads(buf.getvalue().strip().splitlines()[-1])
    x = np.array(wav.channels[0], dtype=np.float64)
    peak = float(np.max(np.abs(x)))
    # BS.1770-4 Annex 2: 4x oversampling by its 48-tap FIR (4 phases of 12), the input samples included
    flushed = np.concatenate([x, np.zeros(12)])
    fir = max(peak, max(float(np.max(np.abs(np.convolve(flushed, h)[:len(flushed)]))) for h in BS1770_FIR))
    padded = np.concatenate([np.zeros(64), x, np.zeros(64)])
    scipy_peak = max(peak, float(np.max(np.abs(resample_poly(padded, 4, 1)))))
    db = lambda v: 20 * np.log10(v) if v > 0 else -120.0
    out.append({"rate": wav.rate, "frames": wav.frames, "momentary_max": momentary,
                "sample_peak": float(db(peak)), "true_peak": float(db(fir)), "true_peak_scipy": float(db(scipy_peak))})
print(json.dumps(out))
"""


async def oracle(paths: list) -> list:
    """pyloudnorm momentary max (file mode), sample peak and a 4x-resampled true peak per WAV."""
    here = str(Path(ar.__file__).resolve().parent)

    def run():
        return subprocess.run([ar._uv(), "run", "--quiet", "--no-project", "--with", "numpy", "--with", "pyloudnorm",
                               "--with", "scipy", "python", "-c", _ORACLE, here, json.dumps([str(p) for p in paths])],
                              capture_output=True, text=True, timeout=1200)

    out = await asyncio.to_thread(run)
    if out.returncode != 0:
        raise RuntimeError(f"oracle failed: {out.stderr[-2000:]}")
    return json.loads(out.stdout.strip().splitlines()[-1])


async def generate(ed: Editor, path: str, preset: str, **args) -> dict:
    args.setdefault("image", "none")
    return await ed.call("generate_sound", path=path, preset=preset, **args)


def meta_of(ed: Editor, rel: str) -> dict:
    return yaml.safe_load((ed.project / (rel + ".meta")).read_text())


def sidecar(ed: Editor, rel: str) -> dict:
    """the clip's provenance section"""
    return meta_of(ed, rel)["provenance"]


# -- 3.6 every preset at its type's target ----------------------------------------------------------


async def test_presets_meet_their_targets(ed: Editor):
    ed.require_tools("generate_sound", "analyze_audio")
    files = []   # (preset, seed, rel, entry)
    for preset in PRESETS:
        r = await generate(ed, f"{DIR}/{preset}/{preset}.wav", preset, seed=1, variations=8)
        ed.check_eq(len(r["files"]), 8, f"{preset}: 8 variations")
        ed.check_eq((r["type"], r["licence"]), (PRESET_TYPES[preset], "generated"), f"{preset} type and licence")
        for i, entry in enumerate(r["files"]):
            ed.check_eq(entry["path"], f"{DIR}/{preset}/{preset}_{i + 1}.wav", f"{preset} variation path")
            ed.check_eq((entry["preset"], entry["seed"]), (preset, i + 1), f"{preset} variation {i + 1} seed")
            files.append((preset, i + 1, entry["path"], entry))

    for preset, seed, rel, entry in files:
        what = f"{preset} seed {seed}"
        kind, target = TYPES[PRESET_TYPES[preset]]
        item = entry["analysis"]
        ed.check(entry["limited_by"] is None, f"{what}: limited by {entry['limited_by']} (true peak {item['peak']['true_peak_dbtp']})")
        ed.check(item["peak"]["sample_dbfs"] > -40.0, f"{what}: silent ({item['peak']['sample_dbfs']} dBFS)")
        near(ed, item["loudness"]["momentary_max"], target, 1.0, f"{what} momentary max vs the {PRESET_TYPES[preset]} target")
        ed.check(item["peak"]["true_peak_dbtp"] <= -1.0, f"{what}: true peak {item['peak']['true_peak_dbtp']} over -1 dBTP")
        ed.check_eq((item["kind"], item["kind_source"], item["type"]), (kind, "type", PRESET_TYPES[preset]), f"{what} kind")
        ed.check_eq(item["format"]["sample_rate"], 44100 if preset in SFXR else 48000, f"{what} rate")
        ed.check_eq((item["format"]["channels"], item["format"]["encoding"]), (1, "pcm16"), f"{what} layout")
        ed.check_eq(item["licence"], {"id": "generated", "source": "generate_sound"}, f"{what} licence")

    # the independent measurement of the files as written
    measured = await oracle([ed.project / rel for _, _, rel, _ in files])
    spread = []
    for (preset, seed, rel, entry), m in zip(files, measured):
        what = f"{preset} seed {seed} (pyloudnorm/scipy)"
        target = TYPES[PRESET_TYPES[preset]][1]
        near(ed, m["momentary_max"], target, 1.0, f"{what} momentary max vs target")
        near(ed, m["momentary_max"], entry["analysis"]["loudness"]["momentary_max"], 0.2, f"{what} momentary max vs the tool")
        ed.check(m["sample_peak"] > -40.0, f"{what}: sample peak {m['sample_peak']}")
        near(ed, m["sample_peak"], entry["analysis"]["peak"]["sample_dbfs"], 0.02, f"{what} sample peak vs the tool")
        near(ed, m["true_peak"], entry["analysis"]["peak"]["true_peak_dbtp"], 0.02, f"{what} BS.1770 true peak vs the tool")
        ed.check(m["true_peak"] <= -0.99, f"{what}: true peak {m['true_peak']} over -1 dBTP")
        spread.append(m["true_peak_scipy"] - m["true_peak"])
    # another interpolator (scipy's polyphase Kaiser) reconstructs a little differently: a sanity bound
    print(f"     scipy 4x true peak minus BS.1770's: {min(spread):+.3f} .. {max(spread):+.3f} dB")
    ed.check(max(abs(d) for d in spread) < 0.6, f"scipy and BS.1770 true peaks agree within 0.6 dB: {min(spread)} .. {max(spread)}")

    # random: one of the 13 by xorshift32(seed) % 13, generated with the same seed
    for seed in range(1, 9):
        expected = xorshift32_pick(seed)
        r = await generate(ed, f"{DIR}/random/random_{seed}.wav", "random", seed=seed)
        entry = r["files"][0]
        ed.check_eq((entry["preset"], r["type"]), (expected, PRESET_TYPES[expected]), f"random seed {seed} picks {expected}")
        kind, target = TYPES[PRESET_TYPES[expected]]
        ed.check(entry["limited_by"] is None and entry["analysis"]["peak"]["true_peak_dbtp"] <= -1.0, f"random seed {seed} peak")
        near(ed, entry["analysis"]["loudness"]["momentary_max"], target, 1.0, f"random seed {seed} momentary max")
        ed.check_eq((entry["analysis"]["kind"], entry["analysis"]["kind_source"]), (kind, "type"), f"random seed {seed} kind")
        ed.check_eq(sha1(ed.project / entry["path"]), sha1(ed.project / f"{DIR}/{expected}/{expected}_{seed}.wav"),
                    f"random seed {seed} equals {expected} seed {seed}")
        ed.check_eq(sidecar(ed, entry["path"])["generator"]["preset"], expected, "the sidecar names the picked preset")


# -- determinism ---------------------------------------------------------------------------------------


async def test_determinism_and_variations(ed: Editor):
    first = await generate(ed, f"{DIR}/det/jump.wav", "jump", seed=7)
    again = await generate(ed, f"{DIR}/det/jump.wav", "jump", seed=7, overwrite=True)
    ed.check_eq(again["files"][0]["gain_db"], first["files"][0]["gain_db"], "same gain")
    jump = sha1(ed.project / f"{DIR}/det/jump.wav")
    await generate(ed, f"{DIR}/det/bell.wav", "bell", seed=7, params={"freq_hz": 660, "brightness": 0.8})
    bell = sha1(ed.project / f"{DIR}/det/bell.wav")
    await generate(ed, f"{DIR}/det/jump_b.wav", "jump", seed=7)
    ed.check_eq(sha1(ed.project / f"{DIR}/det/jump_b.wav"), jump, "same preset and seed: same bytes")
    await generate(ed, f"{DIR}/det/jump_8.wav", "jump", seed=8)
    ed.check(sha1(ed.project / f"{DIR}/det/jump_8.wav") != jump, "another seed: other bytes")
    await generate(ed, f"{DIR}/det/bell_8.wav", "bell", seed=8, params={"freq_hz": 660, "brightness": 0.8})
    ed.check(sha1(ed.project / f"{DIR}/det/bell_8.wav") != bell, "another recipe seed: other bytes")

    # variations 3 = single calls with seeds s, s+1, s+2 (sfxr and a recipe)
    for preset in ("coin", "pluck"):
        r = await generate(ed, f"{DIR}/var/{preset}.wav", preset, seed=100, variations=3)
        ed.check_eq([f["path"] for f in r["files"]], [f"{DIR}/var/{preset}_{i}.wav" for i in (1, 2, 3)], f"{preset} names")
        ed.check(not (ed.project / f"{DIR}/var/{preset}.wav").exists(), "variations do not write path itself")
        for i in range(3):
            await generate(ed, f"{DIR}/var/{preset}_single_{i}.wav", preset, seed=100 + i)
            ed.check_eq(sha1(ed.project / f"{DIR}/var/{preset}_{i + 1}.wav"), sha1(ed.project / f"{DIR}/var/{preset}_single_{i}.wav"),
                        f"{preset} variation {i + 1} equals seed {100 + i}")
        hashes = {sha1(ed.project / f"{DIR}/var/{preset}_{i}.wav") for i in (1, 2, 3)}
        ed.check_eq(len(hashes), 3, f"{preset} variations differ")

    # an editor restart (another process on a fresh copy of the project) gives the same bytes
    async with Editor(create=True) as other:
        await generate(other, f"{DIR}/det/jump.wav", "jump", seed=7)
        await generate(other, f"{DIR}/det/bell.wav", "bell", seed=7, params={"freq_hz": 660, "brightness": 0.8})
        ed.check_eq(sha1(other.project / f"{DIR}/det/jump.wav"), jump, "jump seed 7 after a restart")
        ed.check_eq(sha1(other.project / f"{DIR}/det/bell.wav"), bell, "bell seed 7 after a restart")

    # a seed omitted is random and reported; the sidecar can reproduce it
    r = await generate(ed, f"{DIR}/det/free.wav", "thump")
    seed = r["files"][0]["seed"]
    ed.check(isinstance(seed, int) and 0 <= seed <= 2147483647, f"reported seed {seed}")
    ed.check_eq(sidecar(ed, f"{DIR}/det/free.wav")["generator"]["seed"], seed, "the sidecar has the seed")
    await generate(ed, f"{DIR}/det/free_again.wav", "thump", seed=seed)
    ed.check_eq(sha1(ed.project / f"{DIR}/det/free_again.wav"), sha1(ed.project / f"{DIR}/det/free.wav"), "reported seed reproduces")


# -- explicit loudness ---------------------------------------------------------------------------------


async def test_explicit_loudness(ed: Editor):
    r = await generate(ed, f"{DIR}/loud/quiet.wav", "bell", seed=2, loudness_lufs=-30)
    entry = r["files"][0]
    ed.check(entry["limited_by"] is None, "-30 needs no limit")
    near(ed, entry["analysis"]["loudness"]["momentary_max"], -30.0, 0.5, "loudness_lufs -30")
    ed.check_eq(sidecar(ed, entry["path"])["generator"]["loudness_lufs"], -30, "the sidecar keeps the target")

    r = await generate(ed, f"{DIR}/loud/hot.wav", "explosion", seed=1, loudness_lufs=-6)
    entry = r["files"][0]
    ed.check_eq(entry["limited_by"], "true_peak", "-6 LUFS-M needs more than -1 dBTP of headroom")
    near(ed, entry["analysis"]["peak"]["true_peak_dbtp"], -1.0, 0.05, "held at -1 dBTP")
    ed.check(entry["analysis"]["loudness"]["momentary_max"] < -6.5, "under the target it could not reach")
    ed.check_in("-1 dBTP", (await ed.call_raw("generate_sound", {"path": f"{DIR}/loud/hot2.wav", "preset": "explosion", "seed": 1,
                                                                   "loudness_lufs": -6, "image": "none"})).text, "the text says so")
    m = (await oracle([ed.project / entry["path"], ed.project / f"{DIR}/loud/quiet.wav"]))
    near(ed, m[0]["true_peak"], -1.0, 0.05, "the limited file's true peak (BS.1770 FIR in numpy)")
    near(ed, m[1]["momentary_max"], -30.0, 0.5, "-30 by pyloudnorm")


# -- refusals, files, sidecars, undo ---------------------------------------------------------------------


async def test_refusals_files_and_undo(ed: Editor):
    ed.require_tools("undo", "redo")
    err = await ed.call("generate_sound", path=f"{DIR}/bad/a.wav", preset="jump", params={"p_nonsense": 1}, expect_error=True)
    ed.check("p_base_freq" in err and "p_env_decay" in err, f"unknown sfxr field lists the valid ones: {err}")
    err = await ed.call("generate_sound", path=f"{DIR}/bad/a.wav", preset="jump", params={"p_base_freq": 2}, expect_error=True)
    ed.check("0 to 1" in err, f"out of range names the range: {err}")
    err = await ed.call("generate_sound", path=f"{DIR}/bad/a.wav", preset="click", params={"freq": 1000}, expect_error=True)
    ed.check("freq_hz" in err and "decay_ms" in err and "length_ms" in err, f"unknown recipe param lists the valid ones: {err}")
    err = await ed.call("generate_sound", path=f"{DIR}/bad/a.wav", preset="click", params={"freq_hz": 100}, expect_error=True)
    ed.check("500 to 12000" in err, f"recipe range: {err}")
    err = await ed.call("generate_sound", path=f"{DIR}/bad/a.wav", preset="jump", params={"wave_type": 1.5}, expect_error=True)
    ed.check("whole number" in err, f"wave_type is an integer: {err}")
    await ed.call("generate_sound", path=f"{DIR}/bad/a.wav", preset="random", seed=1, params={"freq_hz": 500}, expect_error=True)
    await ed.call("generate_sound", path=f"{DIR}/bad/a.wav", preset="pluck", loudness_lufs=-3, expect_error=True)
    await ed.call("generate_sound", path=f"{DIR}/bad/a.wav", preset="jump", variations=9, expect_error=True)
    await ed.call("generate_sound", path=f"{DIR}/bad/a.wav", preset="jump", variations=3, image="both", expect_error=True)
    err = await ed.call("generate_sound", path="../outside.wav", preset="jump", expect_error=True)
    ed.check(not (ed.project.parent / "outside.wav").exists(), f"nothing written outside: {err}")
    ed.check(not (ed.project / DIR / "bad").exists(), "refusals write nothing")

    # a path without .wav gets it; an existing file needs overwrite
    r = await generate(ed, f"{DIR}/files/hit", "thump", seed=5, variations=3)
    names = [f"{DIR}/files/hit_{i}.wav" for i in (1, 2, 3)]
    ed.check_eq([f["path"] for f in r["files"]], names, ".wav appended")
    err = await ed.call("generate_sound", path=f"{DIR}/files/hit.wav", preset="thump", seed=5, variations=3, expect_error=True)
    ed.check_in("overwrite", err, "an existing file is refused")

    for i, name in enumerate(names):
        path = ed.project / name
        ed.check_eq(chunks(path), [("fmt ", 16), ("data", path.stat().st_size - 44)], f"{name}: fmt and data only")
        fmt = struct.unpack("<HHIIHH", path.read_bytes()[20:36])
        ed.check_eq(fmt, (1, 1, 48000, 96000, 2, 16), f"{name}: PCM16 mono 48 kHz")
        meta = meta_of(ed, name)
        ed.check(len(meta["guid"]) == 32, f"{name} has its GUID")
        ed.check_eq(meta["import"]["audioImporter"], {"version": 1, "type": "thump"}, f"{name} type")
        ed.check_eq((meta["provenance"]["licence"], meta["provenance"]["source"]), ("generated", "generate_sound"), f"{name} licence")
        ed.check_eq(meta["provenance"]["generator"], {"preset": "thump", "seed": 5 + i, "params": {}, "loudness_lufs": -16, "version": 1},
                    f"{name} generator")
    sfxr = await generate(ed, f"{DIR}/files/coin.wav", "coin", seed=3)
    fmt = struct.unpack("<HHIIHH", (ed.project / sfxr["files"][0]["path"]).read_bytes()[20:36])
    ed.check_eq(fmt[:3], (1, 1, 44100), "sfxr renders at 44.1 kHz")

    # all files and sidecars of one call are one undo step
    await ed.call("undo", scope="project")   # the coin
    ed.check(not (ed.project / f"{DIR}/files/coin.wav").exists(), "undo removes the coin")
    await ed.call("undo", scope="project")
    for name in names:
        ed.check(not (ed.project / name).exists() and not (ed.project / (name + ".meta")).exists(), f"undo removes {name} and its .meta")
    ed.check(not (ed.project / f"{DIR}/files").exists(), "and the folder it created")
    await ed.call("redo", scope="project")
    for name in names:
        ed.check((ed.project / name).exists() and (ed.project / (name + ".meta")).exists(), f"redo restores {name}")


# -- the .meta keeps what it does not own ----------------------------------------------------------------


async def test_overwrite_keeps_the_rest_of_the_meta(ed: Editor):
    rel = f"{DIR}/keep/step.wav"
    await generate(ed, rel, "click", seed=3)
    path = ed.project / (rel + ".meta")
    first = yaml.safe_load(path.read_text())
    path.write_text(path.read_text() + "thirdparty:\n    quoted: 'as I wrote it'   # a comment\n")
    await ed.call("set_audio_import_settings", paths=[rel], settings={"fade_in_ms": 2})
    await generate(ed, rel, "click", seed=4, overwrite=True, params={"decay_ms": 3})
    text = path.read_text()
    meta = yaml.safe_load(text)
    ed.check_eq(meta["guid"], first["guid"], "the GUID stays")
    ed.check_eq(meta["provenance"]["generator"]["seed"], 4, "the generator is rewritten")
    ed.check_eq(meta["provenance"]["generator"]["params"], {"decay_ms": 3}, "with its params")
    ed.check_eq(meta["import"]["audioImporter"]["fade_in_ms"], 2, "the other import settings stay")
    ed.check("    quoted: 'as I wrote it'   # a comment\n" in text, f"a section it does not own stays byte for byte:\n{text}")
    info = await ed.call("get_asset_info", path=rel)
    ed.check_eq((info["audio"]["import"]["type"], info["audio"]["provenance"]["licence"]), ("click", "generated"), "get_asset_info")
    ed.check_eq(info["audio"]["analysis"]["kind"], "transient", "get_asset_info's kind from the type")


async def test_player_has_no_sfxr(ed: Editor):
    """R14: sfxr compiles into the editor target only."""
    repo = Path(__file__).resolve().parents[3]
    listed = (repo / "engine/libs/soloud/CMakeLists.txt").read_text()
    ed.check("soloud_sfxr.cpp" not in listed, "the engine's SoLoud does not compile sfxr")
    ed.check("soloud_sfxr.cpp" in (repo / "CMakeLists.txt").read_text(), "the editor does")
    # and what the engine library links has no Sfxr symbol
    engine = Path(editor_command()[0]).resolve().parent / "libjasperengine.so"
    ed.check(engine.exists(), f"{engine} is the install's engine")
    symbols = subprocess.run(["nm", "-DC", str(engine)], capture_output=True, text=True).stdout
    ed.check(symbols and "Sfxr" not in symbols, "no Sfxr in the player's engine")


# -- 3.2 images ------------------------------------------------------------------------------------------


async def test_images(ed: Editor):
    raw = await ed.call_raw("generate_sound", {"path": f"{DIR}/img/one.wav", "preset": "riser", "seed": 9})
    ed.check(not raw.is_error, raw.text)
    check_image(ed, raw, 768, [16, 128, 160, 16], "generate_sound both")
    ed.check_in("riser", raw.text, "text names the preset")
    raw = await ed.call_raw("generate_sound", {"path": f"{DIR}/img/many.wav", "preset": "whoosh", "seed": 9, "variations": 5})
    ed.check(not raw.is_error, raw.text)
    data, mime = raw.images[0]
    w, h, _, _ = decode_png(data)
    tile = (768 - 8 * 5) // 4
    ed.check_eq((w, h), (768, 8 + 2 * (14 + 2 * round(tile / 4) + 8)), "a sheet of 5")
    ed.check_eq(raw.structured["image"]["mode"], "sheet", "variations default to a sheet")
    raw = await ed.call_raw("generate_sound", {"path": f"{DIR}/img/none.wav", "preset": "pluck", "seed": 9, "image": "none"})
    ed.check(not raw.images and raw.structured["image"] is None, "image none")
    item = raw.structured["files"][0]["analysis"]
    ed.check("head_silence" not in note_ids(item) and "clipping" not in note_ids(item), f"a clean placeholder: {item['notes']!r}")


# -- phase 2.5 spec 3.5: every render starts and ends at 0 (G-1) -------------------------------------------

# the recipes' parameters (spec 2.10's table): name -> (min, max)
RECIPE_PARAMS = {
    "click": {"freq_hz": (500, 12000), "decay_ms": (0.2, 20), "length_ms": (5, 200)},
    "pluck": {"freq_hz": (40, 2000), "damping": (0.9, 0.9999), "brightness": (0, 1), "length_ms": (50, 4000)},
    "bell": {"freq_hz": (100, 4000), "brightness": (0, 1), "length_ms": (100, 5000)},
    "whoosh": {"low_hz": (100, 2000), "high_hz": (500, 8000), "q": (0.3, 4), "length_ms": (100, 3000)},
    "riser": {"start_hz": (50, 2000), "end_hz": (200, 8000), "noise": (0, 1), "length_ms": (300, 8000)},
    "thump": {"freq_hz": (30, 150), "drop_hz": (0, 200), "decay_ms": (20, 1000), "click": (0, 1), "length_ms": (100, 2000)},
}
SEEDS = 8


# phase 2.5 spec 5.4: the fade-in takes at most 0.05 dB of the energy of the first 400 ms
FADE_IN_MAX_LOSS_DB = 0.05
ATTACK_WINDOW_SECONDS = 0.4


def fade_in_frames(rate: int) -> int:
    """Phase 2.5 spec 2.9: the longest fade-in, max(2, round(0.0005 x rate)): 24 at 48 kHz, 22 at
    44.1 kHz."""
    return max(2, math.floor(0.0005 * rate + 0.5))


def chosen_fade_in(u: list, rate: int) -> tuple:
    """Phase 2.5 spec 5.4's rule on an unfaded signal u: the longest fade-in from fade_in_frames
    down to 3 frames whose raised cosine takes at most FADE_IN_MAX_LOSS_DB of the energy of u's
    first 400 ms, else 0: the 2-frame lead-in. Returns (frames, the loss of that choice and of the
    next longer one, as shares of the allowed loss)."""
    window = min(len(u), math.floor(ATTACK_WINDOW_SECONDS * rate + 0.5))
    allowed = sum(v * v for v in u[:window]) * (1.0 - 10 ** (-FADE_IN_MAX_LOSS_DB / 10))

    def loss(n):
        return sum(u[k] ** 2 * (1.0 - fade_in_weight(k, n) ** 2) for k in range(n))

    longest = min(len(u), fade_in_frames(rate))
    for n in range(longest, 2, -1):
        if loss(n) <= allowed:
            return n, loss(n) / allowed, (loss(n + 1) / allowed if n < longest else None)
    return 0, 0.0, loss(3) / allowed


def fade_out_frames(frames: int, rate: int, max_ms: float = 50.0) -> int:
    """The fade-out over a render's own frames: 10% of them, at least 1, at most 50 ms for a recipe
    (Phase 1's) and at most its kind's click-safe length for sfxr (phase 2.5 spec 5.4)."""
    return min(frames, max(1, math.floor(0.1 * frames + 0.5)), max(1, math.floor(max_ms / 1000 * rate + 0.5)))


def fade_out_ms(preset: str) -> float:
    """Phase 2.5 spec 5.4: a recipe's fade-out is at most 50 ms; an sfxr render's at most its type's
    click-safe length (5 ms for a transient, 12 ms for a tonal sound)."""
    if preset not in SFXR:
        return 50.0
    return 5.0 if TYPES[PRESET_TYPES[preset]][0] == "transient" else 12.0


def fade_in_weight(k: int, n: int) -> float:
    """Phase 2.5 spec 2.5.1: frame k of an n-frame fade-in."""
    return 0.5 * (1.0 - math.cos(math.pi * k / n))


def fade_out_weight(j: int, n: int) -> float:
    """Phase 2.5 spec 2.5.1: frame j of the last n (j = 0 the first of them)."""
    return 0.5 * (1.0 + math.cos(math.pi * (j + 1) / n))


def pcm16_samples(path: Path) -> tuple:
    """(rate, samples) of a mono PCM16 WAV, read with Python's wave module."""
    with wave.open(str(path), "rb") as w:
        assert (w.getnchannels(), w.getsampwidth()) == (1, 2), path
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    return rate, list(struct.unpack(f"<{len(raw) // 2}h", raw))


def render_cases() -> list:
    """(preset, label, params): every preset at its default params, then every recipe parameter
    at its minimum and its maximum, the others at their defaults (G-1)."""
    cases = [(preset, "default", {}) for preset in PRESETS]
    for preset, params in RECIPE_PARAMS.items():
        for name, (low, high) in params.items():
            cases.append((preset, f"{name}_min", {name: low}))
            cases.append((preset, f"{name}_max", {name: high}))
    return cases


def check_edges(ed: Editor, path: Path, what: str) -> None:
    """G-1 on one file: the first and last samples are exactly 0. (The ramps themselves, and the
    fade-in's length, are checked on renders whose unfaded signal is known, test_fade_ramps: a
    render's own peak does not bound its unfaded samples inside a fade, e.g. sfxr's punch makes coin
    loudest at its start.)"""
    rate, x = pcm16_samples(path)
    ed.check(rate in (44100, 48000) and len(x) >= 2 and max(abs(v) for v in x) > 0, f"{what}: {rate} Hz, {len(x)} frames")
    ed.check_eq((x[0], x[-1]), (0, 0), f"{what}: first and last samples")


# phase 2.5 spec 3.5 G-2: no render gets these; the other new notes are recorded 
CLEAN_NOTES = ("edge_click", "clicks", "silence_gap")
NEW_NOTES = ("truncated_tail", "harsh_resonance", "aliasing", "trim_cut")


async def test_every_render_fades_in_and_out(ed: Editor):
    """G-1 edges, G-2 notes and G-3's version on every preset at seeds 1-8, at default params and
    at every recipe parameter's minimum and maximum."""
    ed.require_tools("generate_sound")
    count = 0
    others = {}   # (preset, label) -> {note id: seeds}
    sfxr_own = {}   # note -> renders
    for preset, label, params in render_cases():
        rel = f"{DIR}/fades/{preset}/{label}.wav"
        r = await generate(ed, rel, preset, seed=1, variations=SEEDS, params=params)
        ed.check_eq([(f["preset"], f["seed"]) for f in r["files"]], [(preset, s) for s in range(1, SEEDS + 1)],
                    f"{preset} {label}: seeds 1-{SEEDS}")
        ed.check_eq(sidecar(ed, r["files"][0]["path"])["generator"]["version"], 1, f"{preset} {label}: generator.version 1")
        for entry in r["files"]:
            what = f"{preset} {label} seed {entry['seed']}"
            check_edges(ed, ed.project / entry["path"], what)
            item = entry["analysis"]
            defects = item.get("defects")
            ed.check(isinstance(defects, dict) and defects.get("edges") is not None, f"{what}: the item has defects: {defects!r}")
            ed.check_eq((defects["edges"]["start_click"], defects["edges"]["end_click"]), (False, False), f"{what}: edges {defects['edges']}")
            ids = note_ids(item)
            ed.check("edge_click" not in ids, f"{what}: no edge_click: {item['notes']!r}")
            # sfxr's noise and duty sweeps make steps and digital silence of their own ; a fade can make neither, so any there must lie
            # clear of both fades, and the recipes have none
            if preset in SFXR:
                rate, frames = pcm16_samples(ed.project / entry["path"])
                n = len(frames)
                fade_out_start = (n - fade_out_frames(n, rate, fade_out_ms(preset))) / rate
                for note, times in (("clicks", defects["clicks"]["times_s"]), ("silence_gap", defects["gaps"]["times_s"])):
                    if note in ids:
                        sfxr_own.setdefault(note, []).append(f"{preset} {label} {entry['seed']}")
                        ed.check(times and all(0.002 < t < fade_out_start - 0.002 for t in times),
                                 f"{what}: {note} at {times} s, clear of the fades (fade-out from {fade_out_start:.4f} s)")
            else:
                ed.check(not [n for n in ids if n in CLEAN_NOTES], f"{what}: none of {CLEAN_NOTES}: {item['notes']!r}")
            for n in ids:
                if n in NEW_NOTES:
                    others.setdefault((preset, label), {}).setdefault(n, []).append(entry["seed"])
            count += 1
    print(f"     {count} renders start and end at 0, none with edge_click; sfxr's own clicks and gaps: {sfxr_own}")
    for (preset, label), notes in others.items():
        print(f"     {preset} {label}: " + "; ".join(f"{n} at seeds {seeds}" for n, seeds in notes.items()))


# sfxr squares at 3.5 Hz (p_base_freq 0: a period of 12,500 frames) with an instant attack, a
# sustain and no decay, no slide, vibrato, arpeggio, repeat, phaser or filter: the unfaded signal is
# one level, moving only by sfxr's high-pass leak (a geometric decay of 8e-5 a frame) and flipping
# sign once at frame 6,250, and its last frame is 0 (the decay stage of 0 frames). So both ramps are
# seen whole, and the fade-in's length follows from the sustain: p_env_sustain 0.3 (9,000 frames)
# keeps the whole 22 frames at 44.1 kHz, 0.08 (639 frames) a shorter one (10 frames), and 0.03 (90
# frames, a click's energy in its first 2 ms) the 2-frame lead-in. blip is a ui sound (tonal): its
# fade-out is at most 12 ms (529 frames).
FLAT_SQUARE = {
    "wave_type": 0, "p_base_freq": 0, "p_freq_limit": 0, "p_freq_ramp": 0, "p_freq_dramp": 0, "p_duty": 0,
    "p_duty_ramp": 0, "p_vib_strength": 0, "p_vib_speed": 0, "p_vib_delay": 0, "p_env_attack": 0,
    "p_env_sustain": 0.3, "p_env_decay": 0, "p_env_punch": 0, "filter_on": False, "p_lpf_resonance": 0,
    "p_lpf_freq": 1, "p_lpf_ramp": 0, "p_hpf_freq": 0, "p_hpf_ramp": 0, "p_pha_offset": 0, "p_pha_ramp": 0,
    "p_repeat_speed": 0, "p_arp_speed": 0, "p_arp_mod": 0, "master_vol": 0.05, "sound_vol": 0.5,
}
FLAT_SUSTAINS = {0.3: "whole", 0.08: "shorter", 0.03: "lead-in"}


def geometric_fit(x: list, first: int, last: int):
    """The level a * q^k fitted (least squares on log |x|) over frames first..last-1, as a function."""
    ks = list(range(first, last))
    ys = [math.log(abs(x[k])) for k in ks]
    mk, my = sum(ks) / len(ks), sum(ys) / len(ys)
    slope = sum((k - mk) * (y - my) for k, y in zip(ks, ys)) / sum((k - mk) ** 2 for k in ks)
    return lambda k: math.exp(my + slope * (k - mk))


async def test_fade_ramps(ed: Editor):
    """G-1's ramps: every frame of either fade (or of the lead-in) is the weight of phase 2.5 spec
    2.5.1 times the unfaded level (extrapolated from the frames next to the fade), within one PCM16
    step, and the fade-in's length is spec 5.4's rule applied here to that unfaded level."""
    for sustain, case in FLAT_SUSTAINS.items():
        r = await generate(ed, f"{DIR}/ramps/square_{case}.wav", "blip", seed=1, params=dict(FLAT_SQUARE, p_env_sustain=sustain))
        entry = r["files"][0]
        what = f"flat square, sustain {sustain}"
        rate, x = pcm16_samples(ed.project / entry["path"])
        n = len(x)
        lead = 2 if case == "lead-in" else 0
        longest, fout = fade_in_frames(rate), fade_out_frames(n - lead, rate, fade_out_ms("blip"))
        start = n - fout
        ed.check_eq((rate, longest), (44100, 22), f"{what}: rate and the longest fade-in")
        ed.check(n < 6250 or n > 6250 + 400 + fout, f"{what}: the sign flip at 6,250 lies outside the fades and their fits ({n} frames)")
        fit_in = (longest, min(longest + 200, start))
        fit_out = (max(longest, start - 400), start)
        for a, b in (fit_in, fit_out):
            signs = {(v > 0) - (v < 0) for v in x[a:b]}
            ed.check(len(signs) == 1 and 0 not in signs, f"{what}: one sign over frames {a}-{b - 1}: {signs}")
        # the unfaded level, in the file's frames: before the fade-out from the frames after the
        # longest fade-in, inside it from the frames before it (the render's last frame is sfxr's 0)
        level_in, level_out = geometric_fit(x, *fit_in), geometric_fit(x, *fit_out)
        unfaded = [level_in(k) if k < start else level_out(k) for k in range(n - 1)] + [0.0]
        fin, margin, longer = chosen_fade_in(unfaded[lead:], rate)
        ed.check(margin <= 0.99, f"{what}: the rule's choice {fin} is clear of its limit ({margin:.3f})")
        ed.check(longer is None or longer >= 1.01, f"{what}: the next longer fade-in is clearly over the limit ({longer})")
        expected = {"whole": lambda f: f == 22, "shorter": lambda f: 2 < f < 22, "lead-in": lambda f: f == 0}[case]
        ed.check(expected(fin), f"{what}: the rule gives a {case} fade-in ({fin} frames)")
        if fin:
            ramp = [fade_in_weight(k, fin) * unfaded[k] for k in range(fin)]
        else:
            ramp = [0.0, 0.5 * unfaded[2]]
        ramp_len = len(ramp)
        worst_in = max(abs(abs(x[k]) - ramp[k]) for k in range(ramp_len))
        ed.check(x[0] == 0 and worst_in <= 1.0, f"{what}: the first {ramp_len} frames off the ramp by up to {worst_in:.2f} steps: {x[:longest]}")
        worst_mid = max((abs(abs(x[k]) - unfaded[k]) for k in range(ramp_len, longest)), default=0.0)
        ed.check(worst_mid <= 1.0, f"{what}: frames {ramp_len}-{longest - 1} unfaded (off by {worst_mid:.2f} steps)")
        worst = max(abs(abs(x[start + j]) - fade_out_weight(j, fout) * unfaded[start + j]) for j in range(fout))
        ed.check(worst <= 1.0, f"{what}: fade-out frames off the raised cosine by up to {worst:.2f} steps: {x[start:start + 8]} .. {x[-8:]}")
        print(f"     {what}: {n} frames, peak {max(abs(v) for v in x)}, limited by {entry['limited_by']}: "
              f"{f'fade-in {fin} frames' if fin else 'the 2-frame lead-in'} (loss {margin:.3f} of the allowed, the next longer "
              f"{longer if longer is None else round(longer, 3)}), fade-out {fout} frames, on the ramps within {worst_in:.2f} "
              f"and {worst:.2f} PCM16 steps")
