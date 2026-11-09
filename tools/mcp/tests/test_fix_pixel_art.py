"""fix_pixel_art (docs/specs/S46-pixel-art-cleanup.md R28-R32, R44-R47, AC11's MCP part and AC12; editor/mcp/tools/
PixelArtCleanupTools.cpp), on a new project with an AI sprite made here (fake pixels, slightly noisy) and the committed
photo fixture:

- a preview writes nothing, returns one image and default_output; the same arguments plus output: true write
  <stem>_clean.png whose decoded pixels are the preview's (result_sha256); its jasper.pixelcleanup chunk decodes to the
  resolved recipe with the source's SHA-256;
- one project undo step named "Pixel Art Cleanup (hero_clean.png)", named by the result's undo; undo removes the file;
- rerun gives byte-identical bytes; editing the source flips stale; a re-run over an edited output fails output_edited;
- an unknown recipe key fails bad_recipe naming the valid keys; output_exists; a needs_person write without force
  fails and writes nothing (directory listing unchanged), with force it is written and recorded as forced;
- a cancelled job writes nothing;
- apply on a Sprite sets its texture as a second, scene undo step.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import random
import shutil
import struct
import zlib
from pathlib import Path

import yaml

import pngdiff
from harness import REPO, Editor

CI = True
TIMEOUT = 600
EDITOR = {"create": True, "name": "FixPixelArt"}

ART = "assets/art"
HERO = f"{ART}/hero.png"
HERO_CLEAN = f"{ART}/hero_clean.png"
PHOTO_FIXTURE = REPO / "editor" / "tests" / "fixtures" / "pixelart" / "cleanup" / "photo.jpg"
PHOTO = f"{ART}/photo.jpg"


def sprite_rgba(size: int = 16) -> list[list[tuple[int, int, int, int]]]:
    """A sprite with an outline, a body and a highlight: flat cells a grid can be found in."""
    palette = [(26, 28, 44, 255), (239, 125, 87, 255), (255, 205, 117, 255), (41, 54, 111, 255)]
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            edge = x in (2, size - 3) or y in (2, size - 3)
            inside = 2 < x < size - 3 and 2 < y < size - 3
            if edge and 2 <= x <= size - 3 and 2 <= y <= size - 3:
                row.append(palette[0])
            elif inside:
                row.append(palette[1] if (x + y) % 5 else palette[2] if y < size // 2 else palette[3])
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows


def ai_sprite(scale: int = 8, noise: int = 3, seed: int = 11) -> bytes:
    rng = random.Random(seed)
    rows = sprite_rgba()
    size = len(rows)
    rgba = bytearray()
    for y in range(size * scale):
        for x in range(size * scale):
            r, g, b, a = rows[y // scale][x // scale]
            if a == 0:
                rgba += bytes((0, 0, 0, 0))
            else:
                rgba += bytes((min(255, max(0, r + rng.randint(-noise, noise))), min(255, max(0, g + rng.randint(-noise, noise))),
                               min(255, max(0, b + rng.randint(-noise, noise))), 255))
    return pngdiff.encode(size * scale, size * scale, bytes(rgba))


def chunk_text(png: bytes, keyword: str) -> str | None:
    at = 8
    while at < len(png):
        length, kind = struct.unpack(">I4s", png[at:at + 8])
        body = png[at + 8:at + 8 + length]
        if kind == b"iTXt":
            nul = body.find(b"\0")
            if body[:nul].decode() == keyword:
                # keyword\0 compression flag, method, language\0 translated\0 text
                rest = body[nul + 3:]
                rest = rest[rest.find(b"\0") + 1:]
                rest = rest[rest.find(b"\0") + 1:]
                return rest.decode()
        at += 12 + length
    return None


def pixels_sha(png: bytes) -> str:
    _, _, rgba = pngdiff.decode(png)
    return hashlib.sha256(bytes(rgba)).hexdigest()


def files_in(project: Path, folder: str = ART) -> set[str]:
    return {p.name for p in (project / folder).iterdir() if p.is_file() and not p.name.endswith(".meta")}


def write_hero(ed: Editor) -> None:
    (ed.project / ART).mkdir(parents=True, exist_ok=True)
    (ed.project / HERO).write_bytes(ai_sprite())


async def enable(ed: Editor) -> None:
    await ed.call("tool_groups", enable=["pixel_art"])


async def test_preview_then_write_then_rerun(ed: Editor):
    write_hero(ed)
    await enable(ed)
    before = files_in(ed.project)
    r = await ed.call_raw("fix_pixel_art", {"source": HERO})
    ed.check(not r.is_error, f"preview: {r.text[:300]}")
    v = r.structured
    ed.check_eq(files_in(ed.project), before, "a preview writes nothing")
    ed.check_eq(len(r.images), 1, "a preview returns one image")
    ed.check_eq(v["default_output"], HERO_CLEAN)
    ed.check_eq(v["verdict"], "fixable", f"{v['summary']}")
    ed.check("output: true" in v["next"], f"the next step: {v['next']}")
    grid = v["grid"]
    ed.check(abs(grid["cell_width"] - 8) <= 0.5 and abs(grid["cell_height"] - 8) <= 0.5, f"grid: {grid}")
    ed.check_eq(v["output"]["width"], 16, f"output size {v['output']}")
    recipe = v["recipe"]

    written = await ed.call("fix_pixel_art", {"source": HERO, "output": True})
    ed.check_eq(written["default_output"], HERO_CLEAN)
    ed.check("undo" in written and written["undo"]["name"].endswith(f"Pixel Art Cleanup (hero_clean.png)"), f"undo: {written.get('undo')}")
    png = (ed.project / HERO_CLEAN).read_bytes()
    ed.check_eq(pixels_sha(png), recipe["result_sha256"], "the written pixels are the previewed pixels")
    text = chunk_text(png, "jasper.pixelcleanup")
    ed.check(text is not None, "the output carries its recipe chunk")
    chunk = json.loads(text)
    ed.check_eq(chunk["source"]["sha256"], hashlib.sha256((ed.project / HERO).read_bytes()).hexdigest(), "the chunk records the source's SHA-256")
    ed.check_eq(chunk["grid"], recipe["grid"], "the chunk holds the resolved grid")
    entries = (await ed.call("get_undo_history", scope="project", limit=20))["entries"]
    ed.check(any("Pixel Art Cleanup (hero_clean.png)" in e["name"] for e in entries), f"undo history: {[e['name'] for e in entries][:5]}")

    # rerun: byte-identical (R41), and the output keeps its own path
    rerun = await ed.call("fix_pixel_art", {"source": HERO_CLEAN, "rerun": True, "output": True})
    ed.check_eq((ed.project / HERO_CLEAN).read_bytes(), png, "rerun writes the same bytes")
    ed.check_eq(rerun["stale"]["source_changed"], False)

    # the source changed: the result says stale; a preview still runs (R32)
    (ed.project / HERO).write_bytes(ai_sprite(seed=99))
    stale = await ed.call("fix_pixel_art", {"source": HERO_CLEAN, "rerun": True})
    ed.check_eq(stale["stale"]["source_changed"], True, "editing the source flips stale")

    # a re-run over an edited output fails, and writes nothing
    edited = bytearray(pngdiff.decode(png)[2])
    edited[0:4] = bytes((1, 2, 3, 255))
    new_png = pngdiff.encode(16, 16, bytes(edited))
    # keep the recipe chunk, so only the pixels differ
    chunk_data = json.dumps(chunk, separators=(",", ":")).encode()
    payload = b"jasper.pixelcleanup\0\0\0\0\0" + chunk_data
    ihdr_end = 8 + 12 + struct.unpack(">I", new_png[8:12])[0]
    itxt = struct.pack(">I", len(payload)) + b"iTXt" + payload + struct.pack(">I", zlib.crc32(b"iTXt" + payload) & 0xffffffff)
    (ed.project / HERO_CLEAN).write_bytes(new_png[:ihdr_end] + itxt + new_png[ihdr_end:])
    message = await ed.check_error("fix_pixel_art", {"source": HERO_CLEAN, "rerun": True, "output": True}, contains="output_edited")
    ed.check("force" in message, f"the fix offers force: {message[:300]}")

    # one undo removes the first write's step (the file is gone again once back past it): undo the
    # two writes of this test in order
    await ed.call("undo", scope="project")
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / HERO_CLEAN).exists(), "undo removes the written file")


async def test_bad_arguments_and_existing_output(ed: Editor):
    write_hero(ed)
    await enable(ed)
    message = await ed.check_error("fix_pixel_art", {"source": HERO, "recipe": {"pallete": "sweetie-16"}}, contains="bad_recipe")
    ed.check("palette" in message, f"the error names the valid keys: {message[:300]}")
    await ed.check_error("fix_pixel_art", {"source": "assets/art/nope.png"}, contains="source_missing")
    await ed.check_error("fix_pixel_art", {"source": HERO, "palette": "assets/none.palette"}, contains="palette_missing")

    await ed.call("fix_pixel_art", {"source": HERO, "output": True})
    await ed.check_error("fix_pixel_art", {"source": HERO, "output": True}, contains="output_exists")
    await ed.call("fix_pixel_art", {"source": HERO, "output": True, "overwrite": True})
    await ed.check_error("fix_pixel_art", {"source": HERO, "output": HERO, "overwrite": True}, contains="output_is_source")
    await ed.check_error("fix_pixel_art", {"source": HERO, "output": "../outside.png"}, contains="output_outside_assets")


async def test_needs_person_is_refused_then_forced(ed: Editor):
    (ed.project / ART).mkdir(parents=True, exist_ok=True)
    shutil.copy(PHOTO_FIXTURE, ed.project / PHOTO)
    await enable(ed)
    preview = await ed.call("fix_pixel_art", {"source": PHOTO})
    ed.check_eq(preview["verdict"], "needs_person", preview["summary"])
    ids = {g["id"] for g in preview["guards"]}
    ed.check(ids & {"no_grid", "low_fit", "low_confidence"}, f"guards: {ids}")

    before = files_in(ed.project)
    message = await ed.check_error("fix_pixel_art", {"source": PHOTO, "output": True}, contains="needs_person")
    ed.check("force" in message, f"the fix names force: {message[:300]}")
    ed.check_eq(files_in(ed.project), before, "a refused write leaves the directory as it was")

    if preview.get("output", {}).get("width", 0) > 0:
        forced = await ed.call("fix_pixel_art", {"source": PHOTO, "output": True, "force": True})
        path = ed.project / forced["default_output"]
        ed.check(path.exists(), "force writes it")
        chunk = json.loads(chunk_text(path.read_bytes(), "jasper.pixelcleanup"))
        ed.check_eq(chunk["forced"], True, "the recipe records forced")
        lint = await ed.call("lint_pixel_art", {"source": forced["default_output"]})
        ed.check_eq(lint.get("forced"), True, "lint_pixel_art reports forced on it")


async def test_cancelled_job_writes_nothing(ed: Editor):
    (ed.project / ART).mkdir(parents=True, exist_ok=True)
    width = height = 1500
    rng = random.Random(5)
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            v = (x // 11 + y // 11) % 7 * 30 + rng.randint(0, 2)
            rows += bytes((v, 255 - v, v // 2, 255))
    data = zlib.compress(bytes(rows), 1)
    png = b"\x89PNG\r\n\x1a\n"
    for kind, body in ((b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)), (b"IDAT", data), (b"IEND", b"")):
        png += struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    big = f"{ART}/big.png"
    (ed.project / big).write_bytes(png)
    await enable(ed)
    before = files_in(ed.project)
    started = await ed.call("fix_pixel_art", {"source": big, "output": True, "timeout_seconds": 1})
    ed.check_eq(started.get("status"), "running", f"past 1 s it is a job: {str(started)[:200]}")
    await ed.call("cancel_job", job_id=started["job_id"])
    for _ in range(600):
        job = await ed.call("get_job", job_id=started["job_id"])
        if job["status"] != "running":
            break
        await asyncio.sleep(0.2)
    ed.check_eq(job["status"], "cancelled", str(job)[:300])
    ed.check_eq(files_in(ed.project), before, "a cancelled run writes nothing")


async def test_apply_sets_the_sprite(ed: Editor):
    write_hero(ed)
    await enable(ed)
    await ed.call("create_entity", name="HeroSprite", type="sprite")
    r = await ed.call("fix_pixel_art", {"source": HERO, "output": {"png": f"{ART}/hero_applied.png"}, "apply": "HeroSprite"})
    ed.check("applied" in r, f"applied: {str(r)[:300]}")
    props = await ed.call("get_components", entity="HeroSprite", components=["mesh"])
    text = json.dumps(props)
    ed.check("hero_applied.png" in text, f"the Sprite's texture is the output: {text[:300]}")
    entries = (await ed.call("get_undo_history", scope="timeline", limit=5))["entries"]
    names = [e["name"] for e in entries]
    ed.check(any("Cleaned Sprite" in n for n in names), f"a scene step: {names}")
    ed.check(any("Pixel Art Cleanup" in n for n in names), f"the file step: {names}")
    await ed.check_error("fix_pixel_art", {"source": HERO, "apply": "HeroSprite"}, contains="output")


async def test_import_step(ed: Editor):
    """AC13 (R33, D8): import.pixelCleanup in a raster texture's .meta cooks the fixed pixels at their size; a
    changed source whose verdict is needs_person cooks the raw source and warns; the export carries no recipe chunk."""
    step_source = f"{ART}/step.png"
    (ed.project / ART).mkdir(parents=True, exist_ok=True)
    (ed.project / step_source).write_bytes(ai_sprite())
    await ed.call("tool_groups", enable=["pixel_art", "assets_pipeline"])
    await ed.call("sync_files", paths=[step_source], full=False)
    result = await ed.call("fix_pixel_art", source=step_source, output=True)
    output = f"{ART}/step_clean.png"
    written = (ed.project / output).read_bytes()
    recipe = chunk_text(written, "jasper.pixelcleanup")
    ed.check(recipe is not None, "the written output carries its recipe")
    fixed_width, fixed_height, fixed_pixels = pngdiff.decode(written)

    await ed.call("set_texture_import_settings", paths=[step_source], settings={"quality": "none", "mipmaps": False})
    meta_path = ed.project / f"{step_source}.meta"
    meta = yaml.safe_load(meta_path.read_text())
    meta.setdefault("import", {})["pixelCleanup"] = {"enabled": True, "recipe": recipe}
    meta_path.write_text(yaml.safe_dump(meta))
    await ed.call("sync_files", paths=[f"{step_source}.meta"], full=False)

    async def cooked() -> dict:
        report = await ed.call("reimport_textures", paths=[step_source], wait=True, timeout_s=300)
        row = report["textures"][0]["platforms"]["desktop"]
        ed.check_eq(row["status"], "cooked", f"{row}")
        return row

    row = await cooked()
    ed.check_eq(row["logical_width"], fixed_width, f"the cooked size is the fixed size: {row}")
    ed.check_eq(row["logical_height"], fixed_height, f"{row}")
    preview = await ed.call_raw("get_texture_cook_report", {"paths": [step_source], "platforms": ["desktop"], "include_preview": True})
    ed.check(not preview.is_error, preview.text)
    ed.check(preview.images, "a preview of the cooked pixels")
    cooked_width, cooked_height, cooked_pixels = pngdiff.decode(preview.images[0][0])
    ed.check_eq((cooked_width, cooked_height), (fixed_width, fixed_height), "the cooked preview has the fixed size")
    ed.check(bytes(cooked_pixels) == bytes(fixed_pixels), "the cooked pixels are the written output's")

    # the source changes so the verdict drops: the raw source is cooked and the Console says so
    shutil.copyfile(PHOTO_FIXTURE, ed.project / step_source)
    await ed.call("sync_files", paths=[step_source], full=False)
    await ed.console_since("last")
    row = await cooked()
    ed.check_eq(row["logical_width"], 256, f"the raw source was cooked: {row}")
    console = "".join(str(entry) for entry in await ed.console_since("last"))
    ed.check("step.png" in console and "needs a person" in console, f"a Console warning names the file and the guard: {console}")
    report = await ed.call("get_texture_cook_report", paths=[step_source], platforms=["desktop"])
    ed.check("Cleanup" in json.dumps(report["textures"][0]["platforms"]["desktop"].get("warnings", [])) or "needs a person" in json.dumps(report),
             f"the report keeps the warning: {report}")

    # the export: the recipe chunk never ships
    out = ed.scratch.root / "export-step"
    built = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=900)
    ed.check_eq(built["status"], "succeeded", f"{built}")
    assets = out / "project" / "assets"
    leaked = [str(p) for p in assets.rglob("*") if p.is_file() and b"jasper.pixelcleanup" in p.read_bytes()]
    ed.check(not leaked, f"no exported file carries the recipe chunk: {leaked}")
