"""`jasperengine-editor generate-sprite-normals` (editor/spritenormals/SpriteNormalsCli.cpp, sprite normals Phase 3 spec
10.2): the binary in scratch directories, run from a working directory below the inputs (relative paths resolve
against it), with Phase 2's fixtures (ball, the 128x32 sheet, the 8-frame GIF) and a project made through the editor
for --sprite:

- --help, and the usage errors (exit 2, with the reason): unknown option, --detail 9, --preset x, --region colors=red,
  -o / --preview-out with two sources, --frames-json on a plain image, --no-frames on a GIF, --sprite without --project;
- one source with --format json: exactly one JSON object on stdout with every key, the file and its recipe;
- a batch with --output-dir; a duplicate stem from two folders into one --output-dir exits 2 naming both, nothing
  written;
- an existing output without --overwrite exits 1 (bytes unchanged), with it it is replaced; an output equal to the
  source (also through dir/../ and a symlink) exits 2 even with --overwrite;
- --dry-run writes nothing (not even --preview-out) and lists the planned paths; --preview-out writes a PNG <= 1024 wide;
- a GIF: atlas + normal atlas and --frames-json (version, frame_count, delays_ms, frame_ms, loop);
- all or nothing: a batch whose second output directory is a file writes nothing;
- --threads 1 and 0 give identical pixels;
- the recipe's source.path: relative to the output's directory without --project, project-relative with it; an output
  outside the assets directory exits 2;
- --sprite: frames from the Sprite (from: "sprite");
- --cells-json with a written map's recipe frames.cells reproduces its pixels; with --frame-size it exits 2.
"""

from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

import harness
from harness import REPO, Editor, TestFailure
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "NormalsCli"}
TIMEOUT = 600

FIXTURES = REPO / "editor" / "spritenormals" / "tests" / "fixtures" / "inputs"


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def scratch() -> Path:
    root = Path(tempfile.mkdtemp(prefix="jasper-normals-cli-"))
    art = root / "work" / "art"
    art.mkdir(parents=True)
    for name in ("ball.png", "sheet.png", "spin.gif"):
        shutil.copy(FIXTURES / name, art / name)
    return root / "work"


def run(cwd: Path, *args: str) -> subprocess.CompletedProcess:
    cmd = harness.editor_command() + ["generate-sprite-normals", *args]
    return subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True, timeout=300)


def run_json(cwd: Path, *args: str) -> tuple[int, dict, str]:
    r = run(cwd, *args, "--format", "json")
    try:
        return r.returncode, json.loads(r.stdout), r.stderr
    except json.JSONDecodeError:
        fail(f"stdout is not one JSON object (exit {r.returncode}): {r.stdout[-1500:]!r} {r.stderr[-1500:]}")
        return 0, {}, ""


def chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    out, pos = [], 8
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        out.append((data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]))
        pos += 12 + length
    return out


def recipe(path: Path) -> dict:
    key = b"jasper.spritenormals\x00"
    for kind, data in chunks(path.read_bytes()):
        if kind == b"iTXt" and data.startswith(key):
            return json.loads(data[len(key) + 4:].decode("utf-8"))
    fail(f"{path}: no jasper.spritenormals chunk")
    return {}


def pixels(path: Path) -> list[bytes]:
    return decode_png(path.read_bytes())[3]


def listing(root: Path) -> set[str]:
    return {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file() or p.is_symlink()}


# ---------------------------------------------------------------------------- 1. help and usage errors

async def test_help_and_usage_errors():
    work = scratch()
    r = run(work, "--help")
    check(r.returncode == 0 and "generate-sprite-normals" in r.stdout and "--cells-json" in r.stdout, f"--help: {r.returncode} {r.stdout[:300]}")
    before = listing(work)
    cases = [
        (["--source", "art/ball.png", "--bogus"], "unknown option"),
        (["--source", "art/ball.png", "--detail", "9"], "--detail"),
        (["--source", "art/ball.png", "--preset", "x"], "preset"),
        (["--source", "art/ball.png", "--region", "colors=red"], "red"),
        (["--source", "art/ball.png", "--source", "art/sheet.png", "-o", "x.png"], "--output-dir"),
        (["--source", "art/ball.png", "--source", "art/sheet.png", "--preview-out", "p.png"], "--preview-out"),
        (["--source", "art/ball.png", "--frames-json"], "--frames-json"),
        (["--source", "art/spin.gif", "--no-frames"], "frames"),
        (["--sprite", "Hero"], "--project"),
    ]
    for args, needle in cases:
        r = run(work, *args)
        check(r.returncode == 2 and needle in r.stderr, f"{args}: exit 2 with {needle!r}: {r.returncode} {r.stderr[-600:]}")
    check(listing(work) == before, "usage errors write nothing")


# ---------------------------------------------------------------------------- 2. one source, JSON

async def test_single_source_json():
    work = scratch()
    code, out, _ = run_json(work, "--source", "art/ball.png")
    check(code == 0 and out["ok"] is True and out["dry_run"] is False and isinstance(out["seconds"], (int, float)) and out["errors"] == [], f"{out}")
    res = out["results"][0]
    for key in ("source", "kind", "width", "height", "frames", "settings", "radius_resolved", "metrics", "assessment", "hint", "warnings", "outputs", "written"):
        check(key in res, f"results[0].{key}: {res}")
    check(res["source"] == "art/ball.png" and res["kind"] == "image" and res["written"] is True, f"{res}")
    check(res["outputs"] == {"normal": "art/ball_normal.png", "atlas": None, "height": None, "frames_json": None, "preview": None}, f"{res['outputs']}")
    rec = recipe(work / "art" / "ball_normal.png")
    check(rec["format"] == "jasperengine.spritenormals" and rec["source"]["path"] == "ball.png", f"recipe (path relative to the output's directory): {rec['source']}")


# ---------------------------------------------------------------------------- 3. batch and duplicates

async def test_batch_and_duplicates():
    work = scratch()
    code, out, err = run_json(work, "--source", "art/ball.png", "--source", "art/sheet.png", "--output-dir", "out")
    check(code == 0 and {(work / "out" / n).is_file() for n in ("ball_normal.png", "sheet_normal.png")} == {True}, f"batch: {code} {err}")
    (work / "other").mkdir()
    shutil.copy(work / "art" / "ball.png", work / "other" / "ball.png")
    r = run(work, "--source", "art/ball.png", "--source", "other/ball.png", "--output-dir", "dup")
    check(r.returncode == 2 and "art/ball.png" in r.stderr and "other/ball.png" in r.stderr, f"duplicate stem: {r.returncode} {r.stderr}")
    check(not (work / "dup").exists() or not any((work / "dup").iterdir()), "nothing written")


# ---------------------------------------------------------------------------- 4. overwrite and inputs

async def test_overwrite_and_inputs():
    work = scratch()
    check(run(work, "--source", "art/ball.png", "-q").returncode == 0, "first write")
    out = work / "art" / "ball_normal.png"
    before = out.read_bytes()
    r = run(work, "--source", "art/ball.png", "--preset", "chunky")
    check(r.returncode == 1 and "output_exists" in r.stderr and "art/ball_normal.png" in r.stderr, f"exists: {r.returncode} {r.stderr}")
    check(out.read_bytes() == before, "bytes unchanged")
    check(run(work, "--source", "art/ball.png", "--preset", "chunky", "--overwrite", "-q").returncode == 0 and out.read_bytes() != before, "replaced")
    src = (work / "art" / "ball.png").read_bytes()
    os.symlink("ball.png", work / "art" / "link.png")
    for target in ("art/ball.png", "art/../art/ball.png", "art/link.png"):
        r = run(work, "--source", "art/ball.png", "-o", target, "--overwrite")
        check(r.returncode == 2 and "output_invalid" in r.stderr, f"-o {target}: exit 2: {r.returncode} {r.stderr}")
    check((work / "art" / "ball.png").read_bytes() == src, "the source is unchanged")


# ---------------------------------------------------------------------------- 5 / 6. dry run, preview

async def test_dry_run_and_preview():
    work = scratch()
    before = listing(work)
    code, out, _ = run_json(work, "--source", "art/ball.png", "--preview-out", "p.png", "--dry-run")
    res = out["results"][0]
    check(code == 0 and out["dry_run"] is True and res["written"] is False, f"{out}")
    check(set(res["planned"]) == {"art/ball_normal.png", "p.png"}, f"planned: {res['planned']}")
    check(listing(work) == before, "a dry run writes nothing, not even --preview-out")
    check(run(work, "--source", "art/ball.png", "--preview-out", "p.png", "-q").returncode == 0, "write with a preview")
    w, h, _, _ = decode_png((work / "p.png").read_bytes())
    check(w <= 1024 and h <= 1024, f"preview {w}x{h}")


# ---------------------------------------------------------------------------- 7. GIF

async def test_gif_and_frames_json():
    work = scratch()
    code, out, err = run_json(work, "--source", "art/spin.gif", "--frames-json")
    check(code == 0, err)
    res = out["results"][0]
    check(res["outputs"]["normal"] == "art/spin_sheet_normal.png" and res["outputs"]["atlas"] == "art/spin_sheet.png", f"{res['outputs']}")
    check(res["frames"]["from"] == "gif" and res["frames"]["count"] == 8, f"{res['frames']}")
    j = json.loads((work / "art" / "spin_sheet.frames.json").read_text())
    a = j["animation"]
    check(j["version"] == 1 and a["frame_count"] == 8 and len(a["delays_ms"]) == 8 and len(a["frame_ms"]) == 8 and "loop" in a, f"frames JSON: {j}")
    check(recipe(work / "art" / "spin_sheet.png")["role"] == "atlas", "the atlas carries its recipe")


# ---------------------------------------------------------------------------- 8. all or nothing

async def test_all_or_nothing():
    work = scratch()
    # the second source's normal map can't be created (its name is longer than a file name may be);
    # the preflight can't know that: the write fails and takes the first output back
    long = work / "art" / ("a" * 246 + ".png")
    shutil.copy(work / "art" / "sheet.png", long)
    r = run(work, "--source", "art/ball.png", "--source", str(long.relative_to(work)))
    check(r.returncode == 1 and "write_failed" in r.stderr, f"exit 1 naming the file: {r.returncode} {r.stderr[-800:]}")
    check(not (work / "art" / "ball_normal.png").exists(), "the first output is not written")


# ---------------------------------------------------------------------------- 9. determinism

async def test_threads_never_change_the_output():
    work = scratch()
    check(run(work, "--source", "art/sheet.png", "--frame-size", "32", "-o", "t1.png", "--threads", "1", "-q").returncode == 0, "threads 1")
    check(run(work, "--source", "art/sheet.png", "--frame-size", "32", "-o", "t0.png", "--threads", "0", "-q").returncode == 0, "threads 0")
    check(pixels(work / "t1.png") == pixels(work / "t0.png"), "identical pixels")


# ---------------------------------------------------------------------------- 10. recipe paths and --project

async def test_recipe_paths_and_project():
    work = scratch()
    check(run(work, "--source", "art/ball.png", "-o", "out/n/ball_n.png", "-q").returncode == 0, "write")
    check(recipe(work / "out" / "n" / "ball_n.png")["source"]["path"] == "../../art/ball.png", "relative to the output's directory")
    project = work.parent / "proj"
    (project / "assets" / "art").mkdir(parents=True)
    (project / "project.yaml").write_text("name: Proj\nassetsDir: assets\n")
    shutil.copy(work / "art" / "ball.png", project / "assets" / "art" / "ball.png")
    code, out, err = run_json(project, "--project", str(project), "--source", "assets/art/ball.png")
    check(code == 0, err + json.dumps(out))
    check(out["results"][0]["outputs"]["normal"] == "assets/art/ball_normal.png", f"{out['results'][0]['outputs']}")
    check(recipe(project / "assets" / "art" / "ball_normal.png")["source"]["path"] == "assets/art/ball.png", "project-relative")
    r = run(project, "--project", str(project), "--source", "assets/art/ball.png", "-o", "outside.png")
    check(r.returncode == 2 and "output_invalid" in r.stderr, f"outside the assets directory: {r.returncode} {r.stderr}")
    check(not (project / "outside.png").exists(), "not written")


# ---------------------------------------------------------------------------- 12. --cells-json

async def test_cells_json_reproduces():
    work = scratch()
    check(run(work, "--source", "art/sheet.png", "--frame-size", "32x32", "-o", "grid.png", "-q").returncode == 0, "grid write")
    cells = recipe(work / "grid.png")["frames"]["cells"]
    (work / "cells.json").write_text(json.dumps(cells))
    r = run(work, "--source", "art/sheet.png", "--cells-json", "cells.json", "-o", "cells.png", "-q")
    check(r.returncode == 0 and pixels(work / "cells.png") == pixels(work / "grid.png"), f"same pixels from the recipe's cells: {r.stderr}")
    r = run(work, "--source", "art/sheet.png", "--cells-json", "cells.json", "--frame-size", "32")
    check(r.returncode == 2 and "--cells-json" in r.stderr, f"with --frame-size: {r.returncode} {r.stderr}")


# ---------------------------------------------------------------------------- 11. --sprite (the editor makes the project)

async def test_sprite_frames(ed: Editor):
    art = ed.project / "assets" / "art"
    art.mkdir(parents=True, exist_ok=True)
    shutil.copy(FIXTURES / "sheet.png", art / "sheet.png")
    await ed.call("set_sprite_frames", create={"name": "Hero"}, texture="assets/art/sheet.png", slice={"tile_size": [32, 32]})
    await ed.call("wait", frames=3)   # the Sprite's first update makes its submesh (saved with it)
    await ed.call("save_scene", path="scenes/Main.scene")
    await ed.call("save_project")
    code, out, err = run_json(ed.project, "--project", str(ed.project), "--sprite", "Hero", "--scene", "scenes/Main.scene")
    check(code == 0, f"{err} {out}")
    res = out["results"][0]
    check(res["kind"] == "sprite" and res["entity"] == "Hero" and res["frames"]["from"] == "sprite" and res["frames"]["count"] == 4, f"{res}")
    check(recipe(art / "sheet_normal.png")["frames"]["from"] == "sprite", "the recipe records from: sprite")
