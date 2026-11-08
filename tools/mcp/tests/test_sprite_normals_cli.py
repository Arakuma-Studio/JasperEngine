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
- with --project, symbolic links: an output (-o, --output-dir, --atlas, --frames-json) through a folder of the assets
  that links outside the project, or a dangling link, exits 2 (output_invalid, nothing written), -o the source through
  a link exits 2; a project reached through a link and an assets directory that is a link work;
- --sprite: frames from the Sprite (from: "sprite");
- --cells-json with a written map's recipe frames.cells reproduces its pixels; with --frame-size it exits 2;
- paint (Phase 5 spec 4.2): --paint-json from a file, inline and stdin, --paint-mode keep / replace / clear, the JSON
  output's paint object and the text summary line, exit 2 for invalid ops (and two sources, clear with ops), exit 1 for
  paint_size_mismatch with nothing written.
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
CI_EXCLUDED = ("long-running (about 90 s on a loaded server: 17 tests of the binary, and a project made through the editor); "
               "in the full suite (MCP_SUITE=all) on the build servers")
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


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


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


# ---------------------------------------------------------------------------- 9b. Toon steps (quantize)

def distinct_opaque(path: Path) -> set[bytes]:
    out = set()
    for row in pixels(path):
        for x in range(0, len(row), 4):
            if row[x + 3] > 0:
                out.add(bytes(row[x:x + 3]))
    return out


async def test_quantize_flags_echo_and_errors():
    """--quantize N (--quantize-tilts, -flat, -rotation) snaps the normals (at most 1 + N x tilts colours), the JSON
    echoes settings.quantize, the recipe records it, --preset toon turns it on, and bad values exit 2 naming the flag"""
    work = scratch()
    code, out, err = run_json(work, "--source", "art/ball.png", "-o", "smooth.png")
    check(code == 0 and "quantize" not in out["results"][0]["settings"], f"off: no quantize key: {out['results'][0]['settings']} {err}")
    check(len(distinct_opaque(work / "smooth.png")) > 40, "smooth normals")
    code, out, err = run_json(work, "--source", "art/ball.png", "-o", "q.png", "--quantize", "4", "--quantize-tilts", "1",
                              "--quantize-flat", "5", "--quantize-rotation", "45")
    check(code == 0, f"--quantize: {err}")
    want = {"directions": 4, "tilts": 1, "flat_deg": 5, "rotation_deg": 45}
    check(out["results"][0]["settings"]["quantize"] == want, f"JSON echo: {out['results'][0]['settings']}")
    check(recipe(work / "q.png")["settings"]["quantize"] == want, f"recipe: {recipe(work / 'q.png')['settings']}")
    colours = distinct_opaque(work / "q.png")
    check(1 < len(colours) <= 5, f"at most 1 + 4 x 1 normals: {len(colours)}")
    code, out, err = run_json(work, "--source", "art/ball.png", "-o", "toon.png", "--preset", "toon")
    toon = out["results"][0]["settings"]
    check(code == 0 and toon["preset"] == "toon" and toon["quantize"]["directions"] > 0, f"--preset toon: {toon} {err}")
    q = toon["quantize"]
    check(len(distinct_opaque(work / "toon.png")) <= 1 + q["directions"] * q["tilts"], "toon: palette normals only")
    code, out, err = run_json(work, "--source", "art/ball.png", "-o", "toon_off.png", "--preset", "toon", "--quantize", "0")
    check(code == 0 and out["results"][0]["settings"]["quantize"]["directions"] == 0, f"toon with --quantize 0: recorded off: {out['results'][0]['settings']}")
    check(len(distinct_opaque(work / "toon_off.png")) > 40, "toon off: smooth")
    # the same flags again reproduce the pixels
    check(run(work, "--source", "art/ball.png", "-o", "q2.png", "--quantize", "4", "--quantize-tilts", "1", "--quantize-flat", "5",
              "--quantize-rotation", "45", "-q").returncode == 0, "again")
    check(pixels(work / "q.png") == pixels(work / "q2.png"), "deterministic")
    before = listing(work)
    for args, needle in [
        (["--quantize", "5"], "--quantize must be 0 (off), 4, 6, 8, 12 or 16 (got 5)"),
        (["--quantize", "8", "--quantize-tilts", "9"], "--quantize-tilts must be between 1 and 4 (got 9)"),
        (["--quantize", "8", "--quantize-flat", "50"], "--quantize-flat must be between 0 and 45 (got 50)"),
        (["--quantize", "8", "--quantize-rotation", "361"], "--quantize-rotation must be between 0 and 360 (got 361)"),
        (["--quantize", "eight"], "invalid --quantize value 'eight'"),
        (["--quantize", "4.5"], "invalid --quantize value '4.5'"),
        (["--quantize-flat", "x"], "invalid --quantize-flat value 'x'"),
    ]:
        r = run(work, "--source", "art/ball.png", *args)
        check(r.returncode == 2 and needle in r.stderr, f"{args}: exit 2 with {needle!r}: {r.returncode} {r.stderr[-600:]}")
    check(listing(work) == before, "usage errors write nothing")
    r = run(work, "--help")
    check("--quantize" in r.stdout and "toon" in r.stdout and "360 is 0" in r.stdout, "--help lists the flags and the preset")
    # 360 is 0 (as in the window and MCP)
    code, out, err = run_json(work, "--source", "art/ball.png", "--dry-run", "--quantize", "8", "--quantize-rotation", "360")
    check(code == 0 and out["results"][0]["settings"]["quantize"]["rotation_deg"] == 0, f"360 -> 0: {out['results'][0]['settings']} {err}")
    # flags without Toon steps: a warning (text and JSON), the run still succeeds
    code, out, err = run_json(work, "--source", "art/ball.png", "--dry-run", "--quantize-tilts", "2", "--quantize-flat", "20")
    warns = [w for w in out["results"][0]["warnings"] if w["kind"] == "quantize_off"]
    check(code == 0 and len(warns) == 1 and warns[0]["message"] ==
          "Toon steps are off: --quantize-tilts and --quantize-flat have no effect; add --quantize N", f"quantize_off: {out['results'][0]['warnings']}")
    r = run(work, "--source", "art/ball.png", "--dry-run", "--preset", "toon", "--quantize", "0", "--quantize-rotation", "45")
    check(r.returncode == 0 and "warning: quantize_off: Toon steps are off: --quantize-rotation has no effect; add --quantize N" in r.stderr,
          f"text warning: {r.stderr}")
    code, out, _ = run_json(work, "--source", "art/ball.png", "--dry-run", "--quantize", "8")
    check(not any(w["kind"] == "quantize_off" for w in out["results"][0]["warnings"]), "no warning with Toon steps on")
    # the summary line names Toon steps on a non-toon preset only
    r = run(work, "--source", "art/ball.png", "--dry-run", "--preset", "crisp", "--quantize", "6")
    check(r.returncode == 0 and "preset crisp, toon steps 6)" in r.stderr, f"summary: {r.stderr}")
    r = run(work, "--source", "art/ball.png", "--dry-run", "--preset", "toon")
    check(r.returncode == 0 and "preset toon)" in r.stderr and "toon steps" not in r.stderr, f"toon summary: {r.stderr}")


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
    # --project without --sprite starts no engine: no engine log, no Lua state, no .jasperengine made
    r = run(project, "--project", str(project), "--source", "assets/art/ball.png", "-o", "assets/art/again.png")
    check(r.returncode == 0 and "[INFO]" not in r.stderr and "Lua" not in r.stderr, f"no engine start: {r.stderr[-600:]}")
    check(not (project / ".jasperengine").exists(), "nothing of the engine's project state is created")
    r = run(project, "--project", str(project), "--source", "assets/art/ball.png", "-o", "outside.png")
    check(r.returncode == 2 and "output_invalid" in r.stderr, f"outside the assets directory: {r.returncode} {r.stderr}")
    check(not (project / "outside.png").exists(), "not written")


async def test_project_outputs_and_symbolic_links():
    # with --project the outputs stay inside its assets directory once symbolic links are resolved: a
    # folder of the assets that is a link to outside the project is refused (exit 2, output_invalid,
    # nothing written); an assets directory that is itself a link, and a project reached through a
    # link, keep working
    if not harness.symlinks_permitted():
        harness.skip("this process may not create symbolic links")
    work = scratch()
    project = work.parent / "linkproj"
    (project / "assets" / "art").mkdir(parents=True)
    (project / "project.yaml").write_text("name: Proj\nassetsDir: assets\n")
    shutil.copy(work / "art" / "ball.png", project / "assets" / "art" / "ball.png")
    shutil.copy(work / "art" / "spin.gif", project / "assets" / "art" / "spin.gif")
    outside = work.parent / "outside"
    outside.mkdir()
    os.symlink(outside, project / "assets" / "out")
    for args in (["--source", "assets/art/ball.png", "-o", "assets/out/ball_n.png"],
                 ["--source", "assets/art/ball.png", "--output-dir", "assets/out"],
                 ["--source", "assets/art/spin.gif", "--atlas", "assets/out/spin_sheet.png"],
                 ["--source", "assets/art/spin.gif", "--output-dir", "assets/out", "--frames-json"]):
        r = run(project, "--project", str(project), *args)
        check(r.returncode == 2 and "output_invalid" in r.stderr and "symbolic link" in r.stderr, f"{args}: exit 2 through the link: {r.returncode} {r.stderr[-600:]}")
    # dangling links (to a file and a folder outside that don't exist yet)
    os.symlink(outside / "dangling.png", project / "assets" / "art" / "dl.png")
    os.symlink(outside / "gone", project / "assets" / "dl_dir")
    for args in (["--source", "assets/art/ball.png", "-o", "assets/art/dl.png", "--overwrite"],
                 ["--source", "assets/art/ball.png", "--output-dir", "assets/dl_dir"]):
        r = run(project, "--project", str(project), *args)
        check(r.returncode == 2 and "output_invalid" in r.stderr and "symbolic link" in r.stderr, f"{args}: exit 2 through a dangling link: {r.returncode} {r.stderr[-600:]}")
    check(not any(outside.iterdir()), f"nothing written outside: {list(outside.iterdir())}")
    # an output that is the source through a link: refused even with --overwrite
    os.symlink("ball.png", project / "assets" / "art" / "alias.png")
    src = (project / "assets" / "art" / "ball.png").read_bytes()
    r = run(project, "--project", str(project), "--source", "assets/art/ball.png", "-o", "assets/art/alias.png", "--overwrite")
    check(r.returncode == 2 and "output_invalid" in r.stderr, f"-o the source through a link: {r.returncode} {r.stderr[-600:]}")
    check((project / "assets" / "art" / "ball.png").read_bytes() == src, "the source is unchanged")
    # a project reached through a link
    via = work.parent / "via"
    os.symlink(project, via)
    code, out, err = run_json(via, "--project", str(via), "--source", "assets/art/ball.png")
    check(code == 0, f"through a linked project: {err} {json.dumps(out)}")
    check((project / "assets" / "art" / "ball_normal.png").is_file(), "written in the project")
    # an assets directory that is a link (to a folder outside the project)
    linked = work.parent / "linkedassets"
    real = work.parent / "realassets"
    (real / "art").mkdir(parents=True)
    linked.mkdir()
    (linked / "project.yaml").write_text("name: Linked\nassetsDir: assets\n")
    os.symlink(real, linked / "assets")
    shutil.copy(work / "art" / "ball.png", real / "art" / "ball.png")
    code, out, err = run_json(linked, "--project", str(linked), "--source", "assets/art/ball.png")
    check(code == 0, f"a linked assets directory: {err} {json.dumps(out)}")
    check((real / "art" / "ball_normal.png").is_file(), "written into the linked assets directory")
    # a link below the linked assets directory, leading further out: refused, nothing written there
    further = work.parent / "further"
    further.mkdir()
    os.symlink(further, real / "out")
    r = run(linked, "--project", str(linked), "--source", "assets/art/ball.png", "-o", "assets/out/ball_n.png")
    check(r.returncode != 0 and "output_invalid" in r.stderr, f"a link below the assets directory: {r.returncode} {r.stderr[-600:]}")
    check(not any(further.iterdir()), "nothing written further out")


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
    rec = recipe(art / "sheet_normal.png")
    check(rec["frames"]["from"] == "sprite", "the recipe records from: sprite")
    check(rec["source"]["kind"] == "sprite" and rec["source"]["entity"] == "Hero", f"the recipe records the Sprite like the MCP tool: {rec['source']}")


async def test_sprite_tilemap_with_two_tilesets(ed: Editor):
    art = ed.project / "assets" / "art"
    art.mkdir(parents=True, exist_ok=True)
    for name, shade in (("ground_a.png", 90), ("ground_b.png", 180)):
        rows = [b"\x00" + b"".join(bytes((shade, 140, (x * y) % 90, 255)) for x in range(32)) for y in range(16)]
        (art / name).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 32, 16, 8, 6, 0, 0, 0)) +
                                 chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
    await ed.call("edit_tilemap", create={"name": "Ground"}, texture="assets/art/ground_a.png", tileset={"tile_size": [16, 16]},
                  rects=[{"name": "b0", "rect": [0, 0, 16, 16], "submesh": 1}],
                  ops=[{"op": "fill", "tile": 0, "from": [0, 0], "to": [3, 0]}, {"op": "set", "cells": [[0, 1, "b0"], [1, 1, "b0"]]}])
    await ed.call("wait", frames=3)
    # the second tileset: submesh 1 with its own colour texture
    await ed.call("set_properties", entity="Ground", values={"mesh": {"numSubmeshes": 2}})
    await ed.call("set_properties", entity="Ground", values={"mesh": {"submeshes[1].material.baseColorTexture": "assets/art/ground_b.png"}})
    await ed.call("wait", frames=3)
    await ed.call("save_scene", path="scenes/Main.scene")
    await ed.call("save_project")
    code, out, err = run_json(ed.project, "--project", str(ed.project), "--sprite", "Ground", "--scene", "scenes/Main.scene", "--frame-size", "16",
                              "--edges", "extend")
    check(code == 0, f"{err} {out}")
    sources = sorted(r["source"] for r in out["results"])
    check(sources == ["assets/art/ground_a.png", "assets/art/ground_b.png"], f"one job per tileset texture: {sources}")
    for name in ("ground_a_normal.png", "ground_b_normal.png"):
        rec = recipe(art / name)
        check(rec["source"]["kind"] == "sprite" and rec["source"]["entity"] == "Ground", f"{name}: {rec['source']}")
    check(recipe(art / "ground_b_normal.png")["source"].get("submesh") == 1, "the second tileset's recipe names its submesh")
    r = run(ed.project, "--project", str(ed.project), "--sprite", "Ground", "--scene", "scenes/Main.scene", "-o", "assets/art/one.png", "--overwrite")
    check(r.returncode == 2 and "tileset" in r.stderr, f"-o with several tilesets: {r.returncode} {r.stderr[-400:]}")


# ---------------------------------------------------------------------------- paint (Phase 5)

def paint_chunk(path: Path) -> dict | None:
    key = b"jasper.spritenormals.paint\x00"
    for kind, data in chunks(path.read_bytes()):
        if kind == b"iTXt" and data.startswith(key):
            return json.loads(data[len(key) + 4:].decode("utf-8"))
    return None


async def test_paint_json_modes_and_exit_codes():
    work = scratch()
    out = work / "art" / "ball_normal.png"
    ops = [{"op": "flatten", "where": {"rect": [10, 10, 8, 8]}}]
    (work / "ops.json").write_text(json.dumps(ops))
    # a file
    code, res, err = run_json(work, "--source", "art/ball.png", "--paint-json", "ops.json")
    check(code == 0, f"{err} {res}")
    paint = res["results"][0]["paint"]
    check(paint["mode"] == "keep" and paint["kept_from"] is None and paint["ops"] == [{"op": "flatten", "selected": 64, "changed": 64}]
          and paint["painted_pixels"] == 64 and paint["hidden_pixels"] == 0 and paint["painted_pct"] > 0, f"paint: {paint}")
    check(res["results"][0]["metrics"]["painted_pct"] == paint["painted_pct"], "metrics.painted_pct")
    check(paint_chunk(out)["painted"] == 64 and recipe(out)["paint"] == {"painted": 64}, "the chunk and the recipe")
    px = pixels(out)
    check(tuple(px[12][12 * 4:12 * 4 + 4]) == (128, 128, 255, 255), "flattened")
    # inline JSON (keep adds to the file's paint), and the text summary
    r = run(work, "--source", "art/ball.png", "--overwrite", "--paint-json", '[{"op":"flatten","where":{"pixels":[[20,20]]}}]')
    check(r.returncode == 0 and "paint: 1 op, 65 px painted (0 hidden), kept from art/ball_normal.png" in r.stderr, f"inline: {r.returncode} {r.stderr[-600:]}")
    # stdin, replace
    cmd = harness.editor_command() + ["generate-sprite-normals", "--source", "art/ball.png", "--overwrite", "--paint-mode", "replace",
                                      "--paint-json", "-", "--format", "json"]
    r = subprocess.run(cmd, cwd=str(work), input=json.dumps([{"op": "erase", "where": {"frame": 0}}, {"op": "flatten", "where": {"pixels": [[16, 16]]}}]),
                       capture_output=True, text=True, timeout=300)
    check(r.returncode == 0, f"stdin: {r.stderr[-600:]}")
    paint = json.loads(r.stdout)["results"][0]["paint"]
    check(paint["mode"] == "replace" and paint["painted_pixels"] == 1 and paint["ops"][0] == {"op": "erase", "selected": 540, "changed": 0},
          f"replace from stdin: {paint}")
    # clear: no chunk
    code, res, err = run_json(work, "--source", "art/ball.png", "--overwrite", "--paint-mode", "clear")
    check(code == 0 and res["results"][0]["paint"]["mode"] == "clear" and paint_chunk(out) is None and "paint" not in recipe(out), f"clear: {err}")
    # usage errors (2): invalid ops, modes, two sources, clear with ops
    before = out.read_bytes()
    for args, needle in [
        (["--paint-json", '[{"op":"paint","where":{"frame":0}}]'], "paint[0].op"),
        (["--paint-json", '[{"op":"angle","where":{"frame":0},"direction":{"azimuth_deg":0,"tilt_deg":90}}]'], "85"),
        (["--paint-json", "[not json"], "not JSON"),
        (["--paint-mode", "sometimes"], "keep, replace, clear"),
        (["--paint-mode", "clear", "--paint-json", '[{"op":"erase","where":{"frame":0}}]'], "clear"),
        (["--source", "art/sheet.png", "--paint-json", "ops.json"], "one source"),
        (["--paint-json", '[{"op":"flatten","where":{"frame":3}}]'], "frame"),
    ]:
        r = run(work, "--source", "art/ball.png", "--overwrite", *args)
        check(r.returncode == 2 and needle in r.stderr, f"{args}: exit 2 with {needle!r}: {r.returncode} {r.stderr[-500:]}")
    check(out.read_bytes() == before, "usage errors write nothing")
    # a resized source under kept paint: exit 1, nothing written; --paint-mode clear writes
    run(work, "--source", "art/ball.png", "--overwrite", "--paint-json", "ops.json")
    painted = out.read_bytes()
    shutil.copy(FIXTURES / "sheet.png", work / "art" / "ball.png")
    code, res, err = run_json(work, "--source", "art/ball.png", "--overwrite")
    check(code == 1 and any(e["kind"] == "paint_size_mismatch" for e in res["errors"]) and out.read_bytes() == painted, f"mismatch: {code} {res['errors']}")
    code, res, err = run_json(work, "--source", "art/ball.png", "--overwrite", "--paint-mode", "clear")
    check(code == 0 and paint_chunk(out) is None, f"cleared: {err}")


# ---------------------------------------------------------------------------- high resolution (hires design spec)

async def test_high_resolution_flags():
    work = scratch()
    hires = FIXTURES.parent / "hires"
    for name in ("potion.png", "crate.png", "cobbles_256.png"):
        shutil.copy(hires / name, work / "art" / name)
    r = run(work, "--help")
    for flag in ("--resolution-scale", "--settings-version", "--soft-edge", "painted", "inflate", "0..32"):
        check(flag in r.stdout, f"--help names {flag}")
    before = listing(work)
    for args, needle in [(["--smoothing", "33"], "--smoothing"), (["--softness", "40"], "--softness"), (["--resolution-scale", "on"], "resolution scale"),
                         (["--profile", "pillow"], "inflate"), (["--settings-version", "3"], "--settings-version"), (["--preset", "huge"], "tile"),
                         (["--soft-edge", "hard"], "soft edge")]:
        r = run(work, "--source", "art/potion.png", *args)
        check(r.returncode == 2 and needle in r.stderr, f"{args}: exit 2 naming {needle!r}: {r.returncode} {r.stderr[-400:]}")
    check(listing(work) == before, "usage errors write nothing")
    # the default: inflate, scaled, smooth; the rounded profile: creased (exit 0, a warning)
    code, out, _ = run_json(work, "--source", "art/potion.png", "--dry-run")
    res = out["results"][0]
    check(code == 0 and "inflate" in res["profile_resolved"] and res["scale_range"][1] > 1.2, f"{res['profile_resolved']} {res['scale_range']}")
    check(res["metrics"]["crease_pct"] < 1 and res["metrics"]["roughness_deg"] < 13, f"{res['metrics']}")
    code, out, _ = run_json(work, "--source", "art/potion.png", "--dry-run", "--profile", "rounded")
    res = out["results"][0]
    check(code == 0 and "creased" in [w["kind"] for w in res["warnings"]], f"rounded: {res['warnings']}")
    # presets and flags land in the recipe
    code, out, _ = run_json(work, "--source", "art/crate.png", "--preset", "props", "-o", "art/crate_n.png")
    rec = recipe(work / "art" / "crate_n.png")
    check(code == 0 and rec["version"] == 2 and rec["settings"]["profile"] == "dome" and rec["settings"]["preset"] == "props", f"{rec['settings']}")
    code, out, _ = run_json(work, "--source", "art/potion.png", "--soft-edge", "solid", "--dry-run")
    check(code == 0 and out["results"][0]["settings"]["soft_edge"] == "solid", f"{out['results'][0]['settings']}")
    code, out, _ = run_json(work, "--source", "art/cobbles_256.png", "--preset", "tile", "-o", "art/tile_n.png")
    check(code == 0 and recipe(work / "art" / "tile_n.png")["settings"]["edges"] == "wrap", "tile wraps")
    code, out, _ = run_json(work, "--source", "art/potion.png", "--resolution-scale", "off", "--smoothing", "32", "--softness", "32", "--dry-run")
    check(code == 0 and out["results"][0]["scale_range"] == [1, 1], f"{out['results'][0]['scale_range']}")
    # --settings-version 1: the rules before high-resolution support, a version 1 recipe, the same pixels again
    code, out, _ = run_json(work, "--source", "art/potion.png", "--settings-version", "1", "-o", "art/potion_v1.png")
    rec = recipe(work / "art" / "potion_v1.png")
    check(code == 0 and rec["version"] == 1 and rec["settings"]["version"] == 1, f"{rec['version']} {rec['settings']}")
    first = pixels(work / "art" / "potion_v1.png")
    check(first == pixels(hires / "v1" / "potion_n.png"), "the bytes the editor before high-resolution support wrote")


async def test_exported_games_ship_pngs_without_the_recipe():
    """`export` copies a generated (and painted) normal map without the editor's jasper.* chunks: the same pixels,
    no recipe or paint layer in the game (Exporter::copyPngWithoutEditorChunks); the project's file keeps them"""
    with tempfile.TemporaryDirectory(prefix="jasper-normals-export-") as tmp:
        project = Path(tmp) / "game"
        shutil.copytree(REPO / "tools" / "fixtures" / "stochastic-tiling", project)
        shutil.copy(FIXTURES / "character.png", project / "assets" / "textures" / "hero.png")
        r = run(project, "--project", str(project), "--source", "assets/textures/hero.png",
                "--paint-json", '[{"op":"flatten","where":{"rect":[10,10,4,4]}}]')
        check(r.returncode == 0, f"generate: {r.stdout} {r.stderr}")
        source = (project / "assets" / "textures" / "hero_normal.png").read_bytes()
        check(source.count(b"jasper.spritenormals") == 2, "the project's map has its recipe and paint layer")
        # S14: a cooked texture ships as its cooks only; shipSource keeps this source PNG in the export
        (project / "assets" / "textures" / "hero_normal.png.meta").write_text(
            "metaVersion: 1\nguid: 5e1f2a6e98d71059023406ee04f3df66\nimport:\n  textureImporter:\n    version: 1\n    shipSource: true\n")
        out = Path(tmp) / "out"
        r = subprocess.run(harness.editor_command() + ["export", "--project", str(project), "--out", str(out), "--backend", "opengl"],
                           cwd=str(project), capture_output=True, text=True, timeout=600)
        check(r.returncode == 0, f"export: {r.stdout[-1500:]} {r.stderr[-1500:]}")
        shipped = list(out.rglob("hero_normal.png"))
        check(len(shipped) == 1, f"one exported map: {shipped}")
        data = shipped[0].read_bytes()
        check(b"jasper." not in data, "no jasper.* chunk in the exported map")
        check(decode_png(data) == decode_png(source), "the same pixels")
        check((project / "assets" / "textures" / "hero_normal.png").read_bytes() == source, "the project's file is untouched")
