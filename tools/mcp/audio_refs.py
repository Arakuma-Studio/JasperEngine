"""Reference implementation of the Sound Event picks and draws (docs/specs/S37-sound-events.md R8).

Written from the spec's text, not from the C++ (engine/core/audio/SoundEvent.cpp), so the
tests have an independent oracle for every play mode, range and clip-mode variation. Standard
library only, so every test can import it under the harness's `uv run --with mcp`.

The generator, exactly as the spec states it:

- fnv1a64 over the key's bytes (an event's 32-hex-digit GUID, or a source's "entity:<id>");
- per play: state = splitmix64(seed ^ fnv1a64(key) ^ (play_index * GOLDEN)), GOLDEN if that is 0;
- draws: xorshift64*, u = (next() >> 11) * 2^-53 in [0, 1);
- per play: the pick (random: one u; shuffle: a Fisher-Yates refill when the bag is empty; sequence
  and layer: none), then exactly three draws: pitch, volume, delay, each min + u (max - min).
  Clip mode draws pitch then volume, only the ones that are not 0.

Event values are floats in the engine (weights, ranges), so they go through float32 here before
any arithmetic, which is then double (Python floats), as the spec's draws are.
"""

from __future__ import annotations

import json
import math
import struct
from dataclasses import dataclass, field

MASK = (1 << 64) - 1
GOLDEN = 0x9E3779B97F4A7C15


def f32(value: float) -> float:
    """The float32 nearest `value`, as the engine stores event and component values."""
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def fnv1a64(text: str) -> int:
    h = 14695981039346656037
    for byte in text.encode("utf-8"):
        h ^= byte
        h = (h * 1099511628211) & MASK
    return h


def splitmix64(x: int) -> int:
    x = (x + GOLDEN) & MASK
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
    return z ^ (z >> 31)


class Rng:
    """One play's generator."""

    def __init__(self, seed: int, key: str, play_index: int):
        self.state = splitmix64((seed ^ fnv1a64(key) ^ ((play_index * GOLDEN) & MASK)) & MASK)
        if self.state == 0:
            self.state = GOLDEN

    def next(self) -> int:
        x = self.state
        x ^= x >> 12
        x = (x ^ (x << 25)) & MASK
        x ^= x >> 27
        self.state = x
        return (x * 0x2545F4914F6CDD1D) & MASK

    def uniform(self) -> float:
        return (self.next() >> 11) * (2.0 ** -53)

    def range(self, lo: float, hi: float) -> float:
        lo, hi = f32(lo), f32(hi)
        return lo + self.uniform() * (hi - lo)


@dataclass
class Clip:
    path: str
    weight: float = 1.0
    volume_db: float = 0.0
    pitch_semitones: float = 0.0
    delay_ms: float = 0.0


@dataclass
class Event:
    guid: str
    mode: str = "random"  # random | shuffle | sequence | layer
    clips: list[Clip] = field(default_factory=list)
    avoid_repeat: bool = True
    pitch_semitones: tuple[float, float] = (0.0, 0.0)
    volume_db: tuple[float, float] = (0.0, 0.0)
    delay_ms: tuple[float, float] = (0.0, 0.0)

    @staticmethod
    def from_file(data: dict, guid: str) -> "Event":
        """From a parsed .soundevent and the GUID of its .meta (R1)."""
        clips = [Clip(path=c["clip"].get("path", ""), weight=c.get("weight", 1.0), volume_db=c.get("volume_db", 0.0),
                      pitch_semitones=c.get("pitch_semitones", 0.0), delay_ms=c.get("delay_ms", 0.0))
                 for c in data.get("clips", [])]
        return Event(guid=guid, mode=data.get("mode", "random"), clips=clips,
                     avoid_repeat=data.get("avoid_repeat", True),
                     pitch_semitones=tuple(data.get("pitch_semitones", (0.0, 0.0))),
                     volume_db=tuple(data.get("volume_db", (0.0, 0.0))),
                     delay_ms=tuple(data.get("delay_ms", (0.0, 0.0))))


@dataclass
class Play:
    """One accepted play: the clip indices (one, or every clip in layer mode) and the draws."""
    index: int
    clips: list[int]
    pitch: float  # semitones, the instance's draw
    volume_db: float
    delay_ms: float

    @property
    def variation(self) -> int:
        return self.clips[0]


class EventState:
    """An event's runtime state from a fresh reset: its bag, last clip and sequence position."""

    def __init__(self, event: Event, seed: int):
        self.event = event
        self.seed = seed
        self.bag: list[int] = []
        self.bag_next = 0
        self.last = -1
        self.sequence = 0
        self.plays = 0

    def _random(self, rng: Rng) -> int:
        clips = self.event.clips
        weights = [f32(c.weight) for c in clips]
        weighted = sum(1 for w in weights if w > 0)
        excluded = self.last if (self.event.avoid_repeat and weighted > 1) else -1
        total = sum(w for i, w in enumerate(weights) if i != excluded and w > 0)
        target = rng.uniform() * total
        if total <= 0:
            return 1 if (excluded == 0 and len(clips) > 1) else 0
        acc = 0.0
        chosen = -1
        for i, w in enumerate(weights):
            if i == excluded or w <= 0:
                continue
            acc += w
            chosen = i
            if target < acc:
                break
        return chosen

    def _shuffle(self, rng: Rng) -> int:
        n = len(self.event.clips)
        if self.bag_next >= len(self.bag) or len(self.bag) != n:
            self.bag = list(range(n))
            for i in range(n - 1, 0, -1):
                j = math.floor(rng.uniform() * (i + 1))
                self.bag[i], self.bag[j] = self.bag[j], self.bag[i]
            if n >= 2 and self.bag[0] == self.last:
                self.bag[0], self.bag[1] = self.bag[1], self.bag[0]
            self.bag_next = 0
        clip = self.bag[self.bag_next]
        self.bag_next += 1
        return clip

    def play(self) -> Play:
        """The next accepted play (a rejected one, by cooldown or limit, consumes nothing)."""
        rng = Rng(self.seed, self.event.guid, self.plays)
        mode = self.event.mode
        n = len(self.event.clips)
        clips: list[int] = []
        if n > 0:
            if mode == "random":
                clips = [self._random(rng)]
            elif mode == "shuffle":
                clips = [self._shuffle(rng)]
            elif mode == "sequence":
                clips = [self.sequence % n]
                self.sequence += 1
            elif mode == "layer":
                clips = list(range(n))
            else:
                raise ValueError(f"unknown mode {mode!r}")
            if mode != "layer":
                self.last = clips[0]
        pitch = rng.range(*self.event.pitch_semitones)
        volume = rng.range(*self.event.volume_db)
        delay = rng.range(*self.event.delay_ms)
        play = Play(self.plays, clips, pitch, volume, delay)
        self.plays += 1
        return play

    def plays_n(self, count: int) -> list[Play]:
        return [self.play() for _ in range(count)]


def voice_speed(play: Play, clip: Clip, source_speed: float = 1.0, api_pitch: float = 1.0) -> float:
    """A voice's relative speed (section 2.2.3)."""
    return f32(source_speed) * 2.0 ** ((play.pitch + f32(clip.pitch_semitones)) / 12.0) * api_pitch


def voice_gain(play: Play, clip: Clip, source_volume: float = 1.0, impact_gain: float = 1.0,
               api_volume: float = 1.0) -> float:
    """A voice's linear gain (section 2.2.3)."""
    return source_volume * 10.0 ** ((play.volume_db + f32(clip.volume_db)) / 20.0) * impact_gain * api_volume


def voice_delay_samples(play: Play, clip: Clip, rate: int = 48000) -> int:
    """round((d + c.delay_ms) x rate / 1000), rounding half away from zero (llround)."""
    value = (play.delay_ms + f32(clip.delay_ms)) * rate / 1000.0
    return int(math.floor(value + 0.5)) if value >= 0 else -int(math.floor(-value + 0.5))


def clip_variation(seed: int, entity: int, play_index: int, pitch_random: float, volume_random: float) -> tuple[float, float]:
    """A clip-mode start's (pitch semitones, volume dB): pitch uniform in [-p, p], volume in [-v, 0],
    drawn in that order and only when not 0 (section 2.3.5)."""
    rng = Rng(seed, f"entity:{entity}", play_index)
    p, v = f32(pitch_random), f32(volume_random)
    pitch = -p + rng.uniform() * (2 * p) if p != 0 else 0.0
    volume = -v + rng.uniform() * v if v != 0 else 0.0
    return pitch, volume


def write_tone(path, seconds: float = 0.3, frequency: float = 1000.0, amplitude: float = 0.5, rate: int = 44100) -> None:
    """A 16-bit mono PCM WAV of a sine."""
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = int(seconds * rate)
    data = b"".join(struct.pack("<h", int(round(amplitude * 32767 * math.sin(2 * math.pi * frequency * i / rate))))
                    for i in range(samples))
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    path.write_bytes(header + b"data" + struct.pack("<I", len(data)) + data)


TRACE_ENV = {"JASPERENGINE_AUDIO_TRACE": "1", "JASPERENGINE_AUDIO_BACKEND": "null"}
TRACE_PREFIX = "[audio] "


def voice_lines(entries: list[dict]) -> list[dict]:
    """The voice lines of JASPERENGINE_AUDIO_TRACE=1 in read_console entries, in order."""
    lines = []
    for entry in entries:
        message = entry.get("message", "")
        at = message.find(TRACE_PREFIX)
        if at >= 0:
            lines.append(json.loads(message[at + len(TRACE_PREFIX):]))
    return lines
