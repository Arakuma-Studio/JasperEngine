"""`jasperengine-editor aseprite info|render|resave` without a project (S44 R37, AC18).

- info --json is the document description (describe, Detail::Summary; --cels adds the cels) with
  the fixture's known values, and no "warnings" key (those go to stderr); the plain form is a
  summary naming the layer tree and the warnings;
- render: frame 0 equals Aseprite's export of it (alpha everywhere, RGB where alpha > 0); --tag
  lays a tag's frames out as a horizontal strip equal to that part of Aseprite's sheet export,
  --sheet rows wraps them, --scale upscales nearest-neighbour, --layers draws one layer;
- resave --expect-identical exits 0 and writes the input's bytes for every fixture, and exits 4
  (naming the header field) for a file whose header palette size disagrees with its palette
  chunk: it decodes to the same document, and the writer re-encodes that field;
- usage errors exit 2, read errors 3.
"""

from __future__ import annotations

import atexit
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import harness
from harness import REPO, TestFailure

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import pngdiff  # noqa: E402  (tools/mcp/pngdiff.py)

CI = True
TIMEOUT = 600

FIXTURES = REPO / "editor" / "tests" / "fixtures" / "aseprite"
ASEFILE = FIXTURES / "asefile"
LAYERS_AND_TAGS = ASEFILE / "layers_and_tags.aseprite"

S: dict = {}


def check(cond: bool, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def scratch() -> Path:
    if "tmp" not in S:
        S["tmp"] = Path(tempfile.mkdtemp(prefix="jasper-aseprite-cli-"))
        (S["tmp"] / "home").mkdir()
        (S["tmp"] / "cwd").mkdir()
        if not os.environ.get("JASPER_KEEP_SCRATCH"):
            atexit.register(shutil.rmtree, S["tmp"], True)
    return S["tmp"]


def run(*args: str) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_CONFIG_HOME")}
    env["HOME"] = str(scratch() / "home")
    return subprocess.run(harness.editor_command() + ["aseprite", *args], cwd=str(scratch() / "cwd"), env=env,
                          capture_output=True, text=True, timeout=300)


def exits(r: subprocess.CompletedProcess, code: int, what: str) -> subprocess.CompletedProcess:
    check(r.returncode == code, f"{what}: exit {r.returncode}, expected {code}\nstdout: {r.stdout[-3000:]}\nstderr: {r.stderr[-3000:]}")
    return r


def load_png(path: Path):
    return pngdiff.decode(path.read_bytes())


def same_picture(got, expected, what: str) -> None:
    """Aseprite's PNGs and ours agree when alpha is equal everywhere and RGB where alpha > 0."""
    gw, gh, g = got
    ew, eh, e = expected
    check((gw, gh) == (ew, eh), f"{what}: {gw}x{gh}, expected {ew}x{eh}")
    for i in range(0, len(e), 4):
        a, b = g[i:i + 4], e[i:i + 4]
        if a[3] != b[3] or (b[3] > 0 and a[:3] != b[:3]):
            p = i // 4
            raise TestFailure(f"{what}: pixel ({p % ew}, {p // ew}) is {tuple(a)}, expected {tuple(b)}")


def render(name: str, *args: str):
    out = scratch() / name
    exits(run("render", str(LAYERS_AND_TAGS), str(out), *args), 0, f"render {' '.join(args)}")
    return load_png(out)


# ---------------------------------------------------------------------------- info

async def test_info_json_is_the_description():
    r = exits(run("info", str(LAYERS_AND_TAGS), "--json"), 0, "info --json")
    d = json.loads(r.stdout)
    check(d["format"] == "aseprite" and d["width"] == 16 and d["height"] == 16, f"size: {d}")
    check(d["color_mode"] == "rgba", f"color_mode {d['color_mode']}")
    check(d["frame_count"] == 4 and len(d["frame_durations_ms"]) == 4, f"frames: {d['frame_count']}")
    check(d["duration_ms"] == sum(d["frame_durations_ms"]), "duration_ms is the sum of the durations")
    check(len(d["layers"]) == 6, f"{len(d['layers'])} layers")
    check(d["layers"][4]["path"] == "Group 1/Layer 5", f"layer 4 path {d['layers'][4]['path']!r}")
    check(d["layers"][3]["type"] == "group" and d["layers"][4]["parent"] == 3, "Group 1 holds Layer 5")
    check([t["name"] for t in d["tags"]] == ["T1", "T3", "T2"], f"tags {[t['name'] for t in d['tags']]}")
    check((d["tags"][0]["from"], d["tags"][0]["to"]) == (0, 1), f"T1 spans {d['tags'][0]}")
    # the describe shape only: warnings go to stderr, the summary has no cels
    check("warnings" not in d and "cels" not in d, f"keys {sorted(d)}")

    cels = json.loads(exits(run("info", str(LAYERS_AND_TAGS), "--json", "--cels"), 0, "info --json --cels").stdout)
    check(isinstance(cels.get("cels"), list) and len(cels["cels"]) > 0, "--cels lists the cels")
    check(all({"layer", "frame", "type"} <= set(c) for c in cels["cels"]), "every cel has layer, frame and type")
    for key in ("width", "height", "frame_count", "tags", "slices"):
        check(cels[key] == d[key], f"--cels changes {key}")
    # the full detail adds per-layer fields (flags, uuid) and changes none of the summary's
    check(len(cels["layers"]) == len(d["layers"]), "--cels keeps the layers")
    for full, summary in zip(cels["layers"], d["layers"]):
        check(all(full.get(k) == v for k, v in summary.items()), f"--cels changes layer {summary['path']}")


async def test_info_summary():
    r = exits(run("info", str(LAYERS_AND_TAGS)), 0, "info")
    for text in ("16x16 rgba", "4 frames", "Group 1", "Layer 5", "T1: frames 0-1", "warnings"):
        check(text in r.stdout, f"the summary lacks {text!r}:\n{r.stdout}")
    exits(run("info", "--help"), 0, "info --help")


# ---------------------------------------------------------------------------- render

async def test_render_frame_equals_aseprite():
    same_picture(render("frame0.png"), load_png(ASEFILE / "layers_and_tags_01.png"), "frame 0")
    same_picture(render("frame3.png", "--frame", "3"), load_png(ASEFILE / "layers_and_tags_04.png"), "frame 3")
    same_picture(render("layer1.png", "--frame", "2", "--layers", "Layer 1"), load_png(ASEFILE / "single_layer.png"),
                 "frame 2, Layer 1 alone")


async def test_render_tag_sheet():
    d = json.loads(exits(run("info", str(LAYERS_AND_TAGS), "--json"), 0, "info --json").stdout)
    sheet = load_png(ASEFILE / "layers_and_tags.png")   # Aseprite's horizontal export of the 4 frames
    for tag in d["tags"][:2]:
        check(tag["direction"] == "forward", f"{tag['name']} is {tag['direction']}")
        n = tag["to"] - tag["from"] + 1
        strip = render(f"tag_{tag['name']}.png", "--tag", tag["name"], "--sheet", "horizontal")
        check(strip[:2] == (n * 16, 16), f"--tag {tag['name']}: {strip[0]}x{strip[1]}, expected {n * 16}x16")
        same_picture(strip, pngdiff.crop(sheet, tag["from"] * 16, 0, n * 16, 16), f"--tag {tag['name']}")

    # T3 is frames 1-3: rows of ceil(sqrt(3)) = 2 columns
    rows = render("tag_rows.png", "--tag", "T3", "--sheet", "rows")
    check(rows[:2] == (32, 32), f"--sheet rows: {rows[0]}x{rows[1]}")
    same_picture(pngdiff.crop(rows, 0, 16, 16, 16), pngdiff.crop(sheet, 48, 0, 16, 16), "--sheet rows, third frame")

    big = render("scaled.png", "--scale", "3")
    frame = load_png(ASEFILE / "layers_and_tags_01.png")
    check(big[:2] == (48, 48), f"--scale 3: {big[0]}x{big[1]}")
    for y in range(48):
        for x in range(48):
            a = big[2][(y * 48 + x) * 4:(y * 48 + x) * 4 + 4]
            b = frame[2][((y // 3) * 16 + x // 3) * 4:((y // 3) * 16 + x // 3) * 4 + 4]
            check(a[3] == b[3] and (b[3] == 0 or a == b), f"--scale 3: pixel ({x}, {y})")


# ---------------------------------------------------------------------------- resave

def corpus() -> list[Path]:
    files = sorted(p for p in FIXTURES.rglob("*") if p.suffix.lower() in (".aseprite", ".ase"))
    check(len(files) > 10, f"only {len(files)} fixtures under {FIXTURES}")
    return files


async def test_resave_corpus_identical():
    for i, path in enumerate(corpus()):
        out = scratch() / f"resaved_{i}{path.suffix}"
        exits(run("resave", str(path), str(out), "--expect-identical"), 0, f"resave {path.relative_to(FIXTURES)}")
        check(out.read_bytes() == path.read_bytes(), f"{path.relative_to(FIXTURES)}: the output differs")


def with_palette_size_mismatch(data: bytes) -> bytes:
    """The file with its header palette size one below its frame-0 palette chunk's size: the chunk
    sets the palette, so the document is unchanged, and the writer rewrites the header field."""
    pos = 128
    size, _, old, _, _, new = struct.unpack("<IHHHHI", data[pos:pos + 16])
    count = new if (old == 0xFFFF and old < new) or (old == 0 and new > 0) else old
    cp = pos + 16
    for _ in range(count):
        chunk_size, chunk_type = struct.unpack("<IH", data[cp:cp + 6])
        if chunk_type == 0x2019:
            palette = struct.unpack("<I", data[cp + 6:cp + 10])[0]
            header = struct.unpack("<H", data[32:34])[0]
            check(palette >= 2 and header == palette, f"palette chunk {palette}, header {header}")
            return data[:32] + struct.pack("<H", palette - 1) + data[34:]
        cp += chunk_size
    raise TestFailure("no palette chunk in frame 0")


async def test_resave_reports_a_difference():
    original = LAYERS_AND_TAGS.read_bytes()
    mismatched = scratch() / "palette_mismatch.aseprite"
    mismatched.write_bytes(with_palette_size_mismatch(original))
    # the same document either way
    a = json.loads(exits(run("info", str(LAYERS_AND_TAGS), "--json", "--cels"), 0, "info original").stdout)
    b = json.loads(exits(run("info", str(mismatched), "--json", "--cels"), 0, "info mismatched").stdout)
    check(a == b, "the header change altered the document")

    out = scratch() / "palette_mismatch_out.aseprite"
    r = exits(run("resave", str(mismatched), str(out), "--expect-identical"), 4, "resave --expect-identical")
    check("byte 32" in r.stderr and "palette size" in r.stderr, f"the difference is not located:\n{r.stderr}")
    check(out.read_bytes() == original, "the output is not the original file (the header field re-encoded)")
    # without --expect-identical the same resave succeeds and says where it differs
    r = exits(run("resave", str(mismatched), str(out)), 0, "resave")
    check("palette size" in r.stdout, f"resave does not name the difference:\n{r.stdout}")


# ---------------------------------------------------------------------------- errors

async def test_usage_and_read_errors():
    exits(run(), 2, "no subcommand")
    exits(run("bogus"), 2, "unknown subcommand")
    exits(run("info"), 2, "info without a file")
    exits(run("info", str(LAYERS_AND_TAGS), "--bogus"), 2, "unknown option")
    exits(run("render", str(LAYERS_AND_TAGS)), 2, "render without an output")
    out = str(scratch() / "never.png")
    exits(run("render", str(LAYERS_AND_TAGS), out, "--frame", "x"), 2, "--frame x")
    exits(run("render", str(LAYERS_AND_TAGS), out, "--frame", "4"), 2, "--frame past the end")
    exits(run("render", str(LAYERS_AND_TAGS), out, "--scale", "33"), 2, "--scale 33")
    exits(run("render", str(LAYERS_AND_TAGS), out, "--frame", "0", "--tag", "T1"), 2, "--frame with --tag")
    exits(run("render", str(LAYERS_AND_TAGS), out, "--sheet", "grid"), 2, "--sheet grid")
    r = exits(run("render", str(LAYERS_AND_TAGS), out, "--tag", "nope"), 2, "unknown tag")
    check('"T1"' in r.stderr, f"the tags are not listed:\n{r.stderr}")
    r = exits(run("render", str(LAYERS_AND_TAGS), out, "--layers", "Layer 1,nope"), 2, "unknown layer")
    check('"Group 1/Layer 5"' in r.stderr, f"the layers are not listed:\n{r.stderr}")
    check(not Path(out).exists(), "a usage error wrote a PNG")
    exits(run("resave", str(LAYERS_AND_TAGS)), 2, "resave without an output")
    for sub in ("render", "resave"):
        exits(run(sub, "--help"), 0, f"{sub} --help")
    exits(run("--help"), 0, "aseprite --help")

    not_aseprite = scratch() / "not.aseprite"
    not_aseprite.write_bytes(b"\x89PNG not an aseprite file" * 10)
    r = exits(run("info", str(not_aseprite)), 3, "info of a PNG")
    check("not_aseprite" in r.stderr and "fix:" in r.stderr, f"the error kind and fix are not printed:\n{r.stderr}")
    exits(run("render", str(scratch() / "missing.aseprite"), out), 3, "render of a missing file")
    exits(run("resave", str(not_aseprite), str(scratch() / "x.aseprite")), 3, "resave of a PNG")
