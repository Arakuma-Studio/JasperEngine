#!/usr/bin/env python3
"""Build and verify the sound essentials pack (40 CC0 clips by Kenney).

The selection lives in tools/sound-library/essentials.json: the pinned Kenney
zips (URL, SHA-256, size) and, for each clip, the zip member it is copied from
and its type, kind, tags and description. Clips are copied unmodified, so each
committed file's SHA-256 equals its zip member's. A clip that starts or ends off
zero carries "import": the click-safe fades every import writes into its .meta
(import.audioImporter). The manifest (v2) gives each clip `meta`, exactly the S04
.meta sections an import writes (soundlib.clip_meta).

Commands (the output defaults to editor/resources/sound-library/essentials):

    fetch   --cache DIR              download the pinned zips, check their SHA-256
    extract --cache DIR [--out DIR]  copy the listed members and each License.txt
    measure [--out DIR]              apply the hard filters, write manifest.json
    verify  [--out DIR] [--zips DIR] offline check of the committed tree
    all     --cache DIR [--out DIR]  fetch, extract, measure

measure and all need numpy, soundfile and pyloudnorm; fetch, extract and verify
use the standard library only. The versions below reproduce the committed
manifest byte for byte (other versions may move a value by 0.01):

    uv run --no-project --with numpy==2.5.4 --with soundfile==0.14.0 \\
        --with pyloudnorm==0.2.0 --with scipy==1.18.1 \\
        tools/sound-library/build_essentials.py all --cache /tmp/kenney

Everything downloaded is untrusted data: zips are checked against their pinned
SHA-256 before they are opened, only the listed members are read, and nothing
is ever extracted by name from the archive.
"""

import argparse
import json
import math
import re
import sys
import urllib.parse
from pathlib import Path

import soundlib
from soundlib import LICENCE_MARKERS, BuildError, manifest_text, read_licence, read_member, sha256, tree_files

ROOT = Path(__file__).resolve().parent.parent.parent
SELECTION = Path(__file__).resolve().parent / "essentials.json"
DEFAULT_OUT = ROOT / "editor/resources/sound-library/essentials"
MANIFEST = "manifest.json"

MAX_ZIP_BYTES = 20 * 1024 * 1024
MAX_MEMBER_BYTES = 1024 * 1024
MAX_TOTAL_BYTES = 1572864
CLIP_COUNT = 40
ALLOWED_HOST = "kenney.nl"

ID_RE = re.compile(r"^[a-z0-9_]+/[a-z0-9_]+$")
PACK_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")

# Hard filters of the spec (phase 1 §2.12.5).
RATES = (44100, 48000)
PEAK_MAX_DBFS = -0.3
CLIP_LEVEL = 1.0 - 2.0 ** -15
MOMENTARY_MIN_LUFS = -36.0
HEAD_SILENCE_MAX_MS = 15.0
SILENCE_DBFS = -60.0
DURATION_BY_KIND = {
    "transient": (0.01, 2.5),
    "tonal": (0.02, 2.5),
    "swell": (0.1, 3.0),
    "glitch": (0.01, 1.5),
}
LEVEL_FLOOR = -120.0

# A clip's "import" object: the fade keys of its import.audioImporter.
IMPORT_KEYS = ("fade_in_ms", "fade_out_ms")
FADE_MAX_MS = 1000.0


def import_problem(value):
    """Why a clip's "import" object is not a valid subset of the fade keys (None when it is)."""
    if not isinstance(value, dict) or not value:
        return "import must be a non-empty object"
    for key, ms in value.items():
        if key not in IMPORT_KEYS:
            return f"import key {key!r} is not one of {', '.join(IMPORT_KEYS)}"
        if isinstance(ms, bool) or not isinstance(ms, (int, float)) or not math.isfinite(ms) or not 0 < ms <= FADE_MAX_MS:
            return f"import {key} {ms!r} is not a number of milliseconds above 0 and at most {FADE_MAX_MS:g}"
    return None


def load_selection():
    sel = json.loads(SELECTION.read_text(encoding="utf-8"))
    packs = {}
    for p in sel["packs"]:
        if not PACK_RE.match(p["id"]) or p["id"] in packs:
            raise BuildError(f"bad or duplicate pack id {p['id']!r}")
        url = urllib.parse.urlsplit(p["url"])
        if url.scheme != "https" or url.hostname != ALLOWED_HOST:
            raise BuildError(f"pack {p['id']}: URL must be https://{ALLOWED_HOST}/...")
        packs[p["id"]] = p
    ids = set()
    for c in sel["clips"]:
        if not ID_RE.match(c["id"]):
            raise BuildError(f"clip id {c['id']!r} does not match {ID_RE.pattern}")
        if c["id"] in ids:
            raise BuildError(f"duplicate clip id {c['id']}")
        ids.add(c["id"])
        if c["pack"] not in packs:
            raise BuildError(f"clip {c['id']}: unknown pack {c['pack']}")
        if c["kind"] not in DURATION_BY_KIND:
            raise BuildError(f"clip {c['id']}: kind {c['kind']!r} is not one of {sorted(DURATION_BY_KIND)}")
        if "import" in c and import_problem(c["import"]):
            raise BuildError(f"clip {c['id']}: {import_problem(c['import'])}")
    if len(ids) != CLIP_COUNT:
        raise BuildError(f"the selection has {len(ids)} clips, expected exactly {CLIP_COUNT}")
    return sel, packs


def used_packs(sel, packs):
    """The packs at least one clip comes from, sorted by id."""
    return [packs[i] for i in sorted({c["pack"] for c in sel["clips"]})]


def zip_name(pack):
    return f"kenney_{pack['id']}.zip"


def clip_path(clip):
    return clip["id"] + ".ogg"


def licence_path(pack_id):
    return f"licenses/{pack_id}.txt"


# fetch

def download(url, limit):
    """HTTPS GET into memory; redirects and the final URL must stay on kenney.nl."""
    return soundlib.download(url, limit, (ALLOWED_HOST,), "JasperEngine build_essentials.py")


def cmd_fetch(args):
    sel, packs = load_selection()
    cache = Path(args.cache)
    cache.mkdir(parents=True, exist_ok=True)
    for pack in used_packs(sel, packs):
        dest = cache / zip_name(pack)
        if dest.is_file() and dest.stat().st_size == pack["bytes"] and sha256(dest.read_bytes()) == pack["sha256"]:
            print(f"fetch: {dest.name} cached, SHA-256 ok")
            continue
        print(f"fetch: {pack['url']}")
        data = download(pack["url"], MAX_ZIP_BYTES)
        digest = sha256(data)
        if digest != pack["sha256"] or len(data) != pack["bytes"]:
            if dest.exists():
                dest.unlink()
            raise BuildError(
                f"{pack['id']}: got {len(data)} bytes with SHA-256 {digest}, expected {pack['bytes']} bytes "
                f"with {pack['sha256']}; nothing was kept. The pack changed upstream: review it before "
                f"updating essentials.json")
        tmp = dest.with_suffix(".part")
        tmp.write_bytes(data)
        tmp.replace(dest)
        print(f"fetch: {dest.name} {len(data)} bytes, SHA-256 ok")


# extract

def open_pinned_zip(cache, pack):
    return soundlib.open_pinned_zip(Path(cache) / zip_name(pack), pack, "essentials.json")


def expected_files(sel, packs):
    files = {MANIFEST}
    files.update(clip_path(c) for c in sel["clips"])
    files.update(licence_path(p["id"]) for p in used_packs(sel, packs))
    return files


def cmd_extract(args):
    sel, packs = load_selection()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    extra = sorted(tree_files(out) - expected_files(sel, packs))
    if extra:
        raise BuildError(f"{out} holds files that are not part of the pack: {', '.join(extra)}; remove them first")
    for pack in used_packs(sel, packs):
        with open_pinned_zip(args.cache, pack) as zf:
            dest = out / licence_path(pack["id"])
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(read_licence(zf, pack["id"]))
            for clip in (c for c in sel["clips"] if c["pack"] == pack["id"]):
                data = read_member(zf, clip["member"], MAX_MEMBER_BYTES)
                if not data.startswith(b"OggS"):
                    raise BuildError(f"{clip['id']}: {clip['member']} is not an Ogg file")
                dest = out / clip_path(clip)
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
    print(f"extract: {len(sel['clips'])} clips and {len(used_packs(sel, packs))} licences into {out}")


# measure

def dbfs(x):
    return LEVEL_FLOOR if x <= 0.0 else max(LEVEL_FLOOR, 20.0 * math.log10(x))


def measure_clip(path):
    """Measure with an independent decoder (libsndfile) and pyloudnorm."""
    import numpy as np
    import pyloudnorm
    import soundfile

    y, rate = soundfile.read(str(path), dtype="float64", always_2d=True)
    frames, channels = y.shape
    absy = np.abs(y)
    peak = float(absy.max()) if frames else 0.0
    clipped = int(np.count_nonzero(absy >= CLIP_LEVEL))

    # Head and tail silence: first and last sample at or above -60 dBFS on any channel.
    loud = np.nonzero(absy.max(axis=1) >= 10.0 ** (SILENCE_DBFS / 20.0))[0] if frames else []
    if len(loud):
        head_ms = loud[0] / rate * 1000.0
        tail_ms = (frames - 1 - loud[-1]) / rate * 1000.0
    else:
        head_ms = tail_ms = frames / rate * 1000.0

    # Momentary max, file mode (§2.6): 400 ms windows every 10 ms over the signal
    # zero-padded by 400 ms at both ends, K-weighted continuously. pyloudnorm keeps
    # the loudness of every gating block in blockwise_loudness; with a 400 ms block
    # and 97.5% overlap the blocks are exactly those windows.
    pad = np.zeros((int(round(0.4 * rate)), channels))
    meter = pyloudnorm.Meter(rate, block_size=0.4, overlap=0.975)
    meter.integrated_loudness(np.vstack([pad, y, pad]))
    blocks = [b for b in meter.blockwise_loudness if np.isfinite(b)]
    momentary_max = max(blocks) if blocks else None

    return {
        "channels": int(channels),
        "sample_rate": int(rate),
        "frames": int(frames),
        "duration_s": frames / rate,
        "sample_peak_dbfs": dbfs(peak),
        "clipped": clipped,
        "momentary_max_lufs": momentary_max,
        "head_ms": float(head_ms),
        "tail_ms": float(tail_ms),
    }


def check_filters(clip, m):
    problems = []
    if m["channels"] not in (1, 2):
        problems.append(f"{m['channels']} channels (1 or 2 allowed)")
    if m["sample_rate"] not in RATES:
        problems.append(f"sample rate {m['sample_rate']} (44100 or 48000 allowed)")
    if m["sample_peak_dbfs"] > PEAK_MAX_DBFS:
        problems.append(f"sample peak {m['sample_peak_dbfs']:.2f} dBFS > {PEAK_MAX_DBFS}")
    if m["clipped"]:
        problems.append(f"{m['clipped']} clipped samples")
    if m["momentary_max_lufs"] is None:
        problems.append("momentary max undefined (digital silence)")
    elif m["momentary_max_lufs"] < MOMENTARY_MIN_LUFS:
        problems.append(f"momentary max {m['momentary_max_lufs']:.2f} LUFS < {MOMENTARY_MIN_LUFS}")
    if m["head_ms"] > HEAD_SILENCE_MAX_MS:
        problems.append(f"head silence {m['head_ms']:.1f} ms > {HEAD_SILENCE_MAX_MS}")
    lo, hi = DURATION_BY_KIND[clip["kind"]]
    if not lo <= m["duration_s"] <= hi:
        problems.append(f"duration {m['duration_s']:.3f} s outside {lo}-{hi} s for kind {clip['kind']}")
    return problems


def build_manifest(sel, packs, out, measured):
    author = sel["author"]
    licence = sel["licence"]
    manifest_packs = [{
        "id": p["id"], "title": p["title"], "url": p["url"], "landing": p["landing"],
        "sha256": p["sha256"], "bytes": p["bytes"], "licence": licence,
        "licence_file": licence_path(p["id"]), "author": author,
    } for p in used_packs(sel, packs)]
    clips = []
    for c in sorted(sel["clips"], key=lambda c: c["id"]):
        data = (out / clip_path(c)).read_bytes()
        m = measured[c["id"]]
        clip = {
            "id": c["id"], "path": clip_path(c), "sha256": sha256(data), "bytes": len(data),
            "type": c["type"], "kind": c["kind"], "tags": c["tags"], "description": c["description"],
            "licence": licence, "source": sel["source"], "pack": c["pack"], "source_file": c["member"],
            "author": author, "url": packs[c["pack"]]["landing"],
            "measured": {
                "channels": m["channels"],
                "sample_rate": m["sample_rate"],
                "frames": m["frames"],
                "duration_s": round(m["duration_s"], 3),
                "sample_peak_dbfs": round(m["sample_peak_dbfs"], 2),
                "momentary_max_lufs": round(m["momentary_max_lufs"], 2),
                "head_ms": round(m["head_ms"], 1),
                "tail_ms": round(m["tail_ms"], 1),
            },
        }
        clip["meta"] = soundlib.clip_meta(clip, c.get("import"), "essentials")
        clips.append(clip)
    return {
        "format": "jasperengine-sound-essentials",
        "version": 2,
        "licence": licence,
        "generated_by": "tools/sound-library/build_essentials.py",
        "packs": manifest_packs,
        "clips": clips,
    }


def cmd_measure(args):
    sel, packs = load_selection()
    out = Path(args.out)
    missing = sorted(f for f in expected_files(sel, packs) - {MANIFEST} if not (out / f).is_file())
    if missing:
        raise BuildError(f"missing in {out}: {', '.join(missing)}; run extract first")
    failures, measured = [], {}
    for c in sel["clips"]:
        try:
            m = measure_clip(out / clip_path(c))
        except Exception as e:  # an undecodable file is a filter failure
            failures.append(f"{c['id']}: does not decode ({e})")
            continue
        measured[c["id"]] = m
        failures.extend(f"{c['id']}: {p}" for p in check_filters(c, m))
    total = sum((out / clip_path(c)).stat().st_size for c in sel["clips"])
    if total > MAX_TOTAL_BYTES:
        failures.append(f"total size {total} bytes > {MAX_TOTAL_BYTES}")
    if failures:
        raise BuildError("hard filters failed:\n  " + "\n  ".join(failures))
    with open(out / MANIFEST, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(manifest_text(build_manifest(sel, packs, out, measured)))
    print(f"measure: {len(measured)} clips pass the filters; {total} bytes of audio; wrote {out / MANIFEST}")


# verify

def cmd_verify(args):
    sel, packs = load_selection()
    out = Path(args.out)
    errors = []
    manifest_file = out / MANIFEST
    if not manifest_file.is_file():
        raise BuildError(f"{manifest_file} is missing")
    manifest = json.loads(manifest_file.read_text(encoding="utf-8"))

    # The manifest must describe exactly the selection.
    if manifest.get("format") != "jasperengine-sound-essentials" or manifest.get("version") != 2:
        errors.append("manifest format/version is not jasperengine-sound-essentials v2")
    by_id = {c["id"]: c for c in sel["clips"]}
    mclips = manifest.get("clips", [])
    if [c["id"] for c in mclips] != sorted(by_id):
        errors.append("manifest clip ids differ from essentials.json (or are not sorted)")
    for mc in mclips:
        c = by_id.get(mc["id"])
        if c is None:
            continue
        expect = {"path": clip_path(c), "type": c["type"], "kind": c["kind"], "tags": c["tags"],
                  "description": c["description"], "pack": c["pack"], "source_file": c["member"],
                  "licence": sel["licence"], "author": sel["author"], "source": sel["source"],
                  "url": packs[c["pack"]]["landing"]}
        for key, value in expect.items():
            if mc.get(key) != value:
                errors.append(f"{mc['id']}: manifest {key} {mc.get(key)!r}, essentials.json says {value!r}")
        if mc.get("meta") != soundlib.clip_meta(mc, c.get("import"), "essentials"):
            errors.append(f"{mc['id']}: manifest meta {mc.get('meta')!r} is not the .meta sections an import of it writes")
    mpacks = {p["id"]: p for p in manifest.get("packs", [])}
    if sorted(mpacks) != [p["id"] for p in used_packs(sel, packs)]:
        errors.append("manifest packs differ from the packs the clips use")
    for pid, mp in mpacks.items():
        p = packs.get(pid, {})
        for key in ("url", "sha256", "bytes", "landing", "title"):
            if mp.get(key) != p.get(key):
                errors.append(f"pack {pid}: manifest {key} {mp.get(key)!r}, essentials.json says {p.get(key)!r}")
        if mp.get("licence_file") != licence_path(pid):
            errors.append(f"pack {pid}: licence_file should be {licence_path(pid)}")

    # Files: sizes and hashes, licences, nothing else.
    total = 0
    for mc in mclips:
        f = out / mc["path"]
        if not f.is_file():
            errors.append(f"{mc['path']} is missing")
            continue
        data = f.read_bytes()
        total += len(data)
        if len(data) != mc["bytes"]:
            errors.append(f"{mc['path']}: {len(data)} bytes, manifest says {mc['bytes']}")
        if sha256(data) != mc["sha256"]:
            errors.append(f"{mc['path']}: SHA-256 {sha256(data)}, manifest says {mc['sha256']}")
    for pid in mpacks:
        f = out / licence_path(pid)
        if not f.is_file():
            errors.append(f"{licence_path(pid)} is missing")
            continue
        text = f.read_bytes().decode("utf-8", errors="replace")
        for marker in LICENCE_MARKERS:
            if marker not in text:
                errors.append(f"{licence_path(pid)} does not contain {marker!r}")
    extra = sorted(tree_files(out) - expected_files(sel, packs))
    if extra:
        errors.append(f"files not in the manifest: {', '.join(extra)}")
    if len(mclips) != CLIP_COUNT:
        errors.append(f"{len(mclips)} clips, expected {CLIP_COUNT}")
    if total > MAX_TOTAL_BYTES:
        errors.append(f"total {total} bytes > {MAX_TOTAL_BYTES}")

    # Optionally: every file equals its zip member byte for byte.
    if args.zips and not errors:
        for pid in sorted(mpacks):
            try:
                with open_pinned_zip(args.zips, packs[pid]) as zf:
                    if read_licence(zf, pid) != (out / licence_path(pid)).read_bytes():
                        errors.append(f"{licence_path(pid)} differs from {pid}'s License.txt")
                    for mc in (m for m in mclips if m["pack"] == pid):
                        if read_member(zf, mc["source_file"], MAX_MEMBER_BYTES) != (out / mc["path"]).read_bytes():
                            errors.append(f"{mc['path']} differs from {pid}:{mc['source_file']}")
            except BuildError as e:
                errors.append(str(e))

    if errors:
        raise BuildError("verify failed:\n  " + "\n  ".join(errors))
    checked = " and their zip members" if args.zips else ""
    print(f"verify: {len(mclips)} clips ({total} bytes), {len(mpacks)} licences{checked}: ok")


def cmd_all(args):
    cmd_fetch(args)
    cmd_extract(args)
    cmd_measure(args)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name, needs_cache in (("fetch", True), ("extract", True), ("measure", False), ("verify", False), ("all", True)):
        p = sub.add_parser(name)
        if needs_cache:
            p.add_argument("--cache", required=True, help="directory of the downloaded zips")
        if name != "fetch":
            p.add_argument("--out", default=str(DEFAULT_OUT), help="essentials directory (default: %(default)s)")
        if name == "verify":
            p.add_argument("--zips", help="also compare every file with its member in the zips in this directory")
    args = parser.parse_args(argv)
    try:
        {"fetch": cmd_fetch, "extract": cmd_extract, "measure": cmd_measure, "verify": cmd_verify, "all": cmd_all}[args.command](args)
    except BuildError as e:
        print(f"build_essentials.py {args.command}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
