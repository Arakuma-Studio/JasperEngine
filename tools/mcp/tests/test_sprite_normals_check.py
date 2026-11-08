"""`check --normals` (editor/spritenormals/SpriteNormalsCheck.cpp, sprite normals Phase 3 spec 10.3) on a project made
through MCP and saved, then checked with the binary (`check --normals --format json`, and the default set):

- a clean project (a Sprite with its applied map, a Light 2D with height 64): no normals-* finding, exit 0;
- a second run returns the same findings and adds no cache entry; deleting the cache gives the same findings;
- normals-source-changed after rewriting a source (also through `--files <source>`), -source-missing after deleting
  one, -recipe-invalid after corrupting the chunk's CRC;
- normals-size-mismatch (error, exit 1), -filter-mismatch, -directx, -green-convention (a hand-made map with flipped
  green), -not-a-normal-map (the colour image in the normal slot), -no-light-height, -frames-mismatch (the Sprite
  resliced after generating).
"""

from __future__ import annotations

import json
import re
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import harness
from harness import REPO, Editor, TestFailure

EDITOR = {"create": True, "name": "NormalsCheck"}
CI_EXCLUDED = ("long-running (about 6 minutes on a loaded server: projects made through MCP, each checked with the binary); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = 600

FIXTURES = REPO / "editor" / "spritenormals" / "tests" / "fixtures" / "inputs"
ART = "assets/art"
SCENE = "scenes/Main.scene"


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def run_check(project: Path, *args: str) -> tuple[int, dict]:
    cmd = harness.editor_command() + ["check", "--project", str(project), *args, "--format", "json"]
    r = subprocess.run(cmd, cwd=str(project), capture_output=True, text=True, timeout=300)
    try:
        return r.returncode, json.loads(r.stdout)
    except json.JSONDecodeError:
        fail(f"check printed no JSON (exit {r.returncode}): {r.stdout[-1500:]} {r.stderr[-1500:]}")
        return 0, {}


def normals(data: dict) -> list[dict]:
    return [f for f in data["findings"] if f.get("code", "").startswith("normals-")]


def codes(data: dict) -> set[str]:
    return {f["code"] for f in normals(data)}


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def split(png: bytes) -> list[tuple[bytes, bytes]]:
    out, pos = [], 8
    while pos < len(png):
        length = struct.unpack(">I", png[pos:pos + 4])[0]
        out.append((png[pos + 4:pos + 8], png[pos + 8:pos + 8 + length]))
        pos += 12 + length
    return out


def ball_png_wh(width: int, height: int, diameter: int) -> bytes:
    """One shaded ball of `diameter` px centred in a width x height image"""
    def pixel(x, y):
        dx, dy = x + 0.5 - width / 2, y + 0.5 - height / 2
        v = max(40, min(250, int(160 - (dx + dy) * 120 / diameter)))
        return (v, v // 2, 60, 255 if dx * dx + dy * dy <= (diameter / 2) ** 2 else 0)
    return png_rgba(width, height, pixel)


def sheet_png(width: int, height: int, frame: int) -> bytes:
    """A sheet of `frame`-px cells, a ball in each"""
    def pixel(x, y):
        dx, dy = x % frame + 0.5 - frame / 2, y % frame + 0.5 - frame / 2
        return (180, 90, 60, 255 if dx * dx + dy * dy <= (frame / 2 - 2) ** 2 else 0)
    return png_rgba(width, height, pixel)

def ball_png(size: int) -> bytes:
    """A shaded ball on transparency (big enough for the green-convention rims: >= 32 pixels each)"""
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            dx, dy = x + 0.5 - size / 2, y + 0.5 - size / 2
            if dx * dx + dy * dy > (size * 0.45) ** 2:
                row += bytes(4)
            else:
                v = max(40, min(250, int(160 - (dx + dy) * 2)))
                row += bytes((v, v // 2, 40, 255))
        rows.append(b"\x00" + bytes(row))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"".join(rows)))
            + chunk(b"IEND", b""))


def png_rgba(width: int, height: int, pixel) -> bytes:
    rows = [b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height)]
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"".join(rows)))
            + chunk(b"IEND", b""))


def without_recipe(png: bytes) -> bytes:
    return b"\x89PNG\r\n\x1a\n" + b"".join(chunk(k, d) for k, d in split(png) if not (k == b"iTXt" and d.startswith(b"jasper.spritenormals")))


def corrupt_recipe_crc(png: bytes) -> bytes:
    out, pos = bytearray(png[:8]), 8
    while pos < len(png):
        length = struct.unpack(">I", png[pos:pos + 4])[0]
        block = bytearray(png[pos:pos + 12 + length])
        if block[4:8] == b"iTXt" and bytes(block[8:8 + 20]) == b"jasper.spritenormals":
            block[-1] ^= 0xFF
        out += block
        pos += 12 + length
    return bytes(out)


async def gen(ed: Editor, **args) -> dict:
    r = await ed.call_raw("generate_sprite_normals", args, timeout=300)
    if r.is_error:
        fail(f"generate_sprite_normals {args}: {r.text}")
    return r.structured


async def save(ed: Editor) -> None:
    await ed.call("wait", frames=3)
    await ed.call("save_scene", path=SCENE)


async def sprite(ed: Editor, name: str, texture: str, filter: str = "nearest") -> None:
    await ed.call("set_sprite_frames", create={"name": name}, texture=texture, frames=[{"name": "f", "rect": [0, 0, 32, 32]}])
    await ed.call("set_texture_settings", entity=name, component="mesh", property="submeshes[0].material.baseColorTexture",
                  min_filter=filter, mag_filter=filter)


async def assign(ed: Editor, name: str, normal: str, filter: str = "nearest") -> None:
    await ed.call("set_properties", entity=name, values={"mesh": {"submeshes[0].material.normalTexture":
                                                                   {"path": normal, "min_filter": filter, "mag_filter": filter}}})


# ---------------------------------------------------------------------------- clean project, cache

async def test_clean_project(ed: Editor):
    art = ed.project / ART
    art.mkdir(parents=True, exist_ok=True)
    for name in ("sheet.png", "ball.png"):
        shutil.copy(FIXTURES / name, art / name)
    for name in ("coin", "gem", "rock"):
        shutil.copy(FIXTURES / "ball.png", art / f"{name}.png")
        await gen(ed, source=f"{ART}/{name}.png", output=True)
    await ed.call("set_sprite_frames", create={"name": "Hero"}, texture=f"{ART}/sheet.png", slice={"tile_size": [32, 32]})
    await ed.call("set_texture_settings", entity="Hero", component="mesh", property="submeshes[0].material.baseColorTexture",
                  min_filter="nearest", mag_filter="nearest")
    await gen(ed, source="Hero", output=True, apply=True)
    await ed.call("create_entity", name="Lamp", type="light_2d")
    await ed.call("set_properties", entity="Lamp", values={"light2d": {"height": 64}})
    await save(ed)
    code, data = run_check(ed.project, "--normals")
    check(code == 0 and normals(data) == [], f"clean: exit {code}, {normals(data)}")
    check(data["checked"].get("normals", 0) >= 5, f"recipes + maps in use checked: {data['checked']}")
    code, default = run_check(ed.project)
    check("normals" in default["checked"] and normals(default) == [], f"the default set runs it: {default['checked']}")
    cache = ed.project / ".jasperengine" / "normals-check-cache.json"
    check(cache.is_file(), "the cache is written")


async def test_cache_is_stable(ed: Editor):
    cache = ed.project / ".jasperengine" / "normals-check-cache.json"
    (ed.project / ART / "coin.png").write_bytes((FIXTURES / "character.png").read_bytes())   # one finding to compare
    _, first = run_check(ed.project, "--normals")
    before = json.loads(cache.read_text())
    _, second = run_check(ed.project, "--normals")
    after = json.loads(cache.read_text())
    check(normals(first) == normals(second) and normals(first), f"same findings: {normals(first)} vs {normals(second)}")
    check(set(before["pngs"]) == set(after["pngs"]) and set(before["sources"]) == set(after["sources"]) and before == after,
          "a second run adds no cache entry (and decodes or hashes nothing new)")
    cache.unlink()
    _, third = run_check(ed.project, "--normals")
    check(normals(third) == normals(first), f"without the cache, the same findings: {normals(third)}")


# ---------------------------------------------------------------------------- recipes

async def test_source_changed_missing_invalid(ed: Editor):
    code, data = run_check(ed.project, "--normals")
    changed = [f for f in normals(data) if f["code"] == "normals-source-changed"]
    check(any(f["path"] == f"{ART}/coin_normal.png" and f"{ART}/coin.png" in f["message"] for f in changed), f"changed: {normals(data)}")
    check(all(f["severity"] == "warning" and "regenerate" in f["suggestion"] for f in changed), f"{changed}")
    check(code == 0, "warnings only: exit 0")
    code, data = run_check(ed.project, "--normals", "--files", f"{ART}/coin.png")
    check(any(f["path"] == f"{ART}/coin_normal.png" and f["code"] == "normals-source-changed" for f in normals(data)), f"--files <source>: {data}")
    (ed.project / ART / "gem.png").unlink()
    (ed.project / ART / "rock_normal.png").write_bytes(corrupt_recipe_crc((ed.project / ART / "rock_normal.png").read_bytes()))
    code, data = run_check(ed.project, "--normals")
    check(any(f["code"] == "normals-source-missing" and f["path"] == f"{ART}/gem_normal.png" for f in normals(data)), f"missing: {normals(data)}")
    check(any(f["code"] == "normals-recipe-invalid" and f["path"] == f"{ART}/rock_normal.png" for f in normals(data)), f"invalid: {normals(data)}")


# ---------------------------------------------------------------------------- maps in use

async def test_maps_in_use(ed: Editor):
    art = ed.project / ART
    # size: a 128x32 map on a 32x32 sprite
    await sprite(ed, "Big", f"{ART}/ball.png")
    await assign(ed, "Big", f"{ART}/sheet_normal.png")
    # filter: Nearest colour, Linear normal
    await gen(ed, source=f"{ART}/ball.png", output=True)
    await sprite(ed, "Smear", f"{ART}/ball.png")
    await assign(ed, "Smear", f"{ART}/ball_normal.png", "linear")
    # directx: a generated DirectX map
    await gen(ed, source=f"{ART}/ball.png", directx=True, output=f"{ART}/dx/ball_dx.png")
    await sprite(ed, "Dx", f"{ART}/ball.png")
    await assign(ed, "Dx", f"{ART}/dx/ball_dx.png")
    # green convention: a DirectX map of a 64 px ball without its recipe (made elsewhere)
    (art / "big_ball.png").write_bytes(ball_png(64))
    await gen(ed, source=f"{ART}/big_ball.png", directx=True, output=f"{ART}/dx/big_ball_dx.png")
    (art / "hand").mkdir(exist_ok=True)
    (art / "hand" / "ball_flipped.png").write_bytes(without_recipe((art / "dx" / "big_ball_dx.png").read_bytes()))
    await sprite(ed, "Flipped", f"{ART}/big_ball.png")
    await assign(ed, "Flipped", f"{ART}/hand/ball_flipped.png")
    # not a normal map: the colour image in the slot
    await sprite(ed, "Colour", f"{ART}/ball.png")
    await assign(ed, "Colour", f"{ART}/ball.png")
    await save(ed)
    code, data = run_check(ed.project, "--normals")
    by = {}
    for f in normals(data):
        by.setdefault(f["code"], []).append(f)
    size = [f for f in by.get("normals-size-mismatch", []) if "'Big'" in f["message"]]
    check(size and size[0]["severity"] == "error" and size[0]["path"] == SCENE and "128x32" in size[0]["message"] and "32x32" in size[0]["message"],
          f"size: {by.get('normals-size-mismatch')}")
    check(code == 1, f"an error: exit 1 ({code})")
    check(any("'Smear'" in f["message"] for f in by.get("normals-filter-mismatch", [])), f"filter: {by.get('normals-filter-mismatch')}")
    check(any("'Dx'" in f["message"] for f in by.get("normals-directx", [])), f"directx: {by.get('normals-directx')}")
    check(any("'Flipped'" in f["message"] for f in by.get("normals-green-convention", [])), f"green: {by.get('normals-green-convention')} {list(by)}")
    check(any("'Colour'" in f["message"] for f in by.get("normals-not-a-normal-map", [])), f"not a map: {by.get('normals-not-a-normal-map')}")
    check(not any("'Hero'" in f["message"] for f in normals(data)), f"the applied Hero stays clean: {[f for f in normals(data) if 'Hero' in f['message']]}")
    # text output: compiler style with the code
    r = subprocess.run(harness.editor_command() + ["check", "--project", str(ed.project), "--normals"], cwd=str(ed.project), capture_output=True,
                       text=True, timeout=300)
    check(f"{SCENE}: error: GameObject 'Big' submesh 0:" in r.stdout and "[normals-size-mismatch]" in r.stdout, f"text: {r.stdout[-1500:]}")


async def test_light_height_and_frames(ed: Editor):
    await ed.call("set_properties", entity="Lamp", values={"light2d": {"height": 0}})
    await save(ed)
    _, data = run_check(ed.project, "--normals")
    check(any(f["code"] == "normals-no-light-height" and f["path"] == SCENE for f in normals(data)), f"no light height: {normals(data)}")
    await ed.call("set_properties", entity="Lamp", values={"light2d": {"height": 64}})
    # the Sprite resliced after its map was generated
    await ed.call("set_sprite_frames", entity="Hero", slice={"tile_size": [64, 32]})
    await save(ed)
    _, data = run_check(ed.project, "--normals")
    check(not any(f["code"] == "normals-no-light-height" for f in normals(data)), "height 64: gone")
    check(any(f["code"] == "normals-frames-mismatch" and "'Hero'" in f["message"] for f in normals(data)), f"frames: {normals(data)}")


# ---------------------------------------------------------------------------- fix round 1

async def test_files_without_a_recipe_chunk_are_not_garbled_recipes(ed: Editor):
    art = ed.project / ART
    (art / "photo.png").write_bytes((REPO / "engine/tests/htmlui/fixtures/compat/net/img/photo.jpg").read_bytes())
    (art / "trunc.png").write_bytes(b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00")
    (art / "garbage.png").write_bytes(b"not an image at all")
    _, data = run_check(ed.project)
    bogus = [f for f in normals(data) if f["path"] in (f"{ART}/photo.png", f"{ART}/trunc.png", f"{ART}/garbage.png")]
    check(not bogus, f"a JPEG named .png, a truncated PNG and garbage are no garbled recipes: {bogus}")
    check(any(f["code"] == "normals-recipe-invalid" and f["path"] == f"{ART}/rock_normal.png" for f in normals(data)),
          "a damaged recipe chunk still is one")
    for name in ("photo.png", "trunc.png", "garbage.png"):
        (art / name).unlink()


async def test_opaque_tile_with_a_raised_border_is_no_directx_map(ed: Editor):
    # an opaque OpenGL map of a tile whose border is raised: its top rows lean down, its bottom
    # rows up, like a DirectX map's rims would; but the image's border is no silhouette
    art = ed.project / ART
    (art / "tile64.png").write_bytes(png_rgba(64, 64, lambda x, y: (120, 140, 90, 255)))

    def raised(x, y):
        if y < 3:
            return (128, 81, 246, 255)
        if y > 60:
            return (128, 175, 246, 255)
        return (128, 128, 255, 255)
    (art / "hand").mkdir(exist_ok=True)
    (art / "hand" / "tile64_n.png").write_bytes(png_rgba(64, 64, raised))
    await sprite(ed, "Raised", f"{ART}/tile64.png")
    await assign(ed, "Raised", f"{ART}/hand/tile64_n.png")
    await save(ed)
    _, data = run_check(ed.project, "--normals")
    raised_findings = [f for f in normals(data) if "'Raised'" in f["message"]]
    check(not raised_findings, f"no green-convention (or other) finding for the opaque tile: {raised_findings}")


async def test_normalized_frames_on_a_wide_sheet(ed: Editor):
    # a 2048-wide sheet of 64 frames of 32 px, its frames stored as fractions of the texture
    def blobs(x, y):
        cx = (x // 32) * 32 + 16
        inside = (x - cx) ** 2 + (y - 16) ** 2 < 140
        return (200, 90 + (x // 32), 60, 255) if inside else (0, 0, 0, 0)
    art = ed.project / ART
    (art / "wide.png").write_bytes(png_rgba(2048, 32, blobs))
    await ed.call("set_sprite_frames", create={"name": "Wide"}, texture=f"{ART}/wide.png", slice={"tile_size": [32, 32]}, inline=True)
    await ed.call("set_texture_settings", entity="Wide", component="mesh", property="submeshes[0].material.baseColorTexture",
                  min_filter="nearest", mag_filter="nearest")
    await gen(ed, source="Wide", output=True, apply=True)
    await save(ed)
    _, data = run_check(ed.project, "--normals")
    check(not any("'Wide'" in f["message"] for f in normals(data)), f"pixel frames: clean: {[f for f in normals(data) if 'Wide' in f['message']]}")
    # the same frames as fractions (x / 2048, 0, 32 / 2048, 1): what the session scales them to
    scene = ed.project / SCENE
    text = scene.read_text()
    start = text.index("name: Wide")
    end = text.find("- type: Entity", start)
    end = len(text) if end < 0 else end

    def fraction(m):
        x, y, w, h = (float(v) for v in m.group(1).split(","))
        return f"rect: [{x / 2048!r}, {y / 32!r}, {w / 2048!r}, {h / 32!r}]"
    block = re.sub(r"rect: \[([^\]]+)\]", fraction, text[start:end])
    check(block != text[start:end] and "0.015625" in block, "the frames were rewritten as fractions")
    scene.write_text(text[:start] + block + text[end:])
    _, data = run_check(ed.project, "--normals")
    wide = [f for f in normals(data) if "'Wide'" in f["message"]]
    check(not wide, f"normalized frames of a 2048-wide sheet match the map's cells: {wide}")


# ---------------------------------------------------------------------------- 3D meshes are not checked

async def test_maps_made_before_high_resolution_support(ed: Editor):
    """normals-before-hires: a version 1 map whose islands are over 128 px (from its alpha, over its frames), not the
    image size: a 512x256 sheet of 32 px frames and a 192x256 sprite are pixel art; the 384 px painted potion is not"""
    art = ed.project / ART / "v1"
    art.mkdir(parents=True, exist_ok=True)
    (art / "sheet.png").write_bytes(sheet_png(512, 256, 32))
    shutil.copy(REPO / "editor" / "spritenormals" / "tests" / "fixtures" / "hires" / "potion.png", art / "potion.png")
    (art / "small.png").write_bytes(ball_png_wh(192, 256, 110))
    await ed.call("sync_files")
    await gen(ed, source=f"{ART}/v1/sheet.png", frames={"tile_size": 32}, params={"version": 1}, output=True)
    await gen(ed, source=f"{ART}/v1/small.png", params={"version": 1}, output=True)
    await gen(ed, source=f"{ART}/v1/potion.png", params={"version": 1}, output=True)
    await gen(ed, source=f"{ART}/v1/potion.png", output=f"{ART}/v1/potion_v2.png")
    code, data = run_check(ed.project, "--normals")
    before = {f["path"] for f in normals(data) if f["code"] == "normals-before-hires"}
    check(before == {f"{ART}/v1/potion_normal.png"}, f"only the painted v1 map: {before} {normals(data)}")
    f = next(f for f in normals(data) if f["code"] == "normals-before-hires")
    check(f["severity"] == "warning" and "version 2" in f["suggestion"], f"{f}")
    # the cached result is the same
    code, again = run_check(ed.project, "--normals")
    check({f["path"] for f in normals(again) if f["code"] == "normals-before-hires"} == before, "cached")


async def test_lit_meshes_are_not_sprite_normal_maps():
    """A 3D project (tools/fixtures/stochastic-tiling: Lit boxes and planes with normal maps) whose normal map is 64x64
    on a 256x256 albedo: a Lit material's normal map is sized independently, so --normals reports nothing about it and
    the default set does not fail on it (only Sprites, Tilemaps and Polygon Meshes are checked)"""
    import tempfile
    with tempfile.TemporaryDirectory(prefix="jasper-normals-3d-") as tmp:
        project = Path(tmp) / "stochastic-tiling"
        shutil.copytree(REPO / "tools" / "fixtures" / "stochastic-tiling", project)
        (project / "assets" / "textures" / "pebbles_normal.png").write_bytes(png_rgba(64, 64, lambda x, y: (128, 128, 255, 255)))
        code, data = run_check(project, "--normals")
        check(code == 0 and not normals(data), f"--normals on Lit meshes: exit {code}, {normals(data)}")
        check(data["checked"].get("normals", 0) == 0, f"no map in use is a sprite's: {data['checked']}")
        code, data = run_check(project)
        check(not normals(data), f"the default set: {normals(data)}")
        check(code == 0, f"the default set exits 0 (exit {code}): {[f for f in data['findings'] if f['severity'] == 'error']}")
