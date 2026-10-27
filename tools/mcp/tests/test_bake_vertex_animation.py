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
- output true takes the default name; the same source again rewrites it (same GUID); another
  source with that default name takes a free name ("rigid_arm 2.vat"); an explicit existing output is
  refused without overwrite;
- rebake keeps hand edits (clip rename, speed, events) and the GUID;
- a long bake is a job (timeout_seconds) that get_job follows; cancel_job cancels one, nothing written;
- place beside / replace (one scene undo step; the source disabled) and crowd;
- imports of a synthetic OpenVAT export (preview + write) and a Houdini VAT 3 soft export;
- get_asset_info on a .vat (clips, layout, staleness, users) and on its .vat.glb (generated_by);
  list_assets type vertex_animation; view_asset refuses; write_file validates (generated fields refused
  with the rebake message, editable ones accepted with warnings); auto-sync reports a broken .vat;
- move_asset / delete_asset keep the .vat / .vat.glb pair;
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
    return {p.relative_to(project).as_posix() for p in (project / "assets").rglob("*") if p.is_file()}


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
    # the same source again: the default file is rewritten, GUID kept
    r = await bake(ed, source=ARM, output=True, fps=20)
    v = r.structured
    check(v["vat"] == vat and v["rewritten"] is True and v["guid"] == S["guid"], f"same source: rewritten in place: {v}")
    check({c["name"] for c in read_vat(ed.project, vat)["clips"]} == {"Swing", "Stretch"}, "now with both clips")
    # another source whose default name is the same: a free name
    made = await ed.call("edit_animation", clip="assets/models/rigid_arm.anim", create={"from": f"{ARM}#Swing"})
    check(made.get("created"), f"the .anim: {made}")
    r = await bake(ed, source=["assets/models/rigid_arm.anim"], on=ARM, output=True)
    check(r.structured["vat"] == "assets/models/rigid_arm 2.vat" and r.structured["rewritten"] is False,
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
    check({c["name"] for c in va["clips"]} >= {"Sway", "Stretch"} and va["layout"]["width"] > 0, f"clips and layout: {va}")
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
    vat.write_text(json.dumps(data, indent=2))
    r = await ed.call_raw("editor_status")
    ext = (r.structured or {}).get("external_changes") or {}
    found = [p for p in ext.get("problems", []) if p.get("path") == "assets/models/rigid_arm.vat"]
    check(found and "vat-layout-mismatch" in found[0]["message"], f"auto-sync reports the mismatch: {ext}")
    await asyncio.sleep(0.05)
    vat.write_text(good)
    r = await ed.call_raw("editor_status")
    ext = (r.structured or {}).get("external_changes") or {}
    check(not [p for p in ext.get("problems", []) if p.get("path") == "assets/models/rigid_arm.vat"], f"clean again: {ext}")
