#!/usr/bin/env python3
"""What every player runtime of a players directory ships, and what its code is made of
(docs/specs/S34-player-size.md): the size table of S34's measurements.

    python3 tools/players/player_sizes.py build/players [--id <runtime id>...] [--modules]
        [--json sizes.json] [--baseline before.json]

Per runtime (player-info.json): the binaries a build stages and their bytes; the web's .wasm and .js
also gzipped (-9, what a server sends); Android's libraries per ABI also deflated (-9, as an APK or
a bundle's device APK stores them below minSdk 23). --modules adds the code by S34 module
(tools/players/lib_size.py --modules) from the runtime's symbols (a Linux player's .debug, Android's
unstripped arm64-v8a library) or from the web's single-thread .wasm. --baseline compares with the
--json of another run (the same runtime ids). Standard library only.
"""

from __future__ import annotations

import argparse
import gzip
import json
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lib_size  # noqa: E402

MIB = 1024 * 1024


def deflated(data: bytes) -> int:
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    return len(compressor.compress(data) + compressor.flush())


def modules_of(library: Path) -> dict[str, int]:
    with library.open("rb") as stream:
        wasm = stream.read(4) == b"\0asm"
    lines = lib_size.wasm_lines(library) if wasm else lib_size.run_nm(library, None, False)
    rolled: dict[str, int] = {}
    for name, (size, _) in lib_size.parse(lines).items():
        module = lib_size.module_of(name)
        rolled[module] = rolled.get(module, 0) + size
    return dict(sorted(rolled.items(), key=lambda kv: -kv[1]))


def measure(runtime: Path, with_modules: bool) -> dict:
    info = json.loads((runtime / "player-info.json").read_text())
    out = {"id": info["id"], "features": info.get("features", []), "files": {}, "modules": {}}
    platform = info["platform"]
    if platform == "android":
        for library in sorted((runtime / "android/lib").glob("*/*.so")):
            data = library.read_bytes()
            out["files"][library.relative_to(runtime / "android").as_posix()] = {"bytes": len(data), "deflated": deflated(data)}
        symbols = runtime / "symbols/lib/arm64-v8a/libjasperengine-android.so"
    elif platform == "web":
        for path in sorted((runtime / "files").glob("jasperengine-player-*")):
            data = path.read_bytes()
            out["files"][path.name] = {"bytes": len(data), "gzip": len(gzip.compress(data, 9))}
        symbols = runtime / "files/jasperengine-player-st.wasm"
    else:
        for name, _ in sorted(info.get("executables", {}).items()):
            path = runtime / name
            if path.is_file():
                out["files"][name] = {"bytes": path.stat().st_size}
        debug = sorted((runtime / "symbols").glob("*.debug")) if (runtime / "symbols").is_dir() else []
        symbols = debug[0] if debug else Path()
    if with_modules and symbols.is_file():
        out["modules"] = modules_of(symbols)
    return out


def mib(value: int) -> str:
    return f"{value / MIB:.2f}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("players", type=Path)
    parser.add_argument("--id", action="append", default=[], help="only these runtimes")
    parser.add_argument("--modules", action="store_true")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    runtimes = sorted(p for p in args.players.iterdir() if (p / "player-info.json").is_file())
    if args.id:
        runtimes = [p for p in runtimes if p.name in args.id]
    if not runtimes:
        sys.exit(f"no player runtime in {args.players}")
    results = [measure(runtime, args.modules) for runtime in runtimes]
    base = {r["id"]: r for r in json.loads(args.baseline.read_text())} if args.baseline else {}
    for result in results:
        before = base.get(result["id"], {})
        print(f"{result['id']}  ({' '.join(result['features'])})")
        for name, sizes in result["files"].items():
            line = f"  {name:48s} {mib(sizes['bytes']):>8} MiB"
            for packed in ("gzip", "deflated"):
                if packed in sizes:
                    line += f"  {packed} {mib(sizes[packed]):>7} MiB"
            old = before.get("files", {}).get(name)
            if old:
                line += f"   (was {mib(old['bytes'])}{', ' + mib(old.get('gzip', old.get('deflated', 0))) + ' packed' if 'gzip' in old or 'deflated' in old else ''})"
            print(line)
        for module, size in result["modules"].items():
            old = before.get("modules", {}).get(module)
            print(f"    {module:30s} {size / 1e6:8.2f} MB" + (f"   (was {old / 1e6:.2f})" if old is not None else ""))
    if args.json:
        args.json.write_text(json.dumps(results, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
