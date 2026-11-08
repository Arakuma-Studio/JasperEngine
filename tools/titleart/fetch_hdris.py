#!/usr/bin/env python3
"""Fetches the title environments' HDRIs from Poly Haven (docs/specs/S86-title-3d.md R24, R66, §6.5).

    python3 tools/titleart/fetch_hdris.py            # download what the manifest lacks, write it
    python3 tools/titleart/fetch_hdris.py --check    # the manifest's hashes against the files

A maintainer tool, never run by the editor. Each environment is found by its Poly Haven title
through the public API (api.polyhaven.com), never by a hard-coded slug and never from another
project's copy; the 2k Radiance .hdr is downloaded into editor/resources/titleart/hdri/ and its
slug, URL, SHA-256 and licence (CC0) are written into manifest.json there. A set of four (S86's
photo set, S84's) may hold at most 30 MB: while it is over, its largest file drops to 1k. A title
the API no longer lists fails loudly, so the maintainer picks a CC0 replacement and records it.

Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HDRI_DIR = REPO / "editor" / "resources" / "titleart" / "hdri"
MANIFEST = HDRI_DIR / "manifest.json"
API = "https://api.polyhaven.com"
SET_LIMIT = 30 * 1024 * 1024
LICENSE = "CC0-1.0 (Poly Haven, https://polyhaven.com/license)"

# environment id -> (Poly Haven title, set): S86's softbox and photos, S84's four
ENVIRONMENTS = {
    "softbox": ("Studio Small 09", "s86"),
    "highway": ("Autumn Road", "s86"),
    "city": ("Potsdamer Platz", "s86"),
    "sky": ("Derelict Highway Midday", "s86"),
}


def get_json(url: str):
    request = urllib.request.Request(url, headers={"User-Agent": "JasperEngine fetch_hdris.py"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "JasperEngine fetch_hdris.py"})
    with urllib.request.urlopen(request, timeout=600) as response:
        return response.read()


def find_slug(assets: dict, title: str) -> str:
    matches = [slug for slug, info in assets.items() if info.get("name", "").strip().lower() == title.lower()]
    if not matches:
        raise SystemExit(f"fetch_hdris: Poly Haven lists no HDRI titled '{title}': pick a CC0 replacement and record it")
    return sorted(matches)[0]


def fetch() -> int:
    HDRI_DIR.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(MANIFEST.read_text()) if MANIFEST.exists() else {"version": 1, "environments": {}}
    assets = get_json(f"{API}/assets?t=hdris")
    for env, (title, group) in ENVIRONMENTS.items():
        slug = find_slug(assets, title)
        files = get_json(f"{API}/files/{slug}")
        entry = {"title": title, "slug": slug, "set": group, "license": LICENSE}
        for resolution in ("2k", "1k"):
            hdr = files["hdri"][resolution]["hdr"]
            entry.update({"resolution": resolution, "url": hdr["url"], "file": f"{slug}_{resolution}.hdr"})
            entry["_size"] = hdr.get("size", 0)
            if resolution == "2k":
                break
        manifest["environments"][env] = entry
    # the 30 MB rule per set: the largest drops to 1k while the set is over
    for group in sorted({g for _, g in ENVIRONMENTS.values()}):
        while True:
            members = [e for e in manifest["environments"].values() if e["set"] == group]
            if sum(e.get("_size", 0) for e in members) <= SET_LIMIT:
                break
            largest = max((e for e in members if e["resolution"] == "2k"), key=lambda e: e.get("_size", 0), default=None)
            if largest is None:
                raise SystemExit(f"fetch_hdris: set {group} stays over 30 MB at 1k")
            files = get_json(f"{API}/files/{largest['slug']}")
            hdr = files["hdri"]["1k"]["hdr"]
            largest.update({"resolution": "1k", "url": hdr["url"], "file": f"{largest['slug']}_1k.hdr", "_size": hdr.get("size", 0)})
    for env, entry in manifest["environments"].items():
        target = HDRI_DIR / entry["file"]
        data = download(entry["url"])
        target.write_bytes(data)
        entry["sha256"] = hashlib.sha256(data).hexdigest()
        entry["bytes"] = len(data)
        entry.pop("_size", None)
        print(f"fetch_hdris: {env}: {entry['file']} ({len(data)} bytes)")
    MANIFEST.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return 0


def check() -> int:
    if not MANIFEST.exists():
        print("fetch_hdris: no manifest")
        return 1
    manifest = json.loads(MANIFEST.read_text())
    status = 0
    for env, entry in manifest.get("environments", {}).items():
        path = HDRI_DIR / entry["file"]
        if not path.exists():
            print(f"fetch_hdris: {env}: {entry['file']} is missing")
            status = 1
            continue
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            print(f"fetch_hdris: {env}: {entry['file']} does not match its SHA-256")
            status = 1
    return status


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="compare the files with the manifest's hashes")
    args = parser.parse_args()
    return check() if args.check else fetch()


if __name__ == "__main__":
    sys.exit(main())
