"""AudioMeter (S38 R1, AC1) against an oracle outside the engine: pyloudnorm's integrated loudness of
the repository's audio fixtures and of signals built here, within 0.1 LU.

The meter has no MCP surface of its own (analyze_audio comes with S36), so this compiles
engine/tests/core/audio/AudioMeterProbe.cpp with AudioMeter.cpp and TruePeak.cpp alone, no engine:
the link also proves the library needs no SoLoud. pyloudnorm runs in a `uv run --with` subprocess
because the harness environment has no numpy. Each compressed fixture is decoded once by FFmpeg, and
meter and oracle read the same float32 WAV.
"""

from __future__ import annotations

import array
import asyncio
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from harness import TestFailure, skip

CI = True
TIMEOUT = 600

REPO = Path(__file__).resolve().parents[3]
AUDIO = REPO / "engine/core/audio"
FIXTURES = [REPO / "engine/tests/core/fixtures/audio" / name for name in ("spaceEngineLow_003.ogg", "sweep.flac", "sweep.mp3")]
FIXTURES += [REPO / "tools/test-projects/upgrade-from-main/project/assets/audio/pickup.wav",
             REPO / "tools/fixtures/legacy_paths_project/assets/sounds/beep.wav"]
TOLERANCE_LU = 0.1

ORACLE = r"""
import json, struct, sys
import numpy as np
import pyloudnorm

results = []
for path in sys.argv[1:]:
    raw = open(path, "rb").read()
    pos = 12
    while pos + 8 <= len(raw):
        cid, size = struct.unpack("<4sI", raw[pos:pos + 8])
        if cid == b"fmt ":
            _, channels, rate = struct.unpack("<HHI", raw[pos + 8:pos + 16])
        elif cid == b"data":
            data = np.frombuffer(raw, dtype="<f4", count=size // 4, offset=pos + 8).astype(np.float64)
            break
        pos += 8 + size + (size & 1)
    data = data.reshape(-1, channels)
    # pyloudnorm raises on a signal shorter than its 400 ms block: there the meter says null too
    value = None
    if data.shape[0] > 0.4 * rate:
        loudness = pyloudnorm.Meter(rate).integrated_loudness(data)
        value = float(loudness) if np.isfinite(loudness) else None
    results.append(value)
print(json.dumps(results))
"""


def write_float_wav(path: Path, channels: list, rate: int) -> Path:
    interleaved = array.array("f", [0.0]) * (len(channels[0]) * len(channels))
    for c, samples in enumerate(channels):
        interleaved[c::len(channels)] = array.array("f", samples)
    if sys.byteorder != "little":
        interleaved.byteswap()
    raw = interleaved.tobytes()
    count = len(channels)
    header = struct.pack("<4sI4s", b"RIFF", 4 + 24 + 8 + len(raw), b"WAVE")
    header += struct.pack("<4sIHHIIHH", b"fmt ", 16, 3, count, rate, rate * count * 4, count * 4, 32)
    header += struct.pack("<4sI", b"data", len(raw))
    path.write_bytes(header + raw)
    return path


def sine(frequency: float, seconds: float, rate: int, dbfs: float) -> list:
    amplitude = 10.0 ** (dbfs / 20.0)
    return [amplitude * math.sin(2.0 * math.pi * frequency * i / rate) for i in range(int(seconds * rate))]


def noise(seconds: float, rate: int, seed: int, amplitude: float) -> list:
    generator = random.Random(seed)
    return [generator.uniform(-amplitude, amplitude) for _ in range(int(seconds * rate))]


def clicks(times: list, seconds: float, rate: int) -> list:
    samples = [0.0] * int(seconds * rate)
    for t in times:
        samples[int(t * rate)] = 0.8
    return samples


def build_probe(work: Path) -> Path:
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        skip("no C++ compiler to build the meter's probe")
    probe = work / "audio-meter-probe"
    subprocess.run([compiler, "-std=c++20", "-O2", "-I", str(REPO / "engine/core"),
                    str(REPO / "engine/tests/core/audio/AudioMeterProbe.cpp"), str(AUDIO / "AudioMeter.cpp"), str(AUDIO / "TruePeak.cpp"),
                    "-o", str(probe)], check=True, capture_output=True, text=True, timeout=300)
    return probe


def meter_values(probe: Path, paths: list) -> list:
    out = subprocess.run([str(probe), *map(str, paths)], capture_output=True, text=True, timeout=300)
    if out.returncode != 0:
        raise TestFailure(f"the meter's probe exited with {out.returncode}: {out.stderr[-2000:]}")
    return [json.loads(line) for line in out.stdout.splitlines()]


def pyloudnorm_values(paths: list) -> list:
    uv = shutil.which("uv") or str(Path(os.environ.get("HOME", "~")).expanduser() / ".local/bin/uv")
    out = subprocess.run([uv, "run", "--quiet", "--no-project", "--with", "numpy==2.2.6", "--with", "scipy==1.15.3",
                          "--with", "pyloudnorm==0.1.1", "python", "-I", "-c", ORACLE, *map(str, paths)],
                         capture_output=True, text=True, timeout=600)
    if out.returncode != 0:
        raise RuntimeError(f"pyloudnorm failed: {out.stderr[-2000:]}")
    return json.loads(out.stdout.strip().splitlines()[-1])


def decode(source: Path, target: Path) -> Path:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        skip("no ffmpeg to decode the compressed audio fixtures")
    subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(source), "-c:a", "pcm_f32le",
                    "-f", "wav", "-bitexact", str(target)], check=True, capture_output=True, text=True, timeout=120)
    return target


def compare(paths: list, probe: Path) -> None:
    measured = meter_values(probe, paths)
    reference = pyloudnorm_values(paths)
    if len(measured) != len(paths) or len(reference) != len(paths):
        raise TestFailure(f"{len(paths)} files, {len(measured)} measured, {len(reference)} from pyloudnorm")
    if sum(value is not None for value in reference) < 3:
        raise TestFailure(f"pyloudnorm measured almost nothing: {reference}")
    failures = []
    for path, ours, theirs in zip(paths, measured, reference):
        value = ours["integrated"]
        print(f"[oracle] {path.name}: meter {value} pyloudnorm {theirs}")
        if (value is None) != (theirs is None):
            failures.append(f"{path.name}: meter {value}, pyloudnorm {theirs}")
        elif value is not None and abs(value - theirs) > TOLERANCE_LU:
            failures.append(f"{path.name}: meter {value:.3f}, pyloudnorm {theirs:.3f}")
    if failures:
        raise TestFailure("over 0.1 LU from pyloudnorm: " + "; ".join(failures))


async def test_fixture_files_agree_with_pyloudnorm():
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        probe = await asyncio.to_thread(build_probe, work)
        paths = [await asyncio.to_thread(decode, source, work / f"{source.name}.wav") for source in FIXTURES]
        await asyncio.to_thread(compare, paths, probe)


async def test_signals_agree_with_pyloudnorm():
    rate = 48000
    tone = sine(1000.0, 20.0, rate, -23.0)
    left = noise(6.0, rate, 5, 0.25)
    right = noise(6.0, rate, 6, 0.25)
    signals = {
        "ebu3341_1.wav": ([tone, tone], rate),
        "sine440.wav": ([sine(440.0, 1.0, rate, -12.0)], rate),
        "sine3k_44k.wav": ([sine(3000.0, 2.0, 44100, -18.0)] * 2, 44100),
        "sine100_burst.wav": ([[0.0] * 9600 + sine(100.0, 0.6, rate, -9.0) + [0.0] * 9600], rate),
        "noise.wav": ([left, right], rate),
        "clicks.wav": ([clicks([0.1, 0.35, 0.6, 0.85], 1.0, rate)], rate),
        "short.wav": ([sine(880.0, 0.2, rate, -6.0)], rate),
        "surround50.wav": ([sine(1000.0, 3.0, rate, level) for level in (-28.0, -28.0, -24.0, -30.0, -30.0)], rate),
    }
    # K-weighting at other rates: pyloudnorm derives its filters per rate too
    for r in (32000, 44100, 96000):
        for frequency in (100.0, 1000.0, 10000.0):
            signals[f"kw_{r}_{int(frequency)}.wav"] = ([sine(frequency, 3.0, r, -23.0)] * 2, r)

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        probe = await asyncio.to_thread(build_probe, work)
        paths = [write_float_wav(work / name, channels, r) for name, (channels, r) in signals.items()]
        await asyncio.to_thread(compare, paths, probe)
