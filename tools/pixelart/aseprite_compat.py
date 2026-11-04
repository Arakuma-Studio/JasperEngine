#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The manual Aseprite compatibility gate of S44 (docs/specs/S44-aseprite-import.md AC5, R43).

    JASPER_ASEPRITE=<aseprite or its wrapper> [JASPER_EDITOR=<jasperengine-editor>] \\
        python3 tools/pixelart/aseprite_compat.py [extra dirs...] [--json report.json] [-j N]

For every .aseprite / .ase under editor/tests/fixtures/aseprite (recursively) and the extra dirs:

- resave-*: our `aseprite resave` output opens in Aseprite (`-b <out> --save-as`), its frames equal
  Aseprite's export of the original pixel for pixel, and Aseprite's --data of it equals the
  original's (frames, tags, layers, slices, user data);
- render: our `aseprite render --frame N --flatten-groups` equals Aseprite's `--save-as` frame N
  and frame N of its `--sheet` (Aseprite's CLI exports never compose group opacity or blend);
- data-*: Aseprite's `--data` JSON (`--format json-array --list-tags --list-slices
  --list-layers`) agrees with our `aseprite info --json` on frame count, durations, tags (name,
  from, to, direction, repeat), slices (names, keys) and layers (name, group, opacity, blend).

A file Aseprite itself cannot open, and a frame where Aseprite's own --save-as and --sheet
disagree (aseprite-sheet), are reported as SKIP. One line per check, then a summary; exit 1
on any mismatch, 2 when the tools are missing. `--aseprite-only` runs only Aseprite's side (its
exports and its frames against its own sheet), to check the setup without an editor build.

Aseprite runs with each file's scratch directory as its working directory and relative paths
only, so the wrapper of a containerised build (which mounts just the current directory) works.
Scratch lives in a temp directory outside the repository, kept when a check fails or with
--keep-scratch.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / "editor" / "tests" / "fixtures" / "aseprite"
DEFAULT_EDITOR = REPO / "build" / "linux" / "editor-install" / "bin" / "jasperengine-editor"

# ---------------------------------------------------------------------------------------- PNG

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def decode_png(data: bytes):
    """(width, height, rgba bytearray) of a non-interlaced PNG of any colour type and bit depth
    Aseprite writes: RGBA, RGB, palette (with tRNS), grey and grey + alpha."""
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG")
    pos, idat, plte, trns = 8, [], b"", None
    width = height = depth = color_type = None
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk[:13])
            if interlace:
                raise ValueError("interlaced PNG")
            if color_type not in _CHANNELS:
                raise ValueError(f"colour type {color_type}")
        elif kind == b"PLTE":
            plte = chunk
        elif kind == b"tRNS":
            trns = chunk
        elif kind == b"IDAT":
            idat.append(chunk)
        elif kind == b"IEND":
            break
        pos += 12 + length
    if width is None:
        raise ValueError("PNG without IHDR")
    ch = _CHANNELS[color_type]
    bits = ch * depth
    stride = (width * bits + 7) // 8
    bpp = max(1, bits // 8)
    raw = zlib.decompress(b"".join(idat))
    if len(raw) < (stride + 1) * height:
        raise ValueError("truncated image data")
    rows, prev, i = [], bytearray(stride), 0
    for y in range(height):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if f == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                c = prev[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + _paeth(a, prev[x], c)) & 255
        elif f != 0:
            raise ValueError(f"unknown filter {f} on row {y}")
        rows.append(line)
        prev = line

    def samples(line: bytearray):
        """The row's samples scaled to 8 bits (palette indices unscaled)."""
        if depth == 8:
            return line
        if depth == 16:
            return line[0::2]
        per_byte, mask = 8 // depth, (1 << depth) - 1
        out = bytearray(width * ch)
        for n in range(width * ch):
            v = (line[n // per_byte] >> (8 - depth * (n % per_byte + 1))) & mask
            out[n] = v if color_type == 3 else v * 255 // mask
        return out

    def key16(line: bytearray, n: int) -> int:
        return (line[n * 2] << 8) | line[n * 2 + 1]

    rgba = bytearray(width * height * 4)
    for y, line in enumerate(rows):
        s = samples(line)
        for x in range(width):
            o = (y * width + x) * 4
            if color_type == 6:
                rgba[o:o + 4] = s[x * 4:x * 4 + 4]
            elif color_type == 2:
                rgba[o:o + 3] = s[x * 3:x * 3 + 3]
                rgba[o + 3] = 255
                if trns and len(trns) >= 6:
                    # RGB is 8 or 16 bits per sample; the key is in the file's sample size
                    if depth == 16:
                        key = tuple(key16(line, x * 3 + c) for c in range(3))
                    else:
                        key = tuple(s[x * 3:x * 3 + 3])
                    if key == struct.unpack(">HHH", trns[:6]):
                        rgba[o + 3] = 0
            elif color_type == 3:
                idx = s[x]
                rgba[o:o + 3] = plte[idx * 3:idx * 3 + 3] if idx * 3 + 3 <= len(plte) else b"\0\0\0"
                rgba[o + 3] = trns[idx] if trns is not None and idx < len(trns) else 255
            elif color_type == 4:
                rgba[o] = rgba[o + 1] = rgba[o + 2] = s[x * 2]
                rgba[o + 3] = s[x * 2 + 1]
            else:
                rgba[o] = rgba[o + 1] = rgba[o + 2] = s[x]
                rgba[o + 3] = 255
                if trns and len(trns) >= 2:
                    raw_value = key16(line, x) if depth == 16 else (s[x] * ((1 << depth) - 1) // 255)
                    if raw_value == struct.unpack(">H", trns[:2])[0]:
                        rgba[o + 3] = 0
    return width, height, rgba


def load_png(path: Path):
    return decode_png(path.read_bytes())


def crop(img, x: int, y: int, w: int, h: int):
    width, _, rgba = img
    out = bytearray()
    for row in range(y, y + h):
        out += rgba[(row * width + x) * 4:(row * width + x + w) * 4]
    return w, h, out


def same_picture(a, b) -> tuple[bool, str]:
    """Pixel equality; fully transparent pixels are equal whatever their colour (Aseprite keeps
    the colour of clear pixels in RGB exports, our renderer writes 0)."""
    if a[:2] != b[:2]:
        return False, f"size {a[0]}x{a[1]} vs {b[0]}x{b[1]}"
    width, height = a[0], a[1]
    pa, pb = a[2], b[2]
    first, count = None, 0
    for n in range(width * height):
        o = n * 4
        x, y = pa[o:o + 4], pb[o:o + 4]
        if x != y and not (x[3] == 0 and y[3] == 0):
            count += 1
            if first is None:
                first = (n % width, n // width, tuple(x), tuple(y))
    if count:
        fx, fy, xa, xb = first
        return False, f"{count} pixel(s) differ, first at ({fx}, {fy}): {xa} vs {xb}"
    return True, ""


# ----------------------------------------------------------------------------------- running

def run_tool(argv: list[str], cwd: Path, timeout: int = 600) -> subprocess.CompletedProcess:
    return subprocess.run(argv, cwd=cwd, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                          errors="replace", timeout=timeout)


def run_aseprite(exe: str, args: list[str], cwd: Path) -> subprocess.CompletedProcess:
    """Aseprite in batch mode, from `cwd`; every path in `args` is relative to it."""
    return run_tool([exe, "-b", *args], cwd)


def aseprite_version(exe: str, cwd: Path) -> str:
    try:
        proc = run_aseprite(exe, ["--version"], cwd)
    except OSError:
        return ""
    return proc.stdout.strip() if proc.returncode == 0 else ""


def frames_arg(stem: str) -> str:
    # 1-based, three digits: {frame001} starts at 1 (Aseprite's filename_formatter)
    return f"{stem}_{{frame001}}.png"


def frame_file(work: Path, stem: str, index: int) -> Path:
    return work / f"{stem}_{index + 1:03d}.png"


def failure_text(proc: subprocess.CompletedProcess) -> str:
    text = (proc.stderr.strip() or proc.stdout.strip()).splitlines()
    return f"exit {proc.returncode}" + (f": {text[-1]}" if text else "")


def export_with_aseprite(exe: str, work: Path, stem: str) -> tuple[bool, str]:
    """<stem>_NNN.png frames, <stem>_sheet.png and <stem>.json from <stem>.aseprite."""
    proc = run_aseprite(exe, [f"{stem}.aseprite", "--save-as", frames_arg(stem)], work)
    if proc.returncode != 0:
        return False, "--save-as " + failure_text(proc)
    proc = run_aseprite(exe, [f"{stem}.aseprite", "--sheet", f"{stem}_sheet.png", "--sheet-type", "horizontal",
                              "--data", f"{stem}.json", "--format", "json-array",
                              "--list-tags", "--list-slices", "--list-layers"], work)
    if proc.returncode != 0 or not (work / f"{stem}.json").is_file():
        return False, "--sheet/--data " + failure_text(proc)
    return True, ""


# ------------------------------------------------------------------------------ comparisons

# our blend names (AsepriteDocument.cpp blendModeName) -> Aseprite's --data names
BLEND_TO_ASEPRITE = {"hue": "hsl_hue", "saturation": "hsl_saturation", "color": "hsl_color",
                     "luminosity": "hsl_luminosity"}


def aseprite_layers_from_ours(info: dict) -> list[dict]:
    """The layers Aseprite's --list-layers names: visible layers whose groups are all visible,
    in file order (groups before their children), with the parent group's name; opacity and
    blend for image and tilemap layers only."""
    layers = info["layers"]

    def shown(i: int) -> bool:
        seen = 0
        while i >= 0 and seen <= len(layers):
            if not layers[i]["visible"]:
                return False
            i = layers[i]["parent"]
            seen += 1
        return True

    out = []
    for i, layer in enumerate(layers):
        if not shown(i):
            continue
        entry = {"name": layer["name"]}
        if layer["parent"] >= 0:
            entry["group"] = layers[layer["parent"]]["name"]
        if layer["type"] != "group":
            entry["opacity"] = layer["opacity"]
            entry["blendMode"] = BLEND_TO_ASEPRITE.get(layer["blend"], layer["blend"])
        out.append(entry)
    return out


def aseprite_layers(data: dict) -> list[dict]:
    out = []
    for layer in data["meta"].get("layers", []):
        entry = {k: layer[k] for k in ("name", "group", "opacity", "blendMode") if k in layer}
        out.append(entry)
    return out


def rect_list(r: dict) -> list[int]:
    return [r["x"], r["y"], r["w"], r["h"]]


def normal_key(frame: int, bounds: list, center, pivot) -> dict:
    # Aseprite has no centre when the centre rectangle is empty (SliceKey::hasCenter)
    key = {"frame": frame, "bounds": list(bounds)}
    if center is not None and center[2] > 0 and center[3] > 0:
        key["center"] = list(center)
    if pivot is not None:
        key["pivot"] = list(pivot)
    return key


def aseprite_tag_order(tags: list[dict]) -> list[dict]:
    """The file's tags in the order Aseprite keeps them: each read tag is inserted before the first
    one that starts later, or starts with it and ends sooner (doc::Tags::add)."""
    out: list[dict] = []
    for tag in tags:
        at = len(out)
        for i, t in enumerate(out):
            if t["from"] > tag["from"] or (t["from"] == tag["from"] and t["to"] < tag["to"]):
                at = i
                break
        out.insert(at, tag)
    return out


def compare_data(info: dict, data: dict) -> list[tuple[str, bool, str]]:
    results = []
    frames = data["frames"]
    results.append(("data-frames", len(frames) == info["frame_count"],
                    f"aseprite {len(frames)}, ours {info['frame_count']}"))
    ase_durations = [f["duration"] for f in frames]
    results.append(("data-durations", ase_durations == info["frame_durations_ms"],
                    f"aseprite {ase_durations}, ours {info['frame_durations_ms']}"))

    ase_tags = [{"name": t["name"], "from": t["from"], "to": t["to"], "direction": t["direction"],
                 "repeat": int(t.get("repeat", 0))} for t in data["meta"].get("frameTags", [])]
    our_tags = aseprite_tag_order([{"name": t["name"], "from": t["from"], "to": t["to"], "direction": t["direction"],
                                    "repeat": t["repeat"]} for t in info["tags"]])
    results.append(("data-tags", ase_tags == our_tags, first_difference(ase_tags, our_tags)))

    # Aseprite inserts each slice it reads at the front (doc::Slices::add): its list is the file's reversed
    ase_slices = [{"name": s["name"],
                   "keys": [normal_key(k["frame"], rect_list(k["bounds"]),
                                       rect_list(k["center"]) if "center" in k else None,
                                       [k["pivot"]["x"], k["pivot"]["y"]] if "pivot" in k else None)
                            for k in s.get("keys", [])]}
                  for s in data["meta"].get("slices", [])]
    our_slices = [{"name": s["name"],
                   "keys": [normal_key(k["frame"], k["bounds"], k.get("center"), k.get("pivot")) for k in s["keys"]]}
                  for s in reversed(info["slices"])]
    results.append(("data-slices", ase_slices == our_slices, first_difference(ase_slices, our_slices)))

    ase_layers, our_layers = aseprite_layers(data), aseprite_layers_from_ours(info)
    results.append(("data-layers", ase_layers == our_layers, first_difference(ase_layers, our_layers)))
    return results


def first_difference(a: list, b: list) -> str:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return f"#{i}: aseprite {json.dumps(x)}, ours {json.dumps(y)}"
    if len(a) != len(b):
        return f"aseprite has {len(a)}, ours {len(b)}"
    return ""


def without_names(data: dict) -> dict:
    """--data minus what names the files (frame filenames, the sheet image)."""
    d = json.loads(json.dumps(data))
    for f in d.get("frames", []):
        f.pop("filename", None)
    d.get("meta", {}).pop("image", None)
    return d


def frame_count_of(work: Path, stem: str) -> int:
    n = 0
    while frame_file(work, stem, n).is_file():
        n += 1
    return n


def sheet_frames(work: Path, stem: str, data: dict) -> list:
    sheet = load_png(work / f"{stem}_sheet.png")
    return [crop(sheet, f["frame"]["x"], f["frame"]["y"], f["frame"]["w"], f["frame"]["h"]) for f in data["frames"]]


# ------------------------------------------------------------------------------------ a file

def check_file(path: Path, label: str, work: Path, aseprite: str, editor: str | None) -> list[dict]:
    results: list[dict] = []

    def add(check: str, ok: bool | None, detail: str = "") -> None:
        status = "SKIP" if ok is None else "PASS" if ok else "FAIL"
        results.append({"file": label, "check": check, "status": status, "detail": "" if ok else detail})

    work.mkdir(parents=True)
    shutil.copyfile(path, work / "orig.aseprite")

    ok, detail = export_with_aseprite(aseprite, work, "orig")
    if not ok:
        add("aseprite-open", None, f"Aseprite cannot export the original ({detail})")
        return results
    data = json.loads((work / "orig.json").read_text(encoding="utf-8"))
    count = frame_count_of(work, "orig")
    frames = [load_png(frame_file(work, "orig", i)) for i in range(count)]
    sheet = sheet_frames(work, "orig", data)

    # Aseprite's two exports can disagree: an indexed sprite with a background layer is opaque in
    # --save-as but its --sheet marks the transparent index clear. Our render is held to the
    # sheet only on the frames where they agree.
    sheet_ok = [i < len(sheet) and same_picture(frames[i], sheet[i])[0] for i in range(count)]
    if count != len(sheet):
        add("aseprite-sheet", False, f"{count} --save-as frames, {len(sheet)} sheet frames")
    elif all(sheet_ok):
        add("aseprite-sheet", True)
    else:
        add("aseprite-sheet", None, "Aseprite's --sheet differs from its --save-as on frames "
            f"{[i for i, ok in enumerate(sheet_ok) if not ok]}; render is compared with --save-as there")
    if editor is None:
        return results

    # (c) describe against --data
    proc = run_tool([editor, "aseprite", "info", str(work / "orig.aseprite"), "--json"], work)
    if proc.returncode != 0:
        add("info", False, failure_text(proc))
        info = None
    else:
        info = json.loads(proc.stdout)
        for check, ok, detail in compare_data(info, data):
            add(check, ok, detail)

    # (a) our re-save opens in Aseprite and shows the same frames and metadata
    proc = run_tool([editor, "aseprite", "resave", str(work / "orig.aseprite"), str(work / "resaved.aseprite")], work)
    if proc.returncode != 0:
        add("resave", False, failure_text(proc))
    else:
        ok, detail = export_with_aseprite(aseprite, work, "resaved")
        add("resave-open", ok, detail)
        if ok:
            resaved_count = frame_count_of(work, "resaved")
            bad = []
            for i in range(min(count, resaved_count)):
                same, why = same_picture(load_png(frame_file(work, "resaved", i)), frames[i])
                if not same:
                    bad.append(f"frame {i}: {why}")
            if resaved_count != count:
                bad.insert(0, f"{resaved_count} frames, the original {count}")
            add("resave-frames", not bad, "; ".join(bad[:3]))
            resaved_data = json.loads((work / "resaved.json").read_text(encoding="utf-8"))
            a, b = without_names(data), without_names(resaved_data)
            detail = ""
            if a != b:
                for key in ("frames", "meta"):
                    if a.get(key) != b.get(key):
                        sub_a, sub_b = a.get(key), b.get(key)
                        if isinstance(sub_a, dict) and isinstance(sub_b, dict):
                            diff = [k for k in sorted(set(sub_a) | set(sub_b)) if sub_a.get(k) != sub_b.get(k)]
                            detail = f"{key}: {', '.join(diff)} differ"
                        else:
                            detail = f"{key}: {first_difference(sub_a or [], sub_b or [])}"
                        break
            add("resave-data", a == b, detail)

    # (b) our composition against Aseprite's exports
    n = info["frame_count"] if info else count
    bad = []
    for i in range(n):
        out = work / f"ours_{i:03d}.png"
        proc = run_tool([editor, "aseprite", "render", str(work / "orig.aseprite"), str(out),
                         "--frame", str(i), "--flatten-groups"], work)
        if proc.returncode != 0:
            bad.append(f"frame {i}: render {failure_text(proc)}")
            continue
        ours = load_png(out)
        if i < count:
            same, why = same_picture(ours, frames[i])
            if not same:
                bad.append(f"frame {i} vs --save-as: {why}")
        if i < count and sheet_ok[i]:
            same, why = same_picture(ours, sheet[i])
            if not same:
                bad.append(f"frame {i} vs --sheet: {why}")
    if n != count:
        bad.insert(0, f"we have {n} frames, Aseprite exported {count}")
    add("render", not bad, "; ".join(bad[:3]) + (f" (+{len(bad) - 3} more)" if len(bad) > 3 else ""))
    return results


def collect(dirs: list[Path]) -> list[tuple[Path, str]]:
    files = []
    for d in dirs:
        for p in sorted(d.rglob("*")):
            if p.is_file() and p.suffix.lower() in (".aseprite", ".ase"):
                try:
                    label = p.relative_to(FIXTURES).as_posix()
                except ValueError:
                    label = str(p)
                files.append((p, label))
    return files


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("dirs", nargs="*", type=Path, help="more directories of .aseprite files")
    parser.add_argument("--json", type=Path, help="write the report here")
    parser.add_argument("-j", "--jobs", type=int, default=1, help="files checked at once (default 1)")
    parser.add_argument("--only", help="only files whose path contains this text")
    parser.add_argument("--no-fixtures", action="store_true", help="skip editor/tests/fixtures/aseprite")
    parser.add_argument("--aseprite-only", action="store_true",
                        help="no editor: Aseprite's exports and its frames against its own sheet")
    parser.add_argument("--scratch", type=Path, help="parent of the scratch directory (default: the system temp)")
    parser.add_argument("--keep-scratch", action="store_true")
    args = parser.parse_args(argv)

    aseprite = os.environ.get("JASPER_ASEPRITE")
    if not aseprite:
        print("aseprite_compat: set JASPER_ASEPRITE to the Aseprite executable (or its wrapper)", file=sys.stderr)
        return 2
    editor = None
    if not args.aseprite_only:
        editor = os.environ.get("JASPER_EDITOR") or str(DEFAULT_EDITOR)
        if not Path(editor).is_file():
            print(f"aseprite_compat: no editor at {editor}; set JASPER_EDITOR or build it (tools/build.sh linux)",
                  file=sys.stderr)
            return 2

    scratch = Path(tempfile.mkdtemp(prefix="je-aseprite-compat-", dir=args.scratch))
    if REPO in scratch.resolve().parents:
        print(f"aseprite_compat: the scratch directory {scratch} is inside the repository", file=sys.stderr)
        scratch.rmdir()
        return 2
    version = aseprite_version(aseprite, scratch)
    if not version:
        print(f"aseprite_compat: `{aseprite} -b --version` failed", file=sys.stderr)
        scratch.rmdir()
        return 2
    print(f"aseprite: {version}")
    print(f"editor: {editor or '(none: --aseprite-only)'}")
    print(f"scratch: {scratch}")

    dirs = ([] if args.no_fixtures else [FIXTURES]) + [d.resolve() for d in args.dirs]
    files = [(p, label) for p, label in collect(dirs) if not args.only or args.only in label]
    if not files:
        print("aseprite_compat: no .aseprite files found", file=sys.stderr)
        return 2

    def one(item):
        index, (path, label) = item
        try:
            return check_file(path, label, scratch / f"{index:04d}_{path.stem}", aseprite, editor)
        except Exception as e:  # a crash in one file is a failure of that file, not of the run
            return [{"file": label, "check": "run", "status": "FAIL", "detail": f"{type(e).__name__}: {e}"}]

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for file_results in pool.map(one, enumerate(files)):
            for r in file_results:
                line = f"{r['status']:4}  {r['file']}  {r['check']}"
                if r["detail"]:
                    line += f"  ({r['detail']})"
                print(line, flush=True)
            results.extend(file_results)

    counts = {s: sum(1 for r in results if r["status"] == s) for s in ("PASS", "FAIL", "SKIP")}
    print(f"summary: {len(files)} files, {len(results)} checks: {counts['PASS']} passed, "
          f"{counts['FAIL']} failed, {counts['SKIP']} skipped (Aseprite could not open the file, or its own "
          "exports disagree)")
    if args.json:
        args.json.write_text(json.dumps({"aseprite": version, "editor": editor, "files": len(files),
                                         "summary": counts, "checks": results}, indent=2) + "\n",
                             encoding="utf-8")
    if counts["FAIL"] or args.keep_scratch:
        print(f"scratch kept: {scratch}")
    else:
        shutil.rmtree(scratch, ignore_errors=True)
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
