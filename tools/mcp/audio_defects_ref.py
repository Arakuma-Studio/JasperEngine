"""The defect measurements of sound layer phase 2.5 (spec 2.2) and the click-safe fade weights
(spec 2.5.1), in numpy, written from the spec's text and never from the C++ (the tests' oracle for
AudioAnalysis::defects), plus the synthetic clips of spec 2.13.

Run with `uv run --no-project --with numpy --with soundfile python audio_defects_ref.py` and a JSON
request on stdin; the answer is one JSON line on stdout:

- {"op": "defects", "items": [{"path": ..., "region": {...}}, ...]}: each file decoded by soundfile
  (libsndfile: a second decoder beside SoLoud's) at its own rate, then measured on the played
  region {start_frame, end_frame (0: the file's end), fade_in_frames, fade_out_frames, gain_db};
  also "pitch" (Phase 1's method, for the aliasing gate) and "envelope_peak_ms" per item;
- {"op": "synth", "dir": ...}: writes the synthetic clips (48 kHz mono PCM16) and returns their
  names;
- {"op": "corpus", "dir": ...} and {"op": "followup", "dir": ...}: the phase 2.5 review's corpus
  (5.5) and the closure follow-ups' (5.6), written into dir; their file names, or their items;
- {"op": "weights", "n": ...}: the fade-in and fade-out weights of n frames, as float32 values;
- {"op": "second_differences", "path": ..., "count": ...}: the sample positions of the largest
  |second differences| of the channel mean (spec 3.1, ui/back's pulse period).

The interpretation choices where the spec leaves room are recorded in the phase 2.5 spec's 5.2
(frames: clicks from the region's start, whole frames only; tails aligned to its end; the
percentile is numpy's linear one; local maxima are greater than the left neighbour and at least
the right one; aliasing counts a frame's folds only when half its fold positions show a peak, a
family; a tail whose end is under TAIL_CUT_MIN_DB decayed, whichever rule)."""

from __future__ import annotations

import json
import math
import struct
import sys
from pathlib import Path

import numpy as np

# -- spec 2.2's constants (the same values SoundTypes.h's `-- defects` block holds; from the spec)

SILENCE_THRESHOLD_DBFS = -60.0
SILENT_PEAK_DBFS = -90.0
FLOOR_DB = -120.0
EDGE_MAX_DB = -40.0
EDGE_ZERO_CROSSING_STEPS = 2.0
CLICK_FRAME_MS = 1.0
CLICK_NEAR, CLICK_FAR, CLICK_MIN_CONTEXT = 3, 10, 4
CLICK_IMPULSE_SAMPLES = 4
CLICK_IMPULSE_SHARE = 0.7
CLICK_RISE_DB = 18.0
CLICK_STEADY_DB = 8.0
CLICK_AUDIBLE_DB = 60.0
CLICK_GUARD_MS = 3.0
CLICK_ATTACK_GUARD = 3
# 5.6: a candidate is the waveform, not a click, in an impulse texture (5% or more of the frames
# within +/- 250 ms impulsive), or when it recurs (an impulsive frame within +/- 50 ms, at most 18 dB
# under it, not the frames next to it) at a lag where the sound itself repeats: the channel mean over
# +/- max(lag / 2, 2.5 ms) around the impulse, both impulses' +/- 2 samples left out, correlates with
# its copy at the lag by 0.9 or more, peaking strictly inside lag +/- 3 samples. Never next to a
# dropout (a run of 2 or more samples of digital silence that 2.2.4's rule calls cut)
CLICK_RECUR_MS = 50.0
CLICK_RECUR_SAME_FRAMES = 1
CLICK_TEXTURE_MS = 250.0
CLICK_TEXTURE_SHARE = 0.05
CLICK_REPEAT_MIN = 0.9
CLICK_REPEAT_SEARCH = 3
CLICK_REPEAT_GUARD = 2
CLICK_REPEAT_MIN_HALF_MS = 2.5
CLICK_DROPOUT_GUARD = 2
CLICK_DROPOUT_MIN_SAMPLES = 2
# 5.6: crackle, an irregular field of clicks through the sound (fire, rain, vinyl, sparks): at least
# 8 clicks, 0.5 a second, spanning half the region, their intervals' coefficient of variation 0.5 or more
CRACKLE_MIN_CLICKS = 8
CRACKLE_MIN_PER_SECOND = 0.5
CRACKLE_MIN_SPREAD = 0.5
CRACKLE_MIN_CV = 0.5
TAIL_FRAME_MS = 5.0
TAIL_MIN_FRAMES = 12
TAIL_CUT_MIN_DB = -45.0
TAIL_DECAY_DB_PER_S = -20.0
# 5.5: the release, from 5 ms windows every 1 ms back from the end: the first that is within 3 dB of
# the decay line starts it; a raised-cosine fade-out of T ms is 3 dB down 0.636 T before its end
TAIL_RELEASE_STEP_MS = 1.0
TAIL_RELEASE_DROP_DB = 3.0
TAIL_RELEASE_SHARE = 0.636
TAIL_RELEASE_MIN_MS = 15.0
TAIL_RELEASE_FIX_MS = 20.0
GAP_AUDIBLE_DB = -60.0
# 5.5: re the played peak (1e-6 at a full-scale peak, as before)
GAP_ZERO_DB = -120.0
GAP_MIN_MS = 2.0
# 5.6: each side of a run against its own neighbourhood: the far window lies 0.5 to 2 ms from the
# sample next to the run; the near window is that sample and the ones before it (after, for the run's
# end) over a quarter of the local period (pi / 2 over the far window's sqrt(sum d^2 / sum x^2)),
# clamped to 0.5 .. 2 ms. A side is cut when its sample is at least -80 dBFS and within 25 dB of the
# far window's largest |x|; or, the other side being so too, within 35 dB with the near window's RMS
# at most 1 dB under the far window's (a cut at a zero crossing). A dropout: both sides cut; or one,
# with both far windows within 60 dB of the peak and 10 dB of each other (the sound goes on)
GAP_NEAR_MS = 0.5
GAP_FAR_MS = 2.0
GAP_CUT_DB = -25.0
GAP_ZERO_CROSSING_DB = -35.0
GAP_ZERO_CROSSING_ENV_DB = -1.0
GAP_FLOOR_DBFS = -80.0
GAP_CONTINUITY_DB = 10.0
# 5.5: the click-safe fade-in a note proposes (SoundSynth's rule): the longest, up to the kind's
# length, that takes at most 0.05 dB of the first 400 ms, at least 3 frames; else 2 frames
FADE_IN_MAX_LOSS_DB = 0.05
ATTACK_WINDOW_SECONDS = 0.4
FADE_IN_MIN_FRAMES = 3
FADE_IN_FLOOR_FRAMES = 2
CLICK_SAFE_FADE_IN_MS = 2.0
LISTED = 8
SPECTRAL_MAX_SECONDS = 10.0
FFT_SIZE, FFT_HOP = 2048, 512
RESONANCE_RANGE_DB = 30.0
TONAL_MIN_CONFIDENCE = 0.6
ALIASING_INFO_SHARE = 0.05
# a frame's folds count only when at least this share of its fold positions shows a peak (spec 5.2)
ALIASING_FAMILY_SHARE = 0.5
# 5.5: the aliasing frames (4096 points every 4096, Blackman-Harris: a main lobe of +/- 4 bins);
# a fold position is judged more than 7 bins (its +/- 2 and a harmonic's +/- 4, plus 1) from every
# harmonic; f0 refined from the frame's harmonics, first from the lowest 8, then from all
ALIASING_FFT_SIZE, ALIASING_HOP = 4096, 4096
ALIASING_HARMONIC_GUARD_BINS = 7
ALIASING_PEAK_BINS = 2
ALIASING_MEDIAN_BINS = 16
# 5.5: a fold's peak against the floor of its neighbours (their 25th percentile), not their median,
# which the next folds' peaks raise when they are dense
ALIASING_FLOOR_PERCENTILE = 25.0
ALIASING_BAND_MIN_DB = -60.0
ALIASING_PEAK_DB = 10.0
ALIASING_REFINE_FIRST = 8
CUT_WINDOW_MS = 0.5
ENVELOPE_PEAK_MS = 20.0


def lround(x: float) -> int:
    """C's lround: halves away from zero (Python's round() rounds them to even)"""
    return int(math.floor(abs(x) + 0.5)) * (1 if x >= 0 else -1)


def db(power_or_amplitude: float, power: bool = False) -> float:
    if power_or_amplitude <= 0:
        return FLOOR_DB
    return max(FLOOR_DB, (10.0 if power else 20.0) * math.log10(power_or_amplitude))


def r(value, step):
    """value to a multiple of step, halves away from zero (as the tools round)"""
    if value is None or not math.isfinite(value):
        return None
    return lround(value / step) * step + 0.0


# -- spec 2.5.1: the fade weights, in double, rounded to float once


def fade_weight(k: int, n: int, fade_in: bool) -> np.float32:
    if fade_in:
        return np.float32(0.5 * (1.0 - math.cos(math.pi * k / n)))
    return np.float32(0.5 * (1.0 + math.cos(math.pi * (k + 1) / n)))


def region_end(region: dict, frames: int) -> int:
    """the region's end frame: end_frame (missing or None: the file's end; 5.5: 0 is an empty region
    at the file's start), never before its start"""
    start = min(region.get("start_frame", 0), frames)
    end = region.get("end_frame")
    return frames if end is None else max(start, min(int(end), frames))


def played(x: np.ndarray, region: dict) -> np.ndarray:
    """The played signal of spec 2.2: the region's frames x the gain x the fade weights, in float32
    as a voice computes them"""
    frames = x.shape[1]
    start = min(region.get("start_frame", 0), frames)
    end = region_end(region, frames)
    y = x[:, start:end].astype(np.float32).copy()
    gain_db = float(region.get("gain_db", 0.0))
    if gain_db != 0.0:
        y *= np.float32(10.0 ** (np.float32(gain_db) / np.float32(20.0)))
    length = end - start
    n_in = min(region.get("fade_in_frames", 0), length)
    n_out = min(region.get("fade_out_frames", 0), length)
    if n_in > 0:
        w = np.array([fade_weight(k, n_in, True) for k in range(n_in)], dtype=np.float32)
        y[:, :n_in] *= w
    if n_out > 0:
        w = np.array([fade_weight(j, n_out, False) for j in range(n_out)], dtype=np.float32)
        y[:, length - n_out:] *= w
    return y


# -- Phase 1's pitch (normalised autocorrelation, lags for 50-2000 Hz, the first peak at 0.9 of the
# best, parabolic refinement), for the aliasing gate and per frame


def pitch_of_window(x: np.ndarray, rate: float):
    length = x.size
    min_lag = max(1, int(math.floor(rate / 2000.0)))
    max_lag = min(int(math.ceil(rate / 50.0)), length // 2)
    if max_lag <= min_lag + 1 or not np.any(x):
        return None, 0.0
    e = np.concatenate([[0.0], np.cumsum(x * x)])
    corr = np.zeros(max_lag + 2)
    for lag in range(min_lag - 1, max_lag + 2):
        if lag < 1 or lag >= length:
            continue
        count = length - lag
        s = float(np.dot(x[:count], x[lag:lag + count]))
        e0, e1 = e[count], e[length] - e[lag]
        corr[lag] = s / math.sqrt(e0 * e1) if e0 > 0 and e1 > 0 else 0.0
    peaks, best = [], 0.0
    for lag in range(min_lag, max_lag + 1):
        v = corr[lag]
        if v > 0 and v > corr[lag - 1] and v >= corr[lag + 1]:
            peaks.append(lag)
            best = max(best, v)
    chosen = next((lag for lag in peaks if corr[lag] >= 0.9 * best), None)
    if chosen is None:
        return None, 0.0
    a, b, c = corr[chosen - 1], corr[chosen], corr[chosen + 1]
    den = a - 2 * b + c
    shift = min(0.5, max(-0.5, 0.5 * (a - c) / den)) if den != 0 else 0.0
    return rate / (chosen + shift), min(1.0, max(0.0, b))


def pitch(y: np.ndarray, rate: float):
    """The pitch of the loudest 100 ms (the 1 ms RMS envelope of the channel maximum)"""
    n = y.shape[1]
    m = y.astype(np.float64).mean(axis=0)
    a = np.abs(y.astype(np.float64)).max(axis=0)
    half = lround(0.001 * rate) // 2
    c = np.concatenate([[0.0], np.cumsum(a * a)])
    i = np.arange(n)
    lo, hi = np.maximum(0, i - half), np.minimum(n - 1, i + half)
    env = np.sqrt(np.maximum(0.0, (c[hi + 1] - c[lo]) / (hi - lo + 1)))
    if env.max() <= 0:
        return None, 0.0
    at = int(np.argmax(env))
    length = min(n, lround(0.1 * rate))
    start = min(max(at - length // 2, 0), n - length)
    return pitch_of_window(m[start:start + length], rate)


def envelope_peak_ms(y: np.ndarray, rate: float):
    """onset.envelope_peak_ms: the first maximum of the 20 ms centred RMS envelope of the mean"""
    m = y.astype(np.float64).mean(axis=0)
    n = m.size
    half = lround(ENVELOPE_PEAK_MS / 1000.0 * rate) // 2
    c = np.concatenate([[0.0], np.cumsum(m * m)])
    i = np.arange(n)
    lo, hi = np.maximum(0, i - half), np.minimum(n - 1, i + half)
    env = np.sqrt(np.maximum(0.0, (c[hi + 1] - c[lo]) / (hi - lo + 1)))
    if env.max() <= 0:
        return None
    return r(int(np.argmax(env)) / rate * 1000.0, 0.1)


# -- spec 2.2.1 edges (and 5.5: the longest click-safe fade-in the attack allows)


def safe_fade_in_frames(squares: np.ndarray, rate: float, longest: int) -> int:
    """5.5 (SoundSynth's rule, shared): the longest raised-cosine fade-in of at most `longest` and at
    least FADE_IN_MIN_FRAMES frames that takes at most FADE_IN_MAX_LOSS_DB of the energy of the first
    ATTACK_WINDOW_SECONDS; 0 when even the shortest takes more. squares: each frame's energy (the
    sum over the channels of x^2)"""
    longest = min(longest, squares.size)
    if longest < FADE_IN_MIN_FRAMES:
        return longest
    window = min(squares.size, lround(ATTACK_WINDOW_SECONDS * rate))
    allowed = float(squares[:window].sum()) * (1.0 - 10 ** (-FADE_IN_MAX_LOSS_DB / 10.0))
    for fade in range(longest, FADE_IN_MIN_FRAMES - 1, -1):
        loss = 0.0
        for k in range(fade):
            w = float(fade_weight(k, fade, True))
            loss += float(squares[k]) * (1.0 - w * w)
        if loss <= allowed:
            return fade
    return 0


def edge_click(v: float, d: float, peak: float) -> bool:
    """an edge sample v (d: the outward step) clicks: loud against the sound, audible, and not moving
    toward 0 fast enough to reach it within two steps (a zero crossing)"""
    crossing = np.sign(d) != np.sign(v) and abs(v) <= EDGE_ZERO_CROSSING_STEPS * abs(d)
    return bool(v != 0 and 20 * math.log10(abs(v) / peak) > EDGE_MAX_DB and 20 * math.log10(abs(v)) >= SILENCE_THRESHOLD_DBFS
                and not crossing)


def edges(y: np.ndarray, peak: float, rate: float) -> dict:
    out = {}
    for side in ("start", "end"):
        clicks, largest = False, 0.0
        for x in y.astype(np.float64):
            v, d = (x[0], x[0] - x[1]) if side == "start" else (x[-1], x[-1] - x[-2])
            largest = max(largest, abs(v))
            clicks = clicks or bool(edge_click(v, d, peak))
        out[side + "_db"] = r(20 * math.log10(largest / peak), 0.1) if largest > 0 else None
        out[side + "_click"] = clicks
    squares = (y.astype(np.float64) ** 2).sum(axis=0)
    longest = lround(CLICK_SAFE_FADE_IN_MS * rate / 1000.0)
    return {"start_db": out["start_db"], "end_db": out["end_db"], "start_click": out["start_click"], "end_click": out["end_click"],
            "safe_fade_in_frames": safe_fade_in_frames(squares, rate, longest)}


# -- spec 2.2.2 clicks inside (5.5: the recurring-feature test is local; 5.6: the waveform is a
# texture or a repeat of the sound itself, a dropout's edges are always clicks, crackle is a texture)


def repeats_at(m: np.ndarray, p: int, q: int, rate: float) -> bool:
    """the sound around the impulse at p repeats at the lag q - p (both impulses left out)"""
    lag = abs(q - p)
    sign = 1 if q > p else -1
    half = max(lag // 2, lround(CLICK_REPEAT_MIN_HALF_MS * rate / 1000.0))
    best, best_at = -2.0, 0
    for d in range(-CLICK_REPEAT_SEARCH, CLICK_REPEAT_SEARCH + 1):
        a0, b0 = p - half, p - half + sign * (lag + d)
        if min(a0, b0) < 0 or max(a0, b0) + 2 * half >= m.size:
            return False
        a = m[a0:a0 + 2 * half + 1].astype(np.float64).copy()
        b = m[b0:b0 + 2 * half + 1].astype(np.float64).copy()
        keep = np.ones(a.size, dtype=bool)
        for c in (p - a0, q - b0):
            keep[max(0, c - CLICK_REPEAT_GUARD):c + CLICK_REPEAT_GUARD + 1] = False
        a, b = a[keep], b[keep]
        den = math.sqrt(float(np.dot(a, a)) * float(np.dot(b, b)))
        r = float(np.dot(a, b)) / den if den > 0 else 0.0
        if r > best:
            best, best_at = r, d
    return best >= CLICK_REPEAT_MIN and abs(best_at) < CLICK_REPEAT_SEARCH


def crackle(times: list, seconds: float) -> bool:
    n = len(times)
    if n < CRACKLE_MIN_CLICKS or n < CRACKLE_MIN_PER_SECOND * seconds or times[-1] - times[0] < CRACKLE_MIN_SPREAD * seconds:
        return False
    intervals = np.diff(np.array(times))
    mean = float(intervals.mean())
    return mean > 0 and float(intervals.std()) / mean >= CRACKLE_MIN_CV


def clicks(m: np.ndarray, rate: float) -> dict:
    n = m.size
    f = lround(rate * CLICK_FRAME_MS / 1000.0)
    k_count = n // f
    d2 = np.zeros(n)
    if n >= 3:
        d2[1:-1] = m[2:] - 2 * m[1:-1] + m[:-2]
    found = []
    if k_count > 0:
        d2f = (d2[:k_count * f] ** 2).reshape(k_count, f)
        mf = (m[:k_count * f] ** 2).reshape(k_count, f)
        e = np.array([db(v, True) for v in d2f.mean(axis=1)])
        l = np.array([db(v, True) for v in mf.mean(axis=1)])
        total = d2f.sum(axis=1)
        top = np.sort(d2f, axis=1)[:, ::-1][:, :CLICK_IMPULSE_SAMPLES].sum(axis=1)
        share = np.where(total > 0, top / np.where(total > 0, total, 1), 0.0)
        impulsive = share >= CLICK_IMPULSE_SHARE
        # each frame's impulse: its largest |second difference| (the first, on a tie)
        at = np.argmax(np.abs(d2[:k_count * f]).reshape(k_count, f), axis=1) + np.arange(k_count) * f
        lmax = l.max()
        loudest = int(np.argmax(l))
        guard = lround(CLICK_GUARD_MS / CLICK_FRAME_MS)
        recur = lround(CLICK_RECUR_MS / CLICK_FRAME_MS)
        texture = lround(CLICK_TEXTURE_MS / CLICK_FRAME_MS)
        count = np.concatenate([[0], np.cumsum(impulsive.astype(np.int64))])
        edges = None
        hits = []
        for k in range(k_count):
            pre = e[max(0, k - CLICK_FAR):max(0, k - CLICK_NEAR + 1)]
            post = e[min(k_count, k + CLICK_NEAR):min(k_count, k + CLICK_FAR + 1)]
            if pre.size < CLICK_MIN_CONTEXT or post.size < CLICK_MIN_CONTEXT:
                continue
            a, b = float(np.median(pre)), float(np.median(post))
            if not (e[k] - max(a, b) > CLICK_RISE_DB and abs(a - b) < CLICK_STEADY_DB and impulsive[k]
                    and l[k] > lmax - CLICK_AUDIBLE_DB and k > guard and (k_count - 1 - k) > guard
                    and abs(k - loudest) > CLICK_ATTACK_GUARD):
                continue
            p = int(at[k])
            # a dropout's edges are clicks, whatever surrounds them
            if edges is None:
                edges = dropout_edges(m, rate)
            if any(abs(p - x) <= CLICK_DROPOUT_GUARD for x in edges):
                hits.append(k)
                continue
            # an impulse texture (sample-and-hold noise, a buzz, a pulse wave)
            lo, hi = max(0, k - texture), min(k_count, k + texture + 1)
            if (count[hi] - count[lo]) / (hi - lo) >= CLICK_TEXTURE_SHARE:
                continue
            # the same impulse where the sound itself repeats (the waveform's period)
            near = [j for j in range(max(0, k - recur), min(k_count, k + recur + 1))
                    if abs(j - k) > CLICK_RECUR_SAME_FRAMES and impulsive[j] and e[j] >= e[k] - CLICK_RISE_DB]
            if any(repeats_at(m, p, int(at[j]), rate) for j in near):
                continue
            hits.append(k)
        group = []
        for k in hits + [None]:
            if group and (k is None or k != group[-1] + 1):
                found.append((group[0] + group[-1] + 1) / 2.0 * f / rate)
                group = []
            if k is not None:
                group.append(k)
    return {"count": len(found), "times_s": [r(t, 0.001) for t in found[:LISTED]], "crackle": crackle(found, n / rate)}


# -- spec 2.2.3 truncated tail (5.5: the release, monotonic in the fade-out)


def tail(m: np.ndarray, rate: float, fade_out_frames: int = 0):
    n = m.size
    f = lround(rate * TAIL_FRAME_MS / 1000.0)
    k_count = n // f
    if k_count < TAIL_MIN_FRAMES:
        return None
    offset = n - k_count * f
    frames = (m[offset:] ** 2).reshape(k_count, f).mean(axis=1)
    raw = np.array([db(v, True) for v in frames])
    ip = int(np.argmax(raw))
    reference = raw[ip]
    e = raw - reference
    if k_count - 1 - ip < TAIL_MIN_FRAMES:
        return None
    end = float(e[-2:].mean())
    lo, hi = max(ip, k_count - 30), k_count - 9
    t = np.arange(lo, hi + 1) * f / rate
    slope, intercept = np.polyfit(t, e[lo:hi + 1], 1)
    slope, intercept = float(slope), float(intercept)
    # the release: how long before the end the sound falls TAIL_RELEASE_DROP_DB under its decay line
    # (5 ms windows ending 0, 1, 2, ... ms before the end, up to the line's last frame)
    step = max(1, lround(rate * TAIL_RELEASE_STEP_MS / 1000.0))
    squares = np.concatenate([[0.0], np.cumsum(m.astype(np.float64) ** 2)])
    departure = None
    j = 0
    while True:
        stop = n - j * step
        start = stop - f
        if start < offset + hi * f:
            break
        level = db((squares[stop] - squares[start]) / f, True) - reference
        line = intercept + slope * ((start - offset) / rate)
        if level >= line - TAIL_RELEASE_DROP_DB:
            departure = j * step / rate * 1000.0
            break
        j += 1
    measured = ((departure if departure is not None else j * step / rate * 1000.0) + TAIL_FRAME_MS / 2) / TAIL_RELEASE_SHARE
    release = max(measured, fade_out_frames * 1000.0 / rate)
    # each condition only gets less true as a fade-out gets longer: the fade lowers every frame (the
    # end level), moves the release's start back, and leaves the line alone until it is 40 ms long
    cut, how = None, None
    if slope < TAIL_DECAY_DB_PER_S and end >= TAIL_CUT_MIN_DB and release < TAIL_RELEASE_MIN_MS:
        cut, how = end, "stop" if departure == 0 else "fade"
    return {"cut_db": r(cut, 0.1), "how": how, "slope_db_per_s": r(slope, 0.1), "end_db": r(end, 0.1),
            "release_ms": r(release, 0.1)}


# -- spec 2.2.4 gaps (5.5: the zero level re the peak; 5.6: each side of a run judged against its
# own neighbourhood, see GAP_* above)


def side(x: np.ndarray, b: int, before: bool, rate: float):
    """(|x| at b over the channels, its dB re the far window's largest |x|, the near window's RMS re
    the far window's in dB, the far window's largest |x|) or None; x: channels x n float64"""
    near_n, far_n = lround(GAP_NEAR_MS * rate / 1000.0), lround(GAP_FAR_MS * rate / 1000.0)
    n = x.shape[1]
    lo, hi = (max(0, b - far_n), b - near_n) if before else (b + near_n + 1, min(n, b + far_n + 1))
    if hi - lo < 2:
        return None
    far = x[:, lo:hi]
    largest = float(np.abs(far).max())
    if largest <= 0:
        return None
    rms = float(np.sqrt((far ** 2).mean(axis=1)).max())
    energy = float((far[:, 1:] ** 2).sum())
    w = math.sqrt(float((np.diff(far, axis=1) ** 2).sum()) / energy) if energy > 0 else 0.0
    q = near_n if w <= 0 else int(max(near_n, min(far_n, math.ceil(math.pi / 2 / w))))
    near = x[:, max(0, b - q + 1):b + 1] if before else x[:, b:min(n, b + q)]
    near_rms = float(np.sqrt((near ** 2).mean(axis=1)).max())
    v = float(np.abs(x[:, b]).max())
    to_db = lambda value: 20 * math.log10(value) if value > 0 else -math.inf  # noqa: E731
    return v, to_db(v / largest), to_db(near_rms / rms) if rms > 0 else -math.inf, largest


def dropout(x: np.ndarray, s: int, e: int, peak: float, rate: float) -> bool:
    """the run [s, e) of digital silence is a dropout (5.6): cut on both sides, or on one while the sound goes on"""
    sides = (side(x, s - 1, True, rate), side(x, e, False, rate))

    def cut(sd, zero_crossing):
        if sd is None or sd[0] <= 0 or 20 * math.log10(sd[0]) < GAP_FLOOR_DBFS:
            return False
        return sd[1] >= GAP_CUT_DB or zero_crossing and sd[1] >= GAP_ZERO_CROSSING_DB and sd[2] >= GAP_ZERO_CROSSING_ENV_DB

    if all(cut(sd, True) for sd in sides):
        return True
    if any(cut(sd, False) for sd in sides) and all(sd is not None for sd in sides):
        levels = [20 * math.log10(sd[3] / peak) for sd in sides]
        return min(levels) >= GAP_AUDIBLE_DB and abs(levels[0] - levels[1]) <= GAP_CONTINUITY_DB
    return False


def zero_runs(a: np.ndarray, level: float):
    """[start, end) of each run of samples at or under level"""
    z = (a <= level).astype(np.int8)
    idx = np.flatnonzero(np.diff(np.concatenate([[0], z, [0]])))
    return list(zip(idx[::2].tolist(), idx[1::2].tolist()))


def dropout_edges(m: np.ndarray, rate: float) -> list:
    """the samples next to each dropout of the channel mean: a run of 2 or more samples of digital
    silence (re its peak) inside it that 2.2.4's rule calls cut"""
    x = m.astype(np.float64)[None, :]
    peak = float(np.abs(x).max())
    out = []
    if peak <= 0:
        return out
    for s, e in zero_runs(np.abs(x[0]), peak * 10 ** (GAP_ZERO_DB / 20)):
        if e - s >= CLICK_DROPOUT_MIN_SAMPLES and s > 0 and e < x.shape[1] and dropout(x, s, e, peak, rate):
            out += [s - 1, e]
    return out


def gaps(y: np.ndarray, peak: float, rate: float) -> dict:
    x = y.astype(np.float64)
    a = np.abs(x).max(axis=0)
    audible = np.nonzero(a > peak * 10 ** (GAP_AUDIBLE_DB / 20))[0]
    found = []
    if audible.size:
        first, last = int(audible[0]), int(audible[-1])
        for s0, e0 in zero_runs(a[first:last + 1], peak * 10 ** (GAP_ZERO_DB / 20)):
            s, e = first + s0, first + e0  # the run [s, e): s - 1 and e are sound
            if (e - s) * 1000.0 / rate >= GAP_MIN_MS and dropout(x, s, e, peak, rate):
                found.append((s, e - s))
    return {"count": len(found), "longest_ms": r(max((l for _, l in found), default=0) * 1000.0 / rate, 0.1),
            "times_s": [r(s / rate, 0.001) for s, _ in found[:LISTED]]}


# -- spectral frames (Phase 1's: 2048-point periodic Hann, hop 512, the last one zero-padded)


def spectral_frames(m: np.ndarray) -> np.ndarray:
    n = m.size
    count = 0 if n == 0 else (1 if n <= FFT_SIZE else 1 + (n - FFT_SIZE + FFT_HOP - 1) // FFT_HOP)
    window = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(FFT_SIZE) / FFT_SIZE)
    padded = np.concatenate([m, np.zeros(FFT_SIZE + count * FFT_HOP)])
    frames = np.stack([padded[i * FFT_HOP:i * FFT_HOP + FFT_SIZE] * window for i in range(count)])
    return np.abs(np.fft.rfft(frames, axis=1)) ** 2


def weight(hz: float) -> float:
    if 2000.0 <= hz <= 6000.0:
        return 1.0
    if 1000.0 <= hz <= 9000.0:
        return 0.7
    return 0.5


def resonance(power: np.ndarray, rate: float):
    s = power.sum(axis=0)
    level = np.array([db(v, True) for v in s])
    bin_hz = rate / FFT_SIZE
    top = min(16000.0, 0.45 * rate)
    grid = []
    i = 0
    while 100.0 * 2 ** (i / 48.0) <= top:
        grid.append(100.0 * 2 ** (i / 48.0))
        i += 1
    lg = []
    for g in grid:
        lo, hi = g * 2 ** (-1 / 96), g * 2 ** (1 / 96)
        k0, k1 = int(math.ceil(lo / bin_hz)), int(math.floor(hi / bin_hz))
        lg.append(float(level[k0:k1 + 1].max()) if k1 >= k0 else float(level[lround(g / bin_hz)]))
    lg = np.array(lg)
    count = lg.size
    prom = np.array([lg[i] - np.median(lg[max(0, i - 16):min(count, i + 17)]) for i in range(count)])
    best = None
    for i in range(count):
        left = i == 0 or prom[i] > prom[i - 1]
        right = i == count - 1 or prom[i] >= prom[i + 1]
        if left and right and prom[i] > 0 and lg[i] > lg.max() - RESONANCE_RANGE_DB:
            score = prom[i] * weight(grid[i])
            if best is None or score > best[0]:
                best = (score, grid[i], prom[i])
    if best is None:
        return {"score_db": None, "hz": None, "prominence_db": None}
    return {"score_db": r(best[0], 0.1), "hz": r(best[1], 1.0), "prominence_db": r(best[2], 0.1)}


def aliasing_frames(m: np.ndarray):
    """5.5: the aliasing measurement's own frames: ALIASING_FFT_SIZE points every ALIASING_HOP, a
    4-term Blackman-Harris window (sidelobes under -92 dB, so a harmonic's leakage never reads as a
    fold), the last one zero-padded; their power spectra and start samples"""
    n, size, hop = m.size, ALIASING_FFT_SIZE, ALIASING_HOP
    count = 0 if n == 0 else (1 if n <= size else 1 + (n - size + hop - 1) // hop)
    j = np.arange(size)
    window = 0.35875 - 0.48829 * np.cos(2 * np.pi * j / size) + 0.14128 * np.cos(4 * np.pi * j / size) - 0.01168 * np.cos(6 * np.pi * j / size)
    padded = np.concatenate([m, np.zeros(size + count * hop)])
    frames = np.stack([padded[i * hop:i * hop + size] * window for i in range(count)])
    return np.abs(np.fft.rfft(frames, axis=1)) ** 2


def peak_bin(pdb: np.ndarray, b: int):
    """the largest bin within +/- ALIASING_PEAK_BINS of b, when it is a local maximum at least
    ALIASING_PEAK_DB over the median of +/- ALIASING_MEDIAN_BINS; its parabolic (dB) position, else None"""
    lo, hi = max(1, b - ALIASING_PEAK_BINS), min(pdb.size - 2, b + ALIASING_PEAK_BINS)
    if hi < lo:
        return None
    k = lo + int(np.argmax(pdb[lo:hi + 1]))
    around = pdb[max(0, k - ALIASING_MEDIAN_BINS):min(pdb.size, k + ALIASING_MEDIAN_BINS + 1)]
    if pdb[k] < np.median(around) + ALIASING_PEAK_DB or not (pdb[k] > pdb[k - 1] and pdb[k] >= pdb[k + 1]):
        return None
    a, b0, c = pdb[k - 1], pdb[k], pdb[k + 1]
    den = a - 2 * b0 + c
    return k + (0.5 * (a - c) / den if den != 0 else 0.0)


def refined_f0(pdb: np.ndarray, f0: float, rate: float) -> float:
    """5.5: f0 from the frame's own harmonics (their peaks, parabolic in dB), by least squares
    through the origin: first from the harmonics up to ALIASING_REFINE_FIRST, then from every one
    under Nyquist"""
    bin_hz = rate / ALIASING_FFT_SIZE
    nyquist = rate / 2
    for most in (ALIASING_REFINE_FIRST, None):
        num = den = 0.0
        found = 0
        j = 1
        while j * f0 < nyquist and (most is None or j <= most):
            at = peak_bin(pdb, lround(j * f0 / bin_hz))
            if at is not None:
                num += j * at * bin_hz
                den += j * j
                found += 1
            j += 1
        if found >= 2:
            f0 = num / den
    return f0


def aliasing(m: np.ndarray, rate: float):
    power = aliasing_frames(m)
    bin_hz = rate / ALIASING_FFT_SIZE
    nyquist = rate / 2
    frame_power = power.sum(axis=1)
    loudest = frame_power.max()
    hi_hz = min(20000.0, nyquist)
    band = [k for k in range(power.shape[1]) if 4000.0 <= k * bin_hz <= hi_hz]
    inband = set(band)
    folds = []
    for i in range(power.shape[0]):
        if frame_power[i] <= 0 or db(frame_power[i] / loudest, True) < -30.0:
            continue
        # Phase 1's pitch of the frame's middle FFT_SIZE samples, then refined from its harmonics
        start = min(i * ALIASING_HOP + (ALIASING_FFT_SIZE - FFT_SIZE) // 2, max(0, m.size - FFT_SIZE))
        f0, confidence = pitch_of_window(m[start:start + FFT_SIZE].astype(np.float64), rate)
        if f0 is None or confidence < TONAL_MIN_CONFIDENCE:
            continue
        p = power[i]
        pdb = np.array([db(v, True) for v in p])
        f0 = refined_f0(pdb, f0, rate)
        harmonics = [j * f0 for j in range(1, int(nyquist / f0) + 2) if j * f0 < nyquist]
        # the family of every partial, and of the odd ones (a square's)
        judged, hits, counted = [0, 0], [0, 0], [set(), set()]
        k = 1
        while k * f0 <= 4 * rate:
            if k * f0 > nyquist:
                pos = abs(k * f0 - rate * float(np.sign(k * f0 / rate) * math.floor(abs(k * f0 / rate) + 0.5)))
                if 4000.0 <= pos <= hi_hz and all(abs(pos - h) > ALIASING_HARMONIC_GUARD_BINS * bin_hz for h in harmonics):
                    b = lround(pos / bin_hz)
                    near = pdb[max(0, b - ALIASING_PEAK_BINS):min(p.size, b + ALIASING_PEAK_BINS + 1)]
                    around = pdb[max(0, b - ALIASING_MEDIAN_BINS):min(p.size, b + ALIASING_MEDIAN_BINS + 1)]
                    peak = near.max() >= float(np.percentile(around, ALIASING_FLOOR_PERCENTILE)) + ALIASING_PEAK_DB
                    for family in ((0, 1) if k % 2 == 1 else (0,)):
                        judged[family] += 1
                        if peak:
                            hits[family] += 1
                            counted[family].update(range(max(0, b - ALIASING_PEAK_BINS), min(p.size, b + ALIASING_PEAK_BINS + 1)))
            k += 1
        total = float(p[band].sum())
        if total <= 0:
            continue
        # 5.5: a band under -60 dB of the frame holds nothing audible to fold (a pure tone's
        # quantization noise lies on its own harmonic grid, and would read as folds)
        if db(total / frame_power[i], True) < ALIASING_BAND_MIN_DB:
            folds.append(0.0)
            continue
        # a family, not a partial that happens to sit at a fold position (spec 5.2): every partial's,
        # else the odd partials' (5.5)
        chosen = next((fam for fam in (0, 1) if judged[fam] > 0 and hits[fam] >= ALIASING_FAMILY_SHARE * judged[fam]), None)
        folds.append(0.0 if chosen is None else sum(float(p[b]) for b in counted[chosen] if b in inband) / total)
    if not folds:
        return {"fold": None, "frames": 0}
    return {"fold": r(float(np.median(folds)), 0.001), "frames": len(folds)}


def cut_level_db(x: np.ndarray, frame: int, rate: float):
    peak = float(np.abs(x).max())
    if peak <= 0:
        return FLOOR_DB
    h = lround(CUT_WINDOW_MS / 1000.0 * rate)
    lo, hi = max(0, frame - h), min(x.shape[1] - 1, frame + h)
    return db(float(np.abs(x[:, lo:hi + 1]).max()) / peak)


def defects(x: np.ndarray, rate: float, region: dict):
    frames = x.shape[1]
    start = min(region.get("start_frame", 0), frames)
    end = region_end(region, frames)
    y = played(x, region)
    if y.shape[1] < 2:
        # 5.5: trims that leave under 2 frames play nothing: said, not measured
        return {"empty": True, "frames": int(y.shape[1])} if start > 0 or end < frames else None
    peak = float(np.abs(y.astype(np.float64)).max())
    if peak <= 0 or 20 * math.log10(peak) < SILENT_PEAK_DBFS:
        return None
    m = y.astype(np.float64).mean(axis=0)
    n_out = min(region.get("fade_out_frames", 0), y.shape[1])
    out = {"edges": edges(y, peak, rate), "clicks": clicks(m, rate), "tail": tail(m, rate, n_out), "gaps": gaps(y, peak, rate)}
    if y.shape[1] / rate <= SPECTRAL_MAX_SECONDS:
        power = spectral_frames(m)
        out["resonance"] = resonance(power, rate)
        f0, confidence = pitch(y, rate)
        out["aliasing"] = aliasing(m, rate) if confidence >= TONAL_MIN_CONFIDENCE else None
    else:
        out["resonance"] = None
        out["aliasing"] = None
    out["cuts"] = {"head_db": r(cut_level_db(x, start, rate), 0.1) if start > 0 else None,
                   "tail_db": r(cut_level_db(x, end, rate), 0.1) if end < frames else None}
    return out


# -- spec 2.3.2: which defects become notes (the table, re-implemented here)

DESIGNED_END = {"glitch", "ui", "zap", "pickup", "jump", "stinger"}
ONE_SHOT = {"transient", "tonal", "swell", "riser", "speech"}


def note_ids(d, kind: str, type_name: str = "") -> list:
    """The new note ids of spec 2.3.1 for a defects object, gated by kind and type (2.3.2)"""
    if d is None:
        return []
    if d.get("empty"):
        return ["empty_region"]
    designed = type_name in DESIGNED_END
    e, out = d["edges"], []
    if kind in ONE_SHOT and (e["start_click"] or e["end_click"]) or kind == "loop" and e["start_click"]:
        out.append("edge_click")
    if kind != "glitch" and d["clicks"]["count"] > 0:
        out.append("crackle" if d["clicks"].get("crackle") else "clicks")
    if kind in ONE_SHOT and not designed and d["tail"] and d["tail"]["cut_db"] is not None:
        out.append("truncated_tail")
    if kind != "glitch" and d["gaps"]["count"] > 0:
        out.append("silence_gap")
    res = d["resonance"]
    if kind in ("swell", "riser", "speech") and res and res["score_db"] is not None and res["score_db"] > 15.0:
        out.append("harsh_resonance")
    al = d["aliasing"]
    if kind in ("transient", "tonal", "swell", "riser", "glitch") and al and al["fold"] is not None and al["fold"] > 0.05:
        out.append("aliasing")
    cuts = d["cuts"]
    if any(v is not None and v > -30.0 for v in (cuts["head_db"], cuts["tail_db"])):
        out.append("trim_cut")
    return out


def read(path: str):
    import soundfile
    data, rate = soundfile.read(path, dtype="float32", always_2d=True)
    return np.ascontiguousarray(data.T), float(rate)


# -- spec 2.13's synthetic clips (48 kHz mono)

RATE = 48000


def raised_fade(x: np.ndarray, rate: int, in_ms: float, out_ms: float) -> np.ndarray:
    x = x.copy()
    n_in, n_out = lround(in_ms * rate / 1000), lround(out_ms * rate / 1000)
    if n_in:
        x[:n_in] *= 0.5 * (1 - np.cos(np.pi * np.arange(n_in) / n_in))
    if n_out:
        x[x.size - n_out:] *= 0.5 * (1 + np.cos(np.pi * (np.arange(n_out) + 1) / n_out))
    return x


def polyblep_saw(freq: float, seconds: float, rate: int) -> np.ndarray:
    n = lround(seconds * rate)
    dt = freq / rate
    out = np.zeros(n)
    phase = 0.0
    for i in range(n):
        v = 2 * phase - 1
        if phase < dt:
            t = phase / dt
            v -= t + t - t * t - 1
        elif phase > 1 - dt:
            t = (phase - 1) / dt
            v -= t * t + t + t + 1
        out[i] = v
        phase += dt
        if phase >= 1:
            phase -= 1
    return out


def blep(t: float, dt: float) -> float:
    """the polyBLEP correction of a unit step at phase 0, t the phase in [0, 1)"""
    if t < dt:
        t /= dt
        return t + t - t * t - 1
    if t > 1 - dt:
        t = (t - 1) / dt
        return t * t + t + t + 1
    return 0.0


def polyblep_square(freq: float, n: int, rate: int) -> np.ndarray:
    dt = freq / rate
    out = np.zeros(n)
    phase = 0.0
    for i in range(n):
        v = 1.0 if phase < 0.5 else -1.0
        out[i] = v + blep(phase, dt) - blep((phase + 0.5) % 1.0, dt)
        phase = (phase + dt) % 1.0
    return out


def synth() -> dict:
    """name -> samples (float64 in [-1, 1]); the names and expectations are spec 2.13's table"""
    rate = RATE
    a6 = 10 ** (-6 / 20)
    t = lambda seconds: np.arange(lround(seconds * rate)) / rate  # noqa: E731
    clips = {}
    # a 200 Hz cosine from its peak (a step from silence), 50 ms decay
    clips["start_step"] = a6 * np.cos(2 * np.pi * 200 * t(0.3)) * np.exp(-t(0.3) / 0.05)
    # a 1 kHz sine whose last sample is a peak: 12 + 48 k samples, k = 250
    i = np.arange(12 + 48 * 250 + 1)
    clips["end_step"] = a6 * np.sin(2 * np.pi * 1000 * i / rate)
    i = np.arange(12000)
    clips["whole_cycles_1k"] = a6 * np.sin(2 * np.pi * 1000 * i / rate)
    clips["whole_cycles_5k"] = a6 * np.sin(2 * np.pi * 5000 * i / rate)
    # a 500 Hz tone decaying over 150 ms, faded out over its last 100 ms (no tail or edge of its own)
    tone = raised_fade(0.8 * np.sin(2 * np.pi * 500 * t(0.6)) * np.exp(-t(0.6) / 0.15), rate, 0, 100)
    dropout = tone.copy()
    at = lround(0.2005 * rate)          # a peak of the 500 Hz sine: 24 + 96 k
    dropout[at - 1:at + 2] = 0.0
    clips["dropout"] = dropout
    spike = tone.copy()
    spike[lround(0.25 * rate)] += 0.5
    clips["spike"] = spike
    gap = tone.copy()
    gap[lround(0.2 * rate):lround(0.205 * rate)] = 0.0
    clips["gap"] = gap
    # a 131 Hz pulse wave with 3-sample edges (as ui/back), 200 ms decay, faded in and out
    period = rate / 131.0
    ph = (np.arange(lround(0.4 * rate)) % period)
    pulse = np.where(ph < period / 2, 1.0, -1.0)
    for edge in (0.0, period / 2):
        ramp = (ph >= edge) & (ph < edge + 3)
        sign = 1.0 if edge == 0.0 else -1.0
        pulse = np.where(ramp, sign * (-1 + 2 * (ph - edge) / 3), pulse)
    clips["pulse_131"] = raised_fade(0.5 * pulse * np.exp(-t(0.4) / 0.2), rate, 2, 100)
    clips["square_100"] = raised_fade(0.5 * np.sign(np.sin(2 * np.pi * 100 * t(0.4) + 1e-9)) * np.exp(-t(0.4) / 0.2), rate, 2, 100)
    # 440 Hz, 150 ms exponential decay (amplitude e^(-t/0.15)), cut where it is 20 dB under its start
    cut = math.log(10) * 0.15
    ring = 0.8 * np.sin(2 * np.pi * 440 * t(cut)) * np.exp(-t(cut) / 0.15)
    clips["tail_cut"] = ring
    clips["tail_cut_fast_fade"] = raised_fade(ring, rate, 0, 5)
    # the 5 ms fade leaves the last 10 ms only 4.5 dB under the level 40 ms before the end, so
    # 2.2.3 reads it as a stop; a 20 ms fade is the fast final fade
    clips["tail_cut_fade_20"] = raised_fade(ring, rate, 0, 20)
    clips["tail_long_fade"] = raised_fade(ring, rate, 0, 100)
    # a 300 Hz square, linear decay to 0 (faded in over 2 ms: its start is not the case)
    sq = np.sign(np.sin(2 * np.pi * 300 * t(0.3) + 1e-9)) * 0.5 * (1 - t(0.3) / 0.3)
    clips["linear_decay"] = raised_fade(sq, rate, 2, 0)
    # decaying noise, with and without a 3 kHz ring, faded in and out
    rng = np.random.default_rng(25)
    noise = rng.standard_normal(lround(0.5 * rate)) * 0.15 * np.exp(-t(0.5) / 0.2)
    clips["noise"] = raised_fade(noise, rate, 2, 100)
    clips["noise_ring"] = raised_fade(noise + 0.2 * np.sin(2 * np.pi * 3000 * t(0.5)) * np.exp(-t(0.5) / 0.2), rate, 2, 100)
    # naive and polyBLEP saws of 32.5 samples a period (1,476.9 Hz: their folds land halfway
    # between harmonics, where a 1.5 kHz saw's, 32 samples a period, would land on them)
    f0 = rate / 32.5
    naive = 2 * ((np.arange(lround(0.5 * rate)) * f0 / rate) % 1.0) - 1
    clips["saw_naive"] = raised_fade(0.5 * naive, rate, 2, 100)
    clips["saw_blep"] = raised_fade(0.5 * polyblep_saw(f0, 0.5, rate), rate, 2, 100)
    # 100 ms of zeros, then a step to -6 dBFS (start_step's tone) or a 10 ms rise into it (a 5 ms
    # raised-cosine rise is -30.3 dB re the peak 0.5 ms after the auto trim's cut: within 1 dB of
    # trim_cut's limit)
    zeros = np.zeros(lround(0.1 * rate))
    clips["silence_then_step"] = np.concatenate([zeros, clips["start_step"]])
    clips["silence_then_rise"] = np.concatenate([zeros, raised_fade(clips["start_step"], rate, 10, 0)])
    # a 200 Hz cosine whose envelope is 9 dB under its peak at 50 ms (a cosine peak there)
    tau = 0.05 / (9 / (20 * math.log10(math.e)))
    clips["trim_into"] = 0.8 * np.cos(2 * np.pi * 200 * t(0.4)) * np.exp(-t(0.4) / tau)
    clips["long_tone"] = raised_fade(0.3 * np.sin(2 * np.pi * 440 * t(12.0)), rate, 2, 100)
    # D-6: one file with a jump at its start, clicks and a gap inside, a 3 kHz ring and an end cut
    g = np.cos(2 * np.pi * 220 * t(0.5)) * 0.4 + 0.25 * np.sin(2 * np.pi * 3000 * t(0.5))
    g *= np.exp(-t(0.5) / 0.25)
    g[lround(0.15 * rate)] += 0.6
    g[lround(0.3 * rate):lround(0.305 * rate)] = 0.0
    clips["gating"] = g
    return clips


# -- 5.5: the review corpus (the phase 2.5 review's probes, in numpy and ffmpeg): music with drums,
# flite speech, ambience, bells, reverb, UI beeps, naive and band-limited waves, and broken copies

NOTES_HZ = {"A3": 220.0, "C4": 261.63, "E4": 329.63, "F3": 174.61, "G3": 196.0, "B3": 246.94, "D4": 293.66}
CHORDS = [("A3", "C4", "E4"), ("F3", "A3", "C4"), ("C4", "E4", "G3"), ("G3", "B3", "D4")]


def raised(k: np.ndarray, n: int) -> np.ndarray:
    return np.where(k < n, 0.5 * (1.0 - np.cos(np.pi * np.minimum(k, n) / n)), 1.0)


def master_fades(ch: np.ndarray, rate: int, in_s: float, out_s: float) -> np.ndarray:
    ch = ch.copy()
    n_in, n_out = int(in_s * rate), int(out_s * rate)
    n = ch.shape[-1]
    if n_in:
        ch[..., :n_in] *= raised(np.arange(n_in), n_in)
    if n_out:
        ch[..., n - n_out:] *= 0.5 * (1 + np.cos(np.pi * (np.arange(n_out) + 1) / n_out))
    return ch


def pad(seconds: float, rate: int) -> np.ndarray:
    """chords of three partials each, a 10 ms raised-cosine attack and a 60 ms release per 1 s bar"""
    n = int(seconds * rate)
    out = np.zeros(n)
    for b in range(0, n, rate):
        length = min(rate, n - b)
        i = np.arange(length)
        attack, release = int(0.010 * rate), int(0.060 * rate)
        env = raised(i, attack) * np.where(i > length - release, raised(length - 1 - i, release), 1.0)
        for name in CHORDS[(b // rate) % len(CHORDS)]:
            for p in (1, 2, 3):
                out[b:b + length] += 0.07 / p * env * np.sin(2 * np.pi * NOTES_HZ[name] * p * i / rate)
    return out


def music(seconds: float, rate: int, fades: bool = True, drums: bool = True, seed: int = 3) -> np.ndarray:
    """two channels: the pad, a kick every 0.5 s (a falling sine from phase 0) and noise hats
    between the kicks (1 ms ramp, 25 ms decay); faded in over 5 ms and out over 200 ms"""
    rng = np.random.default_rng(seed)
    n = int(seconds * rate)
    p = pad(seconds, rate)
    left, right = p.copy(), 0.9 * p
    if drums:
        beat = rate // 2
        for b in range(0, n, beat):
            length = min(int(0.25 * rate), n - b)
            t = np.arange(length) / rate
            phase = np.cumsum(2 * np.pi * (50.0 + 70.0 * np.exp(-t / 0.03)) / rate)
            kick = 0.35 * np.exp(-t / 0.08) * np.sin(phase)
            left[b:b + length] += kick
            right[b:b + length] += kick
            hat = b + beat // 2
            length = min(int(0.06 * rate), max(0, n - hat))
            i = np.arange(length)
            v = 0.08 * raised(i, int(0.001 * rate)) * np.exp(-i / rate / 0.025) * rng.uniform(-1, 1, length)
            left[hat:hat + length] += 0.7 * v
            right[hat:hat + length] += v
    ch = np.stack([left, right])
    return master_fades(ch, rate, 0.005, 0.2) if fades else ch


def damaged(ch: np.ndarray, what: str, at: float, rate: int) -> np.ndarray:
    k = int(at * rate)
    if what == "splice":
        return np.delete(ch, np.arange(k, k + 37), axis=-1)
    out = ch.copy()
    if what == "spike":
        out[..., k] += 0.3
    elif what == "drop1ms":
        out[..., k:k + 48] = 0.0
    elif what == "drop5ms":
        out[..., k:k + 240] = 0.0
    elif what == "drop3":
        out[..., k:k + 3] = 0.0
    elif what == "drop3_left":
        out[0, k:k + 3] = 0.0
    return out


def bell(seconds: float, rate: int) -> np.ndarray:
    t = np.arange(int(seconds * rate)) / rate
    out = np.zeros(t.size)
    for f, a, tau in ((523.0, 1.0, 1.2), (1046.0 * 1.003, 0.5, 0.8), (1569.0 * 1.01, 0.3, 0.5), (2733.0, 0.2, 0.3), (4120.0, 0.1, 0.15)):
        out += 0.3 * a * np.exp(-t / tau) * np.sin(2 * np.pi * f * t)
    return out


def beeps(rate: int, gap_ms: float) -> np.ndarray:
    """two 80 ms 1 kHz beeps (2 ms in, 10 ms out) with digital silence between: a UI confirm"""
    m = int(0.08 * rate)
    one = master_fades(0.4 * np.sin(2 * np.pi * 1000.0 * np.arange(m) / rate), rate, 0.002, 0.010)
    return np.concatenate([one, np.zeros(int(gap_ms / 1000 * rate)), one])


def wave_of(kind: str, freq: float, seconds: float, rate: int, blep: bool) -> np.ndarray:
    """a naive or polyBLEP saw or square at 0.3, faded in over 2 ms and out over 100 ms"""
    n = int(seconds * rate)
    if blep:
        v = polyblep_saw(freq, n / rate, rate) if kind == "saw" else polyblep_square(freq, n, rate)
    else:
        ph = (np.arange(n) * freq / rate) % 1.0
        v = np.where(ph < 0.5, 1.0, -1.0) if kind == "square" else 2 * ph - 1
    return master_fades(0.3 * v, rate, 0.002, 0.1)


def ffmpeg(args: list, out: Path) -> Path:
    import subprocess
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args, str(out)], check=True)
    return out


def write_float(path: Path, ch: np.ndarray, rate: int, subtype: str = "PCM_16") -> None:
    import soundfile
    path.parent.mkdir(parents=True, exist_ok=True)
    data = np.atleast_2d(ch).T
    soundfile.write(str(path), np.clip(data, -1.0, 1.0) if subtype.startswith("PCM") else data, rate, subtype=subtype)


def corpus(directory: str) -> list:
    """writes the corpus into directory; returns [name] (file names with their extension)"""
    d = Path(directory)
    names = []

    def put(name: str, ch: np.ndarray, rate: int, subtype: str = "PCM_16") -> None:
        write_float(d / name, ch, rate, subtype)
        names.append(name)

    R = 48000
    m = music(8.0, R)
    put("music.wav", m, R)
    put("music_loop.wav", music(8.0, R, fades=False), R)
    pd = master_fades(np.stack([pad(8.0, R), 0.9 * pad(8.0, R)]), R, 0.005, 0.2)
    put("pad.wav", pd, R)
    put("music_tiny.wav", m * 10 ** (-64 / 20), R, "FLOAT")
    put("music_quiet_gain.wav", m, R)  # with gain_db -40 in its sidecar
    for what, at in (("splice", 3.13), ("spike", 2.62), ("drop1ms", 2.37), ("drop5ms", 1.87), ("drop3", 1.37), ("drop3_left", 1.37)):
        put(f"mus_{what}.wav", damaged(m, what, at, R), R)
        put(f"pad_{what}.wav", damaged(pd, what, at, R), R)
    i = np.arange(int(0.6 * R))
    tone = 0.5 * raised(i, 96) * np.exp(-i / R / 0.25) * np.sin(2 * np.pi * 500 * i / R) * raised(i[::-1], 4800)
    for what in ("splice", "spike", "drop1ms", "drop3"):
        put(f"tone_{what}.wav", damaged(tone[None, :], what, 0.2137, R), R)
    put("bell.wav", bell(3.0, R), R)
    put("ui_double_beep.wav", beeps(R, 60.0), R)
    put("knock_knock.wav", beeps(R, 180.0), R)
    n = 2 * R
    ph = np.cumsum(2 * np.pi * 200.0 * 40.0 ** (np.arange(n) / n) / R)
    put("chirp.wav", 0.4 * raised(np.arange(n), 96) * raised(np.arange(n)[::-1], 576) * np.sin(ph), R)
    rng = np.random.default_rng(4)
    n = int(0.8 * R)
    i = np.arange(n)
    hiss = 0.3 * raised(i, 24) * np.exp(-i / R / 0.06) * np.sin(2 * np.pi * 300 * i / R) + 0.003 * rng.uniform(-1, 1, n)
    put("hiss_hit.wav", master_fades(hiss, R, 0.005, 0.005), R)
    for kind in ("saw", "square"):
        for f, rate in ((48000 / 32.5, 48000), (1318.5, 44100), (880.0, 44100), (440.0, 48000), (3000.0, 48000), (660.0, 48000),
                        (1760.0, 48000), (2000.0, 44100), (220.0, 44100)):
            for blep in (False, True):
                put(f"{'blep' if blep else 'naive'}_{kind}_{int(f)}_{rate}.wav", wave_of(kind, f, 0.8, rate, blep), rate)
    # tails: the spec's tail_cut shape (440 Hz, 150 ms decay, cut at -20 dB) and a held tone
    n = int(0.15 * math.log(10) * R)
    i = np.arange(n)
    put("tail_shape.wav", 0.5 * raised(i, 96) * np.exp(-i / R / 0.15) * np.sin(2 * np.pi * 440 * i / R), R)
    i = np.arange(int(0.8 * R))
    put("held.wav", 0.4 * raised(i, 96) * np.sin(2 * np.pi * 523.25 * i / R), R)
    # a 440 Hz cosine from its peak, cut at a peak (12,001 frames: frame 12,000 is 110 cycles in);
    # a 1 s one for the trims that leave nothing
    put("clicky.wav", 0.5 * np.cos(2 * np.pi * 440.0 * np.arange(12001) / R), R)
    put("trim_all.wav", 0.5 * np.cos(2 * np.pi * 440.0 * np.arange(R) / R), R)
    # the cost: a stereo chord of 9.9 s (resonance and aliasing measured) and of 10.1 s (skipped)
    for name, seconds in (("tonal_9_9.wav", 9.9), ("tonal_10_1.wav", 10.1)):
        t = np.arange(int(seconds * R)) / R
        chord = sum(0.12 * np.sin(2 * np.pi * f * t) for f in (220.0, 277.18, 329.63, 440.0))
        chord = master_fades(chord, R, 0.002, 0.002)
        put(name, np.stack([chord, chord]), R)
    # ffmpeg: flite speech, ambience, a burst through echo taps and its cut copy
    ffmpeg(["-f", "lavfi", "-i", "flite=text='The quick brown fox jumps over the lazy dog. Press start to continue, or wait.':voice=slt"],
           d / "speech_slt.wav")
    ffmpeg(["-f", "lavfi", "-i", "flite=text='Mission complete. Returning to base.':voice=kal16", "-ar", "44100"], d / "speech_kal_44k.wav")
    ffmpeg(["-f", "lavfi", "-i", "flite=text='Welcome back, commander.':voice=awb", "-af",
            "afade=t=in:d=0.01,areverse,afade=t=in:d=0.02,areverse", "-ar", "48000", "-c:a", "libvorbis"], d / "speech_awb.ogg")
    ffmpeg(["-f", "lavfi", "-i", "anoisesrc=color=pink:amplitude=0.3:duration=6:seed=5", "-af",
            "lowpass=f=4000,afade=t=in:d=0.5,afade=t=out:st=5.5:d=0.5", "-ar", "48000", "-ac", "2", "-c:a", "libvorbis"], d / "amb_pink.ogg")
    ffmpeg(["-f", "lavfi", "-i", "anoisesrc=color=brown:amplitude=0.01:duration=5:seed=7", "-af",
            "afade=t=in:d=0.2,afade=t=out:st=4.8:d=0.2", "-ar", "44100"], d / "amb_room.wav")
    names += ["speech_slt.wav", "speech_kal_44k.wav", "speech_awb.ogg", "amb_pink.ogg", "amb_room.wav"]
    rng = np.random.default_rng(2)
    i = np.arange(3 * R)
    burst = np.where(i < 960, 0.6 * raised(i, 48) * np.exp(-i / R / 0.004) * rng.uniform(-1, 1, i.size), 0.0)
    write_float(d / "burst_dry.wav", burst, R)
    taps = "aecho=0.8:0.85:23|37|53|71:0.5|0.4|0.3|0.25,aecho=0.8:0.7:113|167:0.4|0.3"
    ffmpeg(["-i", str(d / "burst_dry.wav"), "-af", taps + ",afade=t=out:st=2.6:d=0.4"], d / "impact_reverb.wav")
    full = ffmpeg(["-i", str(d / "burst_dry.wav"), "-af", taps], d / "reverb_full.wav")
    rv, _ = read(str(full))
    put("reverb_cut.wav", rv[:, :int(0.45 * R)], R)
    sp = ffmpeg(["-f", "lavfi", "-i", "flite=text='The quick brown fox jumps over the lazy dog.':voice=slt", "-ar", "48000"], d / "speech_src.wav")
    s, _ = read(str(sp))
    loud = int(np.argmax(np.abs(s[0])))
    put("speech_cut.wav", s[:, :loud + 37], R)
    names += ["impact_reverb.wav"]
    return names


# -- 5.6: the closure follow-ups' corpus (the phase 2.5 closure's N1-N5): click bursts and buffer
# underruns in a pad, designed silences with linear fades, dropouts in quiet passages and at zero
# crossings, a hi-hat's decay, crackle, and the waveforms that must stay clean


def chord_pad(seconds: float, rate: int) -> np.ndarray:
    """two channels: G3, B3 and D4 with their 2nd and 3rd harmonics, faded in over 20 ms, out over 300 ms"""
    t = np.arange(int(seconds * rate)) / rate
    v = sum(0.1 / k * np.sin(2 * np.pi * f * k * t) for f in (196.0, 246.9, 293.7) for k in (1, 2, 3))
    return master_fades(np.stack([v, 0.9 * v]), rate, 0.02, 0.3)


def linear_fades(x: np.ndarray, n: int) -> np.ndarray:
    """a linear fade-in and fade-out of n frames each (Audacity's: the first and last samples 0)"""
    x = x.copy()
    x[:n] *= np.arange(n) / n
    x[x.size - n:] *= (n - 1 - np.arange(n)) / n
    return x


def bandpass(x: np.ndarray, f0: float, q: float, rate: int) -> np.ndarray:
    """an RBJ band-pass (constant 0 dB peak gain), direct form I"""
    w = 2 * np.pi * f0 / rate
    alpha = np.sin(w) / (2 * q)
    b0, b2, a0, a1, a2 = alpha, -alpha, 1 + alpha, -2 * np.cos(w), 1 - alpha
    y = np.zeros(x.size)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        out = (b0 * v + b2 * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1, y2, y1 = x1, v, y1, out
        y[i] = out
    return y


def hat(rng, rate: int, level: float = 0.25, decay: float = 0.04) -> np.ndarray:
    """a hi-hat: white noise through a 9 kHz band-pass, a 0.2 ms rise, an exponential decay (300 ms)"""
    n = int(0.3 * rate)
    v = bandpass(rng.uniform(-1, 1, n), 9000.0, 1.5, rate) * np.exp(-np.arange(n) / rate / decay)
    v *= level / np.abs(v).max()
    return v * raised(np.arange(n), int(0.0002 * rate))


def followup(directory: str) -> list:
    """writes the corpus; returns [{name, type, what, ...}] (what: the expectation)"""
    d = Path(directory)
    R = 48000
    out = []

    def put(name: str, ch: np.ndarray, typ: str, what: str, **extra) -> None:
        write_float(d / name, ch, R)
        out.append({"name": name, "type": typ, "what": what, **extra})

    p = chord_pad(3.0, R)
    # N1: bursts of single-sample clicks (+0.25), evenly spread, and runs of zeros every N samples
    for count, span in ((3, 30), (3, 60), (4, 60), (5, 100), (8, 100)):
        x = p.copy()
        times = [round(1.2 + i * span / 1000.0 / (count - 1), 6) for i in range(count)]
        for tm in times:
            x[:, int(tm * R)] += 0.25
        put(f"burst_{count}in{span}.wav", x, "music", "clicks", at_s=times)
    for period, length, span in ((512, 32, 0.5), (1024, 48, 0.5), (512, 32, 0.05)):
        x = p.copy()
        starts = list(range(int(1.0 * R), int((1.0 + span) * R), period))
        for at in starts:
            x[:, at:at + length] = 0.0
        put(f"underrun_{period}_{length}_{int(span * 1000)}.wav", x, "music", "underruns", at_s=[a / R for a in starts])
    # N1 negatives: a drum loop with realistic hats; a 45 Hz saw (sub-bass, under Phase 1's pitch range)
    rng = np.random.default_rng(11)
    n = 2 * R
    loop = np.zeros(n)
    for i in range(16):
        at = int(i * 0.125 * R)
        if i % 4 == 0:
            t = np.arange(int(0.4 * R)) / R
            kick = 0.8 * np.exp(-t / 0.12) * np.sin(2 * np.pi * np.cumsum(45 + 110 * np.exp(-t / 0.025)) / R)
            kick *= raised(np.arange(t.size), int(0.0003 * R))
            loop[at:at + kick.size] += kick[:max(0, min(kick.size, n - at))]
        h = hat(rng, R)
        loop[at:at + h.size] += h[:max(0, min(h.size, n - at))]
    loop = master_fades(np.stack([loop, loop]) / np.abs(loop).max() * 0.9, R, 0.002, 0.1)
    put("drums_hats.wav", loop, "music", "clean")
    t = np.arange(int(1.5 * R)) / R
    put("saw_45.wav", master_fades(0.5 * (2 * ((t * 45.0) % 1.0) - 1) * np.exp(-t), R, 0.002, 0.1), "music", "clean")
    # N2: separate parts with digital silence between, linear fades of 0.5-2 ms (raised cosine 0.5 ms)
    for seed in range(6):
        g = np.random.default_rng(100 + seed)
        for shape, ms in (("lin", 0.5), ("lin", 1.0), ("lin", 2.0), ("rc", 0.5)):
            for colour, typ in (("white", "impact"), ("tone", "ui")):
                parts = []
                for length in (60, 150, 300, 90):
                    m = int(length * R / 1000)
                    b = g.uniform(-1, 1, m) if colour == "white" else np.sin(2 * np.pi * 880 * np.arange(m) / R + g.uniform(0, 6.28))
                    f = int(round(ms * R / 1000))
                    b = linear_fades(0.5 * b, f) if shape == "lin" else master_fades(0.5 * b, R, ms / 1000, ms / 1000)
                    parts += [b, np.zeros(int(0.12 * R))]
                put(f"designed_{colour}_{shape}{ms:g}_{seed}.wav", np.concatenate(parts[:-1]), typ, "designed")
    # N3: 5 ms of zeros in a passage 30, 40 and 50 dB under the peak; one period of a 250 Hz tone
    # zeroed from zero crossing to zero crossing
    for level in (30, 40, 50):
        x = p.copy()
        x[:, R:2 * R] *= 10 ** (-level / 20)
        x[:, int(1.5 * R):int(1.5 * R) + 240] = 0.0
        put(f"quiet{level}_drop5ms.wav", x, "music", "gap", at_s=[1.5])
    t = np.arange(int(1.5 * R)) / R
    tone = master_fades(0.5 * np.sin(2 * np.pi * 250 * t), R, 0.01, 0.1)
    k0 = int(0.6 * R)
    tone[k0:k0 + R // 250] = 0.0
    put("tone250_drop_zc.wav", tone, "chime", "gap", at_s=[k0 / R])
    # N4: a click 8 dB under a hi-hat (40 ms decay), 60 ms and 250 ms after it, in the pad
    for off in (60, 250):
        x = p[0].copy() * 0.6
        h = hat(np.random.default_rng(3), R, 0.25, 0.04)
        at = int(0.8 * R)
        x[at:at + h.size] += h
        x[at + int(off * R / 1000)] += 0.1
        put(f"hat_then_spike_{off}.wav", x, "music", "hidden" if off == 60 else "clicks", at_s=[(at + int(off * R / 1000)) / R])
    # N5: a campfire (low-passed noise and sparse 1-4 ms pops), typed and untyped; and as controls,
    # 10 clicks every 250 ms in the pad (regular, not crackle)
    g = np.random.default_rng(2026)
    n = 20 * R
    noise = g.standard_normal(n)
    fire = np.zeros(n)
    a = np.exp(-2 * np.pi * 500 / R)
    acc = 0.0
    for i in range(n):
        acc = (1 - a) * noise[i] + a * acc
        fire[i] = 0.05 * acc
    for _ in range(80):
        at = g.integers(R, n - R)
        m = int(g.uniform(1, 4) * R / 1000)
        burst = g.uniform(-1, 1, m)
        fire[at:at + m] += g.uniform(0.2, 0.6) * np.concatenate([[burst[0]], np.diff(burst)]) * np.exp(-np.arange(m) / (m / 3))
    fire = master_fades(fire, R, 1.0, 1.0)
    put("campfire.wav", fire, "ambience", "crackle")
    put("campfire_untyped.wav", fire, "", "crackle")
    x = p.copy()
    times = [round(0.4 + 0.25 * i, 6) for i in range(10)]
    for tm in times:
        x[:, int(tm * R)] += 0.25
    put("regular_clicks.wav", x, "music", "clicks", at_s=times)
    return out


def write_pcm16(path: Path, x: np.ndarray, rate: int) -> None:
    pcm = np.clip(np.floor(np.asarray(x) * 32767.0 + 0.5), -32768, 32767).astype("<i2").tobytes()
    header = struct.pack("<4sI4s", b"RIFF", 36 + len(pcm), b"WAVE")
    header += struct.pack("<4sIHHIIHH", b"fmt ", 16, 1, 1, rate, rate * 2, 2, 16)
    header += struct.pack("<4sI", b"data", len(pcm))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + pcm)


def main() -> None:
    request = json.loads(sys.stdin.read())
    op = request["op"]
    if op == "defects":
        out = []
        for item in request["items"]:
            x, rate = read(item["path"])
            region = item.get("region") or {}
            y = played(x, region)
            p, c = pitch(y, rate)
            out.append({"defects": defects(x, rate, region), "frames": x.shape[1], "rate": rate,
                        "pitch": {"hz": p, "confidence": c}, "envelope_peak_ms": envelope_peak_ms(y, rate)})
        print(json.dumps(out))
    elif op == "synth":
        names = []
        for name, x in synth().items():
            write_pcm16(Path(request["dir"]) / f"{name}.wav", x, RATE)
            names.append(name)
        print(json.dumps(names))
    elif op == "corpus":
        print(json.dumps(corpus(request["dir"])))
    elif op == "followup":
        print(json.dumps(followup(request["dir"])))
    elif op == "weights":
        n = request["n"]
        print(json.dumps({"in": [float(fade_weight(k, n, True)) for k in range(n)],
                          "out": [float(fade_weight(j, n, False)) for j in range(n)]}))
    elif op == "second_differences":
        x, rate = read(request["path"])
        m = x.astype(np.float64).mean(axis=0)
        d2 = np.abs(m[2:] - 2 * m[1:-1] + m[:-2])
        # the largest, then the largest more than 10 samples from every one taken, ...
        top = []
        for i in np.argsort(d2)[::-1]:
            if all(abs(int(i) - j) > 10 for j in top):
                top.append(int(i))
            if len(top) == request.get("count", 7):
                break
        top = sorted(i + 1 for i in top)
        print(json.dumps({"positions": top, "rate": rate}))
    else:
        raise SystemExit("unknown op " + op)


if __name__ == "__main__":
    main()
