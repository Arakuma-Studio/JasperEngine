#!/usr/bin/env python3
"""Name the frames of a JasperEngine crash or hang report (docs/crash-diagnostics.md).

    python3 -I tools/crash/symbolicate.py REPORT [--symbols DIR] [--binary-dir DIR ...]
                                                [--symbol-map FILE] [--json]

REPORT is a crash.txt or hang-NNN.txt (v1), or a report folder (its crash.txt, else the newest
hang report). Each frame "#N <module>+0x<offset> [<symbol>]" is resolved against the module's
file: --binary-dir directories first, then the S30 symbol layout <symbols>/<platform>/<build_id>/
(a .debug / .dSYM / .pdb next to the binary there), then the path the report's modules section
names; a frame of a library loaded after the handler was installed ("module?+0x<address>") is
placed through the report's raw /proc/self/maps copy. ELF and Mach-O frames go through llvm-symbolizer (else addr2line, else atos on macOS);
frames after the first are return addresses, so one byte before is looked up. Web frames
("wasm-function[N]") are named from the emscripten --emit-symbol-map file. A Windows report
prints the cdb / Visual Studio steps for its crash.dmp instead.

Standard library only; safe under `python3 -I` (it imports nothing from the report's folder).
Exit 0 when at least one frame was named, 1 when none could be, 2 on a bad argument.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

FRAME = re.compile(r"^#(?P<index>\d+)\s+(?P<module>\S+?)\+0x(?P<offset>[0-9a-fA-F]+)(?:\s+\[(?P<symbol>.*)\])?\s*$")
WASM_FRAME = re.compile(r"wasm-function\[(?P<index>\d+)\]")
# /proc/self/maps: start-end perms offset dev inode path
MAPS_LINE = re.compile(r"^[0-9a-fA-F]+-[0-9a-fA-F]+\s+\S+\s+[0-9a-fA-F]+\s+\S+\s+\d+\s+(?P<path>/.+)$")


def read_report(path: Path) -> tuple[Path, str]:
    if path.is_dir():
        crash = path / "crash.txt"
        if crash.is_file():
            return crash, crash.read_text(errors="replace")
        hangs = sorted(path.glob("hang-*.txt"))
        if not hangs:
            raise FileNotFoundError(f"{path} has no crash.txt or hang-NNN.txt")
        return hangs[-1], hangs[-1].read_text(errors="replace")
    return path, path.read_text(errors="replace")


def header_value(text: str, key: str) -> str:
    prefix = key + ": "
    for line in text.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    return ""


def mappings(text: str) -> list[tuple[int, int, int, str]]:
    """(start, end, file offset, path) of the modules section's /proc/self/maps lines"""
    out = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 6 and "-" in parts[0] and parts[5].startswith("/"):
            try:
                start, end = (int(x, 16) for x in parts[0].split("-"))
                out.append((start, end, int(parts[2], 16), parts[5]))
            except ValueError:
                continue
    return out


def resolve_unknown(address: int, maps: list[tuple[int, int, int, str]]) -> tuple[str, int] | None:
    """A frame outside the modules known at install ("module?+0x<absolute>", a library loaded
    later): its module and module-relative address from the raw maps copy"""
    for start, end, offset, path in maps:
        if start <= address < end:
            # the image's base is its offset-0 mapping; else this segment's start - file offset
            base = next((s for s, _, o, p in maps if p == path and o == 0), start - offset)
            return path, address - base
    return None


def module_paths(text: str) -> dict[str, str]:
    """basename -> full path, from the report's modules section"""
    paths: dict[str, str] = {}
    in_modules = False
    for line in text.splitlines():
        if line.strip() == "modules:":
            in_modules = True
            continue
        if not in_modules:
            continue
        match = MAPS_LINE.match(line.strip())
        candidate = match.group("path") if match else line.split()[-1] if line.split() else ""
        if candidate.startswith("/"):
            paths.setdefault(os.path.basename(candidate), candidate)
    return paths


def find_module(name: str, known: dict[str, str], binary_dirs: list[Path], symbols: Path | None,
                platform: str, build_id: str) -> Path | None:
    for directory in binary_dirs:
        for candidate in (directory / name, directory / (name + ".debug")):
            if candidate.is_file():
                return candidate
    if symbols and platform and build_id:
        folder = symbols / platform / build_id
        for candidate in (folder / (name + ".debug"), folder / name,
                          folder / (name + ".dSYM") / "Contents" / "Resources" / "DWARF" / name):
            if candidate.is_file():
                return candidate
    if name in known and Path(known[name]).is_file():
        return Path(known[name])
    return None


def run(command: list[str]) -> str:
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


def symbolize(module: Path, addresses: list[int]) -> list[str]:
    """function (file:line) per address, "" when the tool could not name it"""
    hexes = [hex(a) for a in addresses]
    if shutil.which("llvm-symbolizer"):
        out = run(["llvm-symbolizer", "--obj=" + str(module), "--demangle", "--functions=linkage", *hexes])
        blocks = [b for b in out.strip().split("\n\n")] if out.strip() else []
        names = []
        for block in blocks:
            lines = block.splitlines()
            function = lines[0] if lines else "??"
            location = lines[1] if len(lines) > 1 else ""
            names.append("" if function == "??" else f"{function} ({location})" if location and not location.startswith("??") else function)
        if len(names) == len(addresses):
            return names
    if shutil.which("addr2line"):
        out = run(["addr2line", "-f", "-C", "-e", str(module), *hexes]).splitlines()
        names = []
        for i in range(0, len(out) - 1, 2):
            function, location = out[i], out[i + 1]
            names.append("" if function == "??" else f"{function} ({location})" if not location.startswith("??") else function)
        if len(names) == len(addresses):
            return names
    if sys.platform == "darwin" and shutil.which("atos"):
        # atos treats a zero load address as unspecified. Give it a synthetic
        # nonzero load address and shift each image-relative offset by the same
        # amount; this works for both executable and dylib preferred addresses.
        out = run(["atos", "-o", str(module), "-l", "0x1",
                   *(hex(a + 1) for a in addresses)]).splitlines()
        if len(out) == len(addresses):
            return ["" if line.startswith("0x") else line for line in out]
    return [""] * len(addresses)


def read_symbol_map(path: Path) -> dict[int, str]:
    names: dict[int, str] = {}
    for line in path.read_text(errors="replace").splitlines():
        index, _, name = line.partition(":")
        if index.isdigit():
            names[int(index)] = name
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("report", type=Path)
    parser.add_argument("--symbols", type=Path, help="the S30 symbol folder (symbols/<platform>/<build_id>/...)")
    parser.add_argument("--binary-dir", type=Path, action="append", default=[], help="where the report's modules are (repeatable)")
    parser.add_argument("--symbol-map", type=Path, help="web: the emscripten --emit-symbol-map file")
    parser.add_argument("--json", action="store_true", help="print the frames as JSON")
    args = parser.parse_args()

    try:
        report_file, text = read_report(args.report)
    except (OSError, FileNotFoundError) as error:
        print(f"symbolicate: {error}", file=sys.stderr)
        return 2

    platform = header_value(text, "platform")
    build_id = header_value(text, "build_id")
    if platform.startswith("windows"):
        dump = report_file.parent / "crash.dmp"
        symbols = args.symbols / platform / build_id if args.symbols else Path("<symbol folder>")
        print(f"A Windows report: open {dump} with its .pdb files (build {build_id}).")
        print(f'  cdb -z "{dump}" -y "{symbols}" -c "!analyze -v; .ecxr; k; q"')
        print("  Visual Studio: File > Open > File... the .dmp, Set symbol paths to that folder, Debug with Native Only.")
        print(f"The frames of {report_file.name} name module+offset already; the dump has the rest.")
        return 0

    symbol_map = read_symbol_map(args.symbol_map) if args.symbol_map else {}
    known = module_paths(text)
    frames = []
    for line in text.splitlines():
        match = FRAME.match(line.strip())
        if match:
            frames.append({"index": int(match.group("index")), "module": match.group("module"),
                           "offset": int(match.group("offset"), 16), "symbol": match.group("symbol") or "", "line": line.strip()})
            continue
        wasm = WASM_FRAME.search(line)
        if wasm and symbol_map:
            index = int(wasm.group("index"))
            frames.append({"index": len(frames), "module": "wasm", "offset": index, "symbol": "",
                           "line": line.strip(), "resolved": symbol_map.get(index, "")})

    maps = mappings(text)
    for frame in frames:
        if frame["module"] == "module?":
            found = resolve_unknown(frame["offset"], maps)
            if found:
                path, offset = found
                known.setdefault(os.path.basename(path), path)
                frame["module"], frame["offset"] = os.path.basename(path), offset

    by_module: dict[str, list[dict]] = {}
    for frame in frames:
        if frame["module"] not in ("wasm", "module?"):
            by_module.setdefault(frame["module"], []).append(frame)
    for name, group in by_module.items():
        module = find_module(name, known, args.binary_dir, args.symbols, platform, build_id)
        if module is None:
            for frame in group:
                frame["resolved"] = ""
            continue
        # a return address points after its call: one byte back is inside it
        addresses = [f["offset"] - (1 if f["index"] > 0 and f["offset"] > 0 else 0) for f in group]
        for frame, resolved in zip(group, symbolize(module, addresses)):
            frame["resolved"] = resolved

    named = sum(1 for f in frames if f.get("resolved"))
    if args.json:
        print(json.dumps({"report": str(report_file), "platform": platform, "build_id": build_id, "frames": frames}, indent=2))
    else:
        print(f"{report_file} ({platform}, build {build_id or 'none'}): {named} of {len(frames)} frames named")
        for frame in frames:
            resolved = frame.get("resolved") or frame["symbol"] or "?"
            print(f"  {frame['line']}  =>  {resolved}")
    return 0 if named > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
