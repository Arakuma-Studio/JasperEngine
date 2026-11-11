"""bake_vertex_animation (editor/mcp/tools/VatTools.cpp; vertex animation spec 6) and the .vat parts of
the asset tools, on a new project with the baker fixtures (tools/fixtures/vertex-animation:
rigid_arm.gltf "Swing"/"Stretch", morph_blob.gltf "Pulse"; tools/fixtures/pose-animation
hero_animated.gltf "Walk"/"Idle"/"Wave") and synthetic Houdini VAT 3 / OpenVAT exports written here.

- argument errors are isError results with a fix and the valid choices, nothing written;
- a preview (no output) writes nothing and returns the report of spec 6 (mode, mode_reason, vertices,
  bones, clips, texture, bytes, errors, fetches, warnings with fixes) and a contact sheet PNG (source
  rows vs VAT rows at 4 times per clip); the source and VAT rows of a rigid bake match;
- the same arguments + output write exactly that bake (the .vat's layout equals the preview's), one
  undo step (scope project; undo deletes both files, redo writes them back);
- output true takes the default name; the same source and clips again rewrite it (same GUID);
  other clips of it, or another source with that default name, take a free name ("rigid_arm 2.vat");
  an explicit existing output is
  refused without overwrite;
- rebake keeps hand edits (clip rename, speed, events) and the GUID;
- a long bake is a job (timeout_seconds) that get_job follows; cancel_job cancels one, nothing written;
- place beside / replace (one scene undo step; the source disabled) and crowd;
- imports of a synthetic OpenVAT export (preview + write) and a Houdini VAT 3 soft export;
- get_asset_info on a .vat (clips, layout, staleness, users) and on its .vat.glb (generated_by);
  list_assets type vertex_animation; view_asset refuses; write_file validates (generated fields refused
  with the rebake message, editable ones accepted with warnings); auto-sync reports a broken .vat;
- move_asset / delete_asset keep the .vat / .vat.glb pair; a move or copy to another folder rewrites the
  .vat.glb's texture URIs (its JSON chunk only) so they name the same files, undo puts them back; undoing
  the copy of a .vat.glb drops the copied .vat's GUID from the asset database;
- play: the instances of a crowd move differently (randomStart), and a clip event reaches a Lua script.
"""

from __future__ import annotations

import asyncio
import base64
import json
import math
import shutil
import struct
import tempfile
import zlib
from pathlib import Path

from harness import Editor, TestFailure
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "VatBake"}
CI = True
TIMEOUT = 1500

REPO = Path(__file__).resolve().parents[3]
MODELS = "assets/models"
ARM = f"{MODELS}/rigid_arm.gltf"
BLOB = f"{MODELS}/morph_blob.gltf"
HERO = f"{MODELS}/hero.gltf"

S: dict = {}


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def files_under(project: Path) -> set[str]:
    # (not the asset database's sidecars, S04: it gives a file it adopts a .meta in the
    # background, whatever reads the file)
    return {p.relative_to(project).as_posix() for p in (project / "assets").rglob("*") if p.is_file() and p.suffix != ".meta"}


def copy_fixtures(project: Path) -> None:
    models = project / MODELS
    models.mkdir(parents=True, exist_ok=True)
    vat = REPO / "tools" / "fixtures" / "vertex-animation" / "assets" / "models"
    shutil.copy(vat / "rigid_arm.gltf", models / "rigid_arm.gltf")
    shutil.copy(vat / "morph_blob.gltf", models / "morph_blob.gltf")
    shutil.copy(REPO / "tools" / "fixtures" / "pose-animation" / "assets" / "models" / "hero_animated.gltf", models / "hero.gltf")


KEEP = Path(tempfile.gettempdir()) / "jasper-vat-bake-images"


def keep_image(name: str, data: bytes) -> None:
    """The contact sheets are kept for a look (tmp/jasper-vat-bake-images)."""
    KEEP.mkdir(parents=True, exist_ok=True)
    (KEEP / name).write_bytes(data)


def read_vat(project: Path, rel: str) -> dict:
    return json.loads((project / rel).read_text())


async def bake(ed: Editor, **args):
    r = await ed.call_raw("bake_vertex_animation", args, timeout=600)
    if r.is_error:
        fail(f"bake_vertex_animation {args} failed: {r.text}")
    return r


async def scene(ed: Editor) -> int:
    if "scene" not in S:
        made = await ed.call("create_scene", name="Crowd", type="3D", path="scenes/Crowd")
        S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    return S["scene"]


# ---------------------------------------------------------------------------- tests

async def test_argument_errors(ed: Editor):
    copy_fixtures(ed.project)
    await ed.call("sync_files")
    ed.require_tools("bake_vertex_animation")
    before = files_under(ed.project)
    cases = [
        ({}, "source is required"),
        ({"source": ARM, "sorce": 1}, "sorce"),
        ({"source": "assets/models/nope.gltf"}, "neither a project file nor a GameObject"),
        ({"source": ARM, "mode": "pixels"}, "mode must be auto, vertices or bones"),
        ({"source": ARM, "preset": "console"}, "mobile_web"),
        ({"source": ARM, "fps": 0}, "fps must be"),
        ({"source": ARM, "clips": ["Fly"]}, "choose from: Swing, Stretch"),
        ({"source": ARM, "output": "assets/models/arm.png"}, "output must be a .vat path"),
        ({"source": ARM, "output": "../arm.vat"}, "outside the project"),
        ({"source": ARM, "place": "sideways"}, "place must be none, beside, replace"),
        ({"source": ARM, "on": BLOB}, "on is for .anim"),
        ({"source": ARM, "import": {"axis": "unity"}}, "import options are for"),
        ({"source": ARM, "rebake": "assets/models/rigid_arm.vat"}, "rebake re-runs"),
        ({"source": f"{ARM}#"}, "names no clip"),
        ({"source": ARM, "overwrite": True}, "overwrite needs output"),
        ({"source": ARM, "place": {"crowd": {"count": 0}}}, "count is an integer"),
    ]
    for args, needle in cases:
        msg = await ed.call("bake_vertex_animation", args, expect_error=True)
        check(needle in msg, f"{args}: the error says {needle!r}: {msg}")
    check(files_under(ed.project) == before, f"errors write nothing: {files_under(ed.project) - before}")


async def test_preview_report_and_sheet(ed: Editor):
    before = files_under(ed.project)
    r = await bake(ed, source=ARM, clips=["Swing"], fps=20)
    v = r.structured
    check(files_under(ed.project) == before, f"a preview writes nothing: {files_under(ed.project) - before}")
    for key in ("mode", "mode_reason", "vertices", "bones", "clips", "texture", "bytes", "max_error_mm", "mean_error_mm",
                "fetches_per_vertex", "warnings"):
        check(key in v, f"the report has {key}: {sorted(v)}")
    check(v["mode"] == "bones", f"a rigid hierarchy bakes as bones: {v['mode']} ({v['mode_reason']})")
    check(v["written"] is False, "not written")
    check([c["name"] for c in v["clips"]] == ["Swing"] and v["clips"][0]["frames"] > 1, f"clips: {v['clips']}")
    check(set(v["texture"]) >= {"width", "height", "format"}, f"texture: {v['texture']}")
    check(v.get("default_output") == "assets/models/rigid_arm.vat", f"the default output: {v.get('default_output')}")
    check("Preview only" in r.text and "output" in r.text, f"the text says how to write: {r.text}")
    check(len(r.images) == 1, f"one contact sheet: {len(r.images)}")
    w, h, channels, rows = decode_png(r.images[0][0])
    check(w > 4 * 100 and h > 2 * 100, f"4 columns x (source + VAT) rows: {w}x{h}")
    keep_image("vat_sheet_arm.png", r.images[0][0])
    # the source row and the VAT row of a rigid bake are the same picture (only the labels differ)
    cell = (h - 15) // 2
    def lit(row_index: int) -> int:
        count = 0
        for y in range(15 + row_index * (cell + 1), 15 + row_index * (cell + 1) + cell - 14):
            row = rows[y]
            for x in range(0, w * channels, channels):
                if row[x] > 60:
                    count += 1
        return count
    a, b = lit(0), lit(1)
    check(a > 200 and abs(a - b) <= max(30, a // 20), f"the source and VAT rows show the same shapes: {a} vs {b} lit pixels")
    S["preview"] = v


def lit_rows(png: bytes, rows_count: int) -> list[int]:
    """Lit pixels per sheet row (the cells, not the label strips) of a contact sheet."""
    w, h, channels, rows = decode_png(png)
    cell = (h - 15 - (rows_count - 1)) // rows_count - 14
    out = []
    for r in range(rows_count):
        top = 15 + r * (cell + 14 + 1)
        count = 0
        for y in range(top, top + cell):
            row = rows[y]
            for x in range(0, w * channels, channels):
                if row[x] > 60 or row[x + 1] > 60 or row[x + 2] > 60:
                    count += 1
        out.append(count)
    return out


async def test_auto_mode_per_source_and_their_sheets(ed: Editor):
    hero = await bake(ed, source=f"{HERO}#Walk", fps=15)
    v = hero.structured
    check(v["mode"] == "bones" and v["bones"] > 0 and v["fetches_per_vertex"] == 16, f"a skinned hero bakes as bones: {v['mode']} {v['mode_reason']}")
    keep_image("vat_sheet_hero.png", hero.images[0][0])
    src, vat = lit_rows(hero.images[0][0], 2)
    check(src > 300 and abs(src - vat) <= max(40, src // 10), f"the hero's source and VAT rows agree: {src} vs {vat}")
    blob = await bake(ed, source=BLOB)
    v = blob.structured
    check(v["mode"] == "vertices" and "morph" in v["mode_reason"].lower(), f"a morph target bakes as vertices: {v['mode']} ({v['mode_reason']})")
    keep_image("vat_sheet_blob.png", blob.images[0][0])
    forced = await bake(ed, source=ARM, clips=["Swing"], mode="vertices", normals=False)
    check(forced.structured["mode"] == "vertices" and forced.structured["fetches_per_vertex"] == 2, f"forced vertices without normals: {forced.structured}")


async def test_write_exactly_the_preview_one_undo_step(ed: Editor):
    before = files_under(ed.project)
    r = await bake(ed, source=ARM, clips=["Swing"], fps=20, output=True)
    v = r.structured
    vat, mesh = "assets/models/rigid_arm.vat", "assets/models/rigid_arm.vat.glb"
    check(v["written"] is True and v["vat"] == vat and v["mesh"] == mesh, f"written: {v}")
    check(v["undo"] == {"scope": "project"} and "scope project" in r.text, f"the undo scope is named: {r.text}")
    check(set(v["files"]) == {vat, mesh}, f"files: {v['files']}")
    check(not r.images, "a write returns no sheet")
    data = read_vat(ed.project, vat)
    check(data["format"] == "jasperengine.vat" and data["mode"] == "bones" and data["mesh"] == "rigid_arm.vat.glb", f".vat: {data}")
    p = S["preview"]
    check([data["layout"]["width"], data["layout"]["height"]] == [p["texture"]["width"], p["texture"]["height"]],
          f"the written layout is the previewed one: {data['layout']} vs {p['texture']}")
    check([c["frames"] for c in data["clips"]] == [c["frames"] for c in p["clips"]], "same frames")
    check(v["bytes"] == p["bytes"] and v["max_error_mm"] == p["max_error_mm"], "same report")
    S["guid"] = data["guid"]
    await ed.call("undo", scope="project")
    check(files_under(ed.project) == before, f"one undo removes both files: {files_under(ed.project) - before}")
    await ed.call("redo", scope="project")
    check((ed.project / vat).is_file() and (ed.project / mesh).is_file(), "redo writes them again")
    check(read_vat(ed.project, vat)["guid"] == S["guid"], "the same asset")


async def test_default_name_rule_and_overwrite(ed: Editor):
    vat = "assets/models/rigid_arm.vat"
    # the same source and clips again: the default file is rewritten, GUID kept
    r = await bake(ed, source=ARM, clips=["Swing"], output=True, fps=20)
    v = r.structured
    check(v["vat"] == vat and v["rewritten"] is True and v["guid"] == S["guid"], f"same source: rewritten in place: {v}")
    # other clips of the same model: another asset, the first one untouched
    r = await bake(ed, source=ARM, output=True, fps=20)
    check(r.structured["vat"] == "assets/models/rigid_arm 2.vat" and r.structured["rewritten"] is False,
          f"another clip set takes a free name: {r.structured['vat']}")
    check([c["name"] for c in read_vat(ed.project, vat)["clips"]] == ["Swing"], "the first bake is untouched")
    # another source whose default name is the same: a free name
    made = await ed.call("edit_animation", clip="assets/models/rigid_arm.anim", create={"from": f"{ARM}#Swing"})
    check(made.get("created"), f"the .anim: {made}")
    r = await bake(ed, source=["assets/models/rigid_arm.anim"], on=ARM, output=True)
    check(r.structured["vat"] == "assets/models/rigid_arm 3.vat" and r.structured["rewritten"] is False,
          f"another source takes a free name: {r.structured['vat']}")
    # an explicit path that exists: refused without overwrite, replaced with it (one undo puts it back)
    before = (ed.project / vat).read_bytes()
    msg = await ed.call("bake_vertex_animation", {"source": BLOB, "output": vat}, expect_error=True)
    check("output_exists" in msg and "overwrite" in msg, f"refused with the fix: {msg}")
    r = await bake(ed, source=BLOB, output=vat, overwrite=True)
    check(read_vat(ed.project, vat)["clips"][0]["name"] == "Pulse", "overwritten")
    check(read_vat(ed.project, vat)["guid"] == S["guid"], "an overwrite keeps the file's GUID")
    await ed.call("undo", scope="project")
    check((ed.project / vat).read_bytes() == before, "undo puts the previous bake back")


async def test_rebake_keeps_hand_edits(ed: Editor):
    vat = "assets/models/rigid_arm.vat"
    text = (ed.project / vat).read_text()
    data = json.loads(text)
    swing = next(c for c in data["clips"] if c["name"] == "Swing")
    swing["name"] = "Sway"
    swing["speed"] = 1.5
    swing["events"] = [{"time": 0.25, "function": "onStep", "args": ["left"]}]
    w = await ed.call("write_file", path=vat, content=json.dumps(data, indent=2) + "\n")
    check("Valid Vertex Animation" in json.dumps(w) or not w.get("warnings"), f"an editable change is accepted: {w}")
    r = await bake(ed, rebake=vat)
    v = r.structured
    check(v["vat"] == vat and v["guid"] == S["guid"] and v["rewritten"], f"rebaked in place: {v}")
    after = read_vat(ed.project, vat)
    sway = next((c for c in after["clips"] if c["name"] == "Sway"), None)
    check(sway is not None and sway["speed"] == 1.5 and sway["events"][0]["function"] == "onStep",
          f"the rename, speed and event survive the rebake: {after['clips']}")
    msg = await ed.call("bake_vertex_animation", {"rebake": vat, "fps": 10}, expect_error=True)
    check("rebake re-runs" in msg, msg)


async def test_job_and_cancel(ed: Editor):
    big = {"source": HERO, "mode": "vertices", "fps": 240, "preset": "desktop"}
    r = await ed.call_raw("bake_vertex_animation", {**big, "timeout_seconds": 1}, timeout=600)
    check(not r.is_error, r.text)
    v = r.structured
    check(v.get("status") == "running" and v.get("job_id"), f"a long bake returns a job: {v}")
    state = {}
    for _ in range(1200):
        state = await ed.call("get_job", job_id=v["job_id"])
        if state["status"] != "running":
            break
        await asyncio.sleep(0.5)
    check(state["status"] == "succeeded", f"the job finishes: {state}")
    check(state["result"]["mode"] == "vertices" and state["result"]["written"] is False, f"its result is the report: {state['result']}")

    out = "assets/vat/cancelled.vat"
    r = await ed.call_raw("bake_vertex_animation", {**big, "output": out, "wait": False})
    job = r.structured["job_id"]
    cancel = await ed.call("cancel_job", job_id=job)
    check("ancel" in json.dumps(cancel), f"cancel_job: {cancel}")
    for _ in range(200):
        state = await ed.call("get_job", job_id=job)
        if state["status"] != "running":
            break
        await asyncio.sleep(0.2)
    check(state["status"] == "cancelled", f"cancelled: {state}")
    check(not (ed.project / out).exists(), "nothing written")


async def test_place_beside_and_replace(ed: Editor):
    sid = await scene(ed)
    arm = (await ed.call("create_entity", type="model", name="Arm", model=ARM, scene=sid, position=[2, 0, 0]))["entity_id"]
    anim = "assets/models/rigid_arm.anim"
    r = await bake(ed, source=[anim], on="Arm", scene=sid, output="assets/vat/arm.vat", place="beside")
    v = r.structured
    check(v["undo"]["scope"] == "scene" and "scope scene" in r.text, f"a placed bake is a scene step: {r.text}")
    entity = v.get("entity")
    check(entity and entity["name"] == "Arm (VAT)", f"the placed GameObject: {entity}")
    got = await ed.call("get_entity", scene=sid, entity=entity["id"], components=["model", "vertexanimator", "transform"])
    comps = got["components"]
    check(comps["model"]["filename"].endswith("arm.vat.glb"), f"its Model: {comps['model']}")
    check(comps["vertexanimator"]["animation"].endswith("arm.vat"), f"its Vertex Animator: {comps['vertexanimator']}")
    check(abs(comps["transform"]["position"][0] - 2) < 1e-4, f"at the source's transform: {comps['transform']}")
    S["placed"] = entity["id"]
    # undo: the GameObject and the files go together
    await ed.call("undo", scope="scene", scene=sid)
    check(not (ed.project / "assets/vat/arm.vat").exists(), "undo removes the files")
    await ed.call("redo", scope="scene", scene=sid)
    check((ed.project / "assets/vat/arm.vat").exists(), "redo")
    # replace: the source is disabled
    r = await bake(ed, source=[anim], on="Arm", scene=sid, output="assets/vat/arm_r.vat", place="replace")
    src = await ed.call("get_entity", scene=sid, entity=arm)
    check(src.get("enabled") is False or src.get("active") is False, f"replace disables the source: {json.dumps(src)[:400]}")
    await ed.call("undo", scope="scene", scene=sid)
    src = await ed.call("get_entity", scene=sid, entity=arm)
    check(src.get("enabled", src.get("active")) is not False, "undo enables it again")
    msg = await ed.call("bake_vertex_animation", {"source": ARM, "output": "assets/vat/x.vat", "place": "replace"}, expect_error=True)
    check("replace" in msg and "GameObject" in msg, f"replace needs a GameObject: {msg}")


async def test_asset_tools_on_vat(ed: Editor):
    vat = "assets/models/rigid_arm.vat"
    info = await ed.call("get_asset_info", path=vat)
    check(info["type"] == "vertex_animation", f"type: {info['type']}")
    va = info["vertex_animation"]
    check({c["name"] for c in va["clips"]} == {"Sway"} and va["layout"]["width"] > 0, f"clips and layout: {va}")
    check(va["stale"] is False and va["staleness"] == [], f"fresh: {va['staleness']}")
    check(va["recipe"]["sources"][0]["path"] == ARM, f"recipe: {va['recipe']}")
    placed = await ed.call("get_asset_info", path="assets/vat/arm.vat")
    check(any(u["entity"]["id"] == S["placed"] for u in placed["vertex_animation"]["users"]), f"users: {placed['vertex_animation']['users']}")
    glb = await ed.call("get_asset_info", path="assets/models/rigid_arm.vat.glb")
    check(glb.get("generated_by") == vat, f"the .vat.glb names its .vat: {glb.get('generated_by')}")
    listed = await ed.call("list_assets", type="vertex_animation")
    paths = {e["path"] for e in listed["entries"]}
    check({vat, "assets/vat/arm.vat"} <= paths and not any(p.endswith(".glb") for p in paths), f"list_assets: {paths}")
    msg = await ed.call("view_asset", {"path": vat}, expect_error=True)
    check("bake_vertex_animation" in msg, f"view_asset points to the preview: {msg}")
    # staleness: the source changed
    model = ed.project / ARM
    original = model.read_text()
    model.write_text(original.replace('"name"', '"name" ', 1))
    va = (await ed.call("get_asset_info", path=vat))["vertex_animation"]
    check(va["stale"] and va["staleness"][0]["kind"] == "vat-source-changed" and va["rebake"] == {"rebake": vat}, f"stale: {va}")
    model.write_text(original)

    # write_file: generated fields refused with the rebake message, editable ones kept with warnings
    data = read_vat(ed.project, vat)
    data["layout"]["width"] = 1024
    msg = await ed.call("write_file", {"path": vat, "content": json.dumps(data)}, expect_error=True)
    check("generated field" in msg and "rebake" in msg, f"generated fields are refused: {msg}")
    data = read_vat(ed.project, vat)
    data["clips"][0]["frames"] = data["clips"][0]["frames"] + 1
    msg = await ed.call("write_file", {"path": vat, "content": json.dumps(data)}, expect_error=True)
    check("clips[0]" in msg and "rebake" in msg, f"clip frames are generated: {msg}")
    data = read_vat(ed.project, vat)
    data["clips"][0]["wrap"] = "sideways"
    w = await ed.call("write_file", path=vat, content=json.dumps(data, indent=2))
    check(w["warnings"] and "wrap" in w["warnings"][0], f"a bad editable value is a warning: {w}")
    await ed.call("undo", scope="project")
    msg = await ed.call("write_file", {"path": vat, "content": "{not json"}, expect_error=True)
    check("Not a valid Vertex Animation" in msg, msg)


async def test_autosync_reports_a_broken_vat(ed: Editor):
    vat = ed.project / "assets/models/rigid_arm.vat"
    good = vat.read_text()
    await ed.call_raw("editor_status")
    await asyncio.sleep(0.05)
    data = json.loads(good)
    data["layout"]["layout_hash"] = "0" * 16
    broken = json.dumps(data, indent=2)
    vat.write_text(broken)
    r = await ed.call_raw("editor_status")
    ext = (r.structured or {}).get("external_changes") or {}
    found = [p for p in ext.get("problems", []) if p.get("path") == "assets/models/rigid_arm.vat"]
    check(found and "vat-layout-mismatch" in found[0]["message"],
          f"auto-sync reports the mismatch: {ext} (the file is {'as written' if vat.read_text() == broken else 'rewritten'})")
    await asyncio.sleep(0.05)
    vat.write_text(good)
    r = await ed.call_raw("editor_status")
    ext = (r.structured or {}).get("external_changes") or {}
    check(not [p for p in ext.get("problems", []) if p.get("path") == "assets/models/rigid_arm.vat"], f"clean again: {ext}")


# ---------------------------------------------------------------------------- synthetic exports

def png_rgba8(width: int, height: int, pixels: list[list[float]]) -> bytes:
    """8-bit RGBA PNG of values 0..1 (rows top first, 4 per pixel)."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b""
    for y in range(height):
        raw += b"\x00" + bytes(max(0, min(255, round(v * 255))) for v in pixels[y])
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def write_glb(path: Path, positions, normals, uv0, uv1, indices) -> None:
    """A glTF binary with one triangle mesh (float attributes, uint32 indices)."""
    data = bytearray()
    views, accessors = [], []

    def add(values, fmt, kind, count, target, minmax=False):
        while len(data) % 4:
            data.append(0)
        offset = len(data)
        data.extend(struct.pack("<%d%s" % (len(values), fmt), *values))
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data) - offset, "target": target})
        accessor = {"bufferView": len(views) - 1, "componentType": 5126 if fmt == "f" else 5125, "count": count, "type": kind}
        if minmax:
            accessor["min"] = [min(values[k::3]) for k in range(3)]
            accessor["max"] = [max(values[k::3]) for k in range(3)]
        accessors.append(accessor)
        return len(accessors) - 1

    n = len(positions) // 3
    attributes = {"POSITION": add(positions, "f", "VEC3", n, 34962, True), "NORMAL": add(normals, "f", "VEC3", n, 34962),
                  "TEXCOORD_0": add(uv0, "f", "VEC2", n, 34962), "TEXCOORD_1": add(uv1, "f", "VEC2", n, 34962)}
    index = add(indices, "I", "SCALAR", len(indices), 34963)
    while len(data) % 4:
        data.append(0)
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "Flag"}],
           "meshes": [{"primitives": [{"attributes": attributes, "indices": index, "mode": 4}]}],
           "buffers": [{"byteLength": len(data)}], "bufferViews": views, "accessors": accessors}
    text = json.dumps(doc).encode()
    text += b" " * ((4 - len(text) % 4) % 4)
    body = struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(data), 0x004E4942) + bytes(data)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<III", 0x46546C67, 2, 12 + len(body)) + body)


def write_openvat(folder: Path, name: str = "Flag", frames: int = 4) -> None:
    """An OpenVAT export (format of editor/tests/VatImportTest.cpp writeOpenVat): two quads bending, a
    single-row 8-bit _vat.png with packed normals in its lower half, -remap_info.json with two clips
    and the glTF mesh whose last TEXCOORD is VAT_UV."""
    V, W = 6, 8
    rest_b = [((i % 3) * 0.5, (i // 3) * 0.7, 0.1 * i) for i in range(V)]          # Blender space
    offs = [[(0.0, 0.0, 0.0) if f == 0 else (0.1 * math.sin(f + i), 0.05 * f * i, 0.3 * math.cos(f * 0.5 + i) - 0.3 * math.cos(i))
             for i in range(V)] for f in range(frames)]
    mn = [min(o[k] for fr in offs for o in fr) - 0.01 for k in range(3)]
    mx = [max(o[k] for fr in offs for o in fr) + 0.01 for k in range(3)]
    H = frames * 2
    pixels = [[0.0] * (W * 4) for _ in range(H)]
    for f in range(frames):
        for i in range(V):
            o = offs[f][i]
            pixels[f][i * 4:i * 4 + 4] = [(o[k] - mn[k]) / (mx[k] - mn[k]) for k in range(3)] + [1.0]
            pixels[f + H // 2][i * 4:i * 4 + 4] = [0.5, 0.5, 1.0, 1.0]       # normal +Z (Blender)
    folder.mkdir(parents=True, exist_ok=True)
    (folder / f"{name}_vat.png").write_bytes(png_rgba8(W, H, pixels))
    remap = {"os-remap": {"Min": mn, "Max": mx, "Frames": frames},
             "animations": {"Wave": {"startFrame": 1, "endFrame": 2, "framerate": 24, "looping": True},
                            "Flap": {"startFrame": 3, "endFrame": frames, "framerate": 30, "looping": False}}}
    (folder / f"{name}-remap_info.json").write_text(json.dumps(remap, indent=2))
    positions, normals, uv0, uv1 = [], [], [], []
    for i in range(V):
        x, y, z = rest_b[i]
        positions += [x, z, -y]           # Blender Z-up -> glTF Y-up
        normals += [0.0, 1.0, 0.0]
        uv0 += [(i % 3) / 2.0, float(i // 3)]
        uv1 += [(i % W + 0.5) / W, (0 * frames + 0.5) / H]
    write_glb(folder / f"{name}.glb", positions, normals, uv0, uv1, [0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4])


def g17(v: float) -> str:
    return "%.17g" % v


def write_fbx_ascii(path: Path, points, corners, uv_names, uvs, normals, tangents, binormals) -> None:
    """A minimal ASCII FBX 7.4 mesh (VatImportTest.cpp writeFbx): what ufbx reads."""
    o = ['; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: "test_bake_vertex_animation"\n}\n',
         'GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n'
         '\t\tP: "UpAxis", "int", "Integer", "",1\n\t\tP: "UpAxisSign", "int", "Integer", "",1\n'
         '\t\tP: "FrontAxis", "int", "Integer", "",2\n\t\tP: "FrontAxisSign", "int", "Integer", "",1\n'
         '\t\tP: "CoordAxis", "int", "Integer", "",0\n\t\tP: "CoordAxisSign", "int", "Integer", "",1\n'
         '\t\tP: "UnitScaleFactor", "double", "Number", "",1\n\t}\n}\n',
         'Objects:  {\n\tGeometry: 1000, "Geometry::MESH", "Mesh" {\n']
    o.append(f"\t\tVertices: *{len(points) * 3} {{\n\t\t\ta: " + ",".join(g17(c) for p in points for c in p) + "\n\t\t}\n")
    o.append(f"\t\tPolygonVertexIndex: *{len(corners)} {{\n\t\t\ta: " +
             ",".join(str(-c - 1 if i % 3 == 2 else c) for i, c in enumerate(corners)) + "\n\t\t}\n\t\tGeometryVersion: 124\n")
    for element, name, values in (("LayerElementNormal", "Normals", normals), ("LayerElementTangent", "Tangents", tangents),
                                  ("LayerElementBinormal", "Binormals", binormals)):
        o.append(f'\t\t{element}: 0 {{\n\t\t\tVersion: 102\n\t\t\tName: ""\n\t\t\tMappingInformationType: "ByPolygonVertex"\n'
                 f'\t\t\tReferenceInformationType: "Direct"\n\t\t\t{name}: *{len(values) * 3} {{\n\t\t\t\ta: ' +
                 ",".join(g17(c) for v in values for c in v) + "\n\t\t\t}\n\t\t}\n")
    for s, uv in enumerate(uvs):
        o.append(f'\t\tLayerElementUV: {s} {{\n\t\t\tVersion: 101\n\t\t\tName: "{uv_names[s]}"\n'
                 f'\t\t\tMappingInformationType: "ByPolygonVertex"\n\t\t\tReferenceInformationType: "Direct"\n'
                 f'\t\t\tUV: *{len(uv) * 2} {{\n\t\t\t\ta: ' + ",".join(g17(c) for t in uv for c in t) + "\n\t\t\t}\n\t\t}\n")
    o.append("\t\tLayer: 0 {\n\t\t\tVersion: 100\n")
    for kind in ("LayerElementNormal", "LayerElementTangent", "LayerElementBinormal", "LayerElementUV"):
        o.append(f'\t\t\tLayerElement:  {{\n\t\t\t\tType: "{kind}"\n\t\t\t\tTypedIndex: 0\n\t\t\t}}\n')
    o.append("\t\t}\n")
    for s in range(1, len(uvs)):
        o.append(f'\t\tLayer: {s} {{\n\t\t\tVersion: 100\n\t\t\tLayerElement:  {{\n\t\t\t\tType: "LayerElementUV"\n\t\t\t\tTypedIndex: {s}\n\t\t\t}}\n\t\t}}\n')
    o.append('\t}\n\tModel: 2000, "Model::MESH", "Mesh" {\n\t\tVersion: 232\n\t\tProperties70:  {\n'
             '\t\t\tP: "Lcl Translation", "Lcl Translation", "", "A",0,0,0\n'
             '\t\t\tP: "Lcl Rotation", "Lcl Rotation", "", "A",0,0,0\n'
             '\t\t\tP: "Lcl Scaling", "Lcl Scaling", "", "A",1,1,1\n'
             '\t\t}\n\t}\n}\nConnections:  {\n\tC: "OO",2000,0\n\tC: "OO",1000,2000\n}\n')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(o))


def write_houdini_soft(root: Path, asset: str = "Wave", frames: int = 4) -> None:
    """A Houdini VAT 3 soft-body export, Unity target, LDR (8-bit PNG) with the legacy data JSON
    (VatImportTest.cpp writeSoft, metadata json, hdr off): a 3 x 3 grid waving, plus the guard
    triangle at the bound corners."""
    P, A = 9, 4
    rest = [((i % 3) * 0.5 - 0.5, 1.0 + 0.1 * (i // 3), (i // 3) * 0.5 - 0.5) for i in range(P)]   # Houdini space
    unity = lambda v: (-v[0], v[1], v[2])
    offs = [[(0.0, 0.0, 0.0) if f == 0 else (0.3 * math.sin(f * 0.7 + i), 0.2 * f, -0.1 * math.cos(f + 2.0 * i) + 0.1 * math.cos(2.0 * i))
             for i in range(P)] for f in range(frames)]
    pts = [unity(o) for fr in offs for o in fr] + [unity(r) for r in rest]
    lo = [min(p[k] for p in pts) for k in range(3)]
    hi = [max(p[k] for p in pts) for k in range(3)]
    mx = [(math.ceil(hi[0] * 10) + 1) / 10.0, (math.ceil(hi[1] * 10) + 1) / 10.0, (math.ceil(hi[2] * 10) + 1 + 0.2) / 10.0]
    mn = [-(math.ceil(-lo[0] * 10) + 1) / 10.0, (math.floor(lo[1] * 10) - 1) / 10.0, (math.floor(lo[2] * 10) - 1) / 10.0]
    R = (P + A - 1) // A
    H = R * frames
    pos = [[0.0] * (A * 4) for _ in range(H)]
    rot = [[0.0] * (A * 4) for _ in range(H)]
    for f in range(frames):
        for i in range(P):
            c, r = i % A, i // A + f * R
            o = unity(offs[f][i])
            pos[r][c * 4:c * 4 + 4] = [(o[k] - mn[k]) / (mx[k] - mn[k]) for k in range(3)] + [1.0]
            rot[r][c * 4:c * 4 + 4] = [0.5, 0.5, 0.5, 1.0]      # the identity rotation, (q + 1) / 2
    (root / "tex").mkdir(parents=True, exist_ok=True)
    (root / "tex" / f"{asset}_pos.png").write_bytes(png_rgba8(A, H, pos))
    (root / "tex" / f"{asset}_rot.png").write_bytes(png_rgba8(A, H, rot))
    g0, g1 = unity(mn), unity(mx)
    points = rest + [g0, g1, (g0[0], g1[1], g0[2])]
    corners, uv_a, uv_b, nrm, tan, bin_ = [], [], [], [], [], []
    for t in ((0, 1, 4), (0, 4, 3), (1, 2, 5), (1, 5, 4), (3, 4, 7), (3, 7, 6), (4, 5, 8), (4, 8, 7)):
        for i in t:
            corners.append(i)
            uv_a.append(((i % 3) / 2.0, (i // 3) / 2.0))
            uv_b.append((((i % A) + 0.5) / A, 1.0 - ((i // A) + 0.5) / H))
            nrm.append((0.0, 1.0, 0.0))
            tan.append((1.0, 0.0, 0.0))
            bin_.append((0.0, 0.0, -1.0))
    for k in range(3):
        corners.append(P + k)
        uv_a.append((0.0, 0.0))
        uv_b.append((0.0, 0.0))
        nrm.append((0.0, 1.0, 0.0))
        tan.append((1.0, 0.0, 0.0))
        bin_.append((0.0, 0.0, -1.0))
    write_fbx_ascii(root / "geo" / f"{asset}_mesh.fbx", points, corners, ["uv", "uv2"], [uv_a, uv_b], nrm, tan, bin_)
    meta = {"Bound Min X": mn[0], "Bound Min Y": mn[1], "Bound Min Z": mn[2], "Bound Max X": mx[0], "Bound Max Y": mx[1],
            "Bound Max Z": mx[2], "Houdini FPS": 24.0, "Two Position Textures": False,
            "Axis System": "Y-Z-X Clockwise (Left-Handed Y-Up)", "Frame Count": frames}
    (root / "data").mkdir(parents=True, exist_ok=True)
    (root / "data" / f"{asset}_data.json").write_text(json.dumps([meta], indent=2))


async def test_import_openvat(ed: Editor):
    folder = ed.project / "assets" / "exports" / "flag"
    write_openvat(folder)
    await ed.call("sync_files")
    before = files_under(ed.project)
    msg = await ed.call("bake_vertex_animation", {"source": "assets/exports/flag", "clips": ["Wave"]}, expect_error=True)
    check("clips is for baked sources" in msg, f"an import takes its clips as they are: {msg}")
    r = await bake(ed, source="assets/exports/flag")
    v = r.structured
    check(files_under(ed.project) == before, f"an import preview writes nothing: {sorted(files_under(ed.project) ^ before)}")
    check(v["kind"] == "openvat" and v["mode"] == "vertices" and v["written"] is False, f"report: {v}")
    check(sorted(c["name"] for c in v["clips"]) == ["Flap", "Wave"], f"clips from remap_info: {v['clips']}")
    check(v["default_output"] == "assets/exports/flag/Flag.vat", f"default output next to the export: {v.get('default_output')}")
    check(len(r.images) == 1 and "VAT at 4 times" in r.text, f"VAT rows only: {r.text}")
    keep_image("vat_sheet_openvat.png", r.images[0][0])
    r = await bake(ed, source="assets/exports/flag", output=True)
    v = r.structured
    vat = "assets/exports/flag/Flag.vat"
    check(v["vat"] == vat and (ed.project / vat).is_file() and (ed.project / "assets/exports/flag/Flag.vat.glb").is_file(), f"written: {v}")
    data = read_vat(ed.project, vat)
    check(data["recipe"]["sources"][0]["kind"] == "openvat", f"recipe: {data['recipe']}")
    info = (await ed.call("get_asset_info", path=vat))["vertex_animation"]
    check(info["stale"] is False, f"fresh import: {info['staleness']}")
    # rebake an import: imported again, GUID and edits kept
    data["clips"][0]["speed"] = 2
    await ed.call("write_file", path=vat, content=json.dumps(data, indent=2))
    r = await bake(ed, rebake=vat)
    after = read_vat(ed.project, vat)
    check(after["guid"] == data["guid"] and after["clips"][0]["speed"] == 2, f"rebake of an import keeps edits: {after['clips'][0]}")
    check("Re-imported" in r.text, r.text)


async def test_import_houdini_soft(ed: Editor):
    root = ed.project / "assets" / "exports" / "wave"
    write_houdini_soft(root)
    await ed.call("sync_files")
    r = await bake(ed, source="assets/exports/wave", fps=24, normals=False)
    v = r.structured
    check(v["kind"] == "houdini_vat3" and v["houdini_mode"] == "soft" and v["mode"] == "vertices", f"report: {v}")
    check(v["vertices"] == 9 and v["clips"][0]["frames"] >= 4, f"9 points, 4 frames: {v['vertices']} {v['clips']}")
    keep_image("vat_sheet_houdini.png", r.images[0][0])
    msg = await ed.call("bake_vertex_animation", {"source": "assets/exports/wave", "import": {"axis": "sideways"}}, expect_error=True)
    check("import.axis must be" in msg, msg)
    await bake(ed, **{"source": "assets/exports/wave", "fps": 24, "normals": False, "output": "assets/vat/wave.vat",
                      "import": {"axis": "unity"}})
    data = read_vat(ed.project, "assets/vat/wave.vat")
    check(data["recipe"]["sources"][0]["kind"] == "houdini_vat3" and data["recipe"]["import"]["axis"] == "unity",
          f"the recipe keeps the import options: {data['recipe']}")


STEPPER = """local Stepper = { properties = {} }
function Stepper:onStep(side)
    print("vat step " .. tostring(side))
end
return Stepper
"""


async def test_events_reach_a_lua_script(ed: Editor):
    sid = await scene(ed)
    anim = "assets/models/rigid_arm.anim"
    await ed.call("edit_animation", clip=anim, events=[{"time": 0.25, "function": "onStep", "args": ["left"]}])
    r = await bake(ed, source=[anim], on="Arm", scene=sid, output="assets/vat/arm_events.vat", place="beside")
    data = read_vat(ed.project, "assets/vat/arm_events.vat")
    check(data["clips"][0]["events"] == [{"time": 0.25, "function": "onStep", "args": ["left"]}], f"the clip's events are baked: {data['clips']}")
    entity = r.structured["entity"]["id"]
    script = await ed.call("create_script", kind="lua", name="Stepper", content=STEPPER)
    await ed.call("attach_script", scene=sid, entity=entity, path=script["path"])
    await ed.call("save_scene", scene=sid)
    await ed.call("clear_console")
    started = await ed.call("play", action="start")
    check(started["state"] == "playing", f"play: {started}")
    found = []
    for _ in range(60):
        await asyncio.sleep(0.25)
        logs = await ed.call("read_console", contains="vat step")
        found = [m for m in json.dumps(logs).split("vat step")[1:]]
        if found:
            break
    moving = (await ed.call("eval_lua", code=f"local a = object({entity}, 'Model'):getVertexAnimator(); return a:isLoaded(), a:isPlaying(), a.currentClip"))["values"]
    await ed.call("play", action="stop")
    check(moving[0] is True and moving[1] is True, f"the Vertex Animator plays: {moving}")
    check(found and found[0].startswith(" left"), f"the onStep event reached the script with its argument: {found[:3]}")


async def test_move_and_delete_keep_the_pair(ed: Editor):
    sid = await scene(ed)
    r = await bake(ed, source=ARM, clips=["Swing"], output="assets/vat/pair.vat")
    guid = read_vat(ed.project, "assets/vat/pair.vat")["guid"]
    moved = await ed.call("move_asset", path="assets/vat/pair.vat", to="assets/vat/pair_moved.vat")
    check(moved["pair"] == {"from": "assets/vat/pair.vat.glb", "to": "assets/vat/pair_moved.vat.glb"}, f"the mesh moves too: {moved}")
    check(not (ed.project / "assets/vat/pair.vat.glb").exists() and (ed.project / "assets/vat/pair_moved.vat.glb").is_file(), "both moved")
    data = read_vat(ed.project, "assets/vat/pair_moved.vat")
    check(data["mesh"] == "pair_moved.vat.glb" and data["guid"] == guid, f"the .vat's mesh follows, the GUID stays: {data['mesh']}")
    info = (await ed.call("get_asset_info", path="assets/vat/pair_moved.vat"))["vertex_animation"]
    check(not [s for s in info["staleness"] if "mismatch" in s["kind"]], f"the moved pair still matches: {info['staleness']}")
    await ed.call("undo", scope="project")
    check((ed.project / "assets/vat/pair.vat").is_file() and (ed.project / "assets/vat/pair.vat.glb").is_file(), "undo moves both back")
    check(read_vat(ed.project, "assets/vat/pair.vat")["mesh"] == "pair.vat.glb", "with the old mesh name")
    # naming the mesh moves the .vat; into another folder; a name that breaks the pair is refused
    (ed.project / "assets/vat2").mkdir(exist_ok=True)
    await ed.call("sync_files")
    moved = await ed.call("move_asset", path="assets/vat/pair.vat.glb", to="assets/vat2/pair.vat.glb")
    check((ed.project / "assets/vat2/pair.vat").is_file() and moved["pair"]["to"] == "assets/vat2/pair.vat", f"the .vat follows its mesh: {moved}")
    msg = await ed.call("move_asset", {"path": "assets/vat2/pair.vat", "to": "assets/vat2/pair.json"}, expect_error=True)
    check("must end in .vat" in msg, msg)
    # a copy is a new asset with its own GUID
    await ed.call("move_asset", path="assets/vat2/pair.vat", to="assets/vat2/copy.vat", copy=True)
    copy = read_vat(ed.project, "assets/vat2/copy.vat")
    check(copy["guid"] != guid and copy["mesh"] == "copy.vat.glb" and (ed.project / "assets/vat2/copy.vat.glb").is_file(), f"copy: {copy['guid']}")
    # delete either half: both go to the trash, one undo brings both back
    deleted = await ed.call("delete_asset", paths=["assets/vat2/pair.vat"])
    check(deleted["with_pair"] == ["assets/vat2/pair.vat.glb"], f"the mesh goes too: {deleted}")
    check(not (ed.project / "assets/vat2/pair.vat.glb").exists(), "deleted")
    await ed.call("undo", scope="project")
    check((ed.project / "assets/vat2/pair.vat").is_file() and (ed.project / "assets/vat2/pair.vat.glb").is_file(), "undo restores both")


def glb_chunks(path: Path) -> tuple[dict, bytes]:
    """The JSON chunk (parsed) and the bytes after it (the BIN chunk) of a GLB."""
    data = path.read_bytes()
    magic, version, length = struct.unpack_from("<4sII", data, 0)
    check(magic == b"glTF" and version == 2 and length == len(data), f"{path.name} is a GLB 2.0 of its length")
    json_length, kind = struct.unpack_from("<II", data, 12)
    check(kind == 0x4E4F534A and json_length % 4 == 0, f"{path.name}: a 4-byte aligned JSON chunk first")
    return json.loads(data[20:20 + json_length]), data[20 + json_length:]


def set_glb_json(path: Path, gltf: dict) -> None:
    _, rest = glb_chunks(path)
    text = json.dumps(gltf).encode()
    text += b" " * (-len(text) % 4)
    path.write_bytes(struct.pack("<4sII", b"glTF", 2, 20 + len(text) + len(rest)) + struct.pack("<II", len(text), 0x4E4F534A) + text + rest)


def png_2x2() -> bytes:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + b"\x80\x40\x20\xff" * 2 for _ in range(2))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def image_uri(project: Path, glb: str) -> str:
    return glb_chunks(project / glb)[0]["images"][0]["uri"]


def asset_db(project: Path) -> str:
    # S04's cache of the asset database (written in the background after a change)
    db = project / ".jasperengine" / "assetdb" / "index.json"
    return db.read_text() if db.exists() else ""


async def asset_db_has(project: Path, guid: str, want: bool = True, timeout: float = 10.0) -> bool:
    """Whether the asset database's cache lists `guid` as wanted, waiting for its background save"""
    deadline = asyncio.get_running_loop().time() + timeout
    while (guid in asset_db(project)) != want:
        if asyncio.get_running_loop().time() > deadline:
            return False
        await asyncio.sleep(0.1)
    return True


async def test_move_and_copy_keep_the_textures(ed: Editor):
    await scene(ed)
    await bake(ed, source=ARM, clips=["Swing"], output="assets/vat/skin.vat")
    texture = ed.project / "assets/textures/skin.png"
    texture.parent.mkdir(parents=True, exist_ok=True)
    texture.write_bytes(png_2x2())
    # the mesh names a project texture (as a bake of a textured model does)
    glb = ed.project / "assets/vat/skin.vat.glb"
    gltf, bin_chunk = glb_chunks(glb)
    gltf["images"] = [{"uri": "../textures/skin.png"}]
    gltf["textures"] = [{"source": 0}]
    set_glb_json(glb, gltf)
    original = glb.read_bytes()
    (ed.project / "assets/vat/deep/er").mkdir(parents=True, exist_ok=True)
    await ed.call("sync_files")

    await ed.call("move_asset", path="assets/vat/skin.vat", to="assets/vat/deep/er/skin.vat")
    moved = "assets/vat/deep/er/skin.vat.glb"
    check(image_uri(ed.project, moved) == "../../../textures/skin.png", f"the moved mesh names the same texture: {image_uri(ed.project, moved)}")
    check(glb_chunks(ed.project / moved)[1] == bin_chunk, "the BIN chunk is the same bytes")
    check(((ed.project / moved).parent / image_uri(ed.project, moved)).resolve() == texture.resolve(), "it resolves to the texture")
    info = (await ed.call("get_asset_info", path="assets/vat/deep/er/skin.vat"))["vertex_animation"]
    check(not [s for s in info["staleness"] if "mismatch" in s["kind"]], f"the moved pair still matches: {info['staleness']}")
    await ed.call("undo", scope="project")
    check(glb.read_bytes() == original, "undo moves it back with the URI it had")
    await ed.call("redo", scope="project")
    check(image_uri(ed.project, moved) == "../../../textures/skin.png", "redo rewrites it again")
    await ed.call("undo", scope="project")

    # a copy of the mesh (with its .vat) to another folder: the copy names the same texture; undo
    # removes both and the copied .vat's GUID leaves the asset database
    await ed.call("move_asset", path="assets/vat/skin.vat.glb", to="assets/vat/deep/copy.vat.glb", copy=True)
    copy_vat = read_vat(ed.project, "assets/vat/deep/copy.vat")
    copy_guid = copy_vat["guid"]
    check(copy_guid != read_vat(ed.project, "assets/vat/skin.vat")["guid"], "the copy has a GUID of its own")
    check(image_uri(ed.project, "assets/vat/deep/copy.vat.glb") == "../../textures/skin.png",
          f"the copy names the same texture: {image_uri(ed.project, 'assets/vat/deep/copy.vat.glb')}")
    check(glb.read_bytes() == original, "the original is untouched")
    check(await asset_db_has(ed.project, copy_guid), "the copied .vat is in the asset database")
    await ed.call("undo", scope="project")
    check(not (ed.project / "assets/vat/deep/copy.vat").exists() and not (ed.project / "assets/vat/deep/copy.vat.glb").exists(), "undo removes both")
    check(await asset_db_has(ed.project, copy_guid, want=False), f"the copied .vat's GUID left the asset database: {asset_db(ed.project)}")
    await ed.call("redo", scope="project")
    check(read_vat(ed.project, "assets/vat/deep/copy.vat")["guid"] == copy_guid, "redo gives the copy its GUID again")
    check(image_uri(ed.project, "assets/vat/deep/copy.vat.glb") == "../../textures/skin.png", "and the URI")
    await ed.call("undo", scope="project")

    # deleting the pair leaves the texture
    await ed.call("delete_asset", paths=["assets/vat/skin.vat"])
    check(texture.is_file(), "a texture is never deleted with the pair")
    await ed.call("undo", scope="project")
    check(glb.read_bytes() == original, "undo restores the mesh as it was")


async def test_play_instances_move_out_of_step(ed: Editor):
    sid = await scene(ed)
    r = await bake(ed, source=ARM, clips=["Swing"], output="assets/vat/herd.vat", place="beside", scene=sid)
    entity = r.structured["entity"]["id"]
    await ed.call("add_component", scene=sid, entity=entity, component="instancedmesh")
    instances = [{"position": [x * 1.5, 0, z * 1.5]} for x in range(3) for z in range(2)]
    await ed.call("set_properties", scene=sid, entity=entity, values={"instancedmesh": {"instances": instances}})
    await ed.call("save_scene", scene=sid)
    await ed.call("play", action="start")
    code = f"""local a = object({entity}, 'Model'):getVertexAnimator()
        if not a:isLoaded() then return false end
        local t = {{}}
        for i = 0, a:getInstanceCount() - 1 do t[#t + 1] = a:getInstanceNormalizedTime(i) end
        return a:getInstanceCount(), t, a:getInstanceClip(0)"""
    first = None
    for _ in range(80):
        v = (await ed.call("eval_lua", code=code))["values"]
        if v and v[0] is not False:
            first = v
            break
        await asyncio.sleep(0.25)
    await asyncio.sleep(0.4)
    later = (await ed.call("eval_lua", code=code))["values"]
    await ed.call("play", action="stop")
    check(first is not None, "the Vertex Animator loads in play")
    count, times, clip = first
    check(count == 6 and clip == "Swing", f"6 instances playing Swing: {first}")
    times = list(times.values()) if isinstance(times, dict) else times
    check(len({round(t, 3) for t in times}) == 6, f"randomStart: every instance at its own time: {times}")
    later_times = list(later[1].values()) if isinstance(later[1], dict) else later[1]
    check(any(abs(a - b) > 1e-3 for a, b in zip(times, later_times)), f"they move: {times} then {later_times}")


async def test_place_crowd_bake_and_import(ed: Editor):
    sid = await scene(ed)
    r = await bake(ed, source=ARM, clips=["Swing"], output="assets/vat/crowd.vat", scene=sid,
                   place={"crowd": {"count": 12, "area": 8, "seed": 3}})
    v = r.structured
    check(v["undo"]["scope"] == "scene" and v.get("entity"), f"a crowd is a scene step with its GameObject: {v}")
    entity = v["entity"]["id"]
    got = await ed.call("get_entity", scene=sid, entity=entity, components=["instancedmesh", "vertexanimator", "model"])
    comps = got["components"]
    check("instancedmesh" in comps and "vertexanimator" in comps, f"Model + Instanced Mesh + Vertex Animator: {sorted(comps)}")
    count = (await ed.call("eval_lua", code=f"return object({entity}, 'Model'):getNumInstances()",
                           allow_edit_mode=True))["values"]
    check(count == [12], f"12 instances: {count}")
    # one undo removes the crowd and its files
    await ed.call("undo", scope="scene", scene=sid)
    check(not (ed.project / "assets/vat/crowd.vat").exists(), "undo removes the files")
    await ed.call("redo", scope="scene", scene=sid)
    check((ed.project / "assets/vat/crowd.vat").exists(), "redo")
    # play: every instance at its own time, and they move
    await ed.call("save_scene", scene=sid)
    await ed.call("play", action="start")
    code = f"""local a = object({entity}, 'Model'):getVertexAnimator()
        if not a:isLoaded() then return false end
        local t = {{}}
        for i = 0, a:getInstanceCount() - 1 do t[#t + 1] = a:getInstanceNormalizedTime(i) end
        return t"""
    first = None
    for _ in range(80):
        got = (await ed.call("eval_lua", code=code))["values"]
        if got and got[0] is not False:
            first = got[0]
            break
        await asyncio.sleep(0.25)
    await asyncio.sleep(0.4)
    later = (await ed.call("eval_lua", code=code))["values"][0]
    await ed.call("play", action="stop")
    check(first is not None, "the crowd loads in play")
    first = list(first.values()) if isinstance(first, dict) else first
    later = list(later.values()) if isinstance(later, dict) else later
    check(len(first) == 12 and len({round(t, 3) for t in first}) >= 10, f"out of step: {first}")
    check(any(abs(a - b) > 1e-3 for a, b in zip(first, later)), f"they move: {first} then {later}")
    # an import placed as a crowd in the scene given
    r = await bake(ed, **{"source": "assets/exports/flag", "output": "assets/vat/flag_crowd.vat", "scene": sid,
                          "place": {"crowd": {"count": 5, "area": 4}}})
    check(r.structured.get("entity") and r.structured["undo"]["scope"] == "scene", f"an imported crowd: {r.structured}")
    msg = await ed.call("bake_vertex_animation", {"source": "assets/exports/flag", "output": "assets/vat/flag_r.vat", "place": "replace"},
                        expect_error=True)
    check("replace" in msg and "GameObject" in msg, msg)


async def wait_job(ed: Editor, job: str, limit: float = 600.0, until_message: str = "") -> dict:
    """get_job until the job ends (or, with until_message, until its message starts with it)"""
    state = {}
    for _ in range(int(limit / 0.05)):
        state = await ed.call("get_job", job_id=job)
        if state["status"] != "running" or (until_message and state.get("message", "").startswith(until_message)):
            break
        await asyncio.sleep(0.05)
    return state


def job_errors(state: dict) -> list:
    return (state.get("result") or {}).get("errors") or []


def check_job_error(state: dict, kinds: tuple, out: Path, what: str) -> None:
    check(state["status"] == "failed", f"{what}: the job fails: {state}")
    errors = job_errors(state)
    check(errors and all(e.get("kind") and e.get("message") and e.get("fix") for e in errors), f"{what}: errors with kind and fix: {state}")
    check(errors[0]["kind"] in kinds, f"{what}: kind {kinds}: {errors}")
    check("Fix: " in state["message"], f"{what}: the message says how to fix it: {state['message']}")
    check(not out.exists(), f"{what}: nothing written")


async def vanish_scene(ed: Editor, name: str, entities: tuple) -> int:
    """A saved scene with hero Animation Players, their models loaded (Probe's preview waits for them)."""
    made = await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    clips = (await ed.call("get_asset_info", path=HERO))["model"]["animations"]
    names = [clip["name"] for clip in clips]
    check("Walk" in names, f"the hero fixture has its Walk clip: {names}")
    states = [{"clip": f"{HERO}#{clip}", "name": clip} for clip in names]
    for entity in (*entities, "Probe"):
        # New projects' Auto mode omits Animation entities on models with five or more clips.
        # These GameObject bakes need authored states, independently of that import preference.
        await ed.call("create_entity", type="model", name=entity, model=HERO, scene=sid,
                      components=["animationplayer"], values={"animationplayer": {"clips": states}})
        player = (await ed.call("get_animation", scene=sid, entity=entity))["players"][0]
        check([state["name"] for state in player["states"]] == names, f"{entity} plays the fixture's clips: {player}")
    await ed.call("save_scene", scene=sid)
    r = await ed.call_raw("bake_vertex_animation", {"source": "Probe", "scene": sid, "fps": 5}, timeout=600)
    check(not r.is_error, f"the GameObject bakes: {r.text}")
    return sid


async def test_a_gameobject_bakes_again(ed: Editor):
    """The same GameObject bakes any number of times (a preview, place beside, replace and its undo), each
    bake the same layout. One brought back by undo builds its meshes in the next frames: a bake right
    after the undo waits for them (source_loading, never "no POSITION"), as a call and as a job."""
    copy_fixtures(ed.project)
    await ed.call("sync_files")
    sid = await vanish_scene(ed, "Again", ("Hero",))

    async def again(what: str, **args) -> dict:
        r = await ed.call_raw("bake_vertex_animation", {"source": "Hero", "scene": sid, "fps": 5, **args}, timeout=600)
        check(not r.is_error, f"{what}: the GameObject bakes again: {r.text}")
        return r.structured

    first = await again("a preview")
    layout = (first["vertices"], first["bones"], first["texture"])
    for what, args in (("a second preview", {}),
                       ("place beside", {"output": "assets/vat/again_beside.vat", "place": "beside"}),
                       ("after place beside", {}),
                       ("replace", {"output": "assets/vat/again_replace.vat", "place": "replace"})):
        v = await again(what, **args)
        check((v["vertices"], v["bones"], v["texture"]) == layout, f"{what}: the same bake: {v} vs {first}")
    await ed.call("undo", scope="scene", scene=sid)
    v = await again("after undoing replace")
    check((v["vertices"], v["bones"], v["texture"]) == layout, f"after undoing replace: {v}")
    for attempt in range(3):
        await ed.call("delete_entities", scene=sid, entity="Hero")
        await ed.call("undo", scope="scene", scene=sid)
        v = await again(f"right after undoing its deletion ({attempt + 1})")
        check((v["vertices"], v["bones"], v["texture"]) == layout, f"after undoing its deletion: {v}")
    v = await again("a preview after that")
    check((v["vertices"], v["bones"], v["texture"]) == layout, f"a preview after that: {v}")
    # as a job
    await ed.call("delete_entities", scene=sid, entity="Hero")
    await ed.call("undo", scope="scene", scene=sid)
    r = await ed.call_raw("bake_vertex_animation", {"source": "Hero", "scene": sid, "fps": 5, "wait": False}, timeout=600)
    check(not r.is_error, f"a job right after the undo: {r.text}")
    if r.structured.get("job_id"):
        state = await wait_job(ed, r.structured["job_id"])
        check(state["status"] == "succeeded", f"the job bakes: {state}")


async def test_job_errors_when_the_source_vanishes(ed: Editor):
    """A bake job whose GameObject source goes away (its scene closed, the GameObject deleted) while it
    poses or while it encodes fails as an error with a kind and a fix, nothing written (DESIGN.md:
    errors say how to fix them). Every case bakes a GameObject of its own."""
    copy_fixtures(ed.project)
    await ed.call("sync_files")
    sid = await vanish_scene(ed, "Vanish", ("Hero", "Hero2", "Hero3"))
    big = {"scene": sid, "mode": "vertices", "fps": 240, "preset": "desktop", "wait": False}

    # deleted while it poses
    out = "assets/vat/vanish_posing.vat"
    r = await ed.call_raw("bake_vertex_animation", {**big, "source": "Hero", "output": out})
    check(not r.is_error and r.structured.get("job_id"), f"a job: {r.text}")
    await ed.call("delete_entities", scene=sid, entity="Hero")
    check_job_error(await wait_job(ed, r.structured["job_id"]), ("invalid_source",), ed.project / out, "deleted while posing")

    # deleted while it encodes (posing is over: the write finds it gone)
    out = "assets/vat/vanish_encoding.vat"
    r = await ed.call_raw("bake_vertex_animation", {**big, "source": "Hero2", "output": out})
    state = await wait_job(ed, r.structured["job_id"], until_message="Encoding")
    check(state["status"] == "running", f"the job reaches its encoding: {state}")
    await ed.call("delete_entities", scene=sid, entity="Hero2")
    check_job_error(await wait_job(ed, r.structured["job_id"]), ("invalid_source",), ed.project / out, "deleted while encoding")
    # brought back by undo: the next bake of it writes
    await ed.call("undo", scope="scene", scene=sid)

    # the scene closed while it poses
    out = "assets/vat/vanish_closed.vat"
    r = await ed.call_raw("bake_vertex_animation", {**big, "source": "Hero3", "output": out})
    check(not r.is_error and r.structured.get("job_id"), f"a job: {r.text}")
    await ed.call("create_scene", name="Other", type="3D")
    await ed.call("close_scene", scene=sid, unsaved="discard")
    check_job_error(await wait_job(ed, r.structured["job_id"]), ("invalid_source", "scene_playing"), ed.project / out, "closed while posing")

    # the scene closed while it encodes
    sid2 = await vanish_scene(ed, "Vanish2", ("Hero",))
    out = "assets/vat/vanish_closed_encoding.vat"
    r = await ed.call_raw("bake_vertex_animation", {**big, "scene": sid2, "source": "Hero", "output": out})
    state = await wait_job(ed, r.structured["job_id"], until_message="Encoding")
    check(state["status"] == "running", f"the job reaches its encoding: {state}")
    await ed.call("close_scene", scene=sid2, unsaved="discard")
    check_job_error(await wait_job(ed, r.structured["job_id"]), ("invalid_source",), ed.project / out, "closed while encoding")

    # the GameObject gone before the call: refused at once with the fix
    msg = await ed.call("bake_vertex_animation", {**big, "source": "Hero", "scene": "Other"}, expect_error=True)
    check("GameObject" in msg and "Give" in msg, msg)


async def test_vertex_animator_preview_in_edit_mode(ed: Editor):
    """The guide's route to animate a Vertex Animator in edit mode: `preview` is a runtime flag (not a
    saved property: set_properties refuses it and says how), set through eval_lua with allow_edit_mode;
    the clock then moves without play, and the scene file never holds it."""
    copy_fixtures(ed.project)
    await ed.call("sync_files")
    sid = await scene(ed)
    out = "assets/vat/edit_mode_arm.vat"
    r = await bake(ed, source=ARM, clips=["Swing"], output=out, scene=sid, place="beside")
    entity = r.structured["entity"]["id"]
    msg = await ed.call("set_properties", {"scene": sid, "entity": entity, "values": {"vertexanimator": {"preview": True}}},
                        expect_error=True)
    check("eval_lua" in msg and "preview = true" in msg, f"set_properties says how to preview: {msg}")
    code = f"local a = object({entity}, 'Model'):getVertexAnimator()"
    loaded = False
    for _ in range(80):
        got = (await ed.call("eval_lua", code=code + " return a:isLoaded()", allow_edit_mode=True))["values"]
        if got == [True]:
            loaded = True
            break
        await asyncio.sleep(0.25)
    check(loaded, "the Vertex Animator loads in edit mode")
    first = (await ed.call("eval_lua", code=code + " a.preview = true return a.time", allow_edit_mode=True))["values"][0]
    later = first
    for _ in range(40):
        await asyncio.sleep(0.25)
        later = (await ed.call("eval_lua", code=code + " return a.time", allow_edit_mode=True))["values"][0]
        if abs(later - first) > 1e-3:
            break
    check(abs(later - first) > 1e-3, f"with preview on, time moves in edit mode: {first} then {later}")
    await ed.call("eval_lua", code=code + " a.preview = false", allow_edit_mode=True)
    await ed.call("save_scene", scene=sid)
    scene_files = list((ed.project / "scenes").glob("*.scene")) + list((ed.project / "assets").rglob("*.scene"))
    check(scene_files and all("preview:" not in f.read_text() for f in scene_files), "preview is never saved")


async def test_clip_objects_and_contradictions(ed: Editor):
    """clips entries may be {name, wrap, interpolate, fps} objects (the baker's clip rows): the written .vat
    has that wrap and interpolation; bad objects and unknown clips are refused with the choices; output: true
    with overwrite: true contradict (refused with the fix); a negative crowd seed is refused."""
    copy_fixtures(ed.project)
    await ed.call("sync_files")
    out = "assets/vat/clip_objects.vat"
    r = await bake(ed, source=ARM, clips=[{"name": "Swing", "wrap": "once", "interpolate": False, "fps": 12}, "Stretch"], output=out)
    vat = read_vat(ed.project, out)
    clips = {c["name"]: c for c in vat["clips"]}
    check(list(clips) == ["Swing", "Stretch"], f"the clips in order: {list(clips)}")
    check(clips["Swing"].get("wrap") == "once" and clips["Swing"].get("interpolate") is False, f"Swing's settings: {clips['Swing']}")
    check(clips["Stretch"].get("interpolate", True) is True, f"Stretch keeps interpolation: {clips['Stretch']}")
    check(abs(clips["Swing"]["fps"] - 12) < 1.5 and abs(clips["Stretch"]["fps"] - 30) < 3, f"per-clip fps: {vat['clips']}")
    cases = [
        ({"source": ARM, "clips": [{"name": "Swing", "wrap": "sideways"}]}, "clamp_forever"),
        ({"source": ARM, "clips": [{"wrap": "once"}]}, "clips holds clip names or {name"),
        ({"source": ARM, "clips": [{"name": "Swing", "speed": 2}]}, "clips[\"Swing\"].speed"),
        ({"source": ARM, "clips": [{"name": "Swing", "fps": 500}]}, "fps (0..240)"),
        ({"source": ARM, "clips": [{"name": "Fly", "wrap": "once"}]}, "Swing"),
        ({"source": ARM, "output": True, "overwrite": True}, "contradict"),
        ({"source": ARM, "output": "assets/vat/x.vat", "place": {"crowd": {"seed": -1}}}, "seed an integer >= 0"),
    ]
    for args, needle in cases:
        msg = await ed.call("bake_vertex_animation", args, expect_error=True)
        check(needle in msg, f"{args}: the error says {needle!r}: {msg}")
