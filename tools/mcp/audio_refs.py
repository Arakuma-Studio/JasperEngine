"""Reference signals and measurements for the audio tests (sound layer, phases 1 and 2).

Standard library only, so every test can import it under the harness's `uv run --with mcp`. These
are the tests' independent oracles: signals with known values, a WAV reader that is not SoLoud's,
and pyloudnorm, run in a subprocess (`uv run --no-project --with numpy --with pyloudnorm`) because
the harness environment has no numpy.

Signals are lists of channels, each a list of floats in [-1, 1] (`[[...]]` is mono). Times are
seconds, levels dBFS of the peak unless a name says otherwise (EBU Tech 3341 states its levels so).
"""

from __future__ import annotations

import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
from pathlib import Path

PCM16 = "pcm16"
PCM24 = "pcm24"
FLOAT32 = "float32"

WAVE_FORMAT_PCM = 1
WAVE_FORMAT_IEEE_FLOAT = 3
WAVE_FORMAT_EXTENSIBLE = 0xFFFE

# AES17: a full-scale sine reads 0 dBFS RMS
AES17_OFFSET_DB = 10 * math.log10(2)
SILENCE_DB = -120.0


def db_to_amplitude(db: float) -> float:
    return 10.0 ** (db / 20.0)


def amplitude_to_db(amplitude: float) -> float:
    return 20.0 * math.log10(amplitude) if amplitude > 0 else SILENCE_DB


# -- WAV ------------------------------------------------------------------------------------------


def write_wav(path, channels, rate: int, encoding: str = PCM16) -> Path:
    """Write channels (lists of floats) as a RIFF/WAVE file with only `fmt ` and `data` chunks.
    PCM is rounded (lround(x * full scale)) and clamped, without dither."""
    path = Path(path)
    count = len(channels)
    frames = len(channels[0]) if channels else 0
    if any(len(c) != frames for c in channels):
        raise ValueError("channels differ in length")
    if encoding == PCM16:
        fmt, width, scale, top = WAVE_FORMAT_PCM, 2, 32767, 32767
    elif encoding == PCM24:
        fmt, width, scale, top = WAVE_FORMAT_PCM, 3, 8388607, 8388607
    elif encoding == FLOAT32:
        fmt, width, scale, top = WAVE_FORMAT_IEEE_FLOAT, 4, None, None
    else:
        raise ValueError(f"unknown encoding {encoding}")

    data = bytearray()
    for i in range(frames):
        for c in range(count):
            x = channels[c][i]
            if fmt == WAVE_FORMAT_IEEE_FLOAT:
                data += struct.pack("<f", x)
            else:
                # lround: half away from zero
                v = int(math.copysign(math.floor(abs(x) * scale + 0.5), x))
                v = max(-top - 1, min(top, v))
                data += v.to_bytes(width, "little", signed=True)
    block = count * width
    header = struct.pack("<4sI4s", b"RIFF", 4 + (8 + 16) + (8 + len(data)), b"WAVE")
    header += struct.pack("<4sIHHIIHH", b"fmt ", 16, fmt, count, rate, rate * block, block, width * 8)
    header += struct.pack("<4sI", b"data", len(data))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + bytes(data))
    return path


class Wav:
    """A decoded WAV: rate, encoding, channels (lists of floats), and the chunk ids in order."""

    def __init__(self, rate: int, encoding: str, channels: list, chunks: list, fact_frames=None):
        self.rate = rate
        self.encoding = encoding
        self.channels = channels
        self.chunks = chunks
        self.fact_frames = fact_frames

    @property
    def frames(self) -> int:
        return len(self.channels[0]) if self.channels else 0

    @property
    def seconds(self) -> float:
        return self.frames / self.rate


def read_wav(path) -> Wav:
    """PCM 16/24/32 and float32 WAV, any channel count; WAVE_FORMAT_EXTENSIBLE too."""
    raw = Path(path).read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")
    pos, chunks, fmt, data, fact = 12, [], None, None, None
    while pos + 8 <= len(raw):
        cid, size = struct.unpack("<4sI", raw[pos:pos + 8])
        body = raw[pos + 8:pos + 8 + size]
        chunks.append(cid.decode("latin-1"))
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == WAVE_FORMAT_EXTENSIBLE and len(body) >= 26:
                fmt = (struct.unpack("<H", body[24:26])[0],) + fmt[1:]
        elif cid == b"data":
            data = body
        elif cid == b"fact":
            fact = struct.unpack("<I", body[:4])[0]
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise ValueError(f"{path}: no fmt or data chunk")
    tag, count, rate, _, block, bits = fmt
    width = bits // 8
    frames = len(data) // block
    channels = [[0.0] * frames for _ in range(count)]
    if tag == WAVE_FORMAT_IEEE_FLOAT and bits == 32:
        encoding = FLOAT32
        values = struct.unpack(f"<{frames * count}f", data[:frames * block])
        for c in range(count):
            channels[c] = list(values[c::count])
    elif tag == WAVE_FORMAT_PCM and bits in (8, 16, 24, 32):
        encoding = f"pcm{bits}"
        if bits == 16:
            values = struct.unpack(f"<{frames * count}h", data[:frames * block])
            for c in range(count):
                channels[c] = [v / 32768.0 for v in values[c::count]]
        else:
            for i in range(frames):
                for c in range(count):
                    at = i * block + c * width
                    chunk = data[at:at + width]
                    if bits == 8:
                        channels[c][i] = (chunk[0] - 128) / 128.0
                    else:
                        channels[c][i] = int.from_bytes(chunk, "little", signed=True) / float(1 << (bits - 1))
    else:
        raise ValueError(f"{path}: unsupported WAV format {tag} with {bits} bits")
    return Wav(rate, encoding, channels, chunks, fact)


# -- signals --------------------------------------------------------------------------------------


# -- click-safe fades (sound layer phase 2.5, spec 2.5.1 and 5.5)

# the fade-in a note proposes: the longest raised cosine, up to the kind's click-safe length and
# down to 3 frames, that takes at most 0.05 dB of the energy of the first 400 ms; 2 frames when
# even 3 take more
CLICK_SAFE_FADE_IN_MS = {"transient": 0.5, "glitch": 0.5, "tonal": 2.0, "swell": 2.0, "riser": 2.0, "speech": 2.0, "loop": 2.0}
FADE_IN_MAX_LOSS_DB = 0.05
ATTACK_WINDOW_SECONDS = 0.4
FADE_IN_MIN_FRAMES = 3
FADE_IN_FLOOR_FRAMES = 2


def _float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def fade_in_weight(k: int, n: int) -> float:
    """spec 2.5.1's fade-in weight of frame k of n, computed in double and rounded to float once"""
    return 1.0 if k >= n else _float32(0.5 * (1.0 - math.cos(math.pi * k / n)))


def _round_half_away(x: float) -> int:
    return int(math.floor(abs(x) + 0.5)) * (1 if x >= 0 else -1)


def safe_fade_in_frames(channels: list, rate: int, longest: int) -> int:
    """the longest fade-in of at most `longest` and at least FADE_IN_MIN_FRAMES frames that takes at
    most FADE_IN_MAX_LOSS_DB of the first ATTACK_WINDOW_SECONDS' energy (all channels); 0 when none"""
    n = len(channels[0]) if channels else 0
    window = min(n, _round_half_away(ATTACK_WINDOW_SECONDS * rate))
    squares = [sum(c[k] * c[k] for c in channels) for k in range(window)]
    longest = min(longest, len(squares))
    if longest < FADE_IN_MIN_FRAMES:
        return longest
    allowed = sum(squares) * (1.0 - 10 ** (-FADE_IN_MAX_LOSS_DB / 10.0))
    for fade in range(longest, FADE_IN_MIN_FRAMES - 1, -1):
        if sum(squares[k] * (1.0 - fade_in_weight(k, fade) ** 2) for k in range(fade)) <= allowed:
            return fade
    return 0


def click_safe_fade_in_ms(channels: list, rate: int, kind: str) -> float:
    """the fade_in_ms a note's fix proposes for a sound of this kind that starts like `channels`"""
    kind_ms = CLICK_SAFE_FADE_IN_MS[kind]
    kind_frames = _round_half_away(kind_ms * rate / 1000.0)
    safe = safe_fade_in_frames(channels, rate, _round_half_away(max(CLICK_SAFE_FADE_IN_MS.values()) * rate / 1000.0))
    frames = min(safe, kind_frames) if safe > 0 else FADE_IN_FLOOR_FRAMES
    return kind_ms if frames >= kind_frames else round(_round_half_away(frames * 1000.0 / rate / 0.001) * 0.001, 3)


def silence(seconds: float, rate: int) -> list:
    return [0.0] * int(round(seconds * rate))


def sine(freq: float, seconds: float, rate: int, dbfs: float = 0.0, phase: float = 0.0) -> list:
    """A sine of peak level dbfs; phase in radians at sample 0."""
    amplitude = db_to_amplitude(dbfs)
    step = 2 * math.pi * freq / rate
    return [amplitude * math.sin(phase + step * i) for i in range(int(round(seconds * rate)))]


def tone_burst(freq: float, rate: int, segments) -> list:
    """Concatenated sine segments [(seconds, dbfs or None for silence)], continuous in phase."""
    out, i = [], 0
    step = 2 * math.pi * freq / rate
    for seconds, dbfs in segments:
        n = int(round(seconds * rate))
        if dbfs is None:
            out += [0.0] * n
        else:
            amplitude = db_to_amplitude(dbfs)
            out += [amplitude * math.sin(step * (i + k)) for k in range(n)]
        i += n
    return out


def click_train(times, seconds: float, rate: int, amplitude: float = 1.0) -> list:
    """Single-sample impulses at the given times (rounded to the nearest sample)."""
    out = [0.0] * int(round(seconds * rate))
    for t in times:
        at = int(round(t * rate))
        if 0 <= at < len(out):
            out[at] = amplitude
    return out


def white_noise(seconds: float, rate: int, seed: int = 1, dbfs: float = -12.0) -> list:
    """Seeded uniform white noise of peak level dbfs (random.Random, so the same on every host)."""
    rng = random.Random(seed)
    amplitude = db_to_amplitude(dbfs)
    return [amplitude * (2.0 * rng.random() - 1.0) for _ in range(int(round(seconds * rate)))]


def linear_burst(freq: float, rate: int, attack: float, decay: float, amplitude: float = 1.0) -> list:
    """A sine burst under one linear envelope: 0 to `amplitude` over `attack` seconds, then back to 0
    over `decay` (sound layer phase 6: step_peak.wav, a single peak of its 20 ms RMS envelope)"""
    rise, fall = int(round(attack * rate)), int(round(decay * rate))
    step = 2 * math.pi * freq / rate
    out = []
    for i in range(rise + fall):
        gain = i / rise if i < rise else (rise + fall - i) / fall
        out.append(amplitude * gain * math.sin(step * i))
    return out


def goertzel_amplitude(samples: list, freq: float, rate: int) -> float:
    """Peak amplitude of the `freq` component of `samples` (Goertzel at the exact frequency; a
    full-scale sine of a whole number of cycles reads 1.0)"""
    n = len(samples)
    if n == 0:
        return 0.0
    w = 2 * math.pi * freq / rate
    coeff = 2 * math.cos(w)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2, s1 = s1, s0
    real = s1 - s2 * math.cos(w)
    imag = s2 * math.sin(w)
    return 2 * math.sqrt(real * real + imag * imag) / n


def rbj_lowpass_gain_db(freq: float, cutoff: float, q: float, rate: int) -> float:
    """dB response at `freq` of the RBJ cookbook lowpass biquad at `cutoff` and `q` (SoLoud's
    BiquadResonantFilter: alpha = sin(w0) / (2 Q))"""
    w0 = 2 * math.pi * cutoff / rate
    alpha = math.sin(w0) / (2 * q)
    b0 = b2 = (1 - math.cos(w0)) / 2
    b1 = 1 - math.cos(w0)
    a0, a1, a2 = 1 + alpha, -2 * math.cos(w0), 1 - alpha
    w = 2 * math.pi * freq / rate
    z1, z2 = complex(math.cos(-w), math.sin(-w)), complex(math.cos(-2 * w), math.sin(-2 * w))
    h = (b0 + b1 * z1 + b2 * z2) / (a0 + a1 * z1 + a2 * z2)
    return 20 * math.log10(abs(h))


def stereo(mono: list) -> list:
    return [list(mono), list(mono)]


# EBU Tech 3341 (loudness meter tests), the 1 kHz stereo cases with their expected integrated loudness
EBU3341 = {
    1: ([(20.0, -23.0)], -23.0),
    2: ([(20.0, -33.0)], -33.0),
    3: ([(10.0, -36.0), (60.0, -23.0), (10.0, -36.0)], -23.0),
    4: ([(10.0, -72.0), (10.0, -36.0), (60.0, -23.0), (10.0, -36.0), (10.0, -72.0)], -23.0),
}
# EBU Tech 3342 #1 (loudness range): -20 then -30 dBFS, 20 s each, LRA 10 LU
EBU3342_1 = ([(20.0, -20.0), (20.0, -30.0)], 10.0)


def ebu3341(case: int, rate: int = 48000) -> tuple:
    """(stereo channels, expected integrated LUFS) of an EBU Tech 3341 case."""
    segments, expected = EBU3341[case]
    return stereo(tone_burst(1000.0, rate, segments)), expected


def ebu3342_1(rate: int = 48000) -> tuple:
    segments, expected = EBU3342_1
    return stereo(tone_burst(1000.0, rate, segments)), expected


# -- measurements ---------------------------------------------------------------------------------


def linear_resample(samples: list, rate: int, to: int = 48000) -> list:
    """SoLoud's default resampler (RESAMPLER_LINEAR), as the mixer plays a clip at another rate:
    output sample i interpolates the input linearly at i * rate / to"""
    out = []
    for i in range(int(len(samples) * to / rate)):
        p = i * rate / to
        k = int(p)
        f = p - k
        a = samples[k] if k < len(samples) else 0.0
        b = samples[k + 1] if k + 1 < len(samples) else 0.0
        out.append(a + (b - a) * f)
    return out


def window(samples: list, rate: int, start: float, end: float) -> list:
    return samples[max(0, int(round(start * rate))):max(0, int(round(end * rate)))]


def rms(samples: list) -> float:
    return math.sqrt(sum(x * x for x in samples) / len(samples)) if samples else 0.0


def rms_db(samples: list) -> float:
    """RMS level, dB re full-scale amplitude (no AES17 offset: a full-scale sine reads -3.01)."""
    return amplitude_to_db(rms(samples))


def rms_dbfs(samples: list) -> float:
    """AES17 dBFS: a full-scale sine reads 0.00."""
    level = rms(samples)
    return amplitude_to_db(level) + AES17_OFFSET_DB if level > 0 else SILENCE_DB


def windowed_rms_db(channels: list, rate: int, start: float, end: float) -> list:
    """rms_db of each channel over [start, end) seconds."""
    return [rms_db(window(c, rate, start, end)) for c in channels]


def peak(samples: list) -> float:
    return max((abs(x) for x in samples), default=0.0)


def onset(samples: list, threshold_db: float = -60.0, start: int = 0):
    """Index of the first sample at or after `start` whose |x| reaches threshold_db dBFS, or None."""
    level = db_to_amplitude(threshold_db)
    for i in range(start, len(samples)):
        if abs(samples[i]) >= level:
            return i
    return None


def zero_crossing_frequency(samples: list, rate: int, start: float, end: float):
    """Frequency over [start, end) from the rising zero crossings, each placed by linear
    interpolation: (crossings - 1) periods between the first and the last. None without two."""
    lo, hi = max(1, int(round(start * rate))), min(len(samples), int(round(end * rate)))
    crossings = []
    for i in range(lo, hi):
        a, b = samples[i - 1], samples[i]
        if a < 0.0 <= b and b != a:
            crossings.append((i - 1) + (-a) / (b - a))
    if len(crossings) < 2:
        return None
    return (len(crossings) - 1) * rate / (crossings[-1] - crossings[0])


# -- pyloudnorm (subprocess) -----------------------------------------------------------------------


_PYLOUDNORM = r"""
import json, sys
import numpy as np
import pyloudnorm as pyln
sys.path.insert(0, sys.argv[1])
import audio_refs

wav = audio_refs.read_wav(sys.argv[2])
mode = sys.argv[3]
rate = wav.rate
data = np.array(wav.channels, dtype=np.float64).T
meter = pyln.Meter(rate)
if mode == "integrated":
    value = meter.integrated_loudness(data)
else:
    # momentary max (sound layer phase 1, 2.6): 400 ms windows every 10 ms of the K-weighted
    # signal, filtered continuously over the padding; "file" pads 400 ms before and after, "stream"
    # assumes silence before the start and has no padding after the end
    before = int(round(0.4 * rate))
    after = before if mode == "file" else 0
    padded = np.concatenate([np.zeros((before, data.shape[1])), data, np.zeros((after, data.shape[1]))])
    weighted = padded.T
    for f in meter._filters.values():
        weighted = f.apply_filter(weighted)
    weighted = weighted.T
    n = data.shape[0]
    step = rate / 100.0
    length = int(round(0.4 * rate))
    best = None
    k = -39
    while True:
        start = before + int(np.floor(k * step))
        end = start + length
        if mode == "file":
            if k * step > n - step:
                break
        elif end > before + n:
            break
        z = (weighted[start:end] ** 2).mean(axis=0).sum()
        if z > 0:
            value = -0.691 + 10 * np.log10(z)
            best = value if best is None else max(best, value)
        k += 1
    value = best
print(json.dumps(None if value is None or not np.isfinite(value) else float(value)))
"""


def _uv() -> str:
    found = shutil.which("uv")
    if found:
        return found
    home = Path(os.environ.get("HOME", "~")).expanduser() / ".local" / "bin" / "uv"
    return str(home)


def _pyloudnorm(path, mode: str):
    here = str(Path(__file__).resolve().parent)
    out = subprocess.run([_uv(), "run", "--quiet", "--no-project", "--with", "numpy", "--with", "pyloudnorm",
                          "python", "-c", _PYLOUDNORM, here, str(path), mode],
                         capture_output=True, text=True, timeout=600)
    if out.returncode != 0:
        raise RuntimeError(f"pyloudnorm ({mode}) failed: {out.stderr[-2000:]}")
    return json.loads(out.stdout.strip().splitlines()[-1])


def pyloudnorm_integrated(path):
    """BS.1770 integrated loudness of a WAV by pyloudnorm (LUFS, None when undefined)."""
    return _pyloudnorm(path, "integrated")


def pyloudnorm_momentary_max(path, pad: bool = True):
    """Momentary max of a WAV with the windows of sound layer phase 1 2.6, K-weighted by
    pyloudnorm's filters: pad=True is file mode (400 ms of silence before and after), pad=False
    stream mode (silence before the start only, windows ending by the last sample)."""
    return _pyloudnorm(path, "file" if pad else "stream")


# -- numpy measurements (subprocess) ---------------------------------------------------------------
#
# Sound layer phase 2: the mixer tests' independent oracles for true peak and spectra. They run in
# `uv run --no-project --with numpy`, as pyloudnorm does, and read the WAV with this module's reader.

# BS.1770-4 Annex 2's interpolation filter, as the standard prints it (4 phases of 12 taps)
TRUE_PEAK_FIR = [
    [0.0017089843750, 0.0109863281250, -0.0196533203125, 0.0332031250000, -0.0594482421875, 0.1373291015625,
     0.9721679687500, -0.1022949218750, 0.0476074218750, -0.0266113281250, 0.0148925781250, -0.0083007812500],
    [-0.0291748046875, 0.0292968750000, -0.0517578125000, 0.0891113281250, -0.1665039062500, 0.4650878906250,
     0.7797851562500, -0.2003173828125, 0.1015625000000, -0.0582275390625, 0.0330810546875, -0.0189208984375],
    [-0.0189208984375, 0.0330810546875, -0.0582275390625, 0.1015625000000, -0.2003173828125, 0.7797851562500,
     0.4650878906250, -0.1665039062500, 0.0891113281250, -0.0517578125000, 0.0292968750000, -0.0291748046875],
    [-0.0083007812500, 0.0148925781250, -0.0266113281250, 0.0476074218750, -0.1022949218750, 0.9721679687500,
     0.1373291015625, -0.0594482421875, 0.0332031250000, -0.0196533203125, 0.0109863281250, 0.0017089843750],
]

_NUMPY = r"""
import json, sys
import numpy as np
sys.path.insert(0, sys.argv[1])
import audio_refs

request = json.loads(sys.stdin.read())
wav = audio_refs.read_wav(request["path"])
rate = wav.rate
data = np.array(wav.channels, dtype=np.float64)
kind = request["kind"]


def span(x):
    start = int(round(request.get("start", 0.0) * rate))
    end = request.get("end")
    end = x.shape[-1] if end is None else int(round(end * rate))
    return x[..., max(start, 0):max(end, 0)]


if kind == "true_peak":
    # the 4x oversampled peak of each channel (the input itself included), as BS.1770-4 Annex 2
    fir = np.array(audio_refs.TRUE_PEAK_FIR)
    best = 0.0
    finite = np.where(np.isfinite(data), data, 0.0)
    for x in span(finite):
        best = max(best, float(np.max(np.abs(x))) if x.size else 0.0)
        padded = np.concatenate([np.zeros(11), x, np.zeros(11)])
        for phase in fir:
            y = np.convolve(padded, phase, mode="valid")
            if y.size:
                best = max(best, float(np.max(np.abs(y))))
    value = {"true_peak": best, "non_finite": int(np.count_nonzero(~np.isfinite(data)))}
elif kind == "spectrum":
    # dBFS (a full-scale sine reads 0) of the strongest bin within 1.5 bins of each frequency, Hann window
    x = span(data[request.get("channel", 0)])
    window = np.hanning(x.size)
    spectrum = np.abs(np.fft.rfft(x * window)) * 2.0 / window.sum()
    bin_hz = rate / x.size
    levels = []
    for f in request["freqs"]:
        k = f / bin_hz
        lo, hi = max(int(np.floor(k - 1.5)), 0), min(int(np.ceil(k + 1.5)), spectrum.size - 1)
        peak = float(spectrum[lo:hi + 1].max()) if hi >= lo else 0.0
        levels.append(20 * np.log10(peak) if peak > 0 else -300.0)
    value = {"levels": levels, "bin_hz": bin_hz}
else:
    raise SystemExit("unknown kind " + kind)
print(json.dumps(value))
"""


def _numpy(request: dict):
    here = str(Path(__file__).resolve().parent)
    request = dict(request, path=str(request["path"]))
    out = subprocess.run([_uv(), "run", "--quiet", "--no-project", "--with", "numpy", "python", "-c", _NUMPY, here],
                         input=json.dumps(request), capture_output=True, text=True, timeout=600)
    if out.returncode != 0:
        raise RuntimeError(f"numpy ({request['kind']}) failed: {out.stderr[-2000:]}")
    return json.loads(out.stdout.strip().splitlines()[-1])


def true_peak_dbtp(path, start: float = 0.0, end=None) -> float:
    """BS.1770-4 Annex 2 true peak (4x, the samples included) of a WAV over [start, end) s, by
    numpy (dBTP; non-finite samples count as 0)"""
    return amplitude_to_db(_numpy({"kind": "true_peak", "path": path, "start": start, "end": end})["true_peak"])


def non_finite_samples(path) -> int:
    """NaN and infinite samples of a WAV (all channels), by numpy"""
    return _numpy({"kind": "true_peak", "path": path})["non_finite"]


def spectrum_levels(path, freqs, start: float, end: float, channel: int = 0) -> list:
    """dBFS of each frequency's strongest FFT bin (within 1.5 bins; Hann window, a full-scale sine
    reads 0) over [start, end) s of a WAV's channel, by numpy"""
    return _numpy({"kind": "spectrum", "path": path, "freqs": list(freqs), "start": start, "end": end,
                   "channel": channel})["levels"]


def match_levels(lufs: list, true_peaks: list, gains: list, volumes: list, weights=None, ceiling: float = -1.0):
    """Match Clip Levels as sound layer phase 3 spec 5.6 states it, from its text: each clip's level as
    it plays is lufs + gain + volume_db; the target is their mean weighted by how often each plays
    (weights, else equal), lowered so that no clip is raised past `ceiling` dBTP as it plays (or past
    its own peak when that is higher) nor its volume_db past +12; the new volume_db is volume + target
    - level, clamped to -60..12 and rounded to 0.1. A clip with lufs None keeps its volume. Returns
    (new volumes, mean, target)."""
    weights = weights or [1.0] * len(lufs)
    levels = [None if l is None else l + g + v for l, g, v in zip(lufs, gains, volumes)]
    pairs = [(w, l) for w, l in zip(weights, levels) if l is not None]
    mean = sum(w * l for w, l in pairs) / sum(w for w, _ in pairs)
    target = mean
    for l, p, g, v in zip(levels, true_peaks, gains, volumes):
        if l is None:
            continue
        raise_db = 12.0 - v
        if p is not None:
            raise_db = min(raise_db, max(0.0, ceiling - (p + g + v)))
        target = min(target, l + raise_db)
    out = []
    for l, v in zip(levels, volumes):
        out.append(v if l is None else round(min(12.0, max(-60.0, v + target - l)), 1))
    return out, mean, target


if __name__ == "__main__":
    # a quick self-check of the signals and the WAV round trip
    rate = 48000
    tone = sine(1000.0, 0.25, rate, -20.0)
    tmp = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp") / "audio_refs_selfcheck.wav"
    for enc in (PCM16, PCM24, FLOAT32):
        write_wav(tmp, [tone], rate, enc)
        back = read_wav(tmp)
        assert back.encoding == enc and back.frames == len(tone), (enc, back.encoding, back.frames)
        assert max(abs(a - b) for a, b in zip(back.channels[0], tone)) < 1e-4, enc
    print("rms", rms_dbfs(tone), "freq", zero_crossing_frequency(tone, rate, 0.0, 0.25))


def write_meta_section(asset, name: str, lines: list) -> None:
    """Sets (or with no lines removes) a top-level section of an asset's S04 .meta, the way a hand
    edit would: every other section stays as it is. A missing .meta gets a fresh GUID."""
    import uuid
    meta = Path(str(asset) + ".meta")
    text = meta.read_text() if meta.exists() else f"metaVersion: 1\nguid: {uuid.uuid4().hex}\n"
    kept = []
    skipping = False
    for line in text.splitlines():
        if line and not line[0].isspace() and line[0] != "-":
            skipping = line.startswith(name + ":")
        if not skipping:
            kept.append(line)
    if lines:
        kept.append(name + ":")
        kept += ["  " + line for line in lines]
    meta.parent.mkdir(parents=True, exist_ok=True)
    meta.write_text("\n".join(kept) + "\n")


def write_import(asset, text: str) -> None:
    """The asset's import.audioImporter section, from its keys' YAML text ("" removes it)."""
    lines = [line for line in text.splitlines() if line.strip()]
    write_meta_section(asset, "import", ["audioImporter:", "  version: 1"] + ["  " + line for line in lines] if lines else [])
