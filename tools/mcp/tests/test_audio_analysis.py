"""S36 AC1, AC2: analyze_audio, get_asset_info for audio, AudioMeter calibration, images and the
analysis cache (and the essentials manifest's agreement).

Every expected value comes from outside the editor: signals built here or in audio_refs.py with
known levels, times and frequencies; pyloudnorm (run in a `uv run --with numpy --with pyloudnorm`
subprocess, as audio_refs does); ffmpeg for the compressed formats; soundfile and pyloudnorm for
the essentials manifest's `measured` values. Capture input is tested with session directories
written here in S35's format (master.wav float32 with a fact chunk, session.json, events.jsonl)."""

from __future__ import annotations

import array
import asyncio
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

from harness import Editor, skip

CI = True
EDITOR = {"create": True}

sys.path.insert(0, str(Path(__file__).parent))
import audio_refs as ar  # noqa: E402
from test_runtime_regressions import decode_png  # noqa: E402

TIMEOUT = 900
RATE = 48000
DIR = "assets/analysis"
REPO = Path(__file__).resolve().parents[3]
ESSENTIALS = REPO / "editor/resources/sound-library/essentials"
# AudioAnalysis::VERSION
ANALYZER_VERSION = 1


# -- signals and files ------------------------------------------------------------------------------


def write_float(path: Path, channels: list, rate: int) -> Path:
    """A float32 WAV (fmt and data only), fast: the long EBU signals have millions of samples."""
    frames = len(channels[0])
    if len(channels) == 1:
        data = array.array("f", channels[0])
    else:
        data = array.array("f", [0.0]) * (frames * len(channels))
        for c, samples in enumerate(channels):
            data[c::len(channels)] = array.array("f", samples)
    if sys.byteorder != "little":
        data.byteswap()
    raw = data.tobytes()
    count = len(channels)
    header = struct.pack("<4sI4s", b"RIFF", 4 + 24 + 8 + len(raw), b"WAVE")
    header += struct.pack("<4sIHHIIHH", b"fmt ", 16, 3, count, rate, rate * count * 4, count * 4, 32)
    header += struct.pack("<4sI", b"data", len(raw))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + raw)
    return path


def capture_wav(channels: list, rate: int, flushed_frames: int | None = None) -> bytes:
    """master.wav as spec 2.4.2 writes it: fmt (IEEE float, 16 bytes), fact, data, interleaved
    float32. flushed_frames < the frames given leaves the header at an earlier flush."""
    frames = len(channels[0])
    count = len(channels)
    data = array.array("f", [0.0]) * (frames * count)
    for c, samples in enumerate(channels):
        data[c::count] = array.array("f", samples)
    if sys.byteorder != "little":
        data.byteswap()
    raw = data.tobytes()
    told = frames if flushed_frames is None else flushed_frames
    size = told * count * 4
    header = struct.pack("<4sI4s", b"RIFF", 4 + 24 + 12 + 8 + size, b"WAVE")
    header += struct.pack("<4sIHHIIHH", b"fmt ", 16, 3, count, rate, rate * count * 4, count * 4, 32)
    header += struct.pack("<4sII", b"fact", 4, told)
    header += struct.pack("<4sI", b"data", size)
    return header + raw


def ramp_decay(rate: int) -> list:
    """A 1 kHz tone: a 10 ms linear ramp to 0.5, then a 200 ms linear decay to 0."""
    rise, fall = int(0.010 * rate), int(0.200 * rate)
    out = []
    for i in range(rise + fall):
        env = 0.5 * (i / rise if i < rise else 1.0 - (i - rise) / fall)
        out.append(env * math.sin(2 * math.pi * 1000.0 * i / rate))
    return out


def pinkish_noise(seconds: float, rate: int, seed: int, rms_db: float = -20.0) -> list:
    """White noise through Paul Kellet's economy pink filter (-3 dB/octave from about 40 Hz),
    scaled to an RMS level."""
    white = ar.white_noise(seconds, rate, seed, 0.0)
    b0 = b1 = b2 = 0.0
    out = []
    for w in white:
        b0 = 0.99765 * b0 + w * 0.0990460
        b1 = 0.96300 * b1 + w * 0.2965164
        b2 = 0.57000 * b2 + w * 1.0526913
        out.append(b0 + b1 + b2 + w * 0.1848)
    scale = ar.db_to_amplitude(rms_db) / ar.rms(out)
    return [x * scale for x in out]


def chirp_jump(rate: int) -> list:
    """A jump-like placeholder: a sine sweeping 300 -> 900 Hz over 0.3 s with a 5 ms attack and
    an exponential decay."""
    n = int(0.3 * rate)
    out, phase = [], 0.0
    for i in range(n):
        t = i / rate
        freq = 300.0 * (3.0 ** (t / 0.3))
        phase += 2 * math.pi * freq / rate
        env = min(1.0, t / 0.005) * math.exp(-t / 0.12)
        out.append(0.6 * env * math.sin(phase))
    return out


# -- oracles in subprocesses --------------------------------------------------------------------------


_PYLOUDNORM_BATCH = r"""
import contextlib, io, json, sys
here = sys.argv[1]
jobs = json.loads(sys.argv[2])
sys.path.insert(0, here)
import audio_refs
code = compile(audio_refs._PYLOUDNORM, "audio_refs._PYLOUDNORM", "exec")
out = []
for path, mode in jobs:
    sys.argv = ["pyloudnorm", here, path, mode]
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        exec(code, {"__name__": "__main__"})
    out.append(json.loads(buf.getvalue().strip().splitlines()[-1]))
print(json.dumps(out))
"""


async def pyloudnorm(jobs: list) -> list:
    """audio_refs' pyloudnorm measurements ("integrated", "file" or "stream" momentary max) of
    many WAVs in one subprocess."""
    here = str(Path(ar.__file__).resolve().parent)

    def run():
        return subprocess.run([ar._uv(), "run", "--quiet", "--no-project", "--with", "numpy", "--with", "pyloudnorm",
                               "python", "-c", _PYLOUDNORM_BATCH, here, json.dumps([[str(p), m] for p, m in jobs])],
                              capture_output=True, text=True, timeout=1200)

    out = await asyncio.to_thread(run)
    if out.returncode != 0:
        raise RuntimeError(f"pyloudnorm failed: {out.stderr[-2000:]}")
    return json.loads(out.stdout.strip().splitlines()[-1])


_BEDS = r"""
import json, struct, sys
import numpy as np
out_dir, rate, seconds, seeds = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), json.loads(sys.argv[4])
n = int(round(rate * seconds))
freqs = np.fft.rfftfreq(n, 1.0 / rate)
band = (freqs >= 30.0) & (freqs <= 16000.0)
for colour in ("white", "pink"):
    for seed in seeds:
        rng = np.random.default_rng(seed)
        mag = np.where(band, 1.0, 0.0)
        if colour == "pink":
            mag = np.where(band, 1.0 / np.sqrt(np.maximum(freqs, 1.0)), 0.0)
        spectrum = mag * np.exp(1j * rng.uniform(0, 2 * np.pi, len(freqs)))
        x = np.fft.irfft(spectrum, n)   # periodic: sample n-1 continues into sample 0
        x *= 0.1 / np.sqrt(np.mean(x * x))
        raw = x.astype("<f4").tobytes()
        header = struct.pack("<4sI4s", b"RIFF", 36 + len(raw), b"WAVE")
        header += struct.pack("<4sIHHIIHH", b"fmt ", 16, 3, 1, rate, rate * 4, 4, 32)
        header += struct.pack("<4sI", b"data", len(raw))
        open(f"{out_dir}/{colour}_{seed}.wav", "wb").write(header + raw)
"""


async def write_beds(out_dir: Path, seeds: list, seconds: float) -> None:
    """Seeded white and pink noise beds (30 Hz - 16 kHz) made periodic by an inverse FFT."""
    out_dir.mkdir(parents=True, exist_ok=True)

    def run():
        return subprocess.run([ar._uv(), "run", "--quiet", "--no-project", "--with", "numpy", "python", "-c", _BEDS,
                               str(out_dir), str(RATE), str(seconds), json.dumps(seeds)],
                              capture_output=True, text=True, timeout=600)

    out = await asyncio.to_thread(run)
    if out.returncode != 0:
        raise RuntimeError(f"noise beds failed: {out.stderr[-2000:]}")


def ffmpeg_encode(src: Path, dst: Path, args: list) -> None:
    out = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(src)] + args + [str(dst)],
                         capture_output=True, text=True, timeout=120)
    if out.returncode != 0:
        raise RuntimeError(f"ffmpeg {dst.suffix}: {out.stderr[-1000:]}")


# -- tool helpers -----------------------------------------------------------------------------------


async def analyze(ed: Editor, paths, **args) -> dict:
    args.setdefault("image", "none")
    return await ed.call("analyze_audio", paths=list(paths), **args)


async def analyze_one(ed: Editor, path: str, **args) -> dict:
    r = await analyze(ed, [path], **args)
    ed.check(not r["errors"], f"{path}: {r['errors']!r}")
    return r["items"][0]


def near(ed: Editor, actual, expected, tolerance, what: str) -> None:
    ed.check(actual is not None and abs(actual - expected) <= tolerance,
             f"{what}: {actual!r}, expected {expected} +/- {tolerance}")


def note_ids(item: dict) -> list:
    return [n["id"] for n in item["notes"]]


def lane_rows(rows, top: int, height: int):
    return rows[top:top + height]


def busy_share(rows, width: int, channels: int) -> float:
    """Share of a lane's pixels that differ from its most common colour (its background)."""
    counts: dict = {}
    pixels = []
    for row in rows:
        for x in range(width):
            p = bytes(row[x * channels:x * channels + 3])
            pixels.append(p)
            counts[p] = counts.get(p, 0) + 1
    background = max(counts, key=counts.get)
    return sum(1 for p in pixels if p != background) / len(pixels)


def check_image(ed: Editor, result, width: int, heights: list, what: str) -> None:
    """The PNG decodes, has the width and the lane heights given, and every lane is at least 2%
    different from its background."""
    ed.check(result.images, f"{what}: no image")
    data, mime = result.images[0]
    ed.check_eq(mime, "image/png", f"{what} mime")
    w, h, channels, rows = decode_png(data)
    ed.check_eq((w, h), (width, sum(heights)), f"{what} size")
    info = result.structured["image"]
    ed.check_eq((info["width"], info["height"]), (w, h), f"{what} image info")
    top = 0
    for i, lane in enumerate(heights):
        share = busy_share(lane_rows(rows, top, lane), w, channels)
        ed.check(share >= 0.02, f"{what}: lane {i} (rows {top}-{top + lane}) is {share:.3%} non-background")
        top += lane


# -- 3.1 loudness calibration -----------------------------------------------------------------------


async def test_ebu_loudness(ed: Editor):
    ed.require_tools("analyze_audio")
    root = ed.project / DIR / "ebu"
    cases = []
    for case in (1, 2, 3, 4):
        channels, expected = ar.ebu3341(case)
        write_float(root / f"ebu3341_{case}.wav", channels, RATE)
        cases.append((f"{DIR}/ebu/ebu3341_{case}.wav", expected))
    channels, expected = ar.ebu3341(1, 44100)
    write_float(root / "ebu3341_1_44k.wav", channels, 44100)
    cases.append((f"{DIR}/ebu/ebu3341_1_44k.wav", expected))
    channels, lra = ar.ebu3342_1()
    write_float(root / "ebu3342_1.wav", channels, RATE)

    r = await analyze(ed, [p for p, _ in cases] + [f"{DIR}/ebu/ebu3342_1.wav"])
    ed.check(not r["errors"], f"errors: {r['errors']!r}")
    for (path, value), item in zip(cases, r["items"]):
        near(ed, item["loudness"]["integrated_lufs"], value, 0.1, f"{path} integrated")
    ebu3342 = r["items"][-1]
    near(ed, ebu3342["loudness"]["lra"], lra, 1.0, "EBU 3342 #1 LRA")
    ed.check_eq(r["items"][-2]["format"]["sample_rate"], 44100, "44.1 kHz case")


async def test_k_weighting_against_pyloudnorm(ed: Editor):
    root = ed.project / DIR / "kw"
    paths = []
    for rate in (32000, 44100, 48000, 96000):
        for freq in (100.0, 1000.0, 10000.0):
            tone = ar.sine(freq, 10.0, rate, -23.0)
            paths.append(write_float(root / f"kw_{rate}_{int(freq)}.wav", [tone, tone], rate))
    reference = await pyloudnorm([(p, "integrated") for p in paths])
    r = await analyze(ed, [f"{DIR}/kw/{p.name}" for p in paths])
    ed.check(not r["errors"], f"errors: {r['errors']!r}")
    for path, item, value in zip(paths, r["items"], reference):
        near(ed, item["loudness"]["integrated_lufs"], value, 0.1, f"{path.name} integrated vs pyloudnorm")


async def test_true_peak(ed: Editor):
    # fs/4 at 45 degrees: every sample sits at sin(45) of the peak, so the true peak is 3.01 dB over
    amplitude = ar.db_to_amplitude(-6.0) / math.sin(math.pi / 4)
    tone = [amplitude * math.sin(math.pi / 4 + math.pi / 2 * i) for i in range(RATE)]
    write_float(ed.project / DIR / "true_peak.wav", [tone], RATE)
    item = await analyze_one(ed, f"{DIR}/true_peak.wav")
    near(ed, item["peak"]["sample_dbfs"], -6.0, 0.01, "sample peak")
    near(ed, item["peak"]["true_peak_dbtp"] - item["peak"]["sample_dbfs"], 3.01, 0.2, "true peak - sample peak")


async def test_pyloudnorm_fixtures(ed: Editor):
    root = ed.project / DIR / "pyln"
    fixtures = {
        "sine440.wav": ([ar.sine(440.0, 1.0, RATE, -12.0)], RATE),
        "sine3k_44k.wav": (ar.stereo(ar.sine(3000.0, 2.0, 44100, -18.0)), 44100),
        "sine100_burst.wav": ([ar.silence(0.2, RATE) + ar.sine(100.0, 0.6, RATE, -9.0) + ar.silence(0.2, RATE)], RATE),
        "pinkish.wav": (ar.stereo(pinkish_noise(2.0, RATE, 5)), RATE),
        "clicks.wav": ([ar.click_train([0.1, 0.35, 0.6, 0.85], 1.0, RATE, 0.8)], RATE),
        "jump.wav": ([chirp_jump(RATE)], RATE),
    }
    paths = [write_float(root / name, channels, rate) for name, (channels, rate) in fixtures.items()]
    # pyloudnorm refuses a signal shorter than its 400 ms block: there the integrated loudness is null
    long_enough = [p for p, (channels, rate) in zip(paths, fixtures.values()) if len(channels[0]) > 0.4 * rate]
    reference = await pyloudnorm([(p, "integrated") for p in long_enough] + [(p, "file") for p in paths])
    by_path = dict(zip(long_enough, reference[:len(long_enough)]))
    integrated = [by_path.get(p) for p in paths]
    momentary = reference[len(long_enough):]
    ed.check(len(long_enough) == 5, "the jump placeholder is the one fixture under 400 ms")
    r = await analyze(ed, [f"{DIR}/pyln/{p.name}" for p in paths])
    ed.check(not r["errors"], f"errors: {r['errors']!r}")
    for path, item, value, mmax in zip(paths, r["items"], integrated, momentary):
        if value is None:
            ed.check(item["loudness"]["integrated_lufs"] is None, f"{path.name}: integrated should be null")
        else:
            near(ed, item["loudness"]["integrated_lufs"], value, 0.1, f"{path.name} integrated vs pyloudnorm")
        near(ed, item["loudness"]["momentary_max"], mmax, 0.1, f"{path.name} momentary max vs pyloudnorm")


# -- 3.1 the other measurements ---------------------------------------------------------------------


async def test_bands(ed: Editor):
    write_float(ed.project / DIR / "s40.wav", [ar.sine(40.0, 2.0, RATE, -12.0)], RATE)
    write_float(ed.project / DIR / "s1k.wav", [ar.sine(1000.0, 2.0, RATE, -12.0)], RATE)
    r = await analyze(ed, [f"{DIR}/s40.wav", f"{DIR}/s1k.wav"])
    low, mid = r["items"]
    ed.check(low["band_share"]["31.5"] + low["band_share"]["63"] >= 0.95, f"40 Hz shares: {low['band_share']!r}")
    ed.check(low["sub_share"] >= 0.85, f"40 Hz sub share {low['sub_share']}")
    ed.check_in("low_end", note_ids(low), "40 Hz gets the low_end note")
    # 4th-order bands (spec 2.6) leak 0.058 into each neighbour of a centred tone: 0.883 (spec 5.3)
    ed.check(mid["band_share"]["1k"] >= 0.85, f"1 kHz share: {mid['band_share']!r}")
    near(ed, mid["bands_db"]["1k"], -12.0 - 0.53, 0.3, "1 kHz band level (AES17, -0.26 dB per filter edge)")


async def test_onsets_attack_and_silence(ed: Editor):
    write_float(ed.project / DIR / "clicks.wav", [ar.click_train([0.1, 0.35, 0.6, 0.85], 1.0, RATE, 0.8)], RATE)
    write_float(ed.project / DIR / "ramp.wav", [ramp_decay(RATE)], RATE)
    write_float(ed.project / DIR / "headtail.wav",
                [ar.silence(0.062, RATE) + ar.sine(1000.0, 0.2, RATE, -20.0) + ar.silence(0.1, RATE)], RATE)
    r = await analyze(ed, [f"{DIR}/clicks.wav", f"{DIR}/ramp.wav", f"{DIR}/headtail.wav"], detail="full")
    clicks, ramp, headtail = r["items"]
    ed.check_eq(clicks["onset"]["count"], 4, f"click onsets: {clicks['onsets_ms']!r}")
    for got, want in zip(clicks["onsets_ms"], (100.0, 350.0, 600.0, 850.0)):
        near(ed, got, want, 1.0, "click onset ms")
    near(ed, clicks["onset"]["first_ms"], 100.0, 1.0, "first onset")

    near(ed, ramp["onset"]["attack_ms"], 8.0, 1.0, "attack of a 10 ms ramp")
    near(ed, ramp["onset"]["peak_ms"], 10.0, 0.5, "peak of a 10 ms ramp")

    near(ed, headtail["silence"]["head_ms"], 62.0, 0.5, "head silence")
    near(ed, headtail["silence"]["tail_ms"], 100.0, 0.5, "tail silence")
    ed.check_in("head_silence", note_ids(headtail), "head_silence note")
    ed.check("head silence 62.0 ms" in headtail["notes"][note_ids(headtail).index("head_silence")]["text"],
             f"note text: {headtail['notes']!r}")
    # full detail: the 100 ms curves
    ed.check(len(ramp["momentary"]) == 2 and len(ramp["short_term"]) == 2, f"curves: {ramp['momentary']!r}")


async def test_spectral_pitch_and_stereo(ed: Editor):
    tone = ar.sine(1000.0, 1.0, RATE, -12.0)
    write_float(ed.project / DIR / "tone1k.wav", [tone], RATE)
    write_float(ed.project / DIR / "white.wav", [ar.white_noise(1.0, RATE, 9, -12.0)], RATE)
    write_float(ed.project / DIR / "a440.wav", [ar.sine(440.0, 1.0, RATE, -12.0)], RATE)
    write_float(ed.project / DIR / "same.wav", [tone, tone], RATE)
    write_float(ed.project / DIR / "anti.wav", [tone, [-x for x in tone]], RATE)
    r = await analyze(ed, [f"{DIR}/{n}.wav" for n in ("tone1k", "white", "a440", "same", "anti")])
    tone1k, white, a440, same, anti = r["items"]
    near(ed, tone1k["centroid_hz"], 1000.0, 24.0, "1 kHz centroid")
    ed.check(tone1k["flatness"] < 0.05, f"1 kHz flatness {tone1k['flatness']}")
    ed.check(white["flatness"] > 0.5, f"white noise flatness {white['flatness']}")
    near(ed, a440["pitch"]["hz"], 440.0, 1.0, "440 Hz pitch")
    ed.check(a440["pitch"]["confidence"] >= 0.9, f"440 Hz confidence {a440['pitch']!r}")
    ed.check(white["pitch"]["confidence"] < 0.3, f"white noise pitch confidence {white['pitch']!r}")
    near(ed, same["stereo"]["correlation"], 1.0, 0.01, "identical channels")
    near(ed, anti["stereo"]["correlation"], -1.0, 0.01, "L = -R")
    ed.check_in("phase", note_ids(anti), "phase note for L = -R")
    ed.check("phase" not in note_ids(same), "no phase note for identical channels")
    ed.check(tone1k["stereo"]["correlation"] is None, "mono correlation is null")


async def test_loop_seam(ed: Editor):
    whole = ar.sine(1000.0, 0.25, RATE, -6.0)      # 250 whole cycles
    write_float(ed.project / DIR / "loop_whole.wav", [whole], RATE)
    write_float(ed.project / DIR / "loop_cut.wav", [whole[:-12]], RATE)   # a quarter cycle short
    # 4.1 s versions (4100 whole cycles): long and steady enough to be measured loops, so the
    # loop_seam note applies (the type that declares a loop, an ambience sidecar, comes with
    # snd/p1-synth)
    long_whole = ar.sine(1000.0, 4.1, RATE, -6.0)
    write_float(ed.project / DIR / "loop_whole_4s.wav", [long_whole], RATE)
    write_float(ed.project / DIR / "loop_cut_4s.wav", [long_whole[:-12]], RATE)
    r = await analyze(ed, [f"{DIR}/{n}.wav" for n in ("loop_whole", "loop_cut", "loop_whole_4s", "loop_cut_4s")])
    short_whole, short_cut, long_ok, long_cut = r["items"]
    near(ed, short_whole["loop"]["seam_step_db"], 3.0, 0.1, "whole-cycle seam step")
    near(ed, short_cut["loop"]["seam_step_db"], 20.6, 0.2, "quarter-cycle cut seam step")
    near(ed, long_ok["loop"]["seam_step_db"], 3.0, 0.1, "4 s whole-cycle seam step")
    near(ed, long_cut["loop"]["seam_step_db"], 20.6, 0.2, "4 s cut seam step")
    ed.check_eq((long_ok["kind"], long_ok["kind_source"]), ("loop", "measured"), "a steady 4 s tone is a loop")
    ed.check_eq(long_cut["kind"], "loop", "the cut tone is a loop too")
    ed.check("loop_seam" not in note_ids(long_ok), f"seamless loop has no loop_seam: {long_ok['notes']!r}")
    ed.check_in("loop_seam", note_ids(long_cut), "a clicking loop gets loop_seam")
    ed.check("will click" in long_cut["notes"][note_ids(long_cut).index("loop_seam")]["text"], "loop_seam text")

    # noise beds that wrap seamlessly: the seam step stays an ordinary step (spec 5.0.3)
    beds = ed.project / DIR / "beds"
    seeds = list(range(1, 21))
    await write_beds(beds, seeds, 4.0)
    for colour in ("white", "pink"):
        r = await analyze(ed, [f"{DIR}/beds/{colour}_{s}.wav" for s in seeds])
        ed.check(not r["errors"], f"{colour} beds: {r['errors']!r}")
        for seed, item in zip(seeds, r["items"]):
            ed.check_eq(item["kind"], "loop", f"{colour} bed {seed} is a measured loop")
            ed.check("loop_seam" not in note_ids(item),
                     f"{colour} bed {seed}: step {item['loop']['seam_step_db']}, level {item['loop']['seam_level_db']}: {item['notes']!r}")


async def test_loop_seam_ambience_sidecars(ed: Editor):
    """Sounds too short to be measured loops, declared loops by an `ambience` importer type."""
    root = ed.project / DIR / "declared"
    whole = ar.sine(1000.0, 0.25, RATE, -6.0)      # 250 whole cycles
    write_float(root / "sine_whole.wav", [whole], RATE)
    write_float(root / "sine_cut.wav", [whole[:-12]], RATE)   # a quarter cycle short
    seeds = list(range(1, 21))
    await write_beds(root, seeds, 2.0)
    names = ["sine_whole", "sine_cut"] + [f"{colour}_{s}" for colour in ("white", "pink") for s in seeds]
    for name in names:
        ar.write_import(root / f"{name}.wav", "type: ambience\n")

    by_name = {}
    for at in range(0, len(names), 21):
        chunk = names[at:at + 21]
        r = await analyze(ed, [f"{DIR}/declared/{n}.wav" for n in chunk])
        ed.check(not r["errors"], f"errors: {r['errors']!r}")
        by_name.update(zip(chunk, r["items"]))
    for name, item in by_name.items():
        ed.check_eq((item["type"], item["kind"], item["kind_source"]), ("ambience", "loop", "type"), f"{name} is a loop by its sidecar")
    sine, cut = by_name["sine_whole"], by_name["sine_cut"]
    near(ed, sine["loop"]["seam_step_db"], 3.0, 0.1, "declared whole-cycle sine seam step")
    ed.check("loop_seam" not in note_ids(sine), f"a seamless declared loop has no loop_seam: {sine['notes']!r}")
    near(ed, cut["loop"]["seam_step_db"], 20.6, 0.2, "declared cut sine seam step")
    ed.check_in("loop_seam", note_ids(cut), "a declared loop that clicks gets loop_seam")
    ed.check("will click" in cut["notes"][note_ids(cut).index("loop_seam")]["text"], "loop_seam text")
    for name in names[2:]:
        item = by_name[name]
        ed.check(item["format"]["duration_s"] == 2.0, f"{name} is 2 s")
        ed.check("loop_seam" not in note_ids(item),
                 f"{name}: step {item['loop']['seam_step_db']}, level {item['loop']['seam_level_db']}: {item['notes']!r}")


async def test_clipping_and_dc(ed: Editor):
    clipped = [max(-1.0, min(1.0, 1.5 * x)) for x in ar.sine(1000.0, 0.2, RATE, 0.0)]
    write_float(ed.project / DIR / "clipped.wav", [clipped], RATE)
    write_float(ed.project / DIR / "dc.wav", [[0.05 + x for x in ar.sine(1000.0, 0.5, RATE, -20.0)]], RATE)
    r = await analyze(ed, [f"{DIR}/clipped.wav", f"{DIR}/dc.wav"])
    clip, dc = r["items"]
    expected = sum(1 for x in clipped if abs(x) >= 1.0 - 2 ** -15)
    ed.check_eq(clip["peak"]["clipped"], expected, "clipped samples")
    ed.check_in("clipping", note_ids(clip), "clipping note")
    near(ed, dc["dc_offset"], 0.05, 0.001, "DC offset")
    ed.check_in("dc_offset", note_ids(dc), "dc_offset note")
    ed.check("clipping" not in note_ids(dc), "no clipping on the DC file")


async def test_formats(ed: Editor):
    if not shutil.which("ffmpeg"):
        skip("ffmpeg is not installed: the OGG, MP3 and FLAC encodings need it")
    root = ed.project / DIR / "formats"
    tone = ar.stereo(ar.sine(1000.0, 1.0, RATE, -12.0))
    ar.write_wav(root / "tone_pcm16.wav", tone, RATE, ar.PCM16)
    ar.write_wav(root / "tone_pcm24.wav", tone, RATE, ar.PCM24)
    ar.write_wav(root / "tone_float32.wav", tone, RATE, ar.FLOAT32)
    ffmpeg_encode(root / "tone_pcm16.wav", root / "tone.ogg", ["-c:a", "libvorbis", "-q:a", "6"])
    # 320 kbps: at 192 kbps LAME itself takes this tone 0.3 dB down (ffmpeg's own decoder agrees)
    ffmpeg_encode(root / "tone_pcm16.wav", root / "tone.mp3", ["-c:a", "libmp3lame", "-b:a", "320k"])
    ffmpeg_encode(root / "tone_pcm16.wav", root / "tone.flac", ["-c:a", "flac"])
    names = ["tone_pcm16.wav", "tone_pcm24.wav", "tone_float32.wav", "tone.ogg", "tone.mp3", "tone.flac"]
    expected = [("wav", "pcm16"), ("wav", "pcm24"), ("wav", "float32"), ("ogg", "vorbis"), ("mp3", "mp3"), ("flac", "flac")]
    r = await analyze(ed, [f"{DIR}/formats/{n}" for n in names])
    ed.check(not r["errors"], f"errors: {r['errors']!r}")
    reference = r["items"][0]["loudness"]["integrated_lufs"]
    for name, want, item in zip(names, expected, r["items"]):
        fmt = item["format"]
        ed.check_eq((fmt["codec"], fmt["encoding"]), want, f"{name} codec/encoding")
        ed.check_eq((fmt["channels"], fmt["sample_rate"]), (2, RATE), f"{name} layout")
        if name.endswith(".mp3"):
            # dr_mp3 (the decoder SoLoud plays with) keeps the encoder's priming and padding
            # samples; the analysis describes what plays (spec 5.3)
            ed.check(1.0 <= fmt["duration_s"] <= 1.06, f"{name} duration {fmt['duration_s']}")
        else:
            near(ed, fmt["duration_s"], 1.0, 0.002, f"{name} duration")
        near(ed, item["loudness"]["integrated_lufs"], reference, 0.2, f"{name} integrated vs the WAV")
        ed.check_eq(fmt["bytes"], (root / name).stat().st_size, f"{name} bytes")


async def test_errors(ed: Editor):
    (ed.project / DIR).mkdir(parents=True, exist_ok=True)
    (ed.project / DIR / "text.wav").write_text("this is a text file, not audio\n")
    write_float(ed.project / DIR / "good.wav", [ar.sine(1000.0, 0.5, RATE, -12.0)], RATE)
    r = await analyze(ed, [f"{DIR}/good.wav", f"{DIR}/text.wav", f"{DIR}/missing.wav"])
    ed.check_eq([i["path"] for i in r["items"]], [f"{DIR}/good.wav"], "the good file is analysed")
    ed.check_eq(r["items"][0]["n"], 1, "numbered")
    errors = {e["path"]: e["error"] for e in r["errors"]}
    ed.check("not a WAV, OGG, MP3 or FLAC" in errors.get(f"{DIR}/text.wav", ""), f"text file error: {errors!r}")
    ed.check(f"{DIR}/missing.wav" in errors, f"missing file error: {errors!r}")
    text = await ed.call("analyze_audio", paths=[f"{DIR}/text.wav"], image="none", expect_error=True)
    ed.check("not a WAV" in text, f"all-bad call text: {text}")
    await ed.call("analyze_audio", expect_error=True)
    await ed.call("analyze_audio", paths=[f"{DIR}/good.wav"], capture="last", expect_error=True)
    await ed.call("analyze_audio", paths=[f"{DIR}/good.wav"], start=1.0, expect_error=True)
    await ed.call("analyze_audio", paths=[f"{DIR}/good.wav", f"{DIR}/good.wav"], image="both", expect_error=True)


async def test_cache(ed: Editor):
    path = ed.project / DIR / "cached.wav"
    write_float(path, [ar.sine(700.0, 0.5, RATE, -14.0)], RATE)
    import hashlib
    sha1 = hashlib.sha1(path.read_bytes()).hexdigest()
    cache = ed.project / ".jasperengine/cache/audio" / f"{sha1}-v{ANALYZER_VERSION}.json"
    if cache.exists():
        cache.unlink()
    first = await analyze_one(ed, f"{DIR}/cached.wav")
    ed.check_eq(first["sha1"], sha1, "item sha1")
    ed.check(cache.exists(), f"no cache file {cache}")
    stored = json.loads(cache.read_text())
    ed.check_eq((stored["analyzer_version"], stored["sha1"]), (ANALYZER_VERSION, sha1), "cache header")
    for key in ("n", "path", "kind", "kind_source", "notes"):
        ed.check(key not in stored, f"the cache holds {key}")
    ed.check_eq(stored["measured_kind"], first["measured_kind"], "the cache keeps measured_kind")
    ed.check("momentary" in stored and "onsets_ms" in stored, "the cache holds the full detail")

    # a marked value proves the second call read the file
    stored["rms_dbfs"] = -77.77
    cache.write_text(json.dumps(stored))
    second = await analyze_one(ed, f"{DIR}/cached.wav")
    ed.check_eq(second["rms_dbfs"], -77.77, "the second call reads the cache")
    # touched, same bytes: still a hit
    later = time.time() + 5
    os.utime(path, (later, later))
    touched = await analyze_one(ed, f"{DIR}/cached.wav")
    ed.check_eq(touched["rms_dbfs"], -77.77, "a touched file with the same bytes hits the cache")
    # one byte changed: a miss, a new entry
    data = bytearray(path.read_bytes())
    data[-1] ^= 0x01
    path.write_bytes(bytes(data))
    changed = await analyze_one(ed, f"{DIR}/cached.wav")
    ed.check(changed["rms_dbfs"] != -77.77, "a changed file misses the cache")
    new_sha1 = hashlib.sha1(bytes(data)).hexdigest()
    ed.check_eq(changed["sha1"], new_sha1, "new sha1")
    ed.check((cache.parent / f"{new_sha1}-v{ANALYZER_VERSION}.json").exists(), "a new cache entry")


# -- 3.1 capture input ------------------------------------------------------------------------------


def write_session(project: Path, session: str, channels: list, state: str = "complete",
                  flushed_frames: int | None = None) -> Path:
    directory = project / ".jasperengine/audio/captures" / session
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "master.wav").write_bytes(capture_wav(channels, RATE, flushed_frames))
    frames = len(channels[0]) if flushed_frames is None else flushed_frames
    events = [{"n": 1, "kind": "voice_start", "sample": 0, "t": 0.0, "frame": 0, "game_time": 0.0, "voice": 4097,
               "clip": "sounds/tone.wav", "entity": "Player", "entity_id": 1, "scene": "Main Scene", "bus": "master",
               "gain": 1.0, "gain_db": 0.0, "pitch": 1.0, "pan": 0.0, "looping": False, "spatial": False, "cause": "play"}]
    (directory / "events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in events))
    session_json = {
        "format": "jasperengine-audio-capture", "version": 1, "session": session, "state": state,
        "project": "Fixture", "scene": "Main Scene", "started_utc": "2026-10-15T10:15:00Z",
        "ended_utc": "2026-10-15T10:15:03Z", "sample_rate": RATE, "channels": 2, "buffer_size": 512,
        "latency_samples": 1, "backend": "null driver", "fixed_fps": 60, "deterministic": True,
        "frames": frames // 800, "samples": frames, "duration_s": frames / RATE, "captured_samples": frames,
        "truncated": False, "max_seconds": 300, "files": {"master": "master.wav", "events": "events.jsonl"},
        "output_stage": {"clipper": "roundoff", "post_clip_scaler": 0.95, "small_signal_gain_db": -1.655,
                         "pan_law": "constant_power", "centre_gain_db": -3.01, "global_volume": 1.0},
        "counts": {"voice_start": 1, "voice_stop": 0, "voice_pause": 0, "voice_resume": 0, "marker": 0, "input": 0},
        "events_dropped": 0, "voices_peak": 1, "warnings": []
    }
    (directory / "session.json").write_text(json.dumps(session_json, indent=1))
    return directory


async def test_capture_input(ed: Editor):
    project = ed.project
    captures = project / ".jasperengine/audio/captures"
    if captures.exists():
        shutil.rmtree(captures)
    text = await ed.call("analyze_audio", capture="last", expect_error=True)
    ed.check("No capture yet" in text, f"no sessions: {text}")

    # 3 s of stereo: silence, a -20 dBFS burst, a quieter one, as a capture mixes them
    left = ar.silence(0.5, RATE) + ar.sine(1000.0, 1.0, RATE, -20.0) + ar.silence(0.5, RATE) + ar.sine(500.0, 1.0, RATE, -30.0)
    right = [0.5 * x for x in left]
    older = "20261015-101500-01"
    newest = "20261015-101503-01"
    write_session(project, older, [ar.silence(1.0, RATE), ar.silence(1.0, RATE)])
    write_session(project, newest, [left, right])
    # a directory not named like a session is ignored
    (captures / "notes").mkdir(exist_ok=True)

    raw = await ed.call_raw("analyze_audio", {"capture": "last", "image": "both", "detail": "full"})
    ed.check(not raw.is_error, raw.text)
    item = raw.structured["items"][0]
    ed.check_eq(item["path"], f"capture:{newest}", "last is the newest session")
    ed.check_eq(item["range"], {"start": 0.0, "end": 3.0}, "whole range")
    ed.check_eq(item["format"]["frames"], 3 * RATE, "frames")
    ed.check_eq(item["kind_source"], "measured", "captures are measured")
    check_image(ed, raw, 768, [16, 128, 160, 96, 16], "capture both")

    # the same samples as a file: integrated loudness does not depend on the mode
    write_float(project / DIR / "capture_copy.wav", [left, right], RATE)
    sub_left, sub_right = left[int(0.5 * RATE):int(1.5 * RATE)], right[int(0.5 * RATE):int(1.5 * RATE)]
    write_float(project / DIR / "capture_range.wav", [sub_left, sub_right], RATE)
    files = await analyze(ed, [f"{DIR}/capture_copy.wav", f"{DIR}/capture_range.wav"])
    whole_file, range_file = files["items"]
    near(ed, item["loudness"]["integrated_lufs"], whole_file["loudness"]["integrated_lufs"], 0.01, "capture integrated = file integrated")
    ed.check_eq(item["peak"]["sample_dbfs"], whole_file["peak"]["sample_dbfs"], "sample peak")
    reference = await pyloudnorm([(project / DIR / "capture_copy.wav", "stream"), (project / DIR / "capture_copy.wav", "integrated"),
                                  (project / DIR / "capture_range.wav", "stream"), (project / DIR / "capture_range.wav", "integrated")])
    near(ed, item["loudness"]["momentary_max"], reference[0], 0.1, "capture momentary max vs pyloudnorm (stream)")
    near(ed, item["loudness"]["integrated_lufs"], reference[1], 0.1, "capture integrated vs pyloudnorm")
    ed.check_eq(len(item["momentary"]), 30, "100 ms curve over 3 s")

    # a range: [0.5, 1.5] s is the -20 dBFS burst alone
    ranged = await ed.call("analyze_audio", capture=newest, start=0.5, end=1.5, image="none")
    rng = ranged["items"][0]
    ed.check_eq(rng["range"], {"start": 0.5, "end": 1.5}, "range respected")
    ed.check_eq(rng["format"]["frames"], RATE, "range frames")
    near(ed, rng["loudness"]["integrated_lufs"], range_file["loudness"]["integrated_lufs"], 0.01, "range integrated = file")
    near(ed, rng["loudness"]["momentary_max"], reference[2], 0.1, "range momentary max vs pyloudnorm (stream)")
    near(ed, rng["loudness"]["integrated_lufs"], reference[3], 0.1, "range integrated vs pyloudnorm")
    near(ed, rng["peak"]["sample_dbfs"], -20.0, 0.01, "range sample peak")
    await ed.call("analyze_audio", capture=newest, start=5.0, image="none", expect_error=True)
    await ed.call("analyze_audio", capture=newest, start=1.0, end=0.5, image="none", expect_error=True)
    await ed.call("analyze_audio", capture="20991231-000000-01", image="none", expect_error=True)
    text = await ed.call("analyze_audio", capture="current", image="none", expect_error=True)
    ed.check("No capture is running" in text, f"current without Play: {text}")

    # capture images: waveform and spectrogram add the loudness lane
    for mode, heights in (("waveform", [16, 128, 96, 16]), ("spectrogram", [16, 160, 96, 16])):
        raw = await ed.call_raw("analyze_audio", {"capture": newest, "image": mode, "max_width": 512})
        ed.check(not raw.is_error, raw.text)
        check_image(ed, raw, 512, heights, f"capture {mode}")

    # left "capturing" by a crash (no Play is running): read as it was flushed
    crashed = "20261015-101510-01"
    write_session(project, crashed, [left, right], state="capturing", flushed_frames=RATE)
    last = await ed.call("analyze_audio", capture="last", image="none")
    ed.check_eq(last["items"][0]["path"], f"capture:{crashed}", "an aborted session is still a session")
    ed.check_eq(last["items"][0]["format"]["frames"], RATE, "only the flushed second is read")
    text = await ed.call("analyze_audio", capture="current", image="none", expect_error=True)
    ed.check("No capture is running" in text, f"an aborted session is not current: {text}")


# -- 3.2 images ------------------------------------------------------------------------------------


async def test_file_images(ed: Editor):
    sound = [x + 0.05 * n for x, n in zip(chirp_jump(RATE), ar.white_noise(0.3, RATE, 3, 0.0))]
    write_float(ed.project / DIR / "picture.wav", [sound], RATE)
    for mode, heights, width in (("waveform", [16, 128, 16], 768), ("spectrogram", [16, 160, 16], 300),
                                 ("both", [16, 128, 160, 16], 1024)):
        raw = await ed.call_raw("analyze_audio", {"paths": [f"{DIR}/picture.wav"], "image": mode, "max_width": width})
        ed.check(not raw.is_error, raw.text)
        check_image(ed, raw, width, heights, f"file {mode}")
        ed.check_eq(raw.structured["image"]["mode"], mode, "image mode")
    # the default for one path is both
    raw = await ed.call_raw("analyze_audio", {"paths": [f"{DIR}/picture.wav"]})
    ed.check_eq(raw.structured["image"]["height"], 320, "default image for one path")
    ed.check(raw.text.startswith(f"1. {DIR}/picture.wav: 0.30 s, "), f"text line: {raw.text}")
    # image none
    raw = await ed.call_raw("analyze_audio", {"paths": [f"{DIR}/picture.wav"], "image": "none"})
    ed.check(not raw.images and raw.structured["image"] is None, "image none")

    # a sheet of 5: cols 4, two rows
    names = []
    for i, freq in enumerate((200.0, 400.0, 800.0, 1600.0, 3200.0)):
        burst = [x * (1.0 - k / (0.4 * RATE)) for k, x in enumerate(ar.sine(freq, 0.4, RATE, -10.0))]
        write_float(ed.project / DIR / f"sheet_{i}.wav", [burst], RATE)
        names.append(f"{DIR}/sheet_{i}.wav")
    width = 768
    raw = await ed.call_raw("analyze_audio", {"paths": names, "max_width": width})
    ed.check(not raw.is_error, raw.text)
    ed.check_eq(raw.structured["image"]["mode"], "sheet", "several paths default to sheet")
    cols = 4
    tile = (width - 8 * (cols + 1)) // cols
    lane = round(tile / 4)
    height = 8 + 2 * (14 + 2 * lane + 8)
    data, _ = raw.images[0]
    w, h, channels, rows = decode_png(data)
    ed.check_eq((w, h), (width, height), "sheet size")
    for i in range(5):
        x0 = 8 + (i % cols) * (tile + 8)
        y0 = 8 + (i // cols) * (14 + 2 * lane + 8)
        for top, size in ((y0, 14), (y0 + 14, lane), (y0 + 14 + lane, lane)):
            sub = [bytes(row[x0 * channels:(x0 + tile) * channels]) for row in rows[top:top + size]]
            share = busy_share(sub, tile, channels)
            ed.check(share >= 0.02, f"sheet tile {i} lane at {top}: {share:.3%}")
    ed.check_eq([i["n"] for i in raw.structured["items"]], [1, 2, 3, 4, 5], "items numbered as the tiles")


# -- 2.13.5 get_asset_info --------------------------------------------------------------------------


async def test_get_asset_info_audio(ed: Editor):
    ed.require_tools("get_asset_info")
    root = ed.project / DIR / "info"
    ar.write_wav(root / "info.wav", [ar.sine(1000.0, 0.5, 44100, -12.0)], 44100, ar.PCM16)
    root.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ESSENTIALS / "footstep/concrete_1.ogg", root / "step.ogg")
    (root / "broken.ogg").write_bytes(b"OggS" + b"\x00" * 60)
    names = ["info.wav", "step.ogg"]
    if shutil.which("ffmpeg"):
        ffmpeg_encode(root / "info.wav", root / "info.mp3", ["-c:a", "libmp3lame", "-b:a", "128k"])
        ffmpeg_encode(root / "info.wav", root / "info.flac", ["-c:a", "flac"])
        names += ["info.mp3", "info.flac"]
    manifest = {c["id"]: c for c in json.loads((ESSENTIALS / "manifest.json").read_text())["clips"]}
    for name in names:
        info = await ed.call("get_asset_info", path=f"{DIR}/info/{name}")
        ed.check_eq(info["type"], "audio", name)
        audio = info.get("audio")
        ed.check(audio is not None, f"{name}: no audio: {info!r}")
        for key in ("codec", "encoding", "channels", "sample_rate", "frames", "duration_seconds", "bytes", "analysis"):
            ed.check_in(key, audio, f"{name} audio.{key}")
        for key in ("duration_s", "momentary_max", "integrated_lufs", "true_peak_dbtp", "sample_peak_dbfs", "peak_ms",
                    "head_ms", "kind", "notes"):
            ed.check_in(key, audio["analysis"], f"{name} analysis.{key}")
        ed.check_eq(audio["codec"], Path(name).suffix[1:], f"{name} codec")
        ed.check_eq(audio["bytes"], (root / name).stat().st_size, f"{name} bytes")
        ed.check(all(isinstance(t, str) for t in audio["analysis"]["notes"]), f"{name} notes are texts")
    info = await ed.call("get_asset_info", path=f"{DIR}/info/info.wav")
    ed.check_eq((info["audio"]["channels"], info["audio"]["sample_rate"], info["audio"]["encoding"]), (1, 44100, "pcm16"), "wav layout")
    near(ed, info["audio"]["duration_seconds"], 0.5, 0.001, "wav duration")
    step = (await ed.call("get_asset_info", path=f"{DIR}/info/step.ogg"))["audio"]
    measured = manifest["footstep/concrete_1"]["measured"]
    ed.check_eq((step["channels"], step["sample_rate"], step["frames"]),
                (measured["channels"], measured["sample_rate"], measured["frames"]), "ogg layout vs the manifest")
    near(ed, step["analysis"]["momentary_max"], measured["momentary_max_lufs"], 0.2, "ogg momentary max vs the manifest")
    broken = await ed.call("get_asset_info", path=f"{DIR}/info/broken.ogg")
    ed.check("audio" not in broken and broken.get("audio_error"), f"undecodable: {broken!r}")


# -- 3.5 the essentials manifest (measured by soundfile and pyloudnorm) -----------------------------


async def test_essentials_against_manifest(ed: Editor):
    manifest = json.loads((ESSENTIALS / "manifest.json").read_text())
    clips = manifest["clips"]
    ed.check_eq(len(clips), 40, "40 essentials")
    root = ed.project / DIR / "essentials"
    root.mkdir(parents=True, exist_ok=True)
    paths = []
    for clip in clips:
        name = clip["id"].replace("/", "__") + ".ogg"
        shutil.copyfile(ESSENTIALS / clip["path"], root / name)
        paths.append(f"{DIR}/essentials/{name}")
    items = []
    width = 1024
    for chunk in (paths[:20], paths[20:]):
        raw = await ed.call_raw("analyze_audio", {"paths": chunk, "image": "sheet", "max_width": width})
        ed.check(not raw.is_error, raw.text)
        ed.check(not raw.structured["errors"], f"errors: {raw.structured['errors']!r}")
        tile = (width - 8 * 5) // 4
        height = 8 + 5 * (14 + 2 * round(tile / 4) + 8)
        ed.check_eq((raw.structured["image"]["width"], raw.structured["image"]["height"]), (width, height), "sheet of 20")
        items += raw.structured["items"]
    ed.check_eq(len(items), 40, "40 items")
    for clip, item in zip(clips, items):
        m = clip["measured"]
        what = clip["id"]
        ed.check(abs(item["format"]["frames"] - m["frames"]) <= 64, f"{what} frames {item['format']['frames']} vs {m['frames']}")
        near(ed, item["format"]["duration_s"], m["duration_s"], 0.002, f"{what} duration")
        near(ed, item["peak"]["sample_dbfs"], m["sample_peak_dbfs"], 0.2, f"{what} sample peak")
        near(ed, item["loudness"]["momentary_max"], m["momentary_max_lufs"], 0.2, f"{what} momentary max")
        near(ed, item["silence"]["head_ms"], m["head_ms"], 2.0, f"{what} head silence")
        near(ed, item["silence"]["tail_ms"], m["tail_ms"], 2.0, f"{what} tail silence")
        ed.check_eq((item["format"]["channels"], item["format"]["sample_rate"]), (m["channels"], m["sample_rate"]), f"{what} layout")
        for bad in ("clipping", "silent", "head_silence"):
            ed.check(bad not in note_ids(item), f"{what} has note {bad}: {item['notes']!r}")
