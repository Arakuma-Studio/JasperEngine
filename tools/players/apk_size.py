#!/usr/bin/env python3
"""What takes the space in an APK (docs/specs/S30-shipping.md, shipped size): native libraries per
ABI, dex code, compiled resources (resources.arsc, res/), the game's content (assets/), and the
rest, with the bytes each takes in the APK (stored or compressed) and uncompressed.

    python3 tools/players/apk_size.py game.apk [other.apk ...] [--json out.json]

Several APKs are listed side by side (a prebuilt build next to a compiled one). Standard library only.
"""

from __future__ import annotations

import argparse
import json
import sys
import zipfile
from collections import defaultdict
from pathlib import Path


def category(name: str) -> str:
    if name.startswith("lib/"):
        parts = name.split("/")
        return f"native libs ({parts[1]})" if len(parts) > 2 else "native libs"
    if name.endswith(".dex"):
        return "dex code"
    if name == "resources.arsc" or name.startswith("res/"):
        return "resources (arsc + res/)"
    if name.startswith("assets/"):
        return "content (assets/)"
    if name.startswith("META-INF/"):
        return "signature (META-INF/)"
    return "other"


def measure(apk: Path) -> dict:
    sizes = defaultdict(lambda: [0, 0, 0])   # stored, uncompressed, files
    biggest = []
    with zipfile.ZipFile(apk) as zipped:
        for info in zipped.infolist():
            if info.is_dir():
                continue
            entry = sizes[category(info.filename)]
            entry[0] += info.compress_size
            entry[1] += info.file_size
            entry[2] += 1
            biggest.append((info.compress_size, info.filename))
    biggest.sort(reverse=True)
    return {"file": str(apk), "bytes": apk.stat().st_size,
            "categories": {k: {"stored": v[0], "uncompressed": v[1], "files": v[2]} for k, v in sorted(sizes.items())},
            "largest": [{"file": n, "stored": s} for s, n in biggest[:12]]}


def mb(value: int) -> str:
    return f"{value / (1024 * 1024):8.2f} MB"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("apks", nargs="+", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    results = [measure(apk) for apk in args.apks]
    for result in results:
        print(f"{result['file']}: {mb(result['bytes'])}")
        for name, entry in result["categories"].items():
            print(f"  {name:28s} {mb(entry['stored'])} in the APK, {mb(entry['uncompressed'])} uncompressed, {entry['files']} files")
        print("  largest entries:")
        for item in result["largest"]:
            print(f"    {mb(item['stored'])}  {item['file']}")
    if args.json:
        args.json.write_text(json.dumps(results, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
