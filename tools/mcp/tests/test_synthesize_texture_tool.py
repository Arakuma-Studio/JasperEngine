"""synthesize_texture (editor/mcp/tools/SynthesisTools.cpp), the Texture Synthesis window
(editor/window/TextureSynthesisWindow.cpp) and the recipe check (`check --textures`), on a new
project with small generated textures (value noise; a 16-bit height companion; a mis-sized one).

- the schema (one tool, real constraints, no extra properties) and fix-it errors for bad arguments;
- a preview-only call writes nothing and returns the result image, a 2x2 tiling (tileable modes),
  metrics, a hint and the seed; the auto companions are found, the mis-sized one left out with a
  warning;
- the same arguments + seed + output write exactly the previewed pixels: primary + companions,
  each with its recipe (iTXt jasper.synthesis, role), the height map still 16-bit;
- the whole set is one undo step (undo removes the files, redo puts them back);
- existing outputs are refused without overwrite: true;
- the job path: a tiny timeout returns a job_id that get_job follows to the result; cancel_job
  stops a run and nothing is written;
- inpaint keeps the pixels outside the mask; make_tileable keeps the size;
- get_asset_info / view_asset show the recipe, its sources' state and regenerate arguments that
  reproduce the file;
- `check` warns (synthesis-source-changed / -missing) once a source changes or disappears;
- show_in_editor runs the synthesis in the Texture Synthesis window and saves it through the
  window's Save; an editor screenshot shows the window (saved for inspection);
- (last: it switches projects) a run still going when another project is opened is cancelled and
  its job fails saying so; nothing lands in either project, with or without show_in_editor.
"""

from __future__ import annotations

import asyncio
import json
import math
import os
import random
import struct
import subprocess
import zlib
from pathlib import Path

import harness
from harness import Editor, TestFailure
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "SynthTool"}
TIMEOUT = 900

TEX = "assets/textures"
ALBEDO = f"{TEX}/rock_albedo.png"


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


def noise_rows(width: int, height: int, seed: int) -> list[bytes]:
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
    for kind, data in chunks(path.read_bytes()):
        if kind == b"IHDR":
            return struct.unpack(">IIBB", data[:10])
    fail(f"{path}: no IHDR")
    return 0, 0, 0, 0


def recipe(path: Path) -> dict:
    for kind, data in chunks(path.read_bytes()):
        if kind == b"iTXt" and data.startswith(b"jasper.synthesis\x00"):
            return json.loads(data[len(b"jasper.synthesis\x00") + 4:].decode("utf-8"))
    fail(f"{path}: no jasper.synthesis chunk")
    return {}


def rgb_pixels(png: bytes) -> list[tuple[int, int, int]]:
    w, h, channels, rows = decode_png(png)
    out = []
    for row in rows:
        for x in range(w):
            out.append(tuple(row[x * channels:x * channels + 3]))
    return out


def write_textures(project: Path) -> None:
    tex = project / TEX
    tex.mkdir(parents=True, exist_ok=True)
    (tex / "rock_albedo.png").write_bytes(png_bytes(64, 64, 3, noise_rows(64, 64, 1)))
    (tex / "rock_normal.png").write_bytes(png_bytes(64, 64, 3, noise_rows(64, 64, 2)))
    (tex / "rock_height.png").write_bytes(png_bytes(64, 64, 1, gray16_rows(64, 64), depth=16))
    (tex / "rock_ao.png").write_bytes(png_bytes(32, 32, 3, noise_rows(32, 32, 3)))          # wrong size
    (tex / "grass_albedo.png").write_bytes(png_bytes(48, 48, 3, noise_rows(48, 48, 4)))     # not a companion


def files_under(project: Path) -> set[str]:
    return {p.relative_to(project).as_posix() for p in (project / "assets").rglob("*") if p.is_file()}


def run_check(project: Path, *args: str) -> tuple[int, dict]:
    cmd = harness.editor_command() + ["check", "--project", str(project), *args, "--format", "json"]
    r = subprocess.run(cmd, cwd=str(project), capture_output=True, text=True, timeout=120)
    try:
        return r.returncode, json.loads(r.stdout)
    except json.JSONDecodeError:
        fail(f"check printed no JSON (exit {r.returncode}): {r.stdout[-1500:]} {r.stderr[-1500:]}")
        return 0, {}


BASE = {"examples": [ALBEDO], "mode": "tileable", "size": 64, "quality": "draft", "seed": 7}


async def synth(ed: Editor, **args):
    r = await ed.call_raw("synthesize_texture", {**BASE, **args}, timeout=300)
    if r.is_error:
        fail(f"synthesize_texture failed: {r.text}")
    return r


# ---------------------------------------------------------------------------- tests

async def test_setup_and_schema(ed: Editor):
    write_textures(ed.project)
    ed.require_tools("synthesize_texture")
    tool = ed.tool("synthesize_texture")
    schema = tool.input_schema
    # additionalProperties false is enforced by the registry (and left out of tools/list)
    msg = await ed.call("synthesize_texture", {**BASE, "sise": 64}, expect_error=True)
    check("sise" in msg, f"an unknown argument is refused: {msg}")
    check(schema.get("required") == ["examples"], f"examples required: {schema.get('required')}")
    props = schema["properties"]
    check(props["mode"]["enum"] == ["extend", "tileable", "make_tileable", "inpaint", "guided"], f"mode enum: {props['mode']}")
    check(props["quality"]["default"] == "balanced", "quality default")
    # params is a free object (animation clips spec 9.R28: its fields and ranges are documented in
    # jasper://docs/texture-synthesis and checked by the handler, test_argument_errors)
    check(props["params"]["type"] == "object" and "jasper://docs/texture-synthesis" in props["params"].get("description", ""),
          f"params points to the guide: {props['params']}")
    msg = await ed.call("synthesize_texture", {**BASE, "params": {"nearest_neighbors": 257}}, expect_error=True)
    check("nearest_neighbors" in msg and "256" in msg, f"params ranges are checked: {msg}")
    check("jasper://docs/texture-synthesis" in tool.description, "the description points to the guide")
    annotations = tool.annotations
    check(annotations is None or not annotations.read_only_hint, "writes: not read-only")


async def test_argument_errors(ed: Editor):
    cases = [
        ({"examples": []}, "examples is required"),
        ({"examples": ["../outside.png"]}, "inside the project"),
        ({"examples": ["assets/textures/nope.png"]}, "does not exist"),
        ({"mode": "wobble"}, "mode must be"),
        ({"mode": "make_tileable", "size": 128}, "leave size out"),
        ({"mask": {"path": ALBEDO}}, "only used in inpaint"),
        ({"params": {"k": 3}}, "unknown key \"k\""),
        ({"params": {"nearest_neighbors": 999}}, "params.nearest_neighbors must be"),
        ({"border": 0.2}, "only used in make_tileable"),
        ({"output": "../rock_tile.png"}, "inside the project"),
        ({"output": f"{TEX}/rock_tile.jpg"}, ".png"),
        ({"output": ALBEDO, "overwrite": True}, "replace an input"),
        ({"companions": "sometimes"}, "companions must be"),
        ({"companions": [f"{TEX}/rock_ao.png"]}, "must match exactly"),
        ({"mode": "guided"}, "target guide"),
        ({"seed": -1}, "seed must be"),
    ]
    for extra, needle in cases:
        msg = await ed.call("synthesize_texture", {**BASE, **extra}, expect_error=True)
        check(needle in msg, f"{extra}: the error says {needle!r}: {msg}")
    check(files_under(ed.project) == {f"{TEX}/{n}" for n in ("rock_albedo.png", "rock_normal.png", "rock_height.png", "rock_ao.png", "grass_albedo.png")},
          f"errors write nothing: {files_under(ed.project)}")


async def test_preview_writes_nothing(ed: Editor):
    before = files_under(ed.project)
    r = await synth(ed)
    v = r.structured
    check(files_under(ed.project) == before, "a preview writes nothing")
    check(v["written"] is False and v["outputs"] == [], f"preview: {v}")
    check(v["seed"] == 7 and v["mode"] == "tileable" and v["quality"] == "draft", f"echoes: {v}")
    check((v["width"], v["height"]) == (64, 64), "size")
    check({c["name"] for c in v["companions"]} == {"normal", "height"}, f"auto companions: {v['companions']}")
    check(any("rock_ao.png" in w and "32x32" in w for w in v["warnings"]), f"the mis-sized ao map is left out with a warning: {v['warnings']}")
    check("seam_score" in v["metrics"] and "high_uncertainty_pct" in v["metrics"], f"metrics: {v['metrics']}")
    check(isinstance(v["hint"], str) and v["hint"], "a hint")
    check(len(r.images) >= 2, f"the result and a 2x2 tiling: {len(r.images)} images")
    w, h, _, _ = decode_png(r.images[0][0])
    check((w, h) == (64, 64), "the result image at its size (<= 512)")
    w2, h2, _, _ = decode_png(r.images[1][0])
    check((w2, h2) == (128, 128), f"the 2x2 tiling: {w2}x{h2}")
    check("Preview only" in r.text and "seed 7" in r.text, f"text says how to write it: {r.text}")
    # no seed: a random one, returned
    r2 = await ed.call_raw("synthesize_texture", {"examples": [ALBEDO], "size": 32, "quality": "draft", "companions": "none"})
    check(not r2.is_error and isinstance(r2.structured["seed"], int), f"random seed returned: {r2.text}")
    # deterministic: the same seed twice gives the same image
    r3 = await synth(ed)
    check(r3.images[0][0] == r.images[0][0], "same arguments and seed: same pixels")
    S["preview"] = r.images[0][0]


S: dict = {}


async def test_write_matches_preview(ed: Editor):
    out = f"{TEX}/rock_tile.png"
    r = await synth(ed, output=out)
    v = r.structured
    expected = [out, f"{TEX}/rock_tile_normal.png", f"{TEX}/rock_tile_height.png"]
    check(sorted(v["outputs"]) == sorted(expected), f"outputs: {v['outputs']}")
    for rel in expected:
        check((ed.project / rel).is_file(), f"{rel} written")
    roles = {rel: recipe(ed.project / rel)["role"] for rel in expected}
    check(roles == {out: "primary", expected[1]: "normal", expected[2]: "height"}, f"recipe roles: {roles}")
    rec = recipe(ed.project / out)
    check(rec["mode"] == "tileable" and rec["seed"] == 7 and rec["size"] == [64, 64], f"recipe: {rec}")
    check(rec["examples"][0]["path"] == ALBEDO and len(rec["examples"][0]["sha1"]) == 40, f"recipe sources: {rec['examples']}")
    check(ihdr(ed.project / expected[2])[2] == 16, "the height companion stays 16-bit")
    written = rgb_pixels((ed.project / out).read_bytes())
    previewed = rgb_pixels(S["preview"])
    check(written == previewed, "the written texture is exactly the previewed one (same seed)")


async def test_overwrite_refused(ed: Editor):
    out = f"{TEX}/rock_tile.png"
    before = (ed.project / out).read_bytes()
    msg = await ed.call("synthesize_texture", {**BASE, "output": out, "seed": 8}, expect_error=True)
    check("overwrite: true" in msg and "rock_tile.png" in msg, f"refusal says how: {msg}")
    check((ed.project / out).read_bytes() == before, "kept")
    await synth(ed, output=out, seed=8, overwrite=True)
    check((ed.project / out).read_bytes() != before, "overwrite replaces it")
    check(recipe(ed.project / out)["seed"] == 8, "new recipe")
    # one undo step puts the previous set back
    await ed.call("undo", scope="project")
    check((ed.project / out).read_bytes() == before, "undo restores the replaced file")


async def test_undo_removes_set(ed: Editor):
    out = f"{TEX}/undo/rock_u.png"
    r = await synth(ed, output=out, seed=11)
    paths = r.structured["outputs"]
    check(len(paths) == 3 and all((ed.project / p).is_file() for p in paths), f"three files: {paths}")
    await ed.call("undo", scope="project")
    check(not any((ed.project / p).exists() for p in paths), "one undo removes the whole set")
    check(not (ed.project / f"{TEX}/undo").exists(), "and the folder it created")
    await ed.call("redo", scope="project")
    check(all((ed.project / p).is_file() for p in paths), "redo writes them again")


async def test_job_and_cancel(ed: Editor):
    r = await ed.call_raw("synthesize_texture", {"examples": [ALBEDO], "size": 768, "quality": "high", "seed": 3,
                                                 "companions": "none", "timeout_seconds": 1})
    check(not r.is_error, r.text)
    v = r.structured
    check(v.get("status") == "running" and v.get("job_id"), f"a long run returns a job: {v}")
    job = v["job_id"]
    for _ in range(600):
        state = await ed.call("get_job", job_id=job)
        if state["status"] != "running":
            break
        await asyncio.sleep(0.5)
    check(state["status"] == "succeeded", f"the job finishes: {state}")
    check(state["result"]["width"] == 768 and "metrics" in state["result"], f"its result: {state['result']}")

    out = f"{TEX}/cancelled.png"
    r = await ed.call_raw("synthesize_texture", {"examples": [ALBEDO], "size": 1024, "quality": "high", "seed": 4,
                                                 "companions": "none", "output": out, "wait": False})
    job = r.structured["job_id"]
    cancel = await ed.call("cancel_job", job_id=job)
    check("ancel" in json.dumps(cancel), f"cancel_job: {cancel}")
    for _ in range(100):
        state = await ed.call("get_job", job_id=job)
        if state["status"] != "running":
            break
        await asyncio.sleep(0.2)
    check(state["status"] == "cancelled", f"cancelled: {state}")
    check(not (ed.project / out).exists(), "nothing written")


async def test_inpaint_and_make_tileable(ed: Editor):
    # a white square in the middle: regenerated; the rest kept
    rows = [bytes(255 if 24 <= x < 40 and 24 <= y < 40 else 0 for x in range(64)) for y in range(64)]
    (ed.project / TEX / "hole_mask.png").write_bytes(png_bytes(64, 64, 1, rows))
    out = f"{TEX}/rock_inpainted.png"
    r = await ed.call_raw("synthesize_texture", {"examples": [ALBEDO], "mode": "inpaint", "mask": {"path": f"{TEX}/hole_mask.png"},
                                                 "quality": "draft", "seed": 5, "companions": "none", "output": out})
    check(not r.is_error, r.text)
    src = rgb_pixels((ed.project / ALBEDO).read_bytes())
    got = rgb_pixels((ed.project / out).read_bytes())
    outside = [i for i in range(64 * 64) if not (24 <= i % 64 < 40 and 24 <= i // 64 < 40)]
    check(all(src[i] == got[i] for i in outside), "inpaint keeps the unmasked pixels")
    check(recipe(ed.project / out)["inpaintMask"]["path"] == f"{TEX}/hole_mask.png", "the mask is in the recipe")
    r = await ed.call_raw("synthesize_texture", {"examples": [ALBEDO], "mode": "make_tileable", "border": 0.25, "quality": "draft",
                                                 "seed": 5, "companions": "none"})
    check(not r.is_error, r.text)
    v = r.structured
    check((v["width"], v["height"]) == (64, 64) and "seam_score" in v["metrics"], f"make_tileable keeps the size: {v}")
    check(abs(v["params"]["border"] - 0.25) < 1e-6, f"border used: {v['params']}")


async def test_asset_info_shows_recipe(ed: Editor):
    out = f"{TEX}/rock_tile.png"
    info = await ed.call("get_asset_info", path=out)
    syn = info.get("synthesis")
    check(syn and syn["mode"] == "tileable" and syn["role"] == "primary" and syn["seed"] == 7, f"recipe summary: {syn}")
    check(syn["sources_changed"] == 0 and syn["sources_missing"] == 0, f"sources unchanged: {syn['sources']}")
    check({s["path"] for s in syn["sources"]} >= {ALBEDO, f"{TEX}/rock_normal.png", f"{TEX}/rock_height.png"}, f"sources: {syn['sources']}")
    regen = syn["regenerate"]
    check(regen["seed"] == 7 and regen["output"] == out and regen["overwrite"] is True and regen["mode"] == "tileable", f"regenerate: {regen}")
    # the regenerate arguments reproduce the file byte for byte
    before = {p: (ed.project / p).read_bytes() for p in (out, f"{TEX}/rock_tile_normal.png")}
    r = await ed.call_raw("synthesize_texture", regen, timeout=300)
    check(not r.is_error, r.text)
    for p, data in before.items():
        check(rgb_pixels((ed.project / p).read_bytes()) == rgb_pixels(data), f"regenerate reproduces {p}")
    view = await ed.call_raw("view_asset", {"path": out})
    check(view.structured["synthesis"]["mode"] == "tileable" and "Synthesized" in view.text, f"view_asset: {view.text}")
    plain = await ed.call("get_asset_info", path=ALBEDO)
    check("synthesis" not in plain, "a plain PNG has no synthesis entry")


async def test_check_reports_changed_sources(ed: Editor):
    code, data = run_check(ed.project, "--textures")
    check(code == 0 and data["findings"] == [], f"clean before: {data}")
    check(data["checked"].get("textures", 0) >= 3, f"recipes checked (companions covered by their primary): {data['checked']}")
    # an example edited after the synthesis
    (ed.project / ALBEDO).write_bytes(png_bytes(64, 64, 3, noise_rows(64, 64, 99)))
    code, data = run_check(ed.project, "--textures")
    changed = [f for f in data["findings"] if f["code"] == "synthesis-source-changed"]
    check(any(f["path"] == f"{TEX}/rock_tile.png" and ALBEDO in f["message"] for f in changed), f"changed source reported: {data}")
    check(all(f["severity"] == "warning" and f["tool"] == "texture" and "Edit Synthesis" in f["suggestion"] for f in changed), f"{changed}")
    check(not any(f["path"] == f"{TEX}/rock_tile_normal.png" for f in changed), "companions are covered by their primary")
    check(code == 0, "warnings only: exit 0")
    # a source removed
    (ed.project / TEX / "rock_height.png").unlink()
    code, data = run_check(ed.project)
    check(any(f["code"] == "synthesis-source-missing" and "rock_height.png" in f["message"] for f in data["findings"]),
          f"missing source reported by the default check: {data['findings']}")
    # --files with the edited example: the textures made from it
    code, data = run_check(ed.project, "--files", ALBEDO)
    check(any(f["path"] == f"{TEX}/rock_tile.png" for f in data["findings"]), f"--files with a source: {data}")
    info = await ed.call("get_asset_info", path=f"{TEX}/rock_tile.png")
    check(info["synthesis"]["sources_changed"] >= 1 and info["synthesis"]["sources_missing"] == 1, f"get_asset_info agrees: {info['synthesis']}")
    # restore the sources for the next tests
    (ed.project / ALBEDO).write_bytes(png_bytes(64, 64, 3, noise_rows(64, 64, 1)))
    (ed.project / TEX / "rock_height.png").write_bytes(png_bytes(64, 64, 1, gray16_rows(64, 64), depth=16))


async def test_window_show_in_editor(ed: Editor):
    focus = await ed.call("focus_window", window="texture_synthesis")
    check(focus["shown"], f"the window opens: {focus}")
    out = f"{TEX}/window/rock_window.png"
    r = await ed.call_raw("synthesize_texture", {**BASE, "size": 256, "seed": 21, "output": out, "show_in_editor": True}, timeout=300)
    check(not r.is_error, r.text)
    v = r.structured
    check(out in v["outputs"] and len(v["outputs"]) == 3, f"saved through the window: {v['outputs']}")
    check(recipe(ed.project / out)["seed"] == 21, "the window's save writes the recipe")
    # same pixels as a plain run with that seed
    r2 = await ed.call_raw("synthesize_texture", {**BASE, "size": 256, "seed": 21})
    check(rgb_pixels(r2.images[0][0]) == rgb_pixels((ed.project / out).read_bytes()), "the window runs the same synthesis")
    await asyncio.sleep(0.5)
    # saved as <scratch>/editor.png (run with --keep to look at it)
    shot = await ed.screenshot("editor", max_width=1600)
    w, h, _, _ = decode_png(shot)
    check(w > 600 and h > 400, f"an editor screenshot: {w}x{h}")


# ---------------------------------------------------------------------------- review fixes

def latin1(text: str) -> str:
    """A file name with raw Latin-1 bytes (not UTF-8), as a str the OS gets back byte for byte"""
    return os.fsdecode(text.encode("latin-1"))


def with_recipe_text(png: bytes, text: bytes) -> bytes:
    data = b"jasper.synthesis\x00\x00\x00\x00\x00" + text
    chunk = struct.pack(">I", len(data)) + b"iTXt" + data + struct.pack(">I", zlib.crc32(b"iTXt" + data) & 0xFFFFFFFF)
    return png[:33] + chunk + png[33:]


async def test_garbled_recipe_and_non_utf8_names(ed: Editor):
    tex = ed.project / TEX
    plain = (tex / "grass_albedo.png").read_bytes()
    garbled = f"{TEX}/garbled.png"
    (ed.project / garbled).write_bytes(with_recipe_text(plain, json.dumps(
        {"format": 1, "mode": "tileable", "role": "primary", "pathBase": "project", "examples": [{"path": 5}]}).encode()))
    info = await ed.call("get_asset_info", path=garbled)
    syn = info.get("synthesis")
    check(syn and "examples[0].path is a number" in syn.get("error", ""), f"a garbled recipe is reported, the asset info works: {info}")
    check(info.get("width") == 48, f"the rest of the asset info is there: {info}")
    view = await ed.call_raw("view_asset", {"path": garbled})
    check(not view.is_error and "error" in view.structured["synthesis"] and "can't be used" in view.text, f"view_asset: {view.text}")
    code, data = run_check(ed.project, "--textures")
    check(any(f["path"] == garbled and f["code"] == "synthesis-recipe-invalid" for f in data["findings"]), f"check: {data['findings']}")
    (ed.project / garbled).unlink()

    # a companion next to the example whose name is not UTF-8: left out of auto, with a warning
    odd = tex / latin1("rock_normal_\xe9.png")
    odd.write_bytes((tex / "rock_normal.png").read_bytes())
    r = await synth(ed)
    check(any("not valid UTF-8" in w for w in r.structured["warnings"]), f"warning: {r.structured['warnings']}")
    check({c["name"] for c in r.structured["companions"]} == {"normal", "height"}, f"companions: {r.structured['companions']}")
    code, data = run_check(ed.project, "--textures")
    check(code == 0, f"check with a Latin-1 PNG name: exit {code}")
    odd.unlink()


async def test_failed_write_changes_nothing(ed: Editor):
    out = f"{TEX}/atomic/rock_a.png"
    await synth(ed, output=out, seed=1, companions="none")
    before = (ed.project / out).read_bytes()
    (ed.project / TEX / "atomic" / "blocker").write_text("a file where a directory is expected")
    msg = await ed.call("synthesize_texture", {**BASE, "output": out, "seed": 2, "companions": "none", "overwrite": True,
                                               "uncertainty_output": f"{TEX}/atomic/blocker/u.png"}, expect_error=True)
    check("blocker/u.png" in msg and "nothing was changed" in msg, f"the error names the file: {msg}")
    check((ed.project / out).read_bytes() == before, "the replaced primary got its content back")
    check(recipe(ed.project / out)["seed"] == 1, "its recipe too")


async def test_overwrite_race_and_outputs_in_assets(ed: Editor):
    out = f"{TEX}/race/rock_r.png"
    jobs = []
    for seed in (31, 32):
        r = await ed.call_raw("synthesize_texture", {**BASE, "output": out, "seed": seed, "companions": "none", "size": 256, "wait": False})
        check(not r.is_error, r.text)
        jobs.append(r.structured["job_id"])
    states = []
    for job in jobs:
        for _ in range(300):
            state = await ed.call("get_job", job_id=job)
            if state["status"] != "running":
                break
            await asyncio.sleep(0.2)
        states.append(state)
    statuses = sorted(s["status"] for s in states)
    check(statuses == ["failed", "succeeded"], f"one writes, the other is refused at write time: {states}")
    failed = next(s for s in states if s["status"] == "failed")
    check("overwrite: true" in json.dumps(failed), f"the refusal says how to fix it: {failed}")
    winner = next(s for s in states if s["status"] == "succeeded")
    check(recipe(ed.project / out)["seed"] == winner["result"]["seed"], "the file is the winner's, not silently replaced")

    # the companions' and the uncertainty map's outputs follow output's rule (the editor's .jasperengine/
    # is refused whether the assets directory is the project root, as here, or below it)
    for extra in [
        {"companions": [{"path": f"{TEX}/rock_normal.png", "output": ".jasperengine/n.png"}]},
        {"uncertainty_output": ".jasperengine/u.png"},
        {"uncertainty_output": "assets/.hidden/u.png"},
        {"companions": [{"path": f"{TEX}/rock_normal.png", "output": "assets/.cache/n.png"}]},
    ]:
        msg = await ed.call("synthesize_texture", {**BASE, "output": f"{TEX}/where.png", **extra}, expect_error=True)
        check("hidden directory" in msg or "outside the assets directory" in msg, f"{extra}: {msg}")
    check(not (ed.project / ".jasperengine" / "n.png").exists() and not (ed.project / ".jasperengine" / "u.png").exists(), "nothing written there")


async def test_seeds_beyond_2_53_and_files_mode_chains(ed: Editor):
    # a CLI recipe with a seed above 2^53: its regenerate arguments are accepted (a decimal string)
    out = f"{TEX}/bigseed.png"
    cmd = harness.editor_command() + ["synthesize-texture", "--project", str(ed.project), "--example", str(ed.project / ALBEDO),
                                      "--out", str(ed.project / out), "--size", "64", "--quality", "draft", "--seed", "18446744073709551615", "-q"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    check(r.returncode == 0, f"CLI: {r.stderr}")
    info = await ed.call("get_asset_info", path=out)
    regen = info["synthesis"]["regenerate"]
    check(regen["seed"] == "18446744073709551615", f"regenerate seed: {regen}")
    before = rgb_pixels((ed.project / out).read_bytes())
    r = await ed.call_raw("synthesize_texture", regen, timeout=300)
    check(not r.is_error, f"the regenerate arguments are accepted: {r.text}")
    check(r.structured["seed"] == "18446744073709551615", f"echoed losslessly: {r.structured['seed']}")
    check(rgb_pixels((ed.project / out).read_bytes()) == before, "and reproduce the file")
    msg = await ed.call("synthesize_texture", {**BASE, "seed": "18446744073709551616"}, expect_error=True)
    check("seed must be" in msg, msg)

    # check --files: a synthesized PNG that is itself the source of another one reports both roles
    first, second = f"{TEX}/chain/first.png", f"{TEX}/chain/second.png"
    await synth(ed, output=first, seed=41, companions="none")
    await synth(ed, examples=[first], output=second, seed=42, companions="none")
    data = (ed.project / first).read_bytes()
    (ed.project / first).write_bytes(data + b"\x00")   # changed bytes, same image
    code, result = run_check(ed.project, "--files", first)
    check(any(f["path"] == second and f["code"] == "synthesis-source-changed" for f in result["findings"]),
          f"the texture made from the changed synthesized PNG is reported: {result['findings']}")


# ---------------------------------------------------------------------------- last: switches projects

async def test_project_switch_during_a_run(ed: Editor):
    original = ed.project.resolve()
    other = ed.scratch.root / "switched" / "Other"
    for show in (False, True):
        out = f"{TEX}/switch_{int(show)}.png"
        r = await ed.call_raw("synthesize_texture", {"examples": [ALBEDO], "size": 2048, "quality": "high", "seed": 13, "companions": "none",
                                                     "output": out, "wait": False, "show_in_editor": show})
        check(not r.is_error, r.text)
        job = r.structured["job_id"]
        await asyncio.sleep(0.3)
        if not other.exists():
            await ed.call("create_project", path=str(other), name="Other", unsaved="discard")
        else:
            await ed.call("open_project", path=str(other), unsaved="discard")
        for _ in range(200):
            state = await ed.call("get_job", job_id=job)
            if state["status"] != "running":
                break
            await asyncio.sleep(0.2)
        check(state["status"] == "failed" and "project changed" in json.dumps(state), f"show_in_editor {show}: the job says why: {state}")
        check(not (original / out).exists() and not (other / out).exists(), f"show_in_editor {show}: nothing written in either project")
        await ed.call("open_project", path=str(original), unsaved="discard")
    # back in the original project, a synthesis works as before
    r = await ed.call_raw("synthesize_texture", {**BASE, "companions": "none"})
    check(not r.is_error, r.text)
