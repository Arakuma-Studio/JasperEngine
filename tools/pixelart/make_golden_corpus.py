#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Authors S44's golden corpus with Aseprite and exports its goldens (docs/specs/S44 §9).

    JASPER_ASEPRITE=<aseprite or its wrapper> python3 tools/pixelart/make_golden_corpus.py corpus [--out DIR]
        [--only NAME] [--split-layers]
    JASPER_ASEPRITE=... python3 tools/pixelart/make_golden_corpus.py data DIR... [--force]
    JASPER_ASEPRITE=... python3 tools/pixelart/make_golden_corpus.py frames FILE... [--force]

corpus: runs make_golden_corpus.lua (`aseprite -b --script-param out=<dir> --script ...`), writes
into the files what a batch save cannot (see below), and exports each file with Aseprite into
--out (default editor/tests/fixtures/aseprite/s44-corpus), laid out like asefile/:
    <name>.aseprite            the file
    <name>_<NN>.png            `--save-as <name>_{frame01}.png`: each frame, 1-based, two digits
    <name>.png, <name>.json    `--sheet <name>.png --sheet-type horizontal --data <name>.json
                               --format json-array --list-layers --list-tags --list-slices`
    <name>_composed_<NN>.png   files with group opacity/blend: Aseprite's render with composed groups
                               (Image:drawSprite with the compose_groups preference)
    <name>_<layer>_<NN>.png    with --split-layers: `--split-layers --save-as <name>_{layer}_{frame01}.png`
    corpus.json, LICENSE       the Aseprite version, each file's command lines and what was patched
data: `<stem>.data.json` (the --data JSON above) beside every .aseprite/.ase in each DIR.
frames: `<stem>_<NN>.png` (Aseprite's --save-as frames) beside each FILE.
data and frames refuse to replace a different existing file unless --force.

What the Lua API cannot do in batch mode, and this script writes into Aseprite's output:
- header flag 2 and the groups' opacity and blend (a batch save never composes groups), on
  group_blend and nested_groups; Aseprite then reads the file back and must report the same values;
- slice keys after frame 0 and hidden keys (width 0) on slices_keys; Aseprite then re-saves it,
  and its --data must list exactly those keys.

Aseprite runs from a scratch directory with relative paths only (a containerised build's wrapper
mounts just the current directory); the scratch lives in the system temp directory.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aseprite_compat as compat  # noqa: E402  (the PNG decoder and the Aseprite runner)

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DEFAULT_OUT = REPO / "editor" / "tests" / "fixtures" / "aseprite" / "s44-corpus"
LUA = HERE / "make_golden_corpus.lua"

# the file's blend mode numbers, by our names (editor/pixelart/aseprite/AsepriteDocument.cpp)
BLEND_VALUES = {name: i for i, name in enumerate([
    "normal", "multiply", "screen", "overlay", "darken", "lighten", "color_dodge", "color_burn",
    "hard_light", "soft_light", "difference", "exclusion", "hue", "saturation", "color",
    "luminosity", "addition", "subtract", "divide"])}

DESCRIPTIONS = {
    "group_blend": "RGB 32x16, 2 frames: one group per blend mode (normal, multiply, screen, overlay, "
                   "difference, color, addition, darken) with opacity 128/200/255/127/64/254/1/180, two "
                   "overlapping children each, over an opaque pattern; header flag 2 (composed groups)",
    "nested_groups": "RGB 16x16, 2 frames: composed groups three levels deep (screen 180 > multiply 128 > "
                     "difference 255), an overlay child, a hidden layer and a hidden group; header flag 2",
    "cel_layer_opacity": "RGB 24x12, 3 frames: layer opacity 1/127/254 (rows) x cel opacity 1/127/254 (frames) "
                         "for normal, multiply, screen, overlay, hue, addition (columns), half-transparent pixels",
    "tilemap_flips": "RGB 16x8, 2 frames: a 4x4-tile tilemap with all eight x/y/diagonal flip combinations of "
                     "two asymmetric tiles (and empty tiles) over an image layer",
    "user_data_all": "RGB 8x8, 2 frames: text, colour and properties of the types the Lua API makes (bool, "
                     "int8..int64, uint8..uint32, double, string, point, size, rect, uuid, vectors, maps 3 levels) "
                     "plus an extension map, on the sprite, a layer, a cel, a tag and a slice",
    "indexed_background": "indexed 8x8, 8 colours, transparent index 3, 2 frames: a background layer using "
                          "index 3 (opaque) under a layer where index 3 is clear",
    "tags_directions": "RGB 4x4, 8 frames of 40..215 ms: tags in the 4 directions x repeat 0/1/3, overlapping",
    "slices_keys": "RGB 32x32, 6 frames: 9-patch, pivot, 9-patch + pivot and plain slices, with keys on later "
                   "frames and hidden keys (width 0)",
}


class AsepriteError(RuntimeError):
    pass


class Runner:
    """Aseprite from one scratch directory, every command recorded for corpus.json."""

    def __init__(self, exe: str, cwd: Path):
        self.exe, self.cwd, self.commands = exe, cwd, []

    def __call__(self, *args: str) -> str:
        self.commands.append(shlex.join(["aseprite", "-b", *args]))
        proc = compat.run_aseprite(self.exe, list(args), self.cwd)
        if proc.returncode != 0:
            raise AsepriteError(f"aseprite -b {shlex.join(args)}: {compat.failure_text(proc)}")
        return proc.stdout


# --------------------------------------------------------------------------- file patching

def read_file(data: bytes):
    """(header, frames): frames are [frame header, [[chunk type, chunk body], ...]]."""
    header = bytearray(data[:128])
    frames, pos = [], 128
    count = struct.unpack_from("<H", header, 6)[0]
    for _ in range(count):
        size, magic, old_count, _, _, new_count = struct.unpack_from("<IHHH2sI", data, pos)
        if magic != 0xF1FA:
            raise ValueError(f"bad frame magic at {pos}")
        n = new_count if new_count else old_count
        frame_header, chunks, cp = bytearray(data[pos:pos + 16]), [], pos + 16
        for _ in range(n):
            chunk_size, chunk_type = struct.unpack_from("<IH", data, cp)
            chunks.append([chunk_type, bytearray(data[cp + 6:cp + chunk_size])])
            cp += chunk_size
        frames.append([frame_header, chunks])
        pos += size
    return header, frames


def write_file(header: bytearray, frames) -> bytes:
    body = bytearray()
    for frame_header, chunks in frames:
        payload = b"".join(struct.pack("<IH", len(c) + 6, t) + bytes(c) for t, c in chunks)
        struct.pack_into("<I", frame_header, 0, 16 + len(payload))
        body += frame_header + payload
    struct.pack_into("<I", header, 0, 128 + len(body))
    return bytes(header + body)


def chunk_name(body: bytearray, offset: int) -> str:
    n = struct.unpack_from("<H", body, offset)[0]
    return body[offset + 2:offset + 2 + n].decode("utf-8")


def patch_groups(path: Path, groups: dict) -> None:
    """Header flag 2 and each named group's blend mode and opacity (layer chunk 0x2004)."""
    header, frames = read_file(path.read_bytes())
    struct.pack_into("<I", header, 14, struct.unpack_from("<I", header, 14)[0] | 2)
    found = set()
    for chunk_type, body in frames[0][1]:
        if chunk_type != 0x2004 or struct.unpack_from("<H", body, 2)[0] != 1:
            continue
        name = chunk_name(body, 16)
        if name in groups:
            struct.pack_into("<HB", body, 10, BLEND_VALUES[groups[name]["blend"]], groups[name]["opacity"])
            found.add(name)
    if found != set(groups):
        raise ValueError(f"{path.name}: group layers not found: {sorted(set(groups) - found)}")
    path.write_bytes(write_file(header, frames))


def patch_slices(src: Path, dst: Path, slices: dict) -> None:
    """Replaces the keys of each named slice (chunk 0x2022) with the manifest's."""
    header, frames = read_file(src.read_bytes())
    found = set()
    for frame_header, chunks in frames:
        for chunk in chunks:
            if chunk[0] != 0x2022:
                continue
            body = chunk[1]
            _, flags, _ = struct.unpack_from("<III", body, 0)
            name = chunk_name(body, 12)
            if name not in slices:
                continue
            name_end = 14 + struct.unpack_from("<H", body, 12)[0]
            keys = sorted(slices[name], key=lambda k: k["frame"])
            out = bytearray(struct.pack("<III", len(keys), flags, 0)) + body[12:name_end]
            for k in keys:
                if bool(flags & 1) != ("center" in k) or bool(flags & 2) != ("pivot" in k):
                    raise ValueError(f"{name}: key {k} does not match the slice's flags {flags}")
                b = k["bounds"]
                out += struct.pack("<IiiII", k["frame"], b["x"], b["y"], b["w"], b["h"])
                if flags & 1:
                    c = k["center"]
                    out += struct.pack("<iiII", c["x"], c["y"], c["w"], c["h"])
                if flags & 2:
                    out += struct.pack("<ii", k["pivot"]["x"], k["pivot"]["y"])
            chunk[1] = out
            found.add(name)
    if found != set(slices):
        raise ValueError(f"{src.name}: slices not found: {sorted(set(slices) - found)}")
    dst.write_bytes(write_file(header, frames))


def expected_slice_keys(slices: dict) -> dict:
    out = {}
    for name, keys in slices.items():
        out[name] = [compat.normal_key(k["frame"], compat.rect_list(k["bounds"]),
                                       compat.rect_list(k["center"]) if "center" in k else None,
                                       [k["pivot"]["x"], k["pivot"]["y"]] if "pivot" in k else None)
                     for k in sorted(keys, key=lambda k: k["frame"])]
    return out


def data_slice_keys(data: dict) -> dict:
    return {s["name"]: [compat.normal_key(k["frame"], compat.rect_list(k["bounds"]),
                                          compat.rect_list(k["center"]) if "center" in k else None,
                                          [k["pivot"]["x"], k["pivot"]["y"]] if "pivot" in k else None)
                        for k in s.get("keys", [])]
            for s in data["meta"].get("slices", [])}


# ------------------------------------------------------------------------------------ corpus

def export(run: Runner, folder: str, name: str, split_layers: bool) -> None:
    f = f"{folder}/{name}.aseprite"
    run(f, "--save-as", f"{folder}/{name}_{{frame01}}.png")
    run(f, "--sheet", f"{folder}/{name}.png", "--sheet-type", "horizontal", "--data", f"{folder}/{name}.json",
        "--format", "json-array", "--list-layers", "--list-tags", "--list-slices")
    if split_layers:
        run("--split-layers", f, "--save-as", f"{folder}/{name}_{{layer}}_{{frame01}}.png")


def manifest_line(stdout: str, prefix: str):
    for line in stdout.splitlines():
        if line.startswith(prefix + " "):
            return json.loads(line[len(prefix) + 1:])
    raise AsepriteError(f"the script printed no {prefix} line:\n{stdout}")


def make_corpus(exe: str, scratch: Path, out: Path, only: str | None, split_layers: bool) -> int:
    run = Runner(exe, scratch)
    version = compat.aseprite_version(exe, scratch)
    if not version:
        raise AsepriteError(f"`{exe} -b --version` failed")
    shutil.copyfile(LUA, scratch / LUA.name)
    folder = "corpus"
    (scratch / folder).mkdir()
    params = ["--script-param", f"out={folder}"] + (["--script-param", f"only={only}"] if only else [])
    manifest = manifest_line(run(*params, "--script", LUA.name), "JE-MANIFEST")
    author = run.commands[-1]
    if isinstance(manifest, dict):  # Aseprite's json.encode writes an empty Lua table as {}
        manifest = list(manifest.values())

    files = {}
    for entry in manifest:
        name = entry["name"]
        first_command = len(run.commands)
        path = scratch / folder / f"{name}.aseprite"
        notes = []
        if entry.get("groups"):
            patch_groups(path, entry["groups"])
            notes.append("header flag 2 and the groups' blend mode and opacity written by make_golden_corpus.py")
            read_back = manifest_line(run("--script-param", f"render={folder}/{name}.aseprite", "--script", LUA.name),
                                      "JE-GROUP")
            if read_back != entry["groups"]:
                raise AsepriteError(f"{name}: Aseprite reads the groups as {read_back}, written {entry['groups']}")
        if entry.get("slices"):
            patched = scratch / folder / f"{name}.patched.aseprite"
            patch_slices(path, patched, entry["slices"])
            run("--script-param", f"resave={folder}/{patched.name}", "--script-param", f"to={folder}/{name}.aseprite",
                "--script", LUA.name)
            patched.unlink()
            notes.append("slice keys after frame 0 written by make_golden_corpus.py, then re-saved by Aseprite")
        export(run, folder, name, split_layers)
        if entry.get("slices"):
            data = json.loads((scratch / folder / f"{name}.json").read_text(encoding="utf-8"))
            if data_slice_keys(data) != expected_slice_keys(entry["slices"]):
                raise AsepriteError(f"{name}: Aseprite lists the slice keys as {data_slice_keys(data)}")
        files[name] = {"what": DESCRIPTIONS.get(name, ""), "patched": notes,
                       "commands": [author] + run.commands[first_command:]}

    if out.exists() and any(out.iterdir()) and not (out / "corpus.json").is_file():
        raise AsepriteError(f"{out} has files and no corpus.json: not replacing it")
    if only and (out / "corpus.json").is_file():
        kept = json.loads((out / "corpus.json").read_text(encoding="utf-8"))["files"]
        files = {**kept, **files}
    record = {
        "generator": "tools/pixelart/make_golden_corpus.py + make_golden_corpus.lua",
        "aseprite": version,
        "preferences": "defaults (a fresh HOME): new blending, groups not composed in CLI exports",
        "files": dict(sorted(files.items())),
    }
    (scratch / folder / "corpus.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    (scratch / folder / "LICENSE").write_text(
        "The files in this folder are generated, not drawn: tools/pixelart/make_golden_corpus.lua authors\n"
        "the .aseprite files through Aseprite's Lua API and tools/pixelart/make_golden_corpus.py exports\n"
        f"their PNG and JSON goldens with Aseprite ({version}); corpus.json lists every command.\n"
        "Like those scripts they are MIT licensed, (c) Eduardo Doria and contributors.\n"
        "Regenerate: JASPER_ASEPRITE=<aseprite> python3 tools/pixelart/make_golden_corpus.py corpus\n",
        encoding="utf-8")

    if out.exists() and not only:
        shutil.rmtree(out)
    out.mkdir(parents=True, exist_ok=True)
    written = 0
    for p in sorted((scratch / folder).iterdir()):
        shutil.copyfile(p, out / p.name)
        written += 1
    print(f"{out}: {written} files written for {len(manifest)} sprite(s), {version}")
    return 0


# ------------------------------------------------------------------- goldens beside fixtures

def place(src: Path, dst: Path, force: bool) -> str:
    if dst.is_file() and dst.read_bytes() == src.read_bytes():
        return "same"
    if dst.is_file() and not force:
        return "differs (kept; --force replaces it)"
    shutil.copyfile(src, dst)
    return "written"


def fixture_data(exe: str, scratch: Path, dirs: list[Path], force: bool) -> int:
    failed = 0
    for d in dirs:
        for i, path in enumerate(sorted(p for p in d.iterdir() if p.suffix.lower() in (".aseprite", ".ase"))):
            work = scratch / f"data_{d.name}_{i:03d}"
            work.mkdir()
            shutil.copyfile(path, work / path.name)
            run = Runner(exe, work)
            try:
                run(path.name, "--sheet", f"{path.stem}_sheet.png", "--sheet-type", "horizontal",
                    "--data", f"{path.stem}.data.json", "--format", "json-array",
                    "--list-layers", "--list-tags", "--list-slices")
                status = place(work / f"{path.stem}.data.json", path.with_name(f"{path.stem}.data.json"), force)
            except AsepriteError as e:
                status, failed = f"FAILED: {e}", failed + 1
            print(f"{path.with_name(path.stem + '.data.json')}: {status}")
            failed += status.startswith("differs")
    return 1 if failed else 0


def fixture_frames(exe: str, scratch: Path, files: list[Path], force: bool) -> int:
    failed = 0
    for i, path in enumerate(files):
        work = scratch / f"frames_{i:03d}"
        work.mkdir()
        shutil.copyfile(path, work / path.name)
        try:
            Runner(exe, work)(path.name, "--save-as", f"{path.stem}_{{frame01}}.png")
        except AsepriteError as e:
            print(f"{path}: FAILED: {e}")
            failed += 1
            continue
        for png in sorted(work.glob(f"{path.stem}_*.png")):
            status = place(png, path.with_name(png.name), force)
            print(f"{path.with_name(png.name)}: {status}")
            failed += status.startswith("differs")
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("corpus", help="author the corpus and its exports")
    p.add_argument("--out", type=Path, default=DEFAULT_OUT)
    p.add_argument("--only", help="one file of the corpus (added to --out, the others kept)")
    p.add_argument("--split-layers", action="store_true", help="also each layer's frames")
    p = sub.add_parser("data", help="<stem>.data.json beside each file of these directories")
    p.add_argument("dirs", nargs="+", type=Path)
    p.add_argument("--force", action="store_true")
    p = sub.add_parser("frames", help="<stem>_<NN>.png beside each of these files")
    p.add_argument("files", nargs="+", type=Path)
    p.add_argument("--force", action="store_true")
    parser.add_argument("--keep-scratch", action="store_true")
    args = parser.parse_args(argv)

    exe = os.environ.get("JASPER_ASEPRITE")
    if not exe:
        print("make_golden_corpus: set JASPER_ASEPRITE to the Aseprite executable (or its wrapper)", file=sys.stderr)
        return 2
    scratch = Path(tempfile.mkdtemp(prefix="je-aseprite-corpus-"))
    code = 1
    try:
        if args.command == "corpus":
            code = make_corpus(exe, scratch, args.out.resolve(), args.only, args.split_layers)
        elif args.command == "data":
            code = fixture_data(exe, scratch, [d.resolve() for d in args.dirs], args.force)
        else:
            code = fixture_frames(exe, scratch, [f.resolve() for f in args.files], args.force)
    except (AsepriteError, ValueError) as e:
        print(f"make_golden_corpus: {e}", file=sys.stderr)
        code = 1
    finally:
        if code or args.keep_scratch:
            print(f"scratch kept: {scratch}", file=sys.stderr)
        else:
            shutil.rmtree(scratch, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
