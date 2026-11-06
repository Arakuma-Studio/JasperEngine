"""Independent measures of a capture, for tools/mcp/tests/test_audio_qa.py (docs/specs/S35-agent-audio.md AC10).

Written from the spec's rules, not from editor/audio/AudioQa.cpp, on the session's files alone, with the standard
library: audio_session check must agree with these within the tolerance each test states.
"""

from __future__ import annotations

import array
import math


def _frames(samples: array.array, channels: int):
    for i in range(0, len(samples), channels):
        yield max(abs(samples[i + c]) for c in range(channels))


def peaks_per_ms(samples: array.array, channels: int, rate: int) -> list[float]:
    """The loudest |sample| of each ms."""
    ms = rate // 1000
    out = [0.0] * (len(samples) // channels // ms + 1)
    for i, value in enumerate(_frames(samples, channels)):
        k = i // ms
        if value > out[k]:
            out[k] = value
    return out


def onset_ms(peaks: list[float], stamp_ms: int, margin_db: float, search_ms: int = 1000) -> int | None:
    """The first ms from stamp_ms that rises margin_db over the loudest ms of the 50 before it, above -60 dBFS."""
    gain = 10 ** (margin_db / 20)
    for m in range(stamp_ms, min(stamp_ms + search_ms, len(peaks))):
        before = max(peaks[max(0, m - 50):m], default=0.0)
        if peaks[m] >= max(before * gain, 1e-3):
            return m
    return None


def longest_gap_seconds(peaks: list[float], from_ms: int) -> float:
    """The longest run of ms under -60 dBFS after from_ms, in seconds."""
    longest = run = 0
    for value in peaks[from_ms:]:
        run = run + 1 if value < 1e-3 else 0
        longest = max(longest, run)
    return longest / 1000.0


def clipped_samples(samples: array.array) -> int:
    level = 1.0 - 2.0 ** -15
    return sum(1 for value in samples if abs(value) >= level)


def most_repeats(starts: list[dict], window_s: float) -> int:
    """The most plays of one clip in a row from one source within window_s."""
    runs: dict[str, tuple[str, list[float]]] = {}
    best = 0
    for line in starts:
        source = line.get("event") or line.get("entity") or (str(line.get("owner")) + " " + str(line.get("clip")))
        clip, times = runs.get(source, ("", []))
        if clip != line["clip"]:
            clip, times = line["clip"], []
        times = [t for t in times if line["t"] - t <= window_s] + [line["t"]]
        runs[source] = (clip, times)
        best = max(best, len(times))
    return best


def true_peak_dbtp_lower_bound(samples: array.array) -> float:
    """The sample peak: a true peak is never below it."""
    peak = max((abs(value) for value in samples), default=0.0)
    return 20 * math.log10(peak) if peak > 0 else -120.0
