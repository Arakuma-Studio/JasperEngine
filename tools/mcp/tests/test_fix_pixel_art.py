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


def reset_art(ed: Editor) -> None:
    """Each test starts from an empty art folder: the file shares one editor, and a test that stops
    half-way must not leave an output the next one's write would find (output_exists)."""
    shutil.rmtree(ed.project / ART, ignore_errors=True)
    (ed.project / ART).mkdir(parents=True, exist_ok=True)


async def finished(ed: Editor, reply: dict) -> dict:
    """The job's result when the call handed back its job_id (past the 1 s it waits)."""
    if reply.get("status") != "running":
        return reply
    for _ in range(1500):
        job = await ed.call("get_job", job_id=reply["job_id"])
        if job["status"] != "running":
            ed.check_eq(job["status"], "succeeded", str(job)[:400])
            return job["result"]
        await asyncio.sleep(0.2)
    ed.fail(f"job {reply['job_id']} still running")


async def fix(ed: Editor, args: dict) -> dict:
    return await finished(ed, await ed.call("fix_pixel_art", args))


def big_png() -> bytes:
    """4,000 px square of flat 3 px cells: bent grid lines on it run for seconds."""
    width = height = 4000
    pixel_rows = []
    for k in range(7):
        a = bytes(((k * 40) & 255, 255 - (k * 30) & 255, (k * 17) & 255, 255))
        b = bytes(((k * 40 + 60) & 255, (k * 30) & 255, 255 - (k * 17) & 255, 255))
        pixel_rows.append(b"\0" + (a * 3 + b * 3) * (width // 6) + a * (width % 6))
    raw = b"".join(pixel_rows[(y // 3) % 7] for y in range(height))
    data = zlib.compress(raw, 1)
    png = b"\x89PNG\r\n\x1a\n"
    for kind, body in ((b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)), (b"IDAT", data), (b"IEND", b"")):
        png += struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    return png


SLOW = {"grid": {"kind": "cells", "cell_width": 3}, "follow": "bend"}
# a palette of black and white loses the sprite's colours: lost_share trips (needs_person), and there are pixels to write
LOSING_PALETTE = {"palette": ["#000000", "#ffffff"]}


async def test_preview_then_write_then_rerun(ed: Editor):
    reset_art(ed)
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

    written = await fix(ed, {"source": HERO, "output": True})
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
    rerun = await fix(ed, {"source": HERO_CLEAN, "rerun": True, "output": True})
    ed.check_eq((ed.project / HERO_CLEAN).read_bytes(), png, "rerun writes the same bytes")
    ed.check_eq(rerun["stale"]["source_changed"], False)

    # the source changed: the result says stale; a preview still runs (R32)
    (ed.project / HERO).write_bytes(ai_sprite(seed=99))
    stale = await fix(ed, {"source": HERO_CLEAN, "rerun": True})
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

    # one undo removes the write (the re-run's identical bytes made no step of its own)
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / HERO_CLEAN).exists(), "undo removes the written file")


async def test_bad_arguments_and_existing_output(ed: Editor):
    reset_art(ed)
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
    reset_art(ed)
    shutil.copy(PHOTO_FIXTURE, ed.project / PHOTO)
    write_hero(ed)
    await enable(ed)
    preview = await fix(ed, {"source": PHOTO})
    ed.check_eq(preview["verdict"], "needs_person", preview["summary"])
    ids = {g["id"] for g in preview["guards"]}
    ed.check(ids & {"no_grid", "low_fit", "low_confidence"}, f"guards: {ids}")

    before = files_in(ed.project)
    message = await ed.check_error("fix_pixel_art", {"source": PHOTO, "output": True}, contains="needs_person")
    ed.check("force" in message, f"the fix names force: {message[:300]}")
    ed.check_eq(files_in(ed.project), before, "a refused write leaves the directory as it was")

    # a needs_person result that has pixels: refused without force, written and recorded with it (AC11)
    wrong = await fix(ed, {"source": HERO, "recipe": LOSING_PALETTE})
    ed.check_eq(wrong["verdict"], "needs_person", wrong["summary"])
    ed.check(wrong["output"]["width"] > 0, f"it has an output: {wrong['output']}")
    await ed.check_error("fix_pixel_art", {"source": HERO, "recipe": LOSING_PALETTE, "output": True}, contains="needs_person")
    ed.check_eq(files_in(ed.project), before, "nothing is written without force")
    forced = await fix(ed, {"source": HERO, "recipe": LOSING_PALETTE, "output": True, "force": True})
    path = ed.project / forced["default_output"]
    ed.check(path.exists(), "force writes it")
    chunk = json.loads(chunk_text(path.read_bytes(), "jasper.pixelcleanup"))
    ed.check_eq(chunk["forced"], True, "the recipe records forced")
    lint = await ed.call("lint_pixel_art", {"source": forced["default_output"]})
    ed.check_eq(lint.get("forced"), True, "lint_pixel_art reports forced on it")


async def test_output_paths_carry_their_roles_extension(ed: Editor):
    """R31: an object-form output path must end in its role's extension; a .meta is never an output."""
    reset_art(ed)
    write_hero(ed)
    await enable(ed)
    before = files_in(ed.project)
    for output in ({"png": f"{ART}/x.png.meta"}, {"png": "project.yaml"}, {"aseprite": f"{ART}/x.png"}, {"png": f"{ART}/x.png", "json": f"{ART}/x.txt"}):
        await ed.check_error("fix_pixel_art", {"source": HERO, "output": output, "overwrite": True}, contains="invalid_value")
    ed.check_eq(files_in(ed.project), before, "no refused output is written")
    ed.check(b"PNG" not in (ed.project / "project.yaml").read_bytes()[:8], "project.yaml is untouched")


async def test_timeout_seconds_ends_the_run(ed: Editor):
    """R37: past 1 s the call hands back the job; timeout_seconds ends the run, and a timed-out run writes nothing."""
    reset_art(ed)
    big = f"{ART}/big.png"
    (ed.project / big).write_bytes(big_png())
    await enable(ed)
    before = files_in(ed.project)
    started = await ed.call("fix_pixel_art", {"source": big, "output": True, "timeout_seconds": 1, "recipe": SLOW})
    ed.check_eq(started.get("status"), "running", f"past 1 s it is a job: {str(started)[:200]}")
    for _ in range(1500):
        job = await ed.call("get_job", job_id=started["job_id"])
        if job["status"] != "running":
            break
        await asyncio.sleep(0.2)
    ed.check(job["status"] in ("failed", "cancelled") and "timeout_seconds" in json.dumps(job), f"timed out: {str(job)[:400]}")
    ed.check_eq(files_in(ed.project), before, "a timed-out run writes nothing")


async def test_cancelled_job_writes_nothing(ed: Editor):
    reset_art(ed)
    big = f"{ART}/big.png"
    (ed.project / big).write_bytes(big_png())
    await enable(ed)
    before = files_in(ed.project)
    started = await ed.call("fix_pixel_art", {"source": big, "output": True, "recipe": SLOW})
    ed.check_eq(started.get("status"), "running", f"past 1 s it is a job: {str(started)[:200]}")
    await ed.call("cancel_job", job_id=started["job_id"])
    for _ in range(1500):
        job = await ed.call("get_job", job_id=started["job_id"])
        if job["status"] != "running":
            break
        await asyncio.sleep(0.2)
    ed.check_eq(job["status"], "cancelled", str(job)[:300])
    ed.check_eq(files_in(ed.project), before, "a cancelled run writes nothing")


async def test_apply_sets_the_sprite(ed: Editor):
    reset_art(ed)
    write_hero(ed)
    await enable(ed)
    await ed.call("create_entity", name="HeroSprite", type="sprite")
    r = await fix(ed, {"source": HERO, "output": {"png": f"{ART}/hero_applied.png"}, "apply": "HeroSprite"})
    ed.check("applied" in r, f"applied: {str(r)[:300]}")
    props = await ed.call("get_entity", entity="HeroSprite", components=["mesh"])
    text = json.dumps(props)
    ed.check("hero_applied.png" in text, f"the Sprite's texture is the output: {text[:300]}")
    entries = (await ed.call("get_undo_history", scope="timeline", limit=5))["entries"]
    names = [e["name"] for e in entries]
    ed.check(any("HeroSprite" in n and "Set Properties" in n for n in names), f"a scene step: {names}")
    ed.check_eq(r["applied"]["undo"]["name"], names[0], "the result names the scene step")
    ed.check(any("Pixel Art Cleanup" in n for n in names), f"the file step: {names}")
    await ed.check_error("fix_pixel_art", {"source": HERO, "apply": "HeroSprite"}, contains="output")


def sheet_png(copies: int = 3, gap: int = 32) -> bytes:
    """Three copies of the AI sprite side by side on a transparent sheet: one frame each."""
    w, h, one = pngdiff.decode(ai_sprite())
    width = w * copies + gap * (copies - 1)
    rgba = bytearray(width * h * 4)
    for c in range(copies):
        for y in range(h):
            start = (y * width + c * (w + gap)) * 4
            rgba[start:start + w * 4] = one[y * w * 4:(y + 1) * w * 4]
    return pngdiff.encode(width, h, bytes(rgba))


async def test_apply_an_aseprite_output(ed: Editor):
    """R44: apply with an .aseprite output makes the Sprite show that sheet, as a scene step after the file step."""
    reset_art(ed)
    write_hero(ed)
    await enable(ed)
    await ed.call("create_entity", name="HeroAse", type="sprite")
    r = await fix(ed, {"source": HERO, "format": "aseprite", "output": True, "apply": "HeroAse"})
    ed.check("applied" in r, f"applied: {str(r)[:400]}")
    ed.check((ed.project / f"{ART}/hero_clean.aseprite").exists(), "the .aseprite is written")
    sprite = (await ed.call("get_entity", entity="HeroAse", components=["sprite"]))["components"]["sprite"]
    ed.check("hero_clean.aseprite" in json.dumps(sprite), f"the Sprite shows the .aseprite: {json.dumps(sprite)[:400]}")
    names = [e["name"] for e in (await ed.call("get_undo_history", scope="timeline", limit=5))["entries"]]
    ed.check_eq(r["applied"]["undo"]["name"], names[0], "the result names the scene step")
    ed.check(any("Pixel Art Cleanup (hero_clean.aseprite)" in n for n in names), f"the file step: {names}")


async def test_apply_a_png_sheet(ed: Editor):
    """R44: apply with a PNG sheet sets the texture, the Sprite's frames and one Sprite Animation per clip."""
    reset_art(ed)
    (ed.project / f"{ART}/walk.png").write_bytes(sheet_png())
    await enable(ed)
    await ed.call("create_entity", name="Walker", type="sprite")
    r = await fix(ed, {"source": f"{ART}/walk.png", "sheet": "on", "format": "png", "output": True, "apply": "Walker", "force": True})
    ed.check("applied" in r, f"applied: {str(r)[:400]}")
    clips = r["animation"]["animations"]
    frames = sum(len(c["frames"]) for c in clips if c["include"])
    ed.check(frames >= 2, f"the sheet's frames: {clips}")
    sprite = (await ed.call("get_entity", entity="Walker", components=["sprite"]))["components"]["sprite"]
    ed.check_eq(sprite.get("numFramesRect"), frames, f"one Sprite frame per sheet frame: {json.dumps(sprite)[:400]}")
    mesh = (await ed.call("get_entity", entity="Walker", components=["mesh"]))["components"]["mesh"]
    ed.check("walk_clean.png" in json.dumps(mesh), f"the texture is the sheet: {json.dumps(mesh)[:300]}")
    tree = json.dumps(await ed.call("get_scene_tree"))
    for clip in clips:
        if clip["include"]:
            ed.check(clip["name"] in tree, f"a Sprite Animation for {clip['name']}: {tree[:600]}")


async def test_import_step_through_the_tool(ed: Editor):
    """R33 for agents: output {import_step: true} stores the resolved recipe in the source's .meta as one undo step, writes no
    image, leaves the source's bytes alone, and refuses needs_person like a write does."""
    source = f"{ART}/agent_step.png"
    (ed.project / ART).mkdir(parents=True, exist_ok=True)
    (ed.project / source).write_bytes(ai_sprite())
    before_bytes = (ed.project / source).read_bytes()
    await ed.call("tool_groups", enable=["pixel_art", "assets_pipeline"])
    await ed.call("sync_files", paths=[source], full=False)
    listing = files_in(ed.project)

    result = await ed.call("fix_pixel_art", source=source, output={"import_step": True})
    ed.check(result["verdict"] == "fixable", f"{result}")
    ed.check_eq(result["import_step"]["source"], source, f"{result}")
    ed.check("reimport_textures" in result["next"], f"the result says how to cook it: {result['next']}")
    ed.check_eq((ed.project / source).read_bytes(), before_bytes, "the source file is unchanged")
    ed.check_eq(files_in(ed.project), listing, "no image is written")
    meta = yaml.safe_load((ed.project / f"{source}.meta").read_text())
    step = meta["import"]["pixelCleanup"]
    ed.check(step["enabled"] is True, f"{step}")
    ed.check_eq(json.loads(step["recipe"])["source"]["path"], source, f"{step}")
    ed.check("Save as Import Step" in result["undo"]["name"], f"{result['undo']}")

    report = await ed.call("reimport_textures", paths=[source], wait=True, timeout_s=300)
    row = report["textures"][0]["platforms"]["desktop"]
    ed.check_eq(row["logical_width"], 16, f"the texture cooks the fixed size: {row}")

    await ed.call("undo", scope="project")
    after_undo = yaml.safe_load((ed.project / f"{source}.meta").read_text()) or {}
    ed.check("pixelCleanup" not in (after_undo.get("import") or {}), f"one undo removes the step: {after_undo}")

    photo = f"{ART}/photo.jpg"
    (ed.project / photo).write_bytes((REPO / "editor/tests/fixtures/pixelart/cleanup/photo.jpg").read_bytes())
    await ed.call("sync_files", paths=[photo], full=False)
    refused = await ed.call_raw("fix_pixel_art", {"source": photo, "output": {"import_step": True}})
    ed.check(refused.is_error and "needs_person" in refused.text, f"needs_person is refused: {refused.text}")
    ed.check(not (ed.project / f"{photo}.meta").exists() or "pixelCleanup" not in (ed.project / f"{photo}.meta").read_text(), "nothing is saved")
    bad = await ed.call_raw("fix_pixel_art", {"source": source, "output": {"import_step": True, "png": f"{ART}/x.png"}})
    ed.check(bad.is_error and "import_step" in bad.text, f"paths with import_step are refused: {bad.text}")


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
    await ed.call("save_project")
    out = ed.scratch.root / "export-step"
    built = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=900)
    ed.check_eq(built["status"], "succeeded", f"{built}")
    assets = out / "project" / "assets"
    leaked = [str(p) for p in assets.rglob("*") if p.is_file() and b"jasper.pixelcleanup" in p.read_bytes()]
    ed.check(not leaked, f"no exported file carries the recipe chunk: {leaked}")
