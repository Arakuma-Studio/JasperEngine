#!/usr/bin/env python3
"""Writes editor/players/PlayerRuntimeHashes.gen.h from a release's player runtime archives
(docs/specs/S30-shipping.md D5): the editor of that release installs only archives with these
SHA-256 values.

    python3 tools/players/write_hashes.py ARCHIVE_DIR [--out editor/players/PlayerRuntimeHashes.gen.h]

ARCHIVE_DIR holds the jasperengine-player-<id>-<version>.zip files (tools/players/package_player.py
--zip). Without archives the header has no entries (a development editor). Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
NAME = re.compile(r"^jasperengine-player-(?P<id>[a-z0-9]+-[a-z0-9]+-[a-z0-9]+-(?:release|development))-(?P<version>.+)\.zip$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("archives", type=Path)
    parser.add_argument("--out", type=Path, default=REPO / "editor/players/PlayerRuntimeHashes.gen.h")
    args = parser.parse_args()
    entries = []
    for archive in sorted(args.archives.rglob("*.zip")) if args.archives.is_dir() else []:
        match = NAME.match(archive.name)
        # a runtime's symbols archive is not installed through the table
        if not match or archive.name.endswith("-symbols.zip"):
            continue
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        entries.append(f'    {{"{match["id"]}", "{digest}", {archive.stat().st_size}ull}},')
        print(f"{match['id']}: {digest}")
    text = ("// Written by tools/players/write_hashes.py for a v* tag: the\n"
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
