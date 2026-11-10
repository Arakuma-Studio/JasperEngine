"""generate_sprite_normals (editor/mcp/tools/SpriteNormalsTools.cpp, sprite normals Phase 3 spec 10.1), on a new
project with inputs made here (a shaded ball, a 3-frame GIF with delays 50/90/0 ms and a disposal, an opaque tile, a
dithered sprite, a large noisy sprite for the job path) and copied from Phase 2's fixtures (the 128x32 4-frame sheet,
the 2-frame APNG, the upstream golden ball):

- invalid arguments fail with their error kind and write nothing;
- a preview writes nothing and returns one contact sheet (PNG, <= 1024 wide), every result key of 3.7, the preset's
  settings, default_output and the "Preview only" text;
- the same arguments + output write exactly that: alpha kept, (128,128,255) where transparent, unit vectors, the
  jasper.spritenormals recipe (format, version, the source's SHA-1, the settings); deterministic; one project undo
  step (undo / redo);
- parity: the upstream fixture with preset detailed and radius 4 matches the upstream output within 1;
- existing outputs need overwrite (one undo puts the old bytes back);
- a Sprite GameObject source takes its frames; apply sets the normal texture with the colour texture's filter (two
  undo steps, the scene one clears the slot and keeps the file); without apply, apply_args;
- a GIF / APNG with apply: atlas + normal atlas, the Sprite's frames, a Sprite Animation with the delays
  (zero_delay warned);
- warnings: no_alpha (gone with edges extend), no_light, no_light_height (gone at height 64), directx_in_engine;
- height: true writes a 16-bit grey PNG; output {normal, frames: true} writes the frames JSON;
- save_path writes the sheet and nothing else;
- symbolic links: no output (normal, height, frames JSON, atlas) nor save_path through a link of the assets to outside
  the project, dangling links included (output_invalid), none that is the source through a link; a link to a folder
  of the assets is fine;
- the job path (timeout -> job_id, get_job, cancel_job writes nothing);
- get_asset_info / view_asset (sprite_normals, regenerate reproducing the pixels, a grid's {cells}, staleness after
  an edit of the source and the auto-sync's external_changes problem, normal_map on the source);
- apply refused while playing (nothing written), a preview works then;
- the tool description and how an agent reaches its guide;
- (last: switches projects) a run when another project opens fails project_changed, nothing written anywhere.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import math
import os
import random
import shutil
import struct
import tempfile
import zlib
from pathlib import Path

from harness import REPO, Editor, TestFailure, skip, symlinks_permitted
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "SpriteNormals"}
CI_EXCLUDED = ("long-running (about 8 minutes on a loaded server: 23 tests generating normal maps); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = 900

FIXTURES = REPO / "editor" / "spritenormals" / "tests" / "fixtures"
SPR = "assets/sprites"
BALL = f"{SPR}/ball.png"
SHEET = f"{SPR}/sheet.png"
GIF = f"{SPR}/torch.gif"
APNG = f"{SPR}/twoframes.png"
TILE = f"{SPR}/tile.png"
DITHER = f"{SPR}/dither.png"
GOLDEN = f"{SPR}/golden_ball.png"
BIG = f"{SPR}/big.png"
S: dict = {}


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


# ---------------------------------------------------------------------------- images

def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png_rgba(width: int, height: int, rows: list[bytes]) -> bytes:
    raw = b"".join(b"\x00" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def ball_rows(size: int = 32, cx: float | None = None) -> list[bytes]:
    """A shaded ball (light from the top-left) on transparency"""
    cx = size / 2 if cx is None else cx
    r = size * 0.42
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            dx, dy = x + 0.5 - cx, y + 0.5 - size / 2
            d = math.hypot(dx, dy)
            if d > r:
                row += bytes(4)
                continue
            shade = max(0.0, min(1.0, 0.75 - 0.5 * (dx + dy) / (2 * r)))
            row += bytes((int(60 + 180 * shade), int(40 + 120 * shade), int(30 + 60 * shade), 255))
        rows.append(bytes(row))
    return rows


def opaque_rows(size: int = 32) -> list[bytes]:
    rnd = random.Random(5)
    return [b"".join(bytes((90 + rnd.randrange(40), 120, 60, 255)) for _ in range(size)) for _ in range(size)]


def dither_rows(size: int = 32) -> list[bytes]:
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            inside = 4 <= x < size - 4 and 4 <= y < size - 4
            v = 200 if (x + y) % 2 else 60
            row += bytes((v, v, v, 255)) if inside else bytes(4)
        rows.append(bytes(row))
    return rows


def noise_rows(size: int, seed: int) -> list[bytes]:
    rnd = random.Random(seed)
    rows = []
    for y in range(size):
        row = bytearray(size * 4)
        for x in range(0, size, 4):
            a = 255 if (x // 16 + y // 16) % 3 else 0
            v = rnd.randrange(256)
            for k in range(4):
                row[(x + k) * 4:(x + k) * 4 + 4] = bytes((v, 255 - v, (v * 7) & 255, a))
        rows.append(bytes(row))
    return rows


def gif_bytes(frames: list[list[bytes]], width: int, height: int, delays_cs: list[int], disposals: list[int]) -> bytes:
    """A GIF89a (global palette, index 0 transparent, NETSCAPE loop forever): frames are rows of RGBA bytes.
    LZW without compression: literals only, a clear code every 200 codes keeps the code width at 9 bits"""
    palette = [(0, 0, 0)]
    index = {}
    indexed = []
    for rows in frames:
        out = []
        for row in rows:
            line = bytearray()
            for x in range(width):
                r, g, b, a = row[x * 4:x * 4 + 4]
                if a < 128:
                    line.append(0)
                    continue
                key = (r & 0xF0, g & 0xF0, b & 0xF0)
                if key not in index:
                    if len(palette) >= 256:
                        key = min(index, key=lambda k: sum(abs(p - q) for p, q in zip(k, key)))
                    else:
                        index[key] = len(palette)
                        palette.append(key)
                line.append(index[key])
            out.append(bytes(line))
        indexed.append(out)
    palette += [(0, 0, 0)] * (256 - len(palette))

    def lzw(pixels: bytes) -> bytes:
        codes = [256]
        for i, p in enumerate(pixels):
            if i and i % 200 == 0:
                codes.append(256)
            codes.append(p)
        codes.append(257)
        acc = bits = 0
        data = bytearray()
        for c in codes:
            acc |= c << bits
            bits += 9
            while bits >= 8:
                data.append(acc & 0xFF)
                acc >>= 8
                bits -= 8
        if bits:
            data.append(acc & 0xFF)
        blocks = bytearray()
        for i in range(0, len(data), 255):
            part = data[i:i + 255]
            blocks += bytes((len(part),)) + part
        return bytes((8,)) + bytes(blocks) + b"\x00"

    out = bytearray(b"GIF89a" + struct.pack("<HHBBB", width, height, 0xF7, 0, 0))
    out += b"".join(bytes(c) for c in palette)
    out += b"!\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"
    for rows, delay, disposal in zip(indexed, delays_cs, disposals):
        out += b"!\xf9\x04" + bytes(((disposal << 2) | 1,)) + struct.pack("<H", delay) + b"\x00\x00"
        out += b"," + struct.pack("<HHHHB", 0, 0, width, height, 0)
        out += lzw(b"".join(rows))
    out += b";"
    return bytes(out)


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
    key = b"jasper.spritenormals\x00"
    for kind, data in chunks(path.read_bytes()):
        if kind == b"iTXt" and data.startswith(key):
            return json.loads(data[len(key) + 4:].decode("utf-8"))
    fail(f"{path}: no jasper.spritenormals chunk")
    return {}


def rgba(png: bytes) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    w, h, channels, rows = decode_png(png)
    check(channels == 4, f"RGBA expected, got {channels} channels")
    px = []
    for row in rows:
        for x in range(w):
            px.append(tuple(row[x * 4:x * 4 + 4]))
    return w, h, px


def files_under(project: Path) -> set[str]:
    # (the assets' S04 .meta sidecars aside: the asset database writes them for every file)
    return {p.relative_to(project).as_posix() for p in (project / "assets").rglob("*") if p.is_file() and p.suffix != ".meta"}


def write_inputs(project: Path) -> None:
    spr = project / SPR
    spr.mkdir(parents=True, exist_ok=True)
    (spr / "ball.png").write_bytes(png_rgba(32, 32, ball_rows()))
    shutil.copy(FIXTURES / "inputs" / "sheet.png", spr / "sheet.png")
    shutil.copy(FIXTURES / "inputs" / "twoframes.png", spr / "twoframes.png")
    shutil.copy(FIXTURES / "inputs" / "ball.png", spr / "golden_ball.png")
    frames = [ball_rows(32, 14), ball_rows(32, 16), ball_rows(32, 18)]
    (spr / "torch.gif").write_bytes(gif_bytes(frames, 32, 32, [5, 9, 0], [1, 2, 1]))
    (spr / "tile.png").write_bytes(png_rgba(32, 32, opaque_rows()))
    (spr / "dither.png").write_bytes(png_rgba(32, 32, dither_rows()))
    (spr / "big.png").write_bytes(png_rgba(2048, 2048, noise_rows(2048, 3)))
    (project / "assets" / "notes.txt").write_text("not an image")


async def gen(ed: Editor, **args):
    r = await ed.call_raw("generate_sprite_normals", args, timeout=600)
    if r.is_error:
        fail(f"generate_sprite_normals {args} failed: {r.text}")
    return r


async def gen_error(ed: Editor, **args) -> dict:
    r = await ed.call_raw("generate_sprite_normals", args, timeout=600)
    check(r.is_error, f"{args} should fail: {r.text[:400]}")
    errors = (r.structured or {}).get("errors") or []
    check(errors and all({"kind", "message", "fix"} <= set(e) for e in errors), f"{args}: errors [{{kind, message, fix}}]: {r.structured}")
    return {"kinds": [e["kind"] for e in errors], "text": r.text, "data": r.structured}


async def poll(ed: Editor, job: str, seconds: float = 300.0) -> dict:
    state = {}
    for _ in range(int(seconds / 0.2)):
        state = await ed.call("get_job", job_id=job)
        if state["status"] != "running":
            return state
        await asyncio.sleep(0.2)
    return state


# ---------------------------------------------------------------------------- 10.1.1 arguments

async def test_invalid_arguments_write_nothing(ed: Editor):
    ed.require_tools("generate_sprite_normals")
    write_inputs(ed.project)
    before = files_under(ed.project)
    cases = [
        ({}, "invalid_source"),
        ({"source": f"{SPR}/nope.png"}, "invalid_source"),
        ({"source": "Nobody"}, "invalid_source"),
        ({"source": "assets/notes.txt"}, "invalid_source"),
        ({"source": BALL, "preset": "x"}, "invalid_settings"),
        ({"source": BALL, "params": {"detail": 9}}, "invalid_settings"),
        ({"source": BALL, "params": {"bogus": 1}}, "invalid_settings"),
        ({"source": BALL, "frames": {"tile_size": 40}}, "frames_mismatch"),
        ({"source": GIF, "frames": "none"}, "frames_mismatch"),
        ({"source": BALL, "apply": True}, "apply_needs_output"),
        ({"source": BALL, "apply": True, "output": True}, "invalid_apply"),
        ({"source": BALL, "output": "../x.png"}, "output_invalid"),
        ({"source": BALL, "output": "assets/.hidden/x.png"}, "output_invalid"),
        ({"source": BALL, "output": "assets/x.jpg"}, "output_invalid"),
        ({"source": BALL, "output": BALL, "overwrite": True}, "output_invalid"),
        ({"source": BALL, "directx": "yes"}, "invalid_settings"),
        ({"source": BALL, "wait": 1}, "invalid_settings"),
    ]
    for args, kind in cases:
        got = await gen_error(ed, **args)
        check(kind in got["kinds"], f"{args}: kind {kind} expected, got {got['kinds']}: {got['text']}")
        check(all(e["fix"] for e in got["data"]["errors"]), f"{args}: every error has a fix: {got['data']['errors']}")
    msg = (await gen_error(ed, source=BALL, params={"detail": 9}))["text"]
    check("params.detail" in msg and "8" in msg and "9" in msg, f"the range error names the setting, the range and the value: {msg}")
    msg = (await gen_error(ed, source=BALL, params={"bogus": 1}))["text"]
    check("bogus" in msg and "strength" in msg, f"unknown key lists the valid ones: {msg}")
    msg = (await gen_error(ed, source="Nobody"))["text"]
    check("no image file 'Nobody'" in msg and "GameObject" in msg, f"both attempts named: {msg}")
    check(files_under(ed.project) == before, f"errors write nothing: {files_under(ed.project) - before}")


# ---------------------------------------------------------------------------- 10.1.2 preview

async def test_preview_writes_nothing(ed: Editor):
    before = files_under(ed.project)
    r = await gen(ed, source=BALL)
    v = r.structured
    check(files_under(ed.project) == before, "a preview writes nothing")
    check(len(r.images) == 1 and r.images[0][1] == "image/png" and r.images[0][0].startswith(b"\x89PNG"), "one PNG image")
    w, h, _, _ = decode_png(r.images[0][0])
    check(w <= 1024 and h <= 1024, f"sheet {w}x{h} within 1024")
    for key, kind in [("written", bool), ("source", dict), ("frames", dict), ("settings", dict), ("metrics", dict), ("assessment", list),
                      ("hint", str), ("warnings", list), ("default_output", dict), ("seconds", (int, float)), ("job_id", str), ("status", str)]:
        check(isinstance(v.get(key), kind), f"{key} is {kind}: {v.get(key)!r}")
    check(isinstance(v["radius_resolved"], (int, float)) or v["radius_resolved"] == "per-island", f"radius_resolved: {v['radius_resolved']}")
    check(v["written"] is False and v["status"] == "succeeded", f"preview: {v['written']} {v['status']}")
    check(v["source"] == {"path": BALL, "kind": "image", "width": 32, "height": 32}, f"source: {v['source']}")
    check(v["frames"]["from"] == "single" and v["frames"]["count"] == 1, f"frames: {v['frames']}")
    for key in ("coverage_pct", "flat_pct", "steep_pct", "mean_tilt_deg", "noise_pct", "lean_deg"):
        check(isinstance(v["metrics"].get(key), (int, float)), f"metrics.{key}: {v['metrics']}")
    check(v["default_output"]["normal"] == f"{SPR}/ball_normal.png" and v["default_output"]["atlas"] is None, f"default_output: {v['default_output']}")
    check(v["settings"]["preset"] == "default" and v["settings"]["strength"] == 1.5 and v["settings"]["detail"] == 3, f"settings: {v['settings']}")
    check("Preview only" in r.text and f"{SPR}/ball_normal.png" in r.text, f"text says how to write: {r.text}")
    d = (await gen(ed, source=BALL, preset="detailed")).structured["settings"]
    check((d["preset"], d["strength"], d["detail"], d["smoothing"], d["volume"]) == ("detailed", 1.5, 6, 0.6, 1.15), f"detailed: {d}")
    S["preview_sheet"] = r.images[0][0]
    S["preview_settings"] = v["settings"]


# ---------------------------------------------------------------------------- 10.1.3 write

async def test_write_matches_preview(ed: Editor):
    r = await gen(ed, source=BALL, output=True)
    v = r.structured
    out = ed.project / SPR / "ball_normal.png"
    check(v["written"] is True and v["files"] == [f"{SPR}/ball_normal.png"] and out.is_file(), f"written: {v.get('files')}")
    check(v["undo"] == [{"scope": "project", "name": "Generate Normal Map ball_normal.png"}], f"undo: {v['undo']}")
    check(r.images and r.images[0][0] == S["preview_sheet"], "the write returns the sheet the preview showed (same pixels)")
    _, _, src = rgba((ed.project / BALL).read_bytes())
    w, h, nrm = rgba(out.read_bytes())
    check((w, h) == (32, 32), "same size")
    for s, n in zip(src, nrm):
        check(s[3] == n[3], "alpha is the source's byte")
        if s[3] <= 7:
            check(n[:3] == (128, 128, 255), f"flat where transparent: {n}")
        else:
            x, y, z = (c / 255 * 2 - 1 for c in n[:3])
            check(abs(math.sqrt(x * x + y * y + z * z) - 1) <= 0.02, f"unit vector: {n}")
    rec = recipe(out)
    check(rec["format"] == "jasperengine.spritenormals" and rec["version"] == 2 and rec["role"] == "normal", f"recipe: {rec}")
    check(rec["source"]["sha1"] == hashlib.sha1((ed.project / BALL).read_bytes()).hexdigest() and rec["source"]["path"] == BALL, f"{rec['source']}")
    check(rec["settings"] == v["settings"], f"recipe settings = result settings: {rec['settings']} vs {v['settings']}")
    # deterministic: another write gives the same pixels
    other = f"{SPR}/again/ball_again.png"
    await gen(ed, source=BALL, output=other)
    check(rgba((ed.project / other).read_bytes())[2] == nrm, "same arguments, same pixels")
    await ed.call("undo", scope="project")
    check(not (ed.project / other).exists(), "undo removes the file")
    await ed.call("redo", scope="project")
    check((ed.project / other).is_file(), "redo writes it again")


# ---------------------------------------------------------------------------- 10.1.4 parity

async def test_upstream_parity(ed: Editor):
    out = f"{SPR}/golden_ball_normal.png"
    await gen(ed, source=GOLDEN, preset="detailed", params={"radius": 4}, output=True)
    _, _, got = rgba((ed.project / out).read_bytes())
    _, _, want = rgba((FIXTURES / "expected" / "ball_n.png").read_bytes())
    check(len(got) == len(want), "same size as upstream")
    worst = max(max(abs(a - b) for a, b in zip(p, q)) for p, q in zip(got, want))
    check(worst <= 1 and all(p[3] == q[3] for p, q in zip(got, want)), f"every channel within 1 of upstream (worst {worst})")


# ---------------------------------------------------------------------------- 10.1.5 overwrite

async def test_overwrite(ed: Editor):
    out = ed.project / SPR / "ball_normal.png"
    before = out.read_bytes()
    got = await gen_error(ed, source=BALL, output=True, preset="chunky")
    check("output_exists" in got["kinds"] and "overwrite: true" in got["text"] and f"{SPR}/ball_normal.png" in got["text"], f"refusal: {got['text']}")
    check(out.read_bytes() == before, "kept")
    await gen(ed, source=BALL, output=True, preset="chunky", overwrite=True)
    check(out.read_bytes() != before and recipe(out)["settings"]["preset"] == "chunky", "replaced")
    await ed.call("undo", scope="project")
    check(out.read_bytes() == before, "one undo puts the old bytes back")


# ---------------------------------------------------------------------------- 10.1.6 GameObject source

async def test_sprite_source_and_apply(ed: Editor):
    await ed.call("set_sprite_frames", create={"name": "Hero"}, texture=SHEET, slice={"tile_size": [32, 32]})
    await ed.call("set_texture_settings", entity="Hero", component="mesh", property="submeshes[0].material.baseColorTexture",
                  min_filter="nearest", mag_filter="nearest")
    r = await gen(ed, source="Hero")
    v = r.structured
    check(v["frames"]["from"] == "sprite" and v["frames"]["count"] == 4, f"the Sprite's frames: {v['frames']}")
    check(v["source"]["kind"] == "sprite" and v["source"]["entity"]["name"] == "Hero" and v["source"]["path"] == SHEET, f"source: {v['source']}")
    check("Sprite 'Hero'" in r.text, r.text)
    # without apply: the calls that wire it
    r = await gen(ed, source="Hero", output=f"{SPR}/hero_unapplied_normal.png")
    args = r.structured.get("apply_args")
    check(args and args[0]["tool"] == "set_properties" and "submeshes[0].material.normalTexture" in args[0]["arguments"]["values"]["mesh"], f"apply_args: {args}")
    check("Use it: set_properties" in r.text, r.text)
    await ed.call("undo", scope="project")
    r = await gen(ed, source="Hero", output=True, apply=True)
    v = r.structured
    check([u["scope"] for u in v["undo"]] == ["project", "scene"] and v["undo"][1]["name"] == "Assign Normal Map", f"undo: {v['undo']}")
    check(v["entity"]["name"] == "Hero" and v["applied"]["normal_texture"] is True, f"applied: {v}")
    mat = (await ed.call("get_entity", entity="Hero", components=["mesh"]))["components"]["mesh"]["submeshes[0].material"]
    check(mat["normalTexture"]["path"] == f"{SPR}/sheet_normal.png", f"normalTexture: {mat['normalTexture']}")
    check(mat["normalTexture"]["min_filter"] == mat["baseColorTexture"]["min_filter"] == "nearest" and mat["normalTexture"]["mag_filter"] == "nearest",
          f"filters copied: {mat['normalTexture']}")
    await ed.call("undo", scope="scene")
    mat = (await ed.call("get_entity", entity="Hero", components=["mesh"]))["components"]["mesh"]["submeshes[0].material"]
    check(not mat.get("normalTexture"), f"the scene undo clears the slot: {mat.get('normalTexture')}")
    check((ed.project / SPR / "sheet_normal.png").is_file(), "the file stays")
    await ed.call("redo", scope="scene")


# ---------------------------------------------------------------------------- 10.1.7 GIF / APNG

async def frame_times(ed: Editor, animation: int) -> list[int]:
    e = await ed.call("get_entity", entity=animation, components=["spriteanimation"], properties=["framesTime", "framesTimeSize"])
    comp = e["components"]["spriteanimation"]
    return list(comp["framesTime"][:comp["framesTimeSize"]])


async def test_gif_and_apng_apply(ed: Editor):
    await ed.call("create_entity", name="Torch", type="sprite")
    r = await gen(ed, source=GIF, output=True, apply="Torch")
    v = r.structured
    check(set(v["files"]) == {f"{SPR}/torch_sheet_normal.png", f"{SPR}/torch_sheet.png"} and v["files"][0] == f"{SPR}/torch_sheet_normal.png", f"files: {v['files']}")
    w, h, _, _ = ihdr(ed.project / SPR / "torch_sheet.png")
    check((w, h) == (64, 64) and v["frames"]["columns"] == 2 and v["frames"]["rows"] == 2, f"a 2x2 atlas: {w}x{h} {v['frames']}")
    check(v["frames"]["frame_ms"] == [50, 90, 100] and v["frames"]["from"] == "gif", f"frame_ms: {v['frames']}")
    check(any(x["kind"] == "zero_delay" for x in v["warnings"]), f"zero_delay: {v['warnings']}")
    check(v["undo"][1]["name"] == "Assign Animated Sprite" and v["applied"]["frames"] == 3, f"{v['undo']} {v['applied']}")
    sprite = (await ed.call("get_entity", entity="Torch", components=["sprite"]))["components"]["sprite"]
    check(sprite["numFramesRect"] == 3 and sprite["framesRect[2].rect"] == [0.0, 32.0, 32.0, 32.0], f"the Sprite's frames are the 3 cells: {sprite}")
    anim = v["applied"]["animation"]
    check(anim["name"] == "torch", f"the Sprite Animation is named after the source: {anim}")
    times = await frame_times(ed, anim["id"])
    check(times == [50, 90, 100], f"Sprite Animation frame times: {times}")
    # APNG likewise: 2 frames, 1/12 s and 1/20 s
    await ed.call("create_entity", name="Blink", type="sprite")
    r = await gen(ed, source=APNG, output=True, apply="Blink")
    v = r.structured
    check(v["frames"]["from"] == "apng" and v["frames"]["count"] == 2 and v["frames"]["frame_ms"] == [83, 50], f"APNG frames: {v['frames']}")
    check((ed.project / SPR / "twoframes_sheet.png").is_file() and (ed.project / SPR / "twoframes_sheet_normal.png").is_file(), "APNG atlases")
    check(await frame_times(ed, v["applied"]["animation"]["id"]) == [83, 50], "APNG frame times")


# ---------------------------------------------------------------------------- 10.1.8 warnings

def kinds(v: dict) -> list[str]:
    return [w["kind"] for w in v["warnings"]]


async def test_warnings(ed: Editor):
    v = (await gen(ed, source=TILE)).structured
    check("no_alpha" in kinds(v), f"opaque tile: {kinds(v)}")
    v = (await gen(ed, source=TILE, params={"edges": "extend"})).structured
    check("no_alpha" not in kinds(v), f"edges extend: {kinds(v)}")
    v = (await gen(ed, source="Hero")).structured
    check("no_light" in kinds(v), f"no Light 2D in the scene: {kinds(v)}")
    made = await ed.call("create_entity", name="Lamp", type="light_2d")
    await ed.call("set_properties", entity="Lamp", values={"light2d": {"height": 0}})
    v = (await gen(ed, source="Hero")).structured
    w = next((x for x in v["warnings"] if x["kind"] == "no_light_height"), None)
    check(w and "height above 0" in w["message"] and "64" in w["fix"], f"no_light_height with P2's text: {v['warnings']}")
    await ed.call("set_properties", entity="Lamp", values={"light2d": {"height": 64}})
    v = (await gen(ed, source="Hero")).structured
    check("no_light_height" not in kinds(v) and "no_light" not in kinds(v), f"height 64: {kinds(v)}")
    v = (await gen(ed, source="Hero", directx=True, output=f"{SPR}/dx/hero_dx.png", apply=True)).structured
    check("directx_in_engine" in kinds(v), f"directx with apply: {kinds(v)}")
    await ed.call("undo", scope="scene")
    await ed.call("undo", scope="project")
    check(made, "the light was made")


async def test_tilemap_source(ed: Editor):
    # a 32x16 tileset of two 16 px tiles on a Tilemap: frames {tile_size}, edges extend, apply
    rows = [b"".join(bytes((80 + 100 * (x // 16), 140, 60 + (x * y) % 50, 255)) for x in range(32)) for y in range(16)]
    (ed.project / SPR / "ground.png").write_bytes(png_rgba(32, 16, rows))
    # rects on a create make a Free Tilemap (S50 R75): this checks a Mesh tilemap's tileset map
    await ed.call("edit_tilemap", create={"name": "Ground"}, texture=f"{SPR}/ground.png",
                  rects=[{"name": "t0", "rect": [0, 0, 16, 16]}, {"name": "t1", "rect": [16, 0, 16, 16]}], cell_size=[16, 16],
                  ops=[{"op": "fill", "tile": 0, "from": [0, 0], "to": [3, 1]}, {"op": "set", "cells": [[1, 1, 1]]}])
    await ed.call("wait", frames=2)
    r = await gen(ed, source="Ground", frames={"tile_size": 16}, params={"edges": "extend"}, output=True, apply=True)
    v = r.structured
    check(isinstance(v.get("results"), list) and len(v["results"]) == 1 and v["results"][0]["frames"]["count"] == 2, f"one result per tileset: {v}")
    check(v["files"] == [f"{SPR}/ground_normal.png"] and v["entity"]["name"] == "Ground", f"files: {v.get('files')}")
    check("no_alpha" not in kinds(v["results"][0]), f"edges extend on tiles: {v['results'][0]['warnings']}")
    mat = (await ed.call("get_entity", entity="Ground", components=["mesh"]))["components"]["mesh"]["submeshes[0].material"]
    tex = mat["normalTexture"]
    check((tex if isinstance(tex, str) else tex["path"]) == f"{SPR}/ground_normal.png", f"the tileset's map: {tex}")


# ---------------------------------------------------------------------------- 10.1.9 / 10 / 11

async def test_height_map(ed: Editor):
    await gen(ed, source=BALL, output={"normal": f"{SPR}/h/ball_h_normal.png"}, height=True)
    out = ed.project / SPR / "ball_height.png"
    check(out.is_file(), "the height map beside the source")
    w, h, depth, color = ihdr(out)
    check((w, h, depth, color) == (32, 32, 16, 0), f"16-bit grey of the same size: {(w, h, depth, color)}")
    check(recipe(out)["role"] == "height", "its recipe")


def opaque_colours(px: list[tuple[int, int, int, int]]) -> set[tuple[int, int, int]]:
    return {p[:3] for p in px if p[3] > 0}


async def test_quantize_toon_steps(ed: Editor):
    """params.quantize (Toon steps): preview and write give the same palette-only pixels, the settings and recipe
    carry quantize, preset toon turns it on, regenerate reproduces both an override to off and an on-by-params map,
    bad values and unknown keys are refused with their fix"""
    r = await gen(ed, source=BALL, params={"quantize": {"directions": 4}})
    v = r.structured
    q = v["settings"]["quantize"]
    check(q == {"directions": 4, "tilts": 1, "flat_deg": 15, "rotation_deg": 0}, f"settings.quantize (missing keys at their defaults): {q}")
    check(v["written"] is False and v["settings"]["preset"] == "default", f"{v['settings']}")
    out = f"{SPR}/q/ball_q4.png"
    w = await gen(ed, source=BALL, params={"quantize": {"directions": 4}}, output=out)
    check(w.images and w.images[0][0] == r.images[0][0], "the write returns the previewed sheet")
    _, _, px = rgba((ed.project / out).read_bytes())
    check(1 < len(opaque_colours(px)) <= 5, f"at most 1 + 4 x 1 normals: {opaque_colours(px)}")
    check(recipe(ed.project / out)["settings"]["quantize"] == q, "the recipe records it")
    check(", toon steps 4," in r.text, f"the summary names Toon steps on a non-toon preset: {r.text[:300]}")
    turn = (await gen(ed, source=BALL, params={"quantize": {"directions": 8, "rotation_deg": 360}})).structured["settings"]["quantize"]
    check(turn["rotation_deg"] == 0, f"360 is 0: {turn}")
    plain = (await gen(ed, source=BALL)).structured["settings"]
    check("quantize" not in plain, f"off: no quantize key: {plain}")
    # the toon preset
    t = (await gen(ed, source=BALL, preset="toon")).structured["settings"]
    check(t["preset"] == "toon" and t["quantize"]["directions"] > 0, f"preset toon: {t}")
    toon_out = f"{SPR}/q/ball_toon.png"
    await gen(ed, source=BALL, preset="toon", output=toon_out)
    _, _, tpx = rgba((ed.project / toon_out).read_bytes())
    tq = t["quantize"]
    check(len(opaque_colours(tpx)) <= 1 + tq["directions"] * tq["tilts"], "toon: palette normals only")
    # toon turned off, written: regenerate keeps it off (and the on-by-params map stays on)
    off_out = f"{SPR}/q/ball_toon_off.png"
    await gen(ed, source=BALL, preset="toon", params={"quantize": {"directions": 0}}, output=off_out)
    _, _, opx = rgba((ed.project / off_out).read_bytes())
    check(len(opaque_colours(opx)) > 40, "toon with directions 0: smooth")
    for path, pixels_before in ((off_out, opx), (out, px)):
        regen = (await ed.call("get_asset_info", path=path))["sprite_normals"]["regenerate"]
        check("quantize" in regen.get("params", {}), f"{path}: regenerate carries quantize: {regen}")
        await gen(ed, **regen)
        check(rgba((ed.project / path).read_bytes())[2] == pixels_before, f"{path}: regenerate reproduces the pixels")
    regen = (await ed.call("get_asset_info", path=toon_out))["sprite_normals"]["regenerate"]
    check(regen["preset"] == "toon" and "quantize" not in regen.get("params", {}), f"the preset's own quantize is not repeated: {regen}")
    # refusals: the field, the value and the fix
    before = files_under(ed.project)
    for params, field, needle in [
        ({"quantize": {"directions": 5}}, "params.quantize.directions", "0 (off), 4, 6, 8, 12 or 16 (got 5)"),
        ({"quantize": {"directions": 8, "tilts": 7}}, "params.quantize.tilts", "between 1 and 4 (got 7)"),
        ({"quantize": {"directions": 8, "flat_deg": 60}}, "params.quantize.flat_deg", "between 0 and 45 (got 60)"),
        ({"quantize": {"directions": 8, "rotation_deg": 400}}, "params.quantize.rotation_deg", "between 0 and 360 (got 400)"),
        ({"quantize": {"directions": 8.5}}, "params", "expected a whole number"),
        ({"quantize": 8}, "params", "expected an object"),
        ({"quantize": {"steps": 8}}, "params.quantize", "unknown key \"steps\""),
    ]:
        got = await gen_error(ed, source=BALL, params=params)
        errors = got["data"]["errors"]
        check(got["kinds"] == ["invalid_settings"] and errors[0].get("field") == field and needle in errors[0]["message"],
              f"{params}: {field} / {needle!r}: {errors}")
        check(errors[0]["fix"], f"{params}: a fix: {errors}")
    got = await gen_error(ed, source=BALL, params={"quantize": {"steps": 8}})
    check("directions, tilts, flat_deg, rotation_deg" in got["data"]["errors"][0]["fix"], f"the valid keys: {got['data']['errors']}")
    got = await gen_error(ed, source=BALL, preset="cel")
    check("toon" in got["data"]["errors"][0]["message"], f"the presets listed: {got['data']['errors']}")
    check(files_under(ed.project) == before, "refusals write nothing")


async def test_frames_json(ed: Editor):
    r = await gen(ed, source=SHEET, frames={"tile_size": 32}, output={"normal": f"{SPR}/n/custom.png", "frames": True})
    check(set(r.structured["files"]) == {f"{SPR}/n/custom.png", f"{SPR}/n/custom.frames.json"}, f"files: {r.structured['files']}")
    j = json.loads((ed.project / SPR / "n" / "custom.frames.json").read_text())
    check(j["version"] == 1 and j["animation"]["frame_count"] == 4, f"frames JSON: {j}")
    check(r.structured["frames"]["from"] == "given" and r.structured["frames"]["count"] == 4, f"{r.structured['frames']}")


async def test_save_path(ed: Editor):
    before = files_under(ed.project)
    r = await gen(ed, source=BALL, save_path=f"{SPR}/sheets/ball_sheet_preview.png")
    path = ed.project / SPR / "sheets" / "ball_sheet_preview.png"
    check(path.read_bytes() == r.images[0][0], "save_path holds the returned sheet")
    check(files_under(ed.project) - before == {f"{SPR}/sheets/ball_sheet_preview.png"}, f"and nothing else: {files_under(ed.project) - before}")
    check(r.structured["written"] is False, "still a preview")


# ---------------------------------------------------------------------------- 10.1.12 job path

async def test_job_and_cancel(ed: Editor):
    r = await ed.call_raw("generate_sprite_normals", {"source": BIG, "preset": "detailed", "timeout_seconds": 1})
    check(not r.is_error, r.text)
    v = r.structured
    job = v.get("job_id") or v.get("id")
    check(job and v.get("status") in ("running", "succeeded"), f"a job id, running or done: {v}")
    state = await poll(ed, job)
    check(state["status"] == "succeeded" and "metrics" in state["result"] and state["result"]["source"]["path"] == BIG, f"get_job: {state}")
    out = f"{SPR}/cancelled_normal.png"
    r = await ed.call_raw("generate_sprite_normals", {"source": BIG, "preset": "detailed", "output": out, "wait": False})
    job = r.structured.get("job_id") or r.structured.get("id")
    await ed.call("cancel_job", job_id=job)
    state = await poll(ed, job)
    check(state["status"] == "cancelled", f"cancelled: {state['status']} {state.get('message')}")
    check(not (ed.project / out).exists(), "a cancelled job writes nothing")


# ---------------------------------------------------------------------------- 10.1.13 asset info

async def test_asset_info_and_staleness(ed: Editor):
    path = f"{SPR}/ball_normal.png"
    info = await ed.call("get_asset_info", path=path)
    sn = info.get("sprite_normals")
    check(sn and sn["source"]["state"] == "fresh" and sn["stale"] is False and sn["role"] == "normal" and sn["source"]["path"] == BALL, f"{sn}")
    regen = sn["regenerate"]
    check(regen["source"] == BALL and regen["overwrite"] is True and regen["output"]["normal"] == path and regen["frames"] == "auto", f"regenerate: {regen}")
    before = rgba((ed.project / path).read_bytes())[2]
    await gen(ed, **regen)
    check(rgba((ed.project / path).read_bytes())[2] == before, "regenerate reproduces the pixels")
    # a grid: the cells come back as {cells}
    grid = await ed.call("get_asset_info", path=f"{SPR}/n/custom.png")
    regen = grid["sprite_normals"]["regenerate"]
    check(isinstance(regen["frames"], dict) and len(regen["frames"]["cells"]) == 4, f"grid regenerate frames: {regen['frames']}")
    before = rgba((ed.project / SPR / "n" / "custom.png").read_bytes())[2]
    await gen(ed, **regen)
    check(rgba((ed.project / SPR / "n" / "custom.png").read_bytes())[2] == before, "the grid map's regenerate reproduces its pixels")
    # the source: its normal map
    src = await ed.call("get_asset_info", path=BALL)
    check(src.get("normal_map") == {"path": path, "stale": False}, f"normal_map on the source: {src.get('normal_map')}")
    view = await ed.call_raw("view_asset", {"path": path})
    check(not view.is_error and "Sprite normal map" in view.text, f"view_asset: {view.text}")
    # the source edited on disk: the next call's external_changes names the map, the state is changed
    (ed.project / BALL).write_bytes(png_rgba(32, 32, ball_rows(32, 15)))
    r = await ed.call_raw("get_asset_info", {"path": path})
    problems = ((r.structured or {}).get("external_changes") or {}).get("problems") or []
    check(any(p.get("path") == path and "regenerate" in p.get("message", "") for p in problems), f"external_changes.problems: {r.structured.get('external_changes')}")
    sn = r.structured["sprite_normals"]
    check(sn["source"]["state"] == "changed" and sn["stale"] is True, f"changed: {sn['source']}")
    check("the source changed since" in r.text, r.text)
    src = await ed.call("get_asset_info", path=BALL)
    check(src["normal_map"]["stale"] is True, f"the source's map is stale: {src['normal_map']}")
    (ed.project / BALL).write_bytes(png_rgba(32, 32, ball_rows()))


# ---------------------------------------------------------------------------- 10.1.14 playing

async def test_apply_refused_while_playing(ed: Editor):
    out = f"{SPR}/play/hero_play.png"
    await ed.call("play", action="start")
    try:
        got = await gen_error(ed, source="Hero", output=out, apply=True)
        check("scene_playing" in got["kinds"] and "play" in got["text"], f"scene_playing: {got}")
        check(not (ed.project / out).exists(), "nothing written")
        r = await gen(ed, source=BALL)
        check(r.structured["written"] is False and r.images, "a preview works while playing")
    finally:
        await ed.call("play", action="stop")


# ---------------------------------------------------------------------------- fix round 1

async def test_files_without_a_recipe_chunk_report_none(ed: Editor):
    # a JPEG named .png, a truncated PNG and garbage: none has a sprite normals recipe to report
    spr = ed.project / SPR
    (spr / "photo.png").write_bytes((REPO / "engine/tests/htmlui/fixtures/compat/net/img/photo.jpg").read_bytes())
    (spr / "trunc.png").write_bytes(b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00")
    (spr / "garbage.png").write_bytes(b"not an image at all")
    for name in ("photo.png", "trunc.png", "garbage.png"):
        r = await ed.call_raw("get_asset_info", {"path": f"{SPR}/{name}"})
        check(not r.is_error and "sprite_normals" not in (r.structured or {}) and "sprite normal map recipe" not in r.text,
              f"get_asset_info {name}: no sprite_normals block: {r.text[:300]} {r.structured}")
    view = await ed.call_raw("view_asset", {"path": f"{SPR}/photo.png"})
    check("sprite_normals" not in (view.structured or {}) and "sprite normal map recipe" not in view.text, f"view_asset: {view.text[:300]}")
    for name in ("photo.png", "trunc.png", "garbage.png"):
        (spr / name).unlink()


async def test_save_path_failure_writes_nothing(ed: Editor):
    (ed.project / SPR / "blocker").write_text("a file where a folder is expected")
    out = f"{SPR}/sp/ball_sp.png"
    got = await gen_error(ed, source="Hero", output=out, apply=True, save_path=f"{SPR}/blocker/sheet.png")
    check("write_failed" in got["kinds"] and "save_path" in got["text"], f"the sheet's failure: {got}")
    check(not (ed.project / out).exists(), "the normal map is not written")
    mat = (await ed.call("get_entity", entity="Hero", components=["mesh"]))["components"]["mesh"]["submeshes[0].material"]
    tex = mat.get("normalTexture")
    check(not tex or (tex if isinstance(tex, str) else tex.get("path")) != out, f"nothing applied: {tex}")
    (ed.project / SPR / "blocker").unlink()


async def test_save_path_rolls_back_when_a_later_step_fails(ed: Editor):
    # the sheet is written first; the apply then refuses (Hero draws sheet.png, not ball.png):
    # the files are taken back, the sheet with the folders made for it, an old sheet restored
    spr = ed.project / SPR
    out = f"{SPR}/rb/ball_rb.png"
    got = await gen_error(ed, source=BALL, output=out, apply="Hero", save_path=f"{SPR}/rbs/new/sheet.png")
    check("not_a_sprite" in got["kinds"], f"the apply refusal: {got}")
    check(not (ed.project / out).exists(), "the normal map is taken back")
    check(not (spr / "rbs").exists(), "the sheet and the folders made for it are removed")
    keep = spr / "keep_sheet.png"
    keep.write_bytes(b"previous bytes")
    got = await gen_error(ed, source=BALL, output=out, apply="Hero", save_path=f"{SPR}/keep_sheet.png")
    check("not_a_sprite" in got["kinds"] and keep.read_bytes() == b"previous bytes", f"the replaced sheet gets its bytes back: {got['kinds']}")
    keep.unlink()


async def test_outputs_through_symbolic_links(ed: Editor):
    # a folder of the assets that is a link to a folder outside the project: no output, nor the
    # sheet, is written through it (output_invalid); a link to a folder of the assets is fine; an
    # output or save_path that is the source through a link is refused
    if not symlinks_permitted():
        skip("this process may not create symbolic links")
    if not (ed.project / BALL).is_file():
        write_inputs(ed.project)
    spr = ed.project / SPR
    outside = Path(tempfile.mkdtemp(prefix="jasper-normals-outside-"))
    os.symlink(outside, spr / "out")
    os.symlink("ball.png", spr / "ball_alias.png")
    # dangling links: files and a folder outside that don't exist (yet)
    os.symlink(outside / "dangling_sheet.png", spr / "dl_sheet.png")
    os.symlink(outside / "dangling_normal.png", spr / "dl_normal.png")
    os.symlink(outside / "missing_folder", spr / "dl_dir")
    (spr / "real").mkdir(exist_ok=True)
    os.symlink(spr / "real", spr / "inside")
    try:
        before = files_under(ed.project)
        cases = [
            {"output": f"{SPR}/out/ball_n.png"},
            {"output": {"normal": f"{SPR}/ln/ball_n.png", "height": f"{SPR}/out/ball_h.png"}, "height": True},
            {"output": {"normal": f"{SPR}/ln/ball_n.png", "frames": f"{SPR}/out/ball.frames.json"}, "frames": {"tile_size": 16}},
            {"save_path": f"{SPR}/out/sheet.png"},
            {"output": f"{SPR}/ln/ball_n.png", "save_path": f"{SPR}/out/sheet.png"},
            {"save_path": f"{SPR}/dl_sheet.png"},
            {"save_path": f"{SPR}/dl_dir/sheet.png"},
            {"output": f"{SPR}/dl_normal.png", "overwrite": True},
            {"output": f"{SPR}/dl_dir/ball_n.png"},
            {"output": {"normal": f"{SPR}/ln/ball_n.png", "height": f"{SPR}/dl_dir/ball_h.png"}, "height": True},
        ]
        for args in cases:
            got = await gen_error(ed, source=BALL, **args)
            check("output_invalid" in got["kinds"] and "symbolic link" in got["text"], f"{args}: output_invalid through the link: {got['kinds']} {got['text'][:400]}")
        got = await gen_error(ed, source=GIF, output={"atlas": f"{SPR}/out/torch_sheet.png"})
        check("output_invalid" in got["kinds"] and "symbolic link" in got["text"], f"the atlas through the link: {got['kinds']} {got['text'][:400]}")
        check(not any(outside.iterdir()), f"nothing written outside (dangling links included): {list(outside.iterdir())}")
        check(files_under(ed.project) == before, f"nothing written: {files_under(ed.project) - before}")
        # the source through a link: never replaced, even with overwrite
        src = (spr / "ball.png").read_bytes()
        got = await gen_error(ed, source=BALL, output=f"{SPR}/ball_alias.png", overwrite=True)
        check("output_invalid" in got["kinds"], f"output = the source through a link: {got['kinds']}")
        got = await gen_error(ed, source=BALL, save_path=f"{SPR}/ball_alias.png")
        check("output_invalid" in got["kinds"], f"save_path = the source through a link: {got['kinds']}")
        check((spr / "ball.png").read_bytes() == src, "the source is unchanged")
        # a link to a folder of the assets is fine (the sheet too)
        await gen(ed, source=BALL, output=f"{SPR}/inside/ball_in.png", save_path=f"{SPR}/inside/ball_in_sheet.png")
        check((spr / "real" / "ball_in.png").is_file() and (spr / "real" / "ball_in_sheet.png").is_file(), "written through the inside link")
        await ed.call("undo", scope="project")
    finally:
        for name in ("out", "ball_alias.png", "inside", "dl_sheet.png", "dl_normal.png", "dl_dir"):
            (spr / name).unlink()
        shutil.rmtree(spr / "real", ignore_errors=True)
        shutil.rmtree(outside, ignore_errors=True)


# ---------------------------------------------------------------------------- 10.4 docs

# ---------------------------------------------------------------------------- high resolution (hires design spec)

def kinds_of(data: dict) -> list[str]:
    return [w["kind"] for w in data.get("warnings", [])]


async def test_high_resolution_settings_presets_and_metrics(ed: Editor):
    hires = FIXTURES / "hires"
    for name in ("potion.png", "crate.png", "cobbles_256.png"):
        shutil.copy(hires / name, ed.project / SPR / name)
    await ed.call("sync_files")
    potion, crate, tile = f"{SPR}/potion.png", f"{SPR}/crate.png", f"{SPR}/cobbles_256.png"
    # the default above 128 px: auto -> inflate, scaled, smooth (no creased / streaky)
    data = (await gen(ed, source=potion)).structured
    check("inflate" in data["profile_resolved"] and data["scale_range"][1] > 1.2, f"{data['profile_resolved']} {data['scale_range']}")
    m = data["metrics"]
    check(m["crease_pct"] < 1 and m["roughness_deg"] < 13, f"smooth: {m}")
    check(not {"creased", "streaky"} & set(kinds_of(data)), f"{kinds_of(data)}")
    check(data["settings"]["profile"] == "auto" and data["settings"]["resolution_scale"] == "auto", f"{data['settings']}")
    # the rounded profile there: creased, with the fix
    data = (await gen(ed, source=potion, params={"profile": "rounded"})).structured
    check("creased" in kinds_of(data) and "inflate" in data["hint"], f"rounded: {kinds_of(data)} {data['hint']}")
    # the presets
    data = (await gen(ed, source=crate, preset="props")).structured
    check(data["profile_resolved"] == ["dome"] and data["settings"]["volume"] == 0.7, f"props: {data['profile_resolved']} {data['settings']}")
    data = (await gen(ed, source=tile, preset="tile")).structured
    check(data["settings"]["edges"] == "wrap" and "no_alpha" not in kinds_of(data), f"tile: {data['settings']} {kinds_of(data)}")
    data = (await gen(ed, source=BALL, preset="painted")).structured
    check(data["profile_resolved"] == ["inflate"] and data["scale_range"] == [1, 1], f"painted on pixel art: {data['profile_resolved']}")
    # resolution scale off, the new limits, the names
    data = (await gen(ed, source=potion, params={"resolution_scale": "off", "smoothing": 32, "softness": 32})).structured
    check(data["scale_range"] == [1, 1], f"off: {data['scale_range']}")
    data = (await gen(ed, source=potion, params={"soft_edge": "solid"})).structured
    check(data["settings"]["soft_edge"] == "solid", f"soft_edge: {data['settings']}")
    # a few painted dots don't make "no crease": not measured (null), no creased warning
    dots = [[x, y] for x in range(20, 360, 40) for y in range(20, 360, 40)]
    data = (await gen(ed, source=potion, params={"profile": "rounded"}, paint=[{"op": "flatten", "where": {"pixels": dots}}])).structured
    check(data["metrics"]["crease_pct"] is None and "creased" not in kinds_of(data), f"sparse paint: {data['metrics']} {kinds_of(data)}")
    check("Creases not measured: paint covers most of the sprite" in data["assessment"], f"{data['assessment']}")
    for params, field in [({"smoothing": 33}, "params.smoothing"), ({"softness": -1}, "params.softness"), ({"resolution_scale": "on"}, "params"),
                          ({"soft_edge": "hard"}, "params"),
                          ({"profile": "pillow"}, "params"), ({"version": 3}, "params.version")]:
        err = await gen_error(ed, source=potion, params=params)
        check(err["kinds"] == ["invalid_settings"] and (err["data"]["errors"][0].get("field") == field), f"{params}: {err['data']['errors']}")
    err = await gen_error(ed, source=potion, preset="huge")
    check("painted, props, tile" in err["data"]["errors"][0]["message"], err["data"]["errors"][0]["message"])
    # a map made with version 1 rules keeps them when regenerated (recipe version 1, regenerate params.version 1)
    out = f"{SPR}/n/potion_v1.png"
    await gen(ed, source=potion, params={"version": 1}, output=out, overwrite=True)
    rec = recipe(ed.project / out)
    check(rec["version"] == 1 and rec["settings"]["version"] == 1, f"recipe: {rec['version']} {rec['settings']}")
    info = await ed.call("get_asset_info", path=out)
    regen = info["sprite_normals"]["regenerate"]
    check(regen.get("params", {}).get("version") == 1, f"regenerate params: {regen}")
    before = rgba((ed.project / out).read_bytes())[2]
    await gen(ed, **regen)
    check(rgba((ed.project / out).read_bytes())[2] == before, "a version 1 map regenerates its pixels")
    # and a version 2 map's recipe records the new settings
    out2 = f"{SPR}/n/potion_v2.png"
    await gen(ed, source=potion, output=out2, overwrite=True)
    rec = recipe(ed.project / out2)
    check(rec["version"] == 2 and "version" not in rec["settings"] and rec["settings"]["resolution_scale"] == "auto", f"{rec}")


async def test_description_and_guide(ed: Editor):
    tool = ed.tool("generate_sprite_normals")
    check(tool and "jasper://docs/sprite-normals" in tool.description, "the description points at the guide")
    size = len(tool.name) + len(tool.description) + len(json.dumps(tool.input_schema, separators=(",", ":")))
    check(size <= 1500, f"the definition stays lean: {size} characters")
    # the agent guide indexes areas and keeps only what an eval case needs: the route is the listing,
    # this description and the docs index
    docs = await ed.session.read_resource("jasper://docs")
    check("jasper://docs/sprite-normals" in "".join(getattr(c, "text", "") or "" for c in docs.contents),
          "the docs index lists the guide")
    got = await ed.session.read_resource("jasper://docs/sprite-normals")
    text = "".join(getattr(c, "text", "") or "" for c in got.contents)
    check("generate_sprite_normals" in text, "the guide is served")


# ---------------------------------------------------------------------------- 10.1.15 (last: switches projects)

async def test_project_switch_during_a_run(ed: Editor):
    original = ed.project.resolve()
    out = f"{SPR}/switch_normal.png"
    # the job has to outlast create_project, and how long each takes depends on the machine: a job
    # that finished first tested nothing, so again, heavier and with the new project's shaders warm
    for attempt, smoothing in enumerate((8, 16, 32)):
        other = ed.scratch.root / "switched" / f"Other{attempt}"
        r = await ed.call_raw("generate_sprite_normals", {"source": BIG, "preset": "detailed", "params": {"smoothing": smoothing}, "output": out, "wait": False})
        check(not r.is_error, r.text)
        job = r.structured.get("job_id") or r.structured.get("id")
        await ed.call("create_project", path=str(other), name="Other", unsaved="discard")
        state = await poll(ed, job)
        if state["status"] != "succeeded":
            break
        (original / out).unlink(missing_ok=True)
        await ed.call("open_project", path=str(original), unsaved="discard")
    check(state["status"] == "failed" and "project_changed" in json.dumps(state), f"the job says why: {state}")
    check(not (original / out).exists() and not (other / out).exists(), "nothing written in either project")
    await ed.call("open_project", path=str(original), unsaved="discard")
