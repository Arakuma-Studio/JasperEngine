"""`jasperengine-editor synthesize-texture`: the texture synthesis CLI (editor/texsynth/SynthesisCli.cpp).

No window, no engine: each test runs the editor binary in a scratch directory of its own on small
generated PNGs (value noise, a 16-bit companion), from a working directory below the inputs so the
relative path arguments must resolve against it (once: the doubled-join bug of the other subcommands).

- usage errors exit 2 with the reason on stderr (unknown option, bad values, a parameter out of range,
  a missing size, a companion before any example, a non-PNG output), --help exits 0;
- a run with relative --example/--companion/--out/--project and --format json prints one object
  (outputs project-relative, width, height, seed, metrics); the companion lands next to the output
  under the naming rule (rock_albedo + rock_normal, --out rock_tile.png -> rock_tile_normal.png) in
  its own format (16-bit grey), and every PNG carries its recipe (iTXt jasper.synthesis);
- an existing output is refused (exit 2, "--overwrite") and kept; --overwrite replaces it;
- identical pixels for --threads 1 and --threads 8, other pixels for another seed;
- a missing input or a companion of the wrong size exits 1 with a compiler-style line naming the file;
- inpaint keeps the unmasked pixels; make_tileable keeps the size and reports a seam score;
- a file name that is not UTF-8 (Latin-1 bytes) is refused up front (exit 2, rename it) instead of
  aborting when the recipe is written; `check` on a project with such a PNG and with a garbled
  recipe (a number where a path belongs) reports instead of aborting;
- a set that can't be written whole (a companion under a path component that is a file) writes
  nothing: the existing primary keeps its content (all or nothing), exit 1 naming the file.
"""

from __future__ import annotations

import json
import math
import os
import random
import shutil
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

import harness
from harness import TestFailure
from htmlui_pixels import decode_png
CI = True

S: dict = {}


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


# ---------------------------------------------------------------------------- PNG helpers

def png_bytes(width: int, height: int, channels: int, rows: list[bytes], depth: int = 8) -> bytes:
    color_type = {1: 0, 2: 4, 3: 2, 4: 6}[channels]

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def noise_rows(width: int, height: int, seed: int, channels: int = 3) -> list[bytes]:
    """Smooth value noise: a texture with structure, not periodic."""
    rnd = random.Random(seed)
    grid = 6
    lattice = [[[rnd.random() for _ in range(3)] for _ in range(grid + 1)] for _ in range(grid + 1)]
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            fx, fy = x / width * (grid - 1), y / height * (grid - 1)
            ix, iy = int(fx), int(fy)
            tx, ty = fx - ix, fy - iy
            for c in range(3):
                a = lattice[iy][ix][c] + (lattice[iy][ix + 1][c] - lattice[iy][ix][c]) * tx
                b = lattice[iy + 1][ix][c] + (lattice[iy + 1][ix + 1][c] - lattice[iy + 1][ix][c]) * tx
                v = a + (b - a) * ty + 0.08 * math.sin(x * 0.9 + c) * math.cos(y * 0.7)
                row.append(max(0, min(255, int(v * 255))))
            if channels == 4:
                row.append(255)
        rows.append(bytes(row))
    return rows


def gray16_rows(width: int, height: int) -> list[bytes]:
    return [b"".join(struct.pack(">H", (y * width + x) * 13 % 65536) for x in range(width)) for y in range(height)]


def chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    out, pos = [], 8
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        out.append((data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]))
        pos += 12 + length
    return out


def ihdr(path: Path) -> tuple[int, int, int, int]:
    """(width, height, bit depth, color type)"""
    for kind, data in chunks(path.read_bytes()):
        if kind == b"IHDR":
            w, h, depth, color_type = struct.unpack(">IIBB", data[:10])
            return w, h, depth, color_type
    fail(f"{path}: no IHDR")
    return 0, 0, 0, 0


def recipe(path: Path) -> dict:
    """The jasper.synthesis iTXt chunk, parsed; the chunk right after IHDR"""
    found = chunks(path.read_bytes())
    check(found[1][0] == b"iTXt", f"{path}: the chunk after IHDR is {found[1][0]!r}, not the recipe")
    for kind, data in found:
        if kind == b"iTXt" and data.startswith(b"jasper.synthesis\x00"):
            text = data[len(b"jasper.synthesis\x00") + 4:]   # compression flag, method, empty language and keyword
            return json.loads(text.decode("utf-8"))
    fail(f"{path}: no jasper.synthesis chunk")
    return {}


def pixels(path: Path) -> bytes:
    return b"".join(chunks_data for kind, chunks_data in chunks(path.read_bytes()) if kind == b"IDAT")


# ---------------------------------------------------------------------------- scratch tree and runs

def layout() -> Path:
    """<tmp>/work: in/ (rock_albedo.png 48x48 RGB, rock_normal.png RGB, rock_height.png 16-bit grey,
    small_normal.png 24x24, hole.png RGBA with a transparent square), proj/project.yaml, sub/ (the
    working directory of the runs)."""
    if "work" in S:
        return S["work"]
    tmp = Path(tempfile.mkdtemp(prefix="jasper-synth-cli-"))
    work = tmp / "work"
    inputs = work / "in"
    inputs.mkdir(parents=True)
    (work / "sub").mkdir()
    (work / "proj").mkdir()
    (work / "proj" / "project.yaml").write_text("name: synth\n")
    (inputs / "rock_albedo.png").write_bytes(png_bytes(48, 48, 3, noise_rows(48, 48, 1)))
    (inputs / "rock_normal.png").write_bytes(png_bytes(48, 48, 3, noise_rows(48, 48, 2)))
    (inputs / "rock_height.png").write_bytes(png_bytes(48, 48, 1, gray16_rows(48, 48), depth=16))
    (inputs / "small_normal.png").write_bytes(png_bytes(24, 24, 3, noise_rows(24, 24, 3)))
    hole = [bytearray(row) for row in noise_rows(40, 40, 4, channels=4)]
    for y in range(10, 22):
        for x in range(12, 30):
            hole[y][x * 4 + 3] = 0
    (inputs / "hole.png").write_bytes(png_bytes(40, 40, 4, [bytes(r) for r in hole]))
    S["tmp"] = tmp
    S["work"] = work
    return work


def run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(harness.editor_command() + ["synthesize-texture", *args], cwd=str(cwd or layout() / "sub"),
                          capture_output=True, text=True, timeout=300)


def ok(r: subprocess.CompletedProcess, what: str) -> subprocess.CompletedProcess:
    check(r.returncode == 0, f"{what}: exit {r.returncode}\nstdout: {r.stdout[-3000:]}\nstderr: {r.stderr[-3000:]}")
    return r


def out_dir(name: str) -> Path:
    d = layout() / name
    if d.exists():
        shutil.rmtree(d)
    return d


# ---------------------------------------------------------------------------- tests

async def test_usage_errors():
    work = layout()
    cases = [
        ([], "--example is required"),
        (["--example", "../in/rock_albedo.png"], "--out is required"),
        (["--example", "../in/rock_albedo.png", "--out", "x.png"], "--size is required"),
        (["--example", "../in/rock_albedo.png", "--out", "x.jpg", "--size", "32"], "must be a .png"),
        (["--bogus"], "unknown option --bogus"),
        (["--mode", "spiral", "--example", "a.png", "--out", "x.png"], "unknown mode"),
        (["--example", "../in/rock_albedo.png", "--out", "x.png", "--size", "32", "--cauchy", "0"], "cauchyDispersion"),
        (["--example", "../in/rock_albedo.png", "--out", "x.png", "--size", "32", "--backtrack-stages", "11"], "backtrackStages"),
        (["--example", "../in/rock_albedo.png", "--out", "x.png", "--size", "0x5"], "invalid --size"),
        (["--companion", "../in/rock_normal.png", "--example", "../in/rock_albedo.png"], "give --example first"),
        (["--mode", "make_tileable", "--example", "../in/rock_albedo.png", "--out", "x.png", "--size", "32"], "leave --size out"),
        (["--mode", "guided", "--example", "../in/rock_albedo.png", "--out", "x.png"], "--target-guide"),
        (["--example", "../in/rock_albedo.png", "--out", "x.png", "--size", "32", "--seed", "-1"], "invalid --seed"),
        (["--example", "../in/rock_albedo.png", "--out", "x.png", "--size", "32", "--project", "../in"], "no project.yaml"),
    ]
    for args, expected in cases:
        r = run(*args)
        check(r.returncode == 2, f"{args}: exit {r.returncode}, want 2\nstderr: {r.stderr[-1500:]}")
        check(expected in r.stderr, f"{args}: stderr lacks {expected!r}:\n{r.stderr[-1500:]}")
    check(not (work / "sub" / "x.png").exists(), "a usage error wrote a file")

    r = ok(run("--help"), "--help")
    for word in ("synthesize-texture", "--companion", "--overwrite", "make_tileable", "jasper.synthesis"):
        check(word in r.stdout, f"--help lacks {word!r}")
    main = subprocess.run(harness.editor_command() + ["--help"], capture_output=True, text=True, timeout=60)
    check("synthesize-texture" in main.stdout, f"the main usage lacks synthesize-texture:\n{main.stdout}")


async def test_relative_paths_json_companions_and_recipe():
    work = layout()
    out_dir("out")
    r = ok(run("--project", "../proj", "--mode", "tileable", "--example", "../in/rock_albedo.png",
               "--companion", "../in/rock_normal.png", "--companion", "../in/rock_height.png",
               "--out", "../proj/assets/rock_tile.png", "--uncertainty-out", "../proj/assets/rock_tile_uncertainty.png",
               "--size", "64x40", "--quality", "draft", "--seed", "7", "--format", "json", "-q"), "tileable run")
    check(r.stderr == "", f"-q: stderr is not empty: {r.stderr}")
    result = json.loads(r.stdout)
    check(result["ok"] is True and result["status"] == "ok", f"json: {result}")
    check(result["width"] == 64 and result["height"] == 40 and result["seed"] == 7, f"json: {result}")
    check(result["outputs"] == {"primary": "assets/rock_tile.png", "normal": "assets/rock_tile_normal.png",
                                "height": "assets/rock_tile_height.png", "uncertainty": "assets/rock_tile_uncertainty.png"},
          f"outputs: {result['outputs']}")
    for key in ("meanUncertainty", "highUncertaintyFraction", "seamScore", "synthesizedPixels"):
        check(key in result["metrics"], f"metrics lack {key}: {result['metrics']}")
    check(result["metrics"]["seamScore"] < 2.0, f"a tileable output with a seam: {result['metrics']}")
    check(result["params"]["nearestNeighbors"] == 20, f"draft params: {result['params']}")
    assets = work / "proj" / "assets"
    check(not (work / "sub" / "proj").exists() and not (work / "proj" / "proj").exists(), "a path joined twice")
    check(ihdr(assets / "rock_tile.png")[:4] == (64, 40, 8, 2), f"primary: {ihdr(assets / 'rock_tile.png')}")
    check(ihdr(assets / "rock_tile_normal.png")[:4] == (64, 40, 8, 2), "normal: 8-bit RGB")
    check(ihdr(assets / "rock_tile_height.png")[:4] == (64, 40, 16, 0), f"height: {ihdr(assets / 'rock_tile_height.png')}")
    check(ihdr(assets / "rock_tile_uncertainty.png")[:4] == (64, 40, 8, 0), "uncertainty: 8-bit grey")

    rec = recipe(assets / "rock_tile.png")
    check(rec["role"] == "primary" and rec["mode"] == "tileable" and rec["seed"] == 7, f"recipe: {rec}")
    check(rec["pathBase"] == "project" and rec["size"] == [64, 40], f"recipe: {rec}")
    check(rec["examples"][0]["path"].endswith("in/rock_albedo.png") and len(rec["examples"][0]["sha1"]) == 40, f"recipe: {rec}")
    check(rec["outputs"]["normal"] == "assets/rock_tile_normal.png", f"recipe outputs: {rec['outputs']}")
    names = [c["name"] for c in rec["companions"]]
    check(names == ["normal", "height"], f"recipe companions: {names}")
    check(recipe(assets / "rock_tile_height.png")["role"] == "height", "the height map's recipe role")
    check(recipe(assets / "rock_tile_uncertainty.png")["role"] == "uncertainty", "the uncertainty map's recipe role")

    # text mode: the summary on stderr, nothing on stdout
    r = ok(run("--example", "../in/rock_albedo.png", "--out", "../out/plain.png", "--size", "32", "--quality", "draft"), "text run")
    check(r.stdout == "", f"stdout of a clean run: {r.stdout}")
    check("synthesize-texture: wrote 1 file (32x32, extend, draft, seed 0)" in r.stderr, f"summary: {r.stderr}")
    check((work / "out" / "plain.png").is_file(), "--out relative to the working directory")
    check(recipe(work / "out" / "plain.png")["pathBase"] == "file", "no --project: paths relative to the file")
    check(recipe(work / "out" / "plain.png")["examples"][0]["path"] == "../in/rock_albedo.png", "file-relative example path")


async def test_overwrite_refusal():
    work = layout()
    out = out_dir("ow") / "tile.png"
    args = ["--example", "../in/rock_albedo.png", "--out", "../ow/tile.png", "--size", "32", "--quality", "draft", "-q"]
    ok(run(*args), "first run")
    before = out.read_bytes()
    r = run(*args, "--seed", "5")
    check(r.returncode == 2, f"existing output: exit {r.returncode}, want 2")
    check("exists (pass --overwrite to replace it)" in r.stderr, f"stderr: {r.stderr}")
    check(out.read_bytes() == before, "the refused run changed the file")
    # an output may not replace an input, even with --overwrite
    r = run("--mode", "make_tileable", "--example", "../in/rock_albedo.png", "--out", "../in/rock_albedo.png", "--overwrite")
    check(r.returncode == 2 and "would replace an input" in r.stderr, f"in place: exit {r.returncode}: {r.stderr}")
    ok(run(*args, "--seed", "5", "--overwrite"), "--overwrite")
    check(out.read_bytes() != before, "--overwrite did not replace the file")
    check(not list(work.glob("ow/*.partial*")) and not list(work.glob("ow/*.previous*")), "a temporary file was left")


async def test_deterministic_across_threads():
    out_dir("det")
    common = ["--mode", "tileable", "--example", "../in/rock_albedo.png", "--companion", "../in/rock_height.png",
              "--size", "56x48", "--seed", "11", "-q"]
    ok(run(*common, "--threads", "1", "--out", "../det/t1.png"), "threads 1")
    ok(run(*common, "--threads", "8", "--out", "../det/t8.png"), "threads 8")
    ok(run(*common[:-3], "--seed", "12", "-q", "--out", "../det/s12.png"), "seed 12")
    det = layout() / "det"
    check(pixels(det / "t1.png") == pixels(det / "t8.png"), "threads 1 and 8 gave different pixels")
    check(pixels(det / "t1_height.png") == pixels(det / "t8_height.png"), "threads 1 and 8 gave different companions")
    check(pixels(det / "t1.png") != pixels(det / "s12.png"), "another seed gave the same pixels")


async def test_failures_name_the_file():
    out_dir("fail")
    r = run("--example", "../in/missing.png", "--out", "../fail/x.png", "--size", "32")
    check(r.returncode == 1, f"missing input: exit {r.returncode}")
    check("in/missing.png: error: cannot open" in r.stdout, f"stdout: {r.stdout}")
    check("nothing written" in r.stderr, f"stderr: {r.stderr}")
    r = run("--example", "../in/rock_albedo.png", "--companion", "../in/small_normal.png", "--out", "../fail/x.png",
            "--size", "32", "--format", "json")
    check(r.returncode == 1, f"companion size: exit {r.returncode}")
    result = json.loads(r.stdout)
    check(result["ok"] is False and result["status"] == "invalid_request", f"json: {result}")
    check(result["errors"][0]["path"].endswith("in/small_normal.png"), f"errors: {result['errors']}")
    check("24x24 but its example is 48x48" in result["errors"][0]["message"], f"errors: {result['errors']}")
    check(result["outputs"] == {}, "outputs of a failed run")
    check(not (layout() / "fail" / "x.png").exists(), "a failed run wrote the output")


async def test_inpaint_and_make_tileable():
    work = layout()
    out_dir("modes")
    ok(run("--mode", "inpaint", "--example", "../in/hole.png", "--out", "../modes/filled.png", "--quality", "draft", "-q"), "inpaint")
    src_w, src_h, src_ch, src_rows = decode_png((work / "in" / "hole.png").read_bytes())
    w, h, ch, rows = decode_png((work / "modes" / "filled.png").read_bytes())
    check((w, h, ch) == (40, 40, 4), f"inpaint output {w}x{h}x{ch}")
    for y in range(h):
        for x in range(w):
            hole = 12 <= x < 30 and 10 <= y < 22
            got = rows[y][x * 4:x * 4 + 4]
            if hole:
                check(got[3] == 255, f"inpainted pixel {x},{y} is still transparent")
            else:
                check(got == src_rows[y][x * 4:x * 4 + 4], f"kept pixel {x},{y} changed")

    r = ok(run("--mode", "make_tileable", "--example", "../in/rock_albedo.png", "--out", "../modes/seamless.png",
               "--format", "json", "-q"), "make_tileable")
    result = json.loads(r.stdout)
    check(result["width"] == 48 and result["height"] == 48, f"make_tileable size: {result}")
    check(result["metrics"]["lockedPixels"] > 0 and result["metrics"]["seamScore"] < 2.0, f"metrics: {result['metrics']}")


def latin1(text: str) -> str:
    """A file name with raw Latin-1 bytes (not UTF-8), as a str the OS gets back byte for byte"""
    return os.fsdecode(text.encode("latin-1"))


async def test_non_utf8_names_and_garbled_recipes():
    work = layout()
    out_dir("utf8")
    (work / "in" / latin1("caf\xe9.png")).write_bytes((work / "in" / "rock_albedo.png").read_bytes())
    r = run("--example", "../in/" + latin1("caf\xe9.png"), "--out", "../utf8/x.png", "--size", "32", "--quality", "draft")
    check(r.returncode == 2, f"a Latin-1 example: exit {r.returncode}, want 2 (it aborted with 134 before)\n{r.stderr}")
    check("caf\\xE9.png" in r.stderr and "not valid UTF-8" in r.stderr and "rename" in r.stderr, f"stderr: {r.stderr}")
    r = run("--example", "../in/rock_albedo.png", "--out", "../utf8/" + latin1("t\xe9.png"), "--size", "32", "--quality", "draft",
            "--format", "json")
    check(r.returncode == 2 and "not valid UTF-8" in r.stderr, f"a Latin-1 output: exit {r.returncode}: {r.stderr}")
    check(not (work / "utf8").exists(), "nothing written")

    # check: a plain PNG with a Latin-1 name, a garbled recipe and a stale synthesized PNG with a Latin-1 name
    proj = work / "proj"
    assets = proj / "assets"
    assets.mkdir(exist_ok=True)
    (proj / ".jasperengine").mkdir(exist_ok=True)
    (assets / latin1("caf\xe9.png")).write_bytes((work / "in" / "rock_albedo.png").read_bytes())
    plain = (work / "in" / "rock_albedo.png").read_bytes()
    text = json.dumps({"format": 1, "mode": "tileable", "role": "primary", "pathBase": "project", "examples": [{"path": 5}]}).encode()
    data = b"jasper.synthesis\x00\x00\x00\x00\x00" + text
    itxt = struct.pack(">I", len(data)) + b"iTXt" + data + struct.pack(">I", zlib.crc32(b"iTXt" + data) & 0xFFFFFFFF)
    (assets / "garbled.png").write_bytes(plain[:33] + itxt + plain[33:])
    (assets / "src.png").write_bytes(plain)
    ok(run("--project", "../proj", "--example", "../proj/assets/src.png", "--out", "../proj/assets/stale.png", "--size", "32",
           "--quality", "draft", "-q", "--overwrite"), "a synthesized texture")
    (assets / "stale.png").rename(assets / latin1("stal\xe9.png"))
    (assets / "src.png").write_bytes(plain + b"\x00")
    for args in (["--textures"], []):
        r = subprocess.run(harness.editor_command() + ["check", "--project", str(proj), *args, "--format", "json"], capture_output=True,
                           timeout=120)
        check(r.returncode == 0, f"check {args}: exit {r.returncode} (it aborted with 134 before)\n{r.stderr[-2000:]!r}")
        result = json.loads(r.stdout.decode("utf-8"))
        codes = {(f["path"], f["code"]) for f in result["findings"] if f["tool"] == "texture"}
        check(("assets/garbled.png", "synthesis-recipe-invalid") in codes, f"check {args}: {codes}")
        check(("assets/stal\ufffd.png", "synthesis-source-changed") in codes, f"check {args}: the Latin-1 name shown with U+FFFD: {codes}")
    r = subprocess.run(harness.editor_command() + ["check", "--project", str(proj), "--textures"], capture_output=True, timeout=120)
    check(r.returncode == 0 and b"stal\xe9.png: warning" in r.stdout, f"text output keeps the bytes: {r.stdout!r}")
    cache = json.loads((proj / ".jasperengine" / "synthesis-check-cache.json").read_text("utf-8"))
    check("assets/garbled.png" in cache["pngs"] and not any("\ufffd" in k for k in cache["pngs"]), f"the cache keeps valid keys only: {cache}")


async def test_a_failed_write_changes_nothing():
    work = layout()
    d = out_dir("atomic")
    d.mkdir()
    (d / "blocker").write_text("a file where a directory is expected")
    args = ["--example", "../in/rock_albedo.png", "--companion", "../in/rock_normal.png", "--out", "../atomic/tile.png", "--size", "32",
            "--quality", "draft", "-q"]
    ok(run(*args), "first run")
    before = {p.name: p.read_bytes() for p in d.iterdir() if p.is_file()}
    r = run(*args, "--seed", "9", "--overwrite", "--uncertainty-out", "../atomic/blocker/u.png", "--format", "json")
    check(r.returncode == 1, f"an unwritable companion: exit {r.returncode}, want 1\n{r.stdout}\n{r.stderr}")
    result = json.loads(r.stdout)
    check(result["status"] == "write_failed" and result["errors"][0]["path"].endswith("atomic/blocker/u.png"), f"json: {result}")
    check("nothing was written" in result["errors"][0]["message"], f"json: {result}")
    after = {p.name: p.read_bytes() for p in d.iterdir() if p.is_file()}
    check(after == before, f"the primary and its companion kept their content, nothing left behind: {sorted(after)} vs {sorted(before)}")
