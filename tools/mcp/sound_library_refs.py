"""A mini starter library for the tests (sound layer phase 5, spec 2.13), and the tampering helpers of spec 3.2.

The behaviour tests run on a mini library, pointed at with JASPERENGINE_SOUND_LIBRARY_DIR (the harness's
Editor(env=...), or the CLI's environment); the data tests run on the real one. build() makes one in a
directory: 8 clips over 4 types (tones, clicks and noise beds made by audio_refs.py), encoded to Ogg Vorbis
by soundfile, a manifest.json per spec 2.2.2 whose `measured` is computed from the decoded files by
soundfile, numpy and pyloudnorm (the pipeline's oracle, here as in tools/sound-library/build_library.py), a
licence file, and an analysis.json written by the editor's `audio-analyze` (spec 2.2.3).

Standard library, as audio_refs.py: the encoding and the measurements run in one
`uv run --no-project --with numpy --with soundfile --with pyloudnorm` subprocess. The OGG bytes differ
between builds (libsndfile's random Ogg serial), so every test reads the hashes from the manifest it got.
"""

from __future__ import annotations

import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parents[1] / "sound-library"))
import soundlib  # noqa: E402

import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_refs as ar  # noqa: E402

RATE = 48000
LIBRARY_FORMAT = "jasperengine-sound-library"
ANALYSIS_FORMAT = "jasperengine-sound-analysis"
ENV_DIR = "JASPERENGINE_SOUND_LIBRARY_DIR"
# the keys an analysis.json item leaves out of the CLI's item (spec 2.2.3)
ANALYSIS_EXCLUDED = ("n", "path", "type", "labels", "licence", "notes", "import")

PACK = {
    "id": "mini-pack",
    "title": "Mini Pack",
    "url": "https://kenney.nl/media/pages/assets/mini-pack/0000000000-0000000000/kenney_mini-pack.zip",
    "landing": "https://kenney.nl/assets/mini-pack",
    "sha256": "0" * 64,
    "bytes": 1,
    "licence": "CC0-1.0",
    "licence_file": "licenses/mini-pack.txt",
    "author": "Kenney (www.kenney.nl)",
}
LICENCE_TEXT = ("Mini Pack (test data)\n\nLicense (Creative Commons Zero, CC0)\n"
                "http://creativecommons.org/publicdomain/zero/1.0/\n")
FS_LICENCE = "http://creativecommons.org/publicdomain/zero/1.0/"


def _shaped(samples: list, fade_in_s: float, fade_out_s: float) -> list:
    """Linear fades over the first and last seconds (the pipeline's one-shot fades, spec 2.11.3)."""
    n = len(samples)
    a, b = int(round(fade_in_s * RATE)), int(round(fade_out_s * RATE))
    out = list(samples)
    for i in range(min(a, n)):
        out[i] *= i / a
    for i in range(min(b, n)):
        out[n - 1 - i] *= i / b
    return out


def _decaying(samples: list, tau_s: float) -> list:
    return [x * math.exp(-i / (tau_s * RATE)) for i, x in enumerate(samples)]


def _click(freq: float, seconds: float, dbfs: float) -> list:
    return [_shaped(_decaying(ar.sine(freq, seconds, RATE, dbfs), seconds / 5.0), 0.002, 0.005)]


def _chime(freq: float, seconds: float, dbfs: float) -> list:
    tone = [a + b for a, b in zip(ar.sine(freq, seconds, RATE, dbfs), ar.sine(freq * 2.0, seconds, RATE, dbfs - 12.0))]
    return [_shaped(_decaying(tone, seconds / 4.0), 0.003, 0.02)]


def _step(seed: int, seconds: float, dbfs: float, low_hz: float = 0.0) -> list:
    noise = ar.white_noise(seconds, RATE, seed=seed, dbfs=dbfs)
    if low_hz:
        noise = [x + y for x, y in zip(noise, ar.sine(low_hz, seconds, RATE, dbfs - 3.0))]
    return [_shaped(_decaying(noise, seconds / 6.0), 0.001, 0.005)]


def _drone_bed(seed: int, seconds: float, dbfs: float) -> list:
    # starts mid-signal, as a bed cut from a longer recording does: three cosines at their crest
    # under a little noise, so its first frame is near the peak and still rising slowly, an edge
    # click without a fade-in; the manifest gives it import {fade_in_ms: 2} (spec decision 15)
    n = int(round(seconds * RATE))
    a = ar.db_to_amplitude(dbfs)
    out = []
    for c in range(2):
        noise = ar.white_noise(seconds, RATE, seed=seed + c, dbfs=dbfs - 30.0)
        out.append([a * (0.45 * math.cos(2 * math.pi * 200 * i / RATE) + 0.3 * math.cos(2 * math.pi * 310 * i / RATE) +
                         0.2 * math.cos(2 * math.pi * 470 * i / RATE)) + noise[i] for i in range(n)])
    return out


def _hum_bed(freq: float, seconds: float, dbfs: float) -> list:
    # a whole number of cycles from a zero crossing: a seamless wrap with no fade
    tone = ar.sine(freq, seconds, RATE, dbfs)
    return [tone, list(tone)]


# id, type, kind, tags, description, source, signal, import (sound layer phase 5 spec 2.2.2's keys)
CLIPS = [
    {"id": "ambience/fs_200003_drone_bed", "kind": "loop", "tags": ["drone", "outdoor"], "description": "Low drone bed",
     "source": "freesound", "user": "tester", "signal": lambda: _drone_bed(7, 4.5, -14.0), "import": {"fade_in_ms": 2}},
    {"id": "ambience/kenney_hum_bed", "kind": "loop", "tags": ["hum", "indoor"], "description": "Room hum bed",
     "source": "kenney", "member": "Audio/hum_bed.ogg", "signal": lambda: _hum_bed(110.0, 4.0, -20.0)},
    {"id": "chime/fs_200001_glass_chime", "kind": "tonal", "tags": ["glass", "soft"], "description": "Soft glass chime",
     "source": "freesound", "user": "tester", "signal": lambda: _chime(1320.0, 0.6, -12.0)},
    # a clip whose own kind differs from its type's (tonal), as essentials' pickup/coins does
    {"id": "chime/kenney_chime_1", "kind": "transient", "tags": ["glass", "bright"], "description": "Bright chime",
     "source": "kenney", "member": "Audio/chime_000.ogg", "signal": lambda: _chime(880.0, 0.5, -10.0)},
    {"id": "click/kenney_click_1", "kind": "transient", "tags": ["ui", "soft"], "description": "Soft interface click",
     "source": "kenney", "member": "Audio/click_000.ogg", "signal": lambda: _click(1000.0, 0.06, -14.0)},
    {"id": "click/kenney_click_2", "kind": "transient", "tags": ["ui", "hard"], "description": "Hard interface click",
     "source": "kenney", "member": "Audio/click_001.ogg", "signal": lambda: _click(2200.0, 0.04, -9.0)},
    {"id": "footstep/fs_200002_gravel", "kind": "transient", "tags": ["gravel", "outdoor"], "description": "Footstep on gravel",
     "source": "freesound", "user": "tester", "signal": lambda: _step(3, 0.18, -10.0)},
    {"id": "footstep/kenney_wood_1", "kind": "transient", "tags": ["wood", "indoor"], "description": "Footstep on wood",
     "source": "kenney", "member": "Audio/footstep_wood_000.ogg", "signal": lambda: _step(5, 0.22, -12.0, low_hz=140.0)},
]


_ENCODE = r"""
import json, sys
import numpy as np
import soundfile as sf
import pyloudnorm as pyln
sys.path.insert(0, sys.argv[1])
import audio_refs

FIR = np.array(audio_refs.TRUE_PEAK_FIR)


def db(x):
    return 20 * np.log10(x) if x > 0 else -120.0


def momentary_max(data, rate):
    # Phase 1 2.6's file mode: 400 ms windows every 10 ms, 400 ms of silence padded before and after
    meter = pyln.Meter(rate)
    before = int(round(0.4 * rate))
    padded = np.concatenate([np.zeros((before, data.shape[1])), data, np.zeros((before, data.shape[1]))])
    weighted = padded.T
    for f in meter._filters.values():
        weighted = f.apply_filter(weighted)
    weighted = weighted.T
    n, step, length, best, k = data.shape[0], rate / 100.0, before, None, -39
    while k * step <= n - step:
        start = before + int(np.floor(k * step))
        z = (weighted[start:start + length] ** 2).mean(axis=0).sum()
        if z > 0:
            v = -0.691 + 10 * np.log10(z)
            best = v if best is None else max(best, v)
        k += 1
    return best


def true_peak(data):
    best = float(np.max(np.abs(data)))
    for x in data.T:
        padded = np.concatenate([np.zeros(11), x, np.zeros(11)])
        for phase in FIR:
            best = max(best, float(np.max(np.abs(np.convolve(padded, phase, mode="valid")))))
    return best


def seam(data, rate):
    # Phase 1's metric as built (AudioAnalysis.cpp, spec A28)
    n, w = data.shape[0], int(round(0.010 * rate))
    step = -120.0
    for x in data.T:
        jump = abs(float(x[0]) - float(x[-1]))
        d = np.diff(x) if n < 2 * w else np.concatenate([np.diff(x[n - w:]), np.diff(x[:w])])
        ref = max(2.0 ** -15, float(np.sqrt(np.mean(d ** 2))) if d.size else 0.0)
        if jump > 0:
            step = max(step, 20 * np.log10(jump / ref))
    edge = max(1, min(n, w))
    head = float(np.mean(data[:edge] ** 2))
    tail = float(np.mean(data[n - edge:] ** 2))
    rms = lambda p: 10 * np.log10(p) if p > 0 else -120.0
    return step, rms(tail) - rms(head)


out = {}
for job in json.loads(sys.stdin.read()):
    x, rate = sf.read(job["wav"], dtype="float64", always_2d=True)
    sf.write(job["ogg"], x, rate, format="OGG", subtype="VORBIS", compression_level=0.6)
    y, rate = sf.read(job["ogg"], dtype="float64", always_2d=True)
    n = y.shape[0]
    level = np.max(np.abs(y), axis=1)
    loud = np.nonzero(level >= 10 ** (-60 / 20))[0]
    integrated = pyln.Meter(rate).integrated_loudness(y) if n >= int(0.4 * rate) else None
    m = {"channels": int(y.shape[1]), "sample_rate": int(rate), "frames": int(n), "duration_s": round(n / rate, 3),
         "sample_peak_dbfs": round(db(float(np.max(np.abs(y)))), 2), "true_peak_dbtp": round(db(true_peak(y)), 2),
         "momentary_max_lufs": round(momentary_max(y, rate), 2),
         "integrated_lufs": None if integrated is None or not np.isfinite(integrated) else round(float(integrated), 2),
         "head_ms": round((loud[0] if loud.size else n) / rate * 1000, 1),
         "tail_ms": round((n - 1 - loud[-1] if loud.size else n) / rate * 1000, 1)}
    if job["loop"]:
        s, l = seam(y, rate)
        m["seam_step_db"], m["seam_level_db"] = round(float(s), 2), round(float(l), 2)
    out[job["id"]] = m
print(json.dumps(out))
"""


def _uv_run(script: str, args: list, stdin: str, packages: tuple) -> str:
    cmd = [ar._uv(), "run", "--quiet", "--no-project"]
    for p in packages:
        cmd += ["--with", p]
    cmd += ["python", "-c", script, *args]
    out = subprocess.run(cmd, input=stdin, capture_output=True, text=True, timeout=900)
    if out.returncode != 0:
        raise RuntimeError(f"{packages} subprocess failed: {out.stderr[-3000:]}")
    return out.stdout.strip().splitlines()[-1]


def sha256(path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def manifest_text(manifest: dict) -> str:
    """indent 1, keys as given, LF and a final newline, ensure_ascii off (spec 2.2.2)"""
    return json.dumps(manifest, indent=1, ensure_ascii=False) + "\n"


def clip_entry(spec: dict, ogg: Path, measured: dict) -> dict:
    """A manifest clip object in the pipeline's key order (spec 5.2)"""
    cid = spec["id"]
    typ, name = cid.split("/")
    fs_id = int(name.split("_")[1]) if spec["source"] == "freesound" else None
    loop = spec["kind"] == "loop"
    entry = {"id": cid, "path": cid + ".ogg", "sha256": sha256(ogg), "bytes": ogg.stat().st_size, "type": typ, "kind": spec["kind"],
             "tags": spec["tags"], "description": spec["description"], "licence": "CC0-1.0", "source": spec["source"]}
    if fs_id is None:
        entry.update({"pack": PACK["id"], "source_file": spec["member"], "author": PACK["author"], "url": PACK["landing"]})
    else:
        user = spec["user"]
        entry.update({"pack": None, "source_file": None, "source_id": fs_id, "author": user,
                      "url": f"https://freesound.org/people/{user}/sounds/{fs_id}/", "licence_url": FS_LICENCE,
                      "attribution": f"{user} (freesound.org)"})
    processing = {"resampled_from": None, "channels_from": measured["channels"], "highpass_hz": 30 if loop else 60,
                  "lowpass_hz": 16000 if loop else 14000}
    if loop:
        processing = {"segment": [0, measured["frames"]], "loop_crossfade_ms": 0.0, **processing}
    else:
        processing.update({"fade_in_ms": 5.0, "fade_out_ms": 5.0})
    processing.update({"gain_db": 0.0, "limited_by": None, "quality": 0.6})
    if fs_id is not None:
        processing["preview"] = "hq.ogg"
    entry["processing"] = processing
    entry["measured"] = measured
    entry["meta"] = soundlib.clip_meta(entry, spec.get("import"), "library")
    return entry


def without_source_prefix(name: str) -> str:
    """kenney_<slug> -> <slug>, fs_<digits>_<slug> -> <slug>, any other name as it is"""
    m = re.fullmatch(r"(?:kenney_|fs_[0-9]+_)(.+)", name)
    return m.group(1) if m else name


ESSENTIALS_MANIFEST = Path(__file__).resolve().parents[2] / "editor/resources/sound-library/essentials/manifest.json"


def essential_ids() -> list:
    """the essentials' ids (<group>/<name>), from the committed manifest the editor embeds"""
    return [c["id"] for c in json.loads(ESSENTIALS_MANIFEST.read_text())["clips"]]


def import_name(cid: str, ids, essentials=None) -> str:
    """the file an import writes for a library clip under its folder, <group>/<name>.ogg (snd/p5-fix-ux,
    SoundLibrary::importName): the name without its source prefix, unless another clip of the manifest's
    group has that name or would get it too, or an essential has that <group>/<name> (snd/p5-fix-lows; all
    compared case-folded), then the id's whole name. `essentials` defaults to the committed essentials' ids.
    Python's own reading of the rule; it is the name in a folder that holds no earlier import of the clip and
    no other file under that name (BuiltinSoundImport::destination)."""
    group, name = cid.split("/", 1)
    short = without_source_prefix(name)
    key = f"{group}/{short}".lower()
    taken = 0
    for other in ids:
        g, n = other.split("/", 1)
        taken += (f"{g}/{without_source_prefix(n)}".lower() == key) + (other.lower() == key and without_source_prefix(n) != n)
    taken += sum(1 for e in (essential_ids() if essentials is None else essentials) if e.lower() == key)
    return f"{group}/{short if taken == 1 else name}.ogg"


class MiniLibrary:
    """A library directory and its manifest (re-read from disk by .manifest)."""

    def __init__(self, root: Path):
        self.root = Path(root)

    @property
    def manifest(self) -> dict:
        return json.loads((self.root / "manifest.json").read_text())

    @property
    def ids(self) -> list:
        return [c["id"] for c in self.manifest["clips"]]

    def clip(self, cid: str) -> dict:
        return next(c for c in self.manifest["clips"] if c["id"] == cid)

    def path(self, cid: str) -> Path:
        return self.root / (cid + ".ogg")

    def import_path(self, cid: str) -> str:
        """<group>/<name>.ogg, where an import of the clip writes it under its folder (import_name)"""
        return import_name(cid, self.ids)

    def env(self) -> dict:
        return {ENV_DIR: str(self.root)}

    # -- tampering (spec 3.2 R-3), each on a fresh copy

    def copy(self, dest: Path) -> "MiniLibrary":
        dest = Path(dest)
        shutil.copytree(self.root, dest, symlinks=True)
        return MiniLibrary(dest)

    def flip_byte(self, cid: str, offset: int | None = None) -> None:
        """One byte of a clip inverted (by default in its middle, inside the audio data)"""
        p = self.path(cid)
        data = bytearray(p.read_bytes())
        at = len(data) // 2 if offset is None else offset
        data[at] ^= 0xFF
        p.write_bytes(bytes(data))

    def add_file(self, rel: str, data: bytes = b"not in the manifest\n") -> Path:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    def drop_file(self, cid: str) -> None:
        """A clip's file removed; the manifest still lists it (a missing clip)"""
        self.path(cid).unlink()

    def drop_clip(self, cid: str) -> None:
        """A clip removed from the library: its file and its manifest entry"""
        self.path(cid).unlink()
        self.rewrite(lambda m: m.update(clips=[c for c in m["clips"] if c["id"] != cid]))

    def symlink_clip(self, cid: str, target: Path) -> None:
        """A clip replaced by a symbolic link to target"""
        p = self.path(cid)
        p.unlink()
        os.symlink(target, p)

    def rewrite(self, change) -> None:
        """manifest.json rewritten after change(manifest) edits it in place"""
        m = self.manifest
        change(m)
        (self.root / "manifest.json").write_text(manifest_text(m))

    def set_clip_key(self, cid: str, key: str, value) -> None:
        def edit(m):
            for c in m["clips"]:
                if c["id"] == cid:
                    c[key] = value
        self.rewrite(edit)

    def truncate_manifest(self) -> None:
        p = self.root / "manifest.json"
        data = p.read_bytes()
        p.write_bytes(data[: len(data) // 2])


def tree(root: Path) -> dict:
    """relative path -> SHA-256 of every file under root (links not followed; a link reads 'symlink')"""
    out = {}
    root = Path(root)
    for p in sorted(root.rglob("*")):
        rel = p.relative_to(root).as_posix()
        if p.is_symlink():
            out[rel] = "symlink"
        elif p.is_file():
            out[rel] = sha256(p)
    return out


def write_analysis(lib: MiniLibrary, editor: list) -> dict:
    """analysis.json from `<editor> audio-analyze --json --detail summary library:*` (spec 2.2.3): one item
    per line, keys sorted, ids sorted, compact items; the CLI item minus ANALYSIS_EXCLUDED and
    format.load_type. Returns the CLI's JSON."""
    env = dict(os.environ, **lib.env())
    r = subprocess.run(editor + ["audio-analyze", "--json", "--detail", "summary", "library:*"], capture_output=True, text=True,
                       env=env, timeout=600)
    if r.returncode != 0:
        raise RuntimeError(f"audio-analyze library:* exit {r.returncode}: {r.stdout[-1500:]} {r.stderr[-1500:]}")
    out = json.loads(r.stdout)
    items = {}
    for it in out["items"]:
        item = {k: v for k, v in it.items() if k not in ANALYSIS_EXCLUDED}
        item["format"] = {k: v for k, v in item["format"].items() if k != "load_type"}
        items[it["path"][len("library:"):]] = item
    lines = [f"{json.dumps(i)}: {json.dumps(items[i], sort_keys=True, separators=(',', ':'))}" for i in sorted(items)]
    version = analyzer_version(editor)
    text = (f'{{"format": "{ANALYSIS_FORMAT}", "version": 1, "analyzer_version": {version}, '
            f'"generated_by": "jasperengine-editor audio-analyze", "items": {{\n' + ",\n".join(lines) + "\n}}\n")
    (lib.root / "analysis.json").write_text(text)
    return out


def analyzer_version(editor: list) -> int:
    r = subprocess.run(editor + ["audio-analyze", "--list-types", "--json"], capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError(f"audio-analyze --list-types exit {r.returncode}: {r.stderr[-1000:]}")
    return json.loads(r.stdout)["analyzer_version"]


def build(dest: Path, editor: list | None = None) -> MiniLibrary:
    """The mini library in dest (created; must not exist): 8 clips, manifest.json, licenses/, and with an
    editor command analysis.json"""
    dest = Path(dest)
    dest.mkdir(parents=True)
    work = Path(tempfile.mkdtemp(prefix="jasper-minilib-"))
    try:
        jobs = []
        for spec in CLIPS:
            wav = work / (spec["id"].replace("/", "__") + ".wav")
            ar.write_wav(wav, spec["signal"](), RATE, ar.FLOAT32)
            ogg = dest / (spec["id"] + ".ogg")
            ogg.parent.mkdir(parents=True, exist_ok=True)
            jobs.append({"id": spec["id"], "wav": str(wav), "ogg": str(ogg), "loop": spec["kind"] == "loop"})
        here = str(Path(__file__).resolve().parent)
        measured = json.loads(_uv_run(_ENCODE, [here], json.dumps(jobs), ("numpy", "soundfile", "pyloudnorm")))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    (dest / "licenses").mkdir()
    (dest / PACK["licence_file"]).write_text(LICENCE_TEXT)
    clips = [clip_entry(spec, dest / (spec["id"] + ".ogg"), measured[spec["id"]]) for spec in CLIPS]
    manifest = {"format": LIBRARY_FORMAT, "version": 4, "licence": "CC0-1.0", "generated_by": "tools/mcp/sound_library_refs.py",
                "packs": [PACK], "clips": sorted(clips, key=lambda c: c["id"])}
    (dest / "manifest.json").write_text(manifest_text(manifest))
    lib = MiniLibrary(dest)
    if editor:
        write_analysis(lib, editor)
    return lib
