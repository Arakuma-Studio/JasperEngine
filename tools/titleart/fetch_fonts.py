#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Fetches the Title Art library's font pack from google/fonts (docs/specs/S84-title-library.md R28, §6.11).

    python3 tools/titleart/fetch_fonts.py             # download the pack ids whose files do not verify
    python3 tools/titleart/fetch_fonts.py --force     # download every pack id again
    python3 tools/titleart/fetch_fonts.py --verify    # offline: the files against SOURCE.json

A maintainer tool, never run by the editor. Every family comes from google/fonts at one pinned
commit and only from its ofl/ tree (SIL Open Font License 1.1): a path in any other licence
directory fails the fetch before anything is downloaded. Each pack id gets
editor/resources/titleart/fonts/<pack id>/ with the font file(s) byte-identical to upstream and
named as upstream, upstream's OFL.txt verbatim and SOURCE.json ({repo, commit, path or paths,
sha256, bytes, fetched, license}); fonts.json there is the manifest the editor reads (family,
style, files, default variation values, axes, scripts, category, look families, the font's own
version string). Variable fonts stay variable: no instance is derived, so no file is modified.

Standard library only.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import posixpath
import struct
import sys
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FONTS_DIR = REPO / "editor" / "resources" / "titleart" / "fonts"
MANIFEST = FONTS_DIR / "fonts.json"
UPSTREAM = "google/fonts"
COMMIT = "2fe1e8c6b137fb13041a7cfca6fdcd8a910ea4dc"
RAW = f"https://raw.githubusercontent.com/{UPSTREAM}/{COMMIT}/"
LICENSE_DIR = "ofl/"
LICENSE = "OFL-1.1"

# pack id -> (family, style, {upstream path: bytes per §6.11}, scripts, category, look families,
# default variations). category is upstream METADATA.pb's first one (DISPLAY, SERIF, SANS_SERIF);
# the variations are the values the looks of §6.8 use most (a look may set others).
FONTS = {
    "dela-gothic-one": ("Dela Gothic One", "Regular", {"ofl/delagothicone/DelaGothicOne-Regular.ttf": 2508848},
                        ["latin", "ja"], "display", ["casino", "anime", "elements"], {}),
    "mplus1p-black": ("M PLUS 1p", "Black", {"ofl/mplus1p/MPLUS1p-Black.ttf": 1774332},
                      ["latin", "ja"], "sans", ["minimal", "anime"], {}),
    "zen-kaku-gothic-new-black": ("Zen Kaku Gothic New", "Black", {"ofl/zenkakugothicnew/ZenKakuGothicNew-Black.ttf": 2300600},
                                  ["latin", "ja"], "sans", ["metal", "sports", "minimal"], {}),
    "zen-old-mincho-black": ("Zen Old Mincho", "Black", {"ofl/zenoldmincho/ZenOldMincho-Black.ttf": 5416348},
                             ["latin", "ja"], "serif", ["casino", "wa", "fantasy", "gems"], {}),
    "yuji-syuku": ("Yuji Syuku", "Regular", {"ofl/yujisyuku/YujiSyuku-Regular.ttf": 8430348},
                   ["latin", "ja"], "serif", ["wa", "horror"], {}),
    "reggae-one": ("Reggae One", "Regular", {"ofl/reggaeone/ReggaeOne-Regular.ttf": 2153256},
                   ["latin", "ja"], "display", ["anime"], {}),
    "rocknroll-one": ("RocknRoll One", "Regular", {"ofl/rocknrollone/RocknRollOne-Regular.ttf": 2682824},
                      ["latin", "ja"], "sans", ["neon"], {}),
    "rampart-one": ("Rampart One", "Regular", {"ofl/rampartone/RampartOne-Regular.ttf": 3722352},
                    ["latin", "ja"], "display", ["retro"], {}),
    "train-one": ("Train One", "Regular", {"ofl/trainone/TrainOne-Regular.ttf": 2152516},
                  ["latin", "ja"], "display", ["scifi", "retro"], {}),
    "potta-one": ("Potta One", "Regular", {"ofl/pottaone/PottaOne-Regular.ttf": 4918516},
                  ["latin", "ja"], "display", ["cartoon"], {}),
    "mochiy-pop-one": ("Mochiy Pop One", "Regular", {"ofl/mochiypopone/MochiyPopOne-Regular.ttf": 5163948},
                       ["latin", "ja"], "sans", ["cartoon", "anime"], {}),
    "playfair-black-italic": ("Playfair", "Italic", {"ofl/playfair/Playfair-Italic[opsz,wdth,wght].ttf": 1783732},
                              ["latin"], "display", ["casino", "gems"], {"wght": 900, "opsz": 5}),
    "anton": ("Anton", "Regular", {"ofl/anton/Anton-Regular.ttf": 170812},
              ["latin"], "display", ["metal", "sports"], {}),
    "russo-one": ("Russo One", "Regular", {"ofl/russoone/RussoOne-Regular.ttf": 39124},
                  ["latin", "cyrillic"], "sans", ["metal", "scifi", "anime"], {}),
    "bungee": ("Bungee", "Regular", {"ofl/bungee/Bungee-Regular.ttf": 118996},
               ["latin"], "display", ["casino", "retro"], {}),
    "monoton": ("Monoton", "Regular", {"ofl/monoton/Monoton-Regular.ttf": 51604},
                ["latin"], "display", ["neon"], {}),
    "tilt-neon": ("Tilt Neon", "Regular", {"ofl/tiltneon/TiltNeon[XROT,YROT].ttf": 583288},
                  ["latin"], "display", ["neon"], {"XROT": 0, "YROT": 0}),
    "cinzel-decorative-black": ("Cinzel Decorative", "Black", {"ofl/cinzeldecorative/CinzelDecorative-Black.ttf": 62692},
                                ["latin"], "display", ["fantasy", "horror"], {}),
    "medievalsharp": ("MedievalSharp", "Regular", {"ofl/medievalsharp/MedievalSharp.ttf": 148712},
                      ["latin"], "display", ["fantasy"], {}),
    "orbitron": ("Orbitron", "Regular", {"ofl/orbitron/Orbitron[wght].ttf": 38576},
                 ["latin"], "sans", ["scifi"], {"wght": 900}),
    "audiowide": ("Audiowide", "Regular", {"ofl/audiowide/Audiowide-Regular.ttf": 69916},
                  ["latin"], "display", ["scifi", "neon", "retro"], {}),
    "creepster": ("Creepster", "Regular", {"ofl/creepster/Creepster-Regular.ttf": 62724},
                  ["latin"], "display", ["horror"], {}),
    "nosifer": ("Nosifer", "Regular", {"ofl/nosifer/Nosifer-Regular.ttf": 43272},
                ["latin"], "display", ["horror"], {}),
    "bangers": ("Bangers", "Regular", {"ofl/bangers/Bangers-Regular.ttf": 93148},
                ["latin"], "display", ["cartoon"], {}),
    "lilita-one": ("Lilita One", "Regular", {"ofl/lilitaone/LilitaOne-Regular.ttf": 28092},
                   ["latin"], "display", ["cartoon"], {}),
    "titan-one": ("Titan One", "Regular", {"ofl/titanone/TitanOne-Regular.ttf": 55712},
                  ["latin"], "display", ["cartoon", "casino"], {}),
    "press-start-2p": ("Press Start 2P", "Regular", {"ofl/pressstart2p/PressStart2P-Regular.ttf": 118204},
                       ["latin"], "display", ["retro"], {}),
    "silkscreen": ("Silkscreen", "Regular, Bold", {"ofl/silkscreen/Silkscreen-Regular.ttf": 32220,
                                                   "ofl/silkscreen/Silkscreen-Bold.ttf": 30632},
                   ["latin"], "display", ["retro"], {}),
    "teko": ("Teko", "Regular", {"ofl/teko/Teko[wght].ttf": 292108},
             ["latin", "devanagari"], "sans", ["sports"], {"wght": 700}),
    "black-ops-one": ("Black Ops One", "Regular", {"ofl/blackopsone/BlackOpsOne-Regular.ttf": 166532},
                      ["latin"], "display", ["sports", "horror"], {}),
    "inter": ("Inter", "Regular", {"ofl/inter/Inter[opsz,wght].ttf": 876576},
              ["latin", "greek", "cyrillic"], "sans", ["minimal"], {"wght": 800}),
    "archivo-black": ("Archivo Black", "Regular", {"ofl/archivoblack/ArchivoBlack-Regular.ttf": 90988},
                      ["latin"], "sans", ["sports", "minimal"], {}),
}


def under_ofl(path: str) -> bool:
    """A google/fonts path in the OFL tree: ofl/<family>/<file>, nothing that climbs out of it."""
    return path.startswith(LICENSE_DIR) and posixpath.normpath(path) == path and ".." not in path.split("/") \
        and path.count("/") == 2


def license_path(paths) -> str:
    return posixpath.dirname(next(iter(paths))) + "/OFL.txt"


def download(path: str) -> bytes:
    url = RAW + urllib.parse.quote(path, safe="/")
    request = urllib.request.Request(url, headers={"User-Agent": "JasperEngine fetch_fonts.py"})
    with urllib.request.urlopen(request, timeout=600) as response:
        return response.read()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sfnt_tables(data: bytes) -> dict:
    count = struct.unpack_from(">H", data, 4)[0]
    tables = {}
    for i in range(count):
        tag, _, offset, length = struct.unpack_from(">4sIII", data, 12 + i * 16)
        tables[tag.decode("latin-1")] = (offset, length)
    return tables


def font_version(data: bytes) -> str:
    """The name table's version string (nameID 5), Windows Unicode first, then Macintosh Roman."""
    tables = sfnt_tables(data)
    if "name" not in tables:
        return ""
    base = tables["name"][0]
    _, count, storage = struct.unpack_from(">HHH", data, base)
    found = {}
    for i in range(count):
        platform, encoding, _, name_id, length, offset = struct.unpack_from(">6H", data, base + 6 + i * 12)
        if name_id != 5:
            continue
        raw = data[base + storage + offset:base + storage + offset + length]
        if platform == 3 and encoding in (1, 10):
            found.setdefault(0, raw.decode("utf-16-be", "replace"))
        elif platform == 0:
            found.setdefault(1, raw.decode("utf-16-be", "replace"))
        elif platform == 1 and encoding == 0:
            found.setdefault(2, raw.decode("mac_roman", "replace"))
    return found[min(found)].strip() if found else ""


def font_axes(data: bytes) -> dict:
    """A variable font's fvar axes: {tag: [min, default, max]}; {} for a static font."""
    tables = sfnt_tables(data)
    if "fvar" not in tables:
        return {}
    base = tables["fvar"][0]
    axes_offset, _, axis_count, axis_size = struct.unpack_from(">HHHH", data, base + 4)
    axes = {}
    for i in range(axis_count):
        tag, low, default, high = struct.unpack_from(">4siii", data, base + axes_offset + i * axis_size)
        axes[tag.decode("latin-1")] = [round(v / 65536.0, 4) for v in (low, default, high)]
    return axes


def source_record(paths: list[str], blobs: list[bytes], licence: bytes, fetched: str) -> dict:
    record = {"repo": UPSTREAM, "commit": COMMIT, "fetched": fetched,
              "license": {"path": license_path(paths), "sha256": sha256(licence), "bytes": len(licence)}}
    if len(paths) == 1:
        record.update({"path": paths[0], "sha256": sha256(blobs[0]), "bytes": len(blobs[0])})
    else:
        record.update({"paths": paths, "sha256": [sha256(b) for b in blobs], "bytes": [len(b) for b in blobs]})
    return record


def recorded_files(record: dict) -> list[tuple[str, str, int]]:
    """(upstream path, sha256, bytes) of a SOURCE.json's font files."""
    if "path" in record:
        return [(record["path"], record["sha256"], record["bytes"])]
    return list(zip(record.get("paths", []), record.get("sha256", []), record.get("bytes", [])))


def pack_problems(pack: str) -> list[str]:
    """What is wrong with one pack directory against its SOURCE.json and FONTS (offline)."""
    directory = FONTS_DIR / pack
    source = directory / "SOURCE.json"
    if not source.is_file():
        return [f"{pack}: no SOURCE.json"]
    try:
        record = json.loads(source.read_text(encoding="utf-8"))
    except ValueError as error:
        return [f"{pack}: SOURCE.json: {error}"]
    problems = []
    if record.get("repo") != UPSTREAM or record.get("commit") != COMMIT:
        problems.append(f"{pack}: SOURCE.json names {record.get('repo')}@{record.get('commit')}, not {UPSTREAM}@{COMMIT}")
    files = recorded_files(record)
    if [path for path, _, _ in files] != list(FONTS[pack][2]):
        problems.append(f"{pack}: SOURCE.json's paths are not fetch_fonts.py's ({', '.join(FONTS[pack][2])})")
    licence = record.get("license", {})
    for path, digest, size in files + [(licence.get("path", ""), licence.get("sha256"), licence.get("bytes"))]:
        if not under_ofl(path):
            problems.append(f"{pack}: {path or '(no licence path)'} is not under {LICENSE_DIR}")
            continue
        local = directory / posixpath.basename(path)
        if not local.is_file():
            problems.append(f"{pack}: {local.name} is missing")
            continue
        data = local.read_bytes()
        if len(data) != size:
            problems.append(f"{pack}: {local.name} has {len(data)} bytes, SOURCE.json says {size}")
        if sha256(data) != digest:
            problems.append(f"{pack}: {local.name} does not match its SHA-256")
    return problems


def verify() -> int:
    problems = []
    for pack, (_, _, paths, *_rest) in FONTS.items():
        problems += [f"{pack}: {path} is not under {LICENSE_DIR}" for path in paths if not under_ofl(path)]
        problems += pack_problems(pack)
    if FONTS_DIR.is_dir():
        problems += [f"{child.name}: a directory fetch_fonts.py does not list" for child in sorted(FONTS_DIR.iterdir())
                     if child.is_dir() and child.name not in FONTS]
    try:
        listed = set(json.loads(MANIFEST.read_text(encoding="utf-8"))["fonts"])
        problems += [f"{pack}: fonts.json does not list it" for pack in FONTS if pack not in listed]
        problems += [f"{pack}: fonts.json lists it, fetch_fonts.py does not" for pack in sorted(listed - set(FONTS))]
    except (OSError, ValueError, KeyError) as error:
        problems.append(f"fonts.json: {error}")
    for problem in problems:
        print(f"fetch_fonts: {problem}")
    if not problems:
        print(f"fetch_fonts: {len(FONTS)} pack ids verified against SOURCE.json ({UPSTREAM} {COMMIT[:12]})")
    return 1 if problems else 0


def fetch(force: bool) -> int:
    outside = [f"{pack}: {path}" for pack, entry in FONTS.items() for path in entry[2] if not under_ofl(path)]
    if outside:
        raise SystemExit("fetch_fonts: only google/fonts' ofl/ tree is allowed: " + "; ".join(outside))
    FONTS_DIR.mkdir(parents=True, exist_ok=True)
    fetched = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    manifest = {"version": 1, "repo": UPSTREAM, "commit": COMMIT, "license": LICENSE, "fonts": {}}
    total = 0
    for pack, (family, style, expected, scripts, category, looks, variations) in FONTS.items():
        directory = FONTS_DIR / pack
        paths = list(expected)
        if not force and not pack_problems(pack):
            blobs = [(directory / posixpath.basename(p)).read_bytes() for p in paths]
        else:
            blobs = [download(p) for p in paths]
            licence = download(license_path(paths))
            directory.mkdir(exist_ok=True)
            for path, data in zip(paths, blobs):
                (directory / posixpath.basename(path)).write_bytes(data)
            (directory / "OFL.txt").write_bytes(licence)
            record = source_record(paths, blobs, licence, fetched)
            (directory / "SOURCE.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            print(f"fetch_fonts: {pack}: {', '.join(posixpath.basename(p) for p in paths)} ({sum(map(len, blobs))} bytes)")
        for path, data in zip(paths, blobs):
            if len(data) != expected[path]:
                print(f"fetch_fonts: {pack}: upstream {path} has {len(data)} bytes, §6.11 says {expected[path]}")
        total += sum(map(len, blobs))
        axes = font_axes(blobs[0])
        unknown = sorted(set(variations) - set(axes))
        if unknown:
            raise SystemExit(f"fetch_fonts: {pack}: the default variations name axes the font lacks: {', '.join(unknown)}")
        manifest["fonts"][pack] = {
            "family": family, "style": style, "files": [posixpath.basename(p) for p in paths],
            "variations": variations, "axes": axes, "scripts": scripts, "category": category,
            "look_families": looks, "font_version": font_version(blobs[0]),
            "bytes": sum(map(len, blobs)), "license": LICENSE, "license_file": "OFL.txt",
            "url": RAW + paths[0],
        }
    manifest["bytes"] = total
    MANIFEST.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"fetch_fonts: {len(FONTS)} pack ids, {total} bytes of fonts; wrote {MANIFEST.relative_to(REPO)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--verify", action="store_true", help="offline: compare the files with SOURCE.json")
    parser.add_argument("--force", action="store_true", help="download every pack id, also those that verify")
    args = parser.parse_args()
    return verify() if args.verify else fetch(args.force)


if __name__ == "__main__":
    sys.exit(main())
