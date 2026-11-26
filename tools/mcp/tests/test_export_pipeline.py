"""Exports never ship wrong silently (docs/superpowers/specs/2026-11-04-export-pipeline-design.md).

A created project with a saved scene; each test exports the C++ source project (no compiler run) and
checks what landed:
- a copy or write that fails fails the export, naming the file (no success with files missing);
- an earlier export left inside the project is left out of the next one, with W-EXPORT-FOLDER-SKIPPED;
  an output folder inside the project is refused by MCP and the CLI alike;
- the saved shader keys of a deleted fork go at the next save (a split vert|frag fork keeps its own);
- Stop puts the variants of the objects scripts made during Play, in scenes a load replaced too, in the
  warm-up list, never in a scene file;
- a never-saved scene is cooked (game.json loads it), with W-SCENE-UNSAVED;
- the web page step (export-settings/apply.cmake) writes index.html and always links a favicon;
- a source export in a folder named with spaces, @ and glob characters configures with every source.

Build Report (docs/specs/build-size-tools.md): what an export left out and why, why each file ships
(Referenced By), the project's report history across a restart, a comparison of two exports, size
budgets that warn or fail a build (MCP and the CLI). Red control: on the base tree get_build_info
refuses report / compare_to / referenced_by (unknown arguments), build-report.json has no left_out
and set_project_settings refuses the player.size_budgets key.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path

import yaml

import harness
from harness import REPO, Editor

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_build_method import RUNTIME, runtime_archive  # noqa: E402

CI = True
TIMEOUT = 1800
EDITOR = {"create": True, "name": "Pipeline Game"}
S: dict = {}

SPAWNER = """local Spawner = {}
function Spawner:init()
  -- an object no authored GameObject draws like: its variant exists only during Play. Kept in
  -- self: the wrapper owns its entity, which goes when the collector takes it (lua-api guide)
  local sphere = Primitive(self.scene)
  self.sphere = sphere
  sphere:createSphere(1)
  local m = sphere:getMaterial()
  m.clearcoatEnabled = true
  m.clearcoatFactor = 1
  sphere:setMaterial(m)
  self.frames = 0
  RegisterEngineEvent(self, "onUpdate")
end
function Spawner:onUpdate()
  self.frames = self.frames + 1
  if self.frames == 20 then SceneManager.loadScene("Second") end
end
return Spawner
"""

SECOND_SPAWNER = """local SecondSpawner = {}
function SecondSpawner:init()
  local sphere = Primitive(self.scene)
  self.sphere = sphere
  sphere:createSphere(1)
  local m = sphere:getMaterial()
  m.sheenEnabled = true
  m.sheenRoughness = 0.5
  sphere:setMaterial(m)
  _G.secondReady = true
end
return SecondSpawner
"""


async def source_export(ed: Editor, out: Path, **args) -> dict:
    return await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"],
                         overwrite=True, timeout_seconds=900, **args)


def warning_codes(job: dict) -> list[str]:
    return [w.get("code", "") for w in job.get("result", {}).get("warnings", []) if isinstance(w, dict)]


def scene_file(ed: Editor, name: str) -> Path:
    found = sorted(ed.project.rglob(f"{name}.scene"))
    if not found:
        ed.fail(f"no {name}.scene in {ed.project}")
    return found[0]


def saved_keys(path: Path) -> list[dict]:
    return yaml.safe_load(path.read_text()).get("shaderKeys") or []


async def test_prepare(ed: Editor):
    await ed.call("create_entity", name="Box", type="cube")
    # named before its first save, so its file is scenes/Main.scene
    await ed.call("rename_scene", name="Main")
    s = await ed.call("save_project", untitled_scene_dir="scenes")
    scene = s["scenes_in_build"][0]
    await ed.call("edit_build_scenes", action="set_start", scene=scene["id"])
    await ed.call("set_project_settings", values={"graphics.runtime_shaders": False})
    # an asset (binary data, written on disk as a user adds one; not a texture, which ships cooked),
    # a Lua file and a header for the copy steps
    (ed.project / "ui").mkdir(parents=True, exist_ok=True)
    (ed.project / "ui" / "level.bin").write_bytes(b"\x00\x01level data")
    await ed.call("write_file", path="Game.lua", content="return {}\n")
    await ed.call("write_file", path="inc/Util.h", content="#pragma once\n")
    ed.check((ed.project / "scenes" / "Main.scene").is_file(), f"the scene file: {sorted(p.name for p in ed.project.rglob('*.scene'))}")


async def test_failed_copy_fails_the_export(ed: Editor):
    """A destination that exists as a folder where a file goes: the copy fails, and so does the
    export, naming the file (before: success, the file missing from the game)"""
    for rel in ("project/assets/ui/level.bin", "project/lua/Game.lua", "project/scripts/inc/Util.h", "project/main.cpp"):
        out = ed.scratch.root / ("blocked-" + rel.replace("/", "_"))
        (out / rel).mkdir(parents=True)
        (out / rel / "keep").write_text("a folder where the file goes")
        msg = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"],
                            overwrite=True, timeout_seconds=900, expect_error=True)
        ed.check_in(Path(rel).name, msg, f"the failure names {rel}")


async def test_earlier_export_inside_project_is_left_out(ed: Editor):
    old = ed.project / "OldBuild"
    (old / "core").mkdir(parents=True)
    (old / "jasperengine.py").write_text("# an earlier source export\n")
    (old / "core" / "EngineThing.h").write_text("#pragma once\n")
    (old / "assets").mkdir()
    (old / "assets" / "stale.txt").write_text("stale")
    (old / "shaders").mkdir()
    (old / "shaders" / "stale.vert").write_text("void main() {}\n")
    # what the cooks take from the asset list, not from the export's walk: a texture and a model
    (old / "assets" / "stale.png").write_bytes(tiny_png())
    (old / "assets" / "stale.gltf").write_text(tiny_gltf())
    # and an engine source, which would make a player build compile
    (old / "core" / "EngineThing.cpp").write_text("int engineThing() { return 0; }\n")
    await ed.call("sync_files")
    # a script root at the project's top would take the earlier export's engine sources: a player
    # build would compile them (Exporter::chooseBuildMethod)
    await ed.call("set_project_settings", values={"scripting.script_dirs": ["."]})
    info = await ed.call("get_build_info")
    await ed.call("set_project_settings", values={"scripting.script_dirs": []})
    reasons = [r for platform in info.get("platforms", {}).values() for r in platform.get("compile_reasons", [])]
    ed.check(not any("OldBuild" in r or "EngineThing" in r for r in reasons), f"no build compiles the earlier export: {reasons}")
    out = ed.scratch.root / "export-skip"
    job = await source_export(ed, out)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    shipped = [p.relative_to(out / "project").as_posix() for p in (out / "project").rglob("*") if "OldBuild" in p.as_posix()]
    ed.check_eq(shipped, [], "nothing of the earlier export ships")
    # the cooks name their files by content hash: their manifests say which assets they took
    cooked = [p.relative_to(out / "project").as_posix() for p in (out / "project" / "assets").rglob("*.json")
              if "je-cooked" in p.as_posix() and "OldBuild" in p.read_text(errors="replace")]
    ed.check_eq(cooked, [], "no texture or model of the earlier export is cooked")
    ed.check_in("W-EXPORT-FOLDER-SKIPPED", warning_codes(job))
    ed.check(any("OldBuild" in w.get("message", "") for w in job["result"]["warnings"] if isinstance(w, dict)),
             f"the warning names the folder: {job['result']['warnings']}")
    shutil.rmtree(old)


async def test_output_inside_project_refused(ed: Editor):
    msg = await ed.call("build_project", mode="source", output_dir=str(ed.project / "Builds"), expect_error=True)
    ed.check_in("inside the project", msg)
    # the command line refuses it too (before: it exported into the project)
    proc = subprocess.run(harness.editor_command() + ["export", "--project", str(ed.project), "--out", str(ed.project / "Builds"),
                                                      "--backend", "opengl"],
                          capture_output=True, text=True, timeout=900, env={**os.environ, "HOME": str(ed.scratch.home)})
    ed.check(proc.returncode != 0, f"the CLI refused: {proc.returncode}\n{proc.stdout[-2000:]}\n{proc.stderr[-2000:]}")
    ed.check_in("inside the project", proc.stderr)
    ed.check(not (ed.project / "Builds" / "CMakeLists.txt").exists(), "nothing exported into the project")
    shutil.rmtree(ed.project / "Builds", ignore_errors=True)


async def test_dead_fork_keys_dropped(ed: Editor):
    """A scene's saved keys naming a fork whose files are gone: the next save drops them and the
    export builds (before: every export failed building them). A split vert|frag fork is live."""
    await ed.call("fork_shader", entity="Box", name="Box")
    await ed.call("save_project")
    # a split fork: its stages in files of different names (shaders/split_a.vert|shaders/split_b.frag)
    await ed.call("write_file", path="shaders/split_a.vert", content=(ed.project / "shaders" / "Box.vert").read_text())
    await ed.call("write_file", path="shaders/split_b.frag", content=(ed.project / "shaders" / "Box.frag").read_text())

    path = scene_file(ed, "Main")
    # the last open scene can't close: an untitled one stays open meanwhile
    await ed.call("create_scene", name="Temp")
    await ed.call("close_scene", scene="Main")
    doc = yaml.safe_load(path.read_text())
    keys = doc.get("shaderKeys") or []
    mesh = next(k for k in keys if "customShader" not in k)
    doc["shaderKeys"] = keys + [dict(mesh, customShader="shaders/gone"),
                                dict(mesh, customShader="shaders/split_a.vert|shaders/split_b.frag")]
    path.write_text(yaml.safe_dump(doc, sort_keys=False))
    await ed.call("open_scene", scene=path.relative_to(ed.project).as_posix())
    await ed.call("close_scene", scene="Temp", unsaved="discard")
    await ed.call("set_transform", entity="Box", position=[0, 0.5, 0], scene="Main")
    await ed.call("save_scene", scene="Main")

    customs = [k.get("customShader") for k in saved_keys(path)]
    ed.check("shaders/gone" not in customs, f"the deleted fork's keys went: {customs}")
    ed.check_in("shaders/split_a.vert|shaders/split_b.frag", customs, "the split fork's keys stay")

    job = await source_export(ed, ed.scratch.root / "export-forks")
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")


async def test_play_created_objects_record_variants(ed: Editor):
    """Stop puts the variants of what scripts made in the warm-up list (ShaderWarmup.json), never in
    a scene file (S98 REQ-27): the played scene's (collected when a load replaced it) and the loaded
    scene's (collected at Stop, before the scripts went)"""
    second = await ed.call("create_scene", name="Second", path="scenes/Second.scene")
    second_id = second["scene"]["id"]
    await ed.call("create_entity", name="SecondSpawner", type="empty", scene=second_id)
    await ed.call("create_script", kind="lua", name="SecondSpawner", entity="SecondSpawner", scene=second_id, content=SECOND_SPAWNER)
    await ed.call("save_scene", scene=second_id)
    await ed.call("select_scene", scene="Main")
    await ed.call("create_entity", name="Spawner", type="empty", scene="Main")
    await ed.call("create_script", kind="lua", name="Spawner", entity="Spawner", scene="Main", content=SPAWNER)
    await ed.call("save_project")

    main_file, second_file = scene_file(ed, "Main"), scene_file(ed, "Second")
    main_before, second_before = main_file.read_bytes(), second_file.read_bytes()
    warmup = ed.project / "ShaderWarmup.json"

    def customs() -> list[str]:
        return [s.get("custom", "") for s in json.loads(warmup.read_text())["shaders"]] if warmup.exists() else []

    def found(feature: str) -> bool:
        return any(feature in c for c in customs())

    await ed.call("play", action="start", scene="Main")
    await ed.call("wait", until="secondReady == true", timeout_seconds=120)
    await ed.call("wait", frames=30)
    await ed.call("play", action="stop", keep_changes="none")
    # Stop adds them from a main-thread task: a few frames later
    for _ in range(100):
        if found("PBR_CLEARCOAT") and found("PBR_SHEEN"):
            break
        await ed.call("editor_status")
        await asyncio.sleep(0.1)

    ed.check(found("PBR_CLEARCOAT"), f"the played scene's clearcoat sphere's variant is listed: {customs()}")
    ed.check(found("PBR_SHEEN"), f"the loaded scene's sheen sphere's variant is listed: {customs()}")
    ed.check(main_file.read_bytes() == main_before and second_file.read_bytes() == second_before, "Stop wrote no scene file")


async def test_never_saved_scene_ships(ed: Editor):
    created = await ed.call("create_scene", name="Draft")
    draft_id = created["scene"]["id"]
    out = ed.scratch.root / "export-unsaved"
    job = await source_export(ed, out)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    ed.check_in("W-SCENE-UNSAVED", warning_codes(job))
    ed.check((out / "project" / "content" / "scenes" / f"{draft_id}.jscn").is_file(), "the never-saved scene is cooked")
    game = json.loads((out / "project" / "content" / "game.json").read_text())
    ed.check_in("Draft", [scene["name"] for scene in game["scenes"]], "game.json loads the never-saved scene")
    await ed.call("close_scene", scene=draft_id, unsaved="discard")


def tiny_png() -> bytes:
    """A valid 1x1 RGBA PNG (the texture cook decodes the project's images)."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(b"\x00\xff\x80\x00\xff")) + chunk(b"IEND", b"")


def tiny_gltf() -> str:
    """A one-triangle glTF, its buffer inline."""
    import base64
    import json
    positions = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    indices = struct.pack("<3H", 0, 1, 2) + b"\x00\x00"
    data = positions + indices
    return json.dumps({
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]})


def logo_bytes() -> bytes:
    header = (Path(__file__).resolve().parents[3] / "editor" / "resources" / "icons" / "jasperengine-logo_png.h").read_text()
    body = header[header.index("{") + 1:header.index("}")]
    return bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", body))


async def test_web_page_and_favicon(ed: Editor):
    """export-settings/apply.cmake, which every web build runs on the linked page: index.html next to
    <app>.export.html, and a favicon (the JasperEngine logo without Web > Favicon or a Player icon)"""
    out = ed.scratch.root / "export-web-page"
    job = await source_export(ed, out)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    apply = out / "project" / "export-settings" / "apply.cmake"
    ed.check(apply.is_file(), f"no {apply}")
    ed.check_eq((out / "project" / "export-settings" / "favicon.png").read_bytes(), logo_bytes(), "the default favicon is the logo")

    for app in ("game", "index"):
        build = ed.scratch.root / f"web-page-{app}"
        build.mkdir()
        page = build / f"{app}.html"
        page.write_text("<html><head><title>x</title></head><body><script src=\"x.js\"></script></body></html>")
        proc = subprocess.run(["cmake", f"-DAPP_FILE={page}", "-DWEB=ON", "-P", str(apply)], capture_output=True, text=True, timeout=120)
        ed.check_eq(proc.returncode, 0, proc.stderr)
        exported = (build / f"{app}.export.html").read_text()
        ed.check_in('<link rel="icon" href="favicon.png">', exported)
        ed.check((build / "favicon.png").is_file(), "the favicon next to the page")
        if app == "index":
            ed.check_in("<script", (build / "index.html").read_text(), "the linker's own index.html is left alone")
            ed.check("favicon" not in (build / "index.html").read_text(), "and not overwritten")
        else:
            ed.check_eq((build / "index.html").read_text(), exported, "index.html is the page")

    # Web > Favicon wins
    (ed.project / "web").mkdir(parents=True, exist_ok=True)
    (ed.project / "web" / "icon.png").write_bytes(tiny_png())
    await ed.call("set_project_settings", values={"web.favicon": "web/icon.png"})
    job = await source_export(ed, out)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    ed.check_eq((out / "project" / "export-settings" / "favicon.png").read_bytes(), tiny_png())
    await ed.call("set_project_settings", values={"web.favicon": ""})


async def test_export_paths_with_special_characters(ed: Editor):
    """A source export below "My Game @1 [x]" configures with the project's sources and the engine's:
    the CMake globs escape the folder (before: "[x]" read as a pattern, the globs found nothing)"""
    out = ed.scratch.root / "My Game @1 [x]"
    job = await source_export(ed, out)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    build = out / "build"
    proc = subprocess.run(["cmake", "-S", str(out), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                           "-DGRAPHIC_BACKEND=glcore"], capture_output=True, text=True, timeout=900)
    ed.check_eq(proc.returncode, 0, proc.stdout[-3000:] + proc.stderr[-3000:])
    ninja = (build / "build.ninja").read_text(errors="replace")
    ed.check(re.search(r"scene_(?!scripts)\w+\.cpp", ninja) is None, "no scene has code: every build loads it cooked")
    ed.check_in("scene_scripts.cpp", ninja)
    ed.check_in("meshoptimizer", ninja)
    ed.check(re.search(r"Source/Core/\w+\.cpp", ninja) is not None, "RmlUi's sources are found")


def noise_png(width: int, height: int, seed: int) -> bytes:
    """An RGBA PNG of pseudo-random pixels (barely compressible: its size is about width * height * 4)"""
    state = seed * 2654435761 & 0xFFFFFFFF
    rows = bytearray()
    for _ in range(height):
        rows.append(0)
        for _ in range(width):
            state = (state * 1103515245 + 12345) & 0xFFFFFFFF
            rows += state.to_bytes(4, "little")
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 1)) + chunk(b"IEND", b""))


PROCMAT = """{
  "formatVersion": 1, "kind": "procedural_material", "kindVersion": 1, "nextId": 2,
  "nodes": [{"id": "n1", "type": "perlin", "position": [0, 0]}], "guid": "fedcba9876543210fedcba9876543210"
}
"""


def report_of(out: Path) -> dict:
    report = out / "build-report.json"
    if not report.is_file():
        raise AssertionError(f"no build-report.json in {out}")
    return json.loads(report.read_text())


async def test_build_report_left_out(ed: Editor):
    """AC3: an .aseprite, an atlas-only texture, a cooked texture and a Procedural Material are listed in
    left_out with their reason and source bytes"""
    await ed.call("tool_groups", enable=["sprites"])
    files = {
        "art/hero.aseprite": (REPO / "editor" / "tests" / "fixtures" / "aseprite" / "s44-mcp" / "hero_idle.aseprite").read_bytes(),
        "art/sword.png": noise_png(16, 16, 1),
        "textures/wall.png": noise_png(64, 64, 2),
        "textures/orphan.png": noise_png(32, 32, 3),
        "materials/wood.procmat": PROCMAT.encode(),
    }
    for rel, data in files.items():
        (ed.project / rel).parent.mkdir(parents=True, exist_ok=True)
        (ed.project / rel).write_bytes(data)
    await ed.call("sync_files", paths=list(files))
    # a texture only a sprite reads, packed in an atlas: it ships as the atlas's pages
    await ed.call("set_sprite_frames", scene="Main", create={"name": "Sword", "position": [0, 0, 0]}, sheet="art/sword.png")
    await ed.call("edit_sprite_atlas", path="art/items.spriteatlas", create=True, add=["art/sword.png"], output=True)
    # a texture a material reads (scene -> material -> texture), for test_build_report_referenced_by
    await ed.call("create_material", path="materials/wall.material", values={"baseColorTexture": "textures/wall.png"})
    await ed.call("link_material", scene="Main", entity="Box", material="materials/wall.material")
    await ed.call("save_project")
    out = ed.scratch.root / "report-1"
    job = await source_export(ed, out)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    data = report_of(out)
    left = {item["source"]: item for item in data["left_out"]}
    for source, reason in (("art/hero.aseprite", "aseprite_import"), ("art/sword.png", "atlas_only"),
                           ("textures/wall.png", "cooked_texture"), ("materials/wood.procmat", "procedural_material_baked")):
        ed.check(source in left, f"{source} is left out: {sorted(left)}")
        ed.check_eq(left[source]["reason"], reason, source)
        ed.check_eq(left[source]["bytes"], os.path.getsize(ed.project / source), f"{source}'s bytes")
    ed.check_eq(data["left_out_bytes"], sum(item["bytes"] for item in data["left_out"]))
    ed.check_eq([item["source"] for item in data["left_out"]], sorted(item["source"] for item in data["left_out"]), "in source path order")
    ed.check(len(data["files"]) >= len(data["largest"]) and data["version"] == 1, "every file, version 1")
    ed.check(data["id"].endswith("-source") and data["created"].endswith("Z"), f"{data['id']} {data['created']}")
    S["first_report"] = data["id"]


async def test_build_report_referenced_by(ed: Editor):
    """AC4: why a texture and a material ship (their chains back to project.yaml), and an orphan unreferenced with the note;
    get_build_report is in the shipping group: hidden in a new session, listed once tool_groups enables it"""
    ed.check("get_build_report" not in {t.name for t in await ed.tools(groups=None)}, "hidden in a new session")
    await ed.call("tool_groups", enable=["shipping"])
    ed.check("get_build_report" in {t.name for t in await ed.tools()}, "listed once shipping is enabled")
    r = await ed.call_raw("get_build_report", {"referenced_by": ["textures/wall.png", "textures/orphan.png"]})
    ed.check(not r.is_error, r.text)
    items = {item["asset"]: item for item in r.structured["referenced_by"]}
    # a linked submesh keeps a copy of the material's values: the scene reaches the texture itself,
    # the shortest way back to a root (and the material file through its link)
    wall = items["textures/wall.png"]
    ed.check_eq(wall["reason"], "reached", f"{wall}")
    ed.check_eq([step["asset"] for step in wall["chain"]], ["textures/wall.png", "scenes/Main.scene", "project.yaml"], f"{wall['chain']}")
    ed.check(wall["chain"][0].get("object") == "Box" and "MeshComponent" in wall["chain"][0]["field"], f"the field of Box: {wall['chain'][0]}")
    ed.check_eq(wall["chain"][1]["kind"], "scene_list")
    ed.check_eq(wall["chain"][-1]["kind"], "root")
    ed.check_eq(wall["live"], False, "from the export's walk")
    material = (await ed.call("get_build_report", referenced_by=["materials/wall.material"]))["referenced_by"][0]
    ed.check_eq([step["asset"] for step in material["chain"]], ["materials/wall.material", "scenes/Main.scene", "project.yaml"], f"{material}")
    ed.check_eq(material["chain"][0]["kind"], "mesh_material_file")
    orphan = items["textures/orphan.png"]
    ed.check_eq(orphan["reason"], "unreferenced")
    ed.check_eq(orphan["chain"], [])
    ed.check_in("the index sees `require` only", r.structured.get("next", ""))
    ed.check_in("delete it, or keep it if a script loads it by a computed path", r.structured.get("next", ""))


async def test_build_report_compare(ed: Editor):
    """AC7: a texture replaced with a larger one between two exports is one grown row with its delta"""
    (ed.project / "textures" / "wall.png").write_bytes(noise_png(256, 256, 4))
    await ed.call("sync_files", paths=["textures/wall.png"])
    out = ed.scratch.root / "report-2"
    job = await source_export(ed, out)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    second = report_of(out)
    r = await ed.call_raw("get_build_report", {"compare_to": "previous"})
    ed.check(not r.is_error, r.text)
    comparison = r.structured["comparison"]
    ed.check_eq(comparison["base"], S["first_report"])
    rows = {row["key"]: row for row in comparison["changes"]}
    ed.check("textures/wall.png" in rows, f"the texture's row: {list(rows)}")
    wall = rows["textures/wall.png"]
    ed.check_eq(wall["change"], "grown")
    ed.check(wall["delta"] > 0 and wall["delta"] == wall["after"] - wall["before"], f"{wall}")
    textures = comparison["categories"]["textures"]
    ed.check(textures["delta"] > 0 and textures["after"] == second["categories"]["textures"], f"{textures}")
    ed.check_eq(comparison["partial"], False)
    ed.check_eq(comparison["platform_differs"], False)
    ed.check_in(f"Build Report source {second['id']}", r.text)
    ed.check_in(f"vs {S['first_report']}", r.text)
    ed.check(re.search(r"textures \+[0-9.]+ [KM]?B \(\d+ grown", r.text) is not None, r.text[:400])
    msg = await ed.call("get_build_report", compare_to="20000101-000000-source", expect_error=True)
    ed.check_in(S["first_report"], msg, "an unknown id lists the history")
    S["second_report"] = second["id"]


async def test_build_report_history_survives_restart(ed: Editor):
    """AC5: after a restart the Build Report window and get_build_info report: last open the newest export, and
    get_build_info's next names get_build_report"""
    # a still copy: the running editor writes its caches meanwhile
    still = ed.scratch.root / "report-restart-project"
    shutil.copytree(ed.project, still, symlinks=True, ignore=shutil.ignore_patterns("*.je-tmp", ".git"))
    async with Editor(project=still, label="report-restart") as again:
        last = await again.call("get_build_report", report="last")
        again.check_eq(last["report"]["id"], S["second_report"])
        again.check_eq(last["report"]["history"][:2], [S["second_report"], S["first_report"]])
        # agents asked why a file ships grepped the project: get_build_info points at the report (no build ran in this session)
        info = await again.call("get_build_info")
        again.check_eq(info.get("next"), "get_build_report (group shipping): sizes, what grew, why a file ships")
        await again.call("tool_groups", enable=["editor_ux"])
        await again.call("run_command", id="window.buildReport")
        for _ in range(240):
            if (await again.call("find_ui_item", label="Unreferenced", window="Build Report"))["items"]:
                break
            await again.call("wait", frames=2)
        else:
            again.fail("the Build Report window shows no report after the restart")


async def install_stand_in_runtime(ed: Editor) -> None:
    """test_build_method's stand-in player, advertising every module of the full variant: the earlier
    tests of this file leave scripts in the project (a C++ one among them), which need every module"""
    info = await ed.call("get_build_info")
    if not any(p["id"] == RUNTIME and p["installed"] for p in info["players"]):
        plain = runtime_archive(ed.scratch.root / f"plain-{RUNTIME}.zip", info["editor_version"])
        archive = ed.scratch.root / f"jasperengine-player-{RUNTIME}.zip"
        modules = json.loads((REPO / "engine" / "player" / "variants.json").read_text())["variants"][0]["modules"]
        with zipfile.ZipFile(plain) as source, zipfile.ZipFile(archive, "w") as target:
            for item in source.infolist():
                data = source.read(item.filename)
                if item.filename == "player-info.json":
                    player = json.loads(data)
                    player["features"] = modules
                    data = json.dumps(player).encode()
                target.writestr(item, data)
        r = await ed.call("install_player_runtime", archive=str(archive), timeout_seconds=120)
        ed.check_eq(r["status"], "succeeded", f"{r}")


async def test_size_budget_warns_and_fails(ed: Editor):
    """AC8: a 1 MB textures budget over a texture of several MB warns naming it; with fail the prebuilt build stops
    before packing, keeping build-report.json and its history entry with the budget rows"""
    await install_stand_in_runtime(ed)
    (ed.project / "textures" / "big.png").write_bytes(noise_png(1024, 1024, 5))
    await ed.call("sync_files", paths=["textures/big.png"])
    await ed.call("set_project_settings", values={"player.size_budgets": [{"platform": "all", "category": "textures", "limit_mb": 1}],
                                                  "player.size_budget_action": "warn"})
    warned = ed.scratch.root / "budget-warn"
    r = await ed.call("build_project", mode="player", platform="linux", method="prebuilt", output_dir=str(warned), timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    budgets = [w["message"] for w in r["result"]["warnings"] if w.get("code") == "W-SIZE-BUDGET"]
    ed.check(budgets and "textures/big.png" in budgets[0], f"W-SIZE-BUDGET names the texture: {r['result']['warnings']}")
    ed.check_in("Project Settings > Player > Size Budgets", budgets[0])
    rows = report_of(warned)["budgets"]
    ed.check(rows and rows[0]["category"] == "textures" and rows[0]["limit_bytes"] == 1048576 and rows[0]["over_bytes"] > 0, f"{rows}")

    await ed.call("set_project_settings", values={"player.size_budget_action": "fail"})
    failed = ed.scratch.root / "budget-fail"
    msg = await ed.call("build_project", mode="player", platform="linux", method="prebuilt", output_dir=str(failed), timeout_seconds=600,
                        expect_error=True)
    ed.check_in("Size budget", msg)
    ed.check_in("textures/big.png", msg)
    data = report_of(failed)
    ed.check(data["budgets"] and data["budgets"][0]["over_bytes"] > 0, f"{data['budgets']}")
    ed.check(not any(f.get("category") == "engine" or f["path"].endswith(".so") for f in data["files"]), "stopped before the engine was measured")
    history = ed.project / ".jasperengine" / "build-reports" / "linux" / f"{data['id']}.json"
    ed.check(history.is_file(), f"the history entry {history}")
    ed.check_eq(json.loads(history.read_text())["budgets"], data["budgets"])


async def test_cli_build_json_budget(ed: Editor):
    """AC18: the CLI's build --json with a fail budget exits non-zero and prints build_report_id and size_budgets"""
    await ed.call("save_project")
    out = ed.scratch.root / "budget-cli"
    proc = await asyncio.to_thread(subprocess.run, harness.editor_command() + ["build", "--project", str(ed.project), "--platform", "linux",
                                                                             "--method", "prebuilt", "--out", str(out), "--json"],
                                   capture_output=True, text=True, timeout=900, env={**os.environ, "HOME": str(ed.scratch.home)})
    ed.check(proc.returncode != 0, f"exit {proc.returncode}: {proc.stdout[-2000:]} {proc.stderr[-2000:]}")
    report = json.loads(proc.stdout[proc.stdout.index("{"):])
    ed.check_eq(report["ok"], False)
    ed.check_in("Size budget", report.get("error", ""))
    ed.check(report.get("build_report_id", "").endswith("-prebuilt"), f"{report.get('build_report_id')}")
    ed.check(report["size_budgets"] and report["size_budgets"][0]["over_bytes"] > 0, f"{report.get('size_budgets')}")
    await ed.call("set_project_settings", values={"player.size_budgets": [], "player.size_budget_action": "warn"})


async def test_a_map_the_editor_just_wrote_is_not_missing():
    """A terrain heightmap set_terrain_maps has just written is on disk before any file watcher walk lists it:
    an export right after takes it as it is, not as a reference to a missing asset (T5's foliage export hit this)"""
    async with Editor(create=True, name="Fresh Map") as ed:
        sid = (await ed.call("create_scene", name="Land", type="3d", path="scenes/Land"))["scene"]["id"]
        terrain = (await ed.call("create_entity", scene=sid, name="Land", type="terrain"))["entity_id"]
        await ed.call("set_terrain_maps", scene=sid, entity=terrain, heightmap={"generate": "fractal_noise", "resolution": 129, "seed": 3})
        await ed.call("save_scene", scene=sid)
        r = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(ed.scratch.root / "fresh-map"), "platform": "linux",
                                                "backends": ["opengl"], "timeout_seconds": 900})
        ed.check(not r.is_error, f"the export takes the map just written: {r.text[:1500]}")
