#!/usr/bin/env python3
"""Writes editor/players/PlayerRuntimeHashes.gen.h from a release's player runtime archives
(docs/specs/S30-shipping.md D5): the editor of that release installs only archives with these
SHA-256 values.

    python3 tools/players/write_hashes.py ARCHIVE_DIR [--version V] [--out editor/players/PlayerRuntimeHashes.gen.h]

ARCHIVE_DIR holds the jasperengine-player-<id>-<version>.zip files (tools/players/package_player.py
--zip). Each archive's id is the one its player-info.json names: a variant's id (-ui, -lite3d...)
cannot be told from its file name, whose version may hold dashes too. An archive whose name is not
its player-info's id and version, an id twice, or (--version) another version than the release's
writes nothing and exits 1. Without archives the header has no entries (a development editor).
Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
NAME = re.compile(r"^jasperengine-player-.+\.zip$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archives", type=Path)
    parser.add_argument("--version", help="the release's version: every archive must be of it")
    parser.add_argument("--out", type=Path, default=REPO / "editor/players/PlayerRuntimeHashes.gen.h")
    args = parser.parse_args()
    entries = []
    seen: dict[str, str] = {}
    problems = []
    for archive in sorted(args.archives.rglob("*.zip")) if args.archives.is_dir() else []:
        # a runtime's symbols archive is not installed through the table
        if not NAME.match(archive.name) or archive.name.endswith("-symbols.zip"):
            continue
        try:
            with zipfile.ZipFile(archive) as zipped:
                info = json.loads(zipped.read("player-info.json"))
            runtime_id, version = info["id"], info["engineVersion"]
        except (OSError, KeyError, ValueError, zipfile.BadZipFile) as e:
            problems.append(f"{archive.name}: no readable player-info.json ({e})")
            continue
        if archive.name != f"jasperengine-player-{runtime_id}-{version}.zip":
            problems.append(f"{archive.name}: its player-info.json says {runtime_id} {version}")
        if args.version and version != args.version:
            problems.append(f"{archive.name}: version {version}, not the release's {args.version}")
        if runtime_id in seen:
            problems.append(f"{archive.name}: {runtime_id} again (also {seen[runtime_id]})")
        seen[runtime_id] = archive.name
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        entries.append(f'    {{"{runtime_id}", "{digest}", {archive.stat().st_size}ull}},')
        print(f"{runtime_id}: {digest}")
    if problems:
        print("write_hashes: no table written:\n  " + "\n  ".join(problems), file=sys.stderr)
        return 1
    text = ("// Written by tools/players/write_hashes.py for a release (v* or nightly-*): the\n"
            "// sha256 of every player runtime archive of that release (docs/specs/S30-shipping.md D5). A\n"
            "// development editor has none, so it trusts only players on this machine.\n"
            "#pragma once\n\n"
            "#define JASPERENGINE_PLAYER_RUNTIME_HASHES \\\n")
    text += " \\\n".join(entries) if entries else "    /* {\"linux-x64-glcore-release\", \"<sha256>\", <bytes>}, */"
    text += "\n"
    args.out.write_text(text, encoding="utf-8")
    print(f"{len(entries)} archives -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
