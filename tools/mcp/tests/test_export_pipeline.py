"""Exports never ship wrong silently (docs/superpowers/specs/2026-11-04-export-pipeline-design.md).

A created project with a saved scene; each test exports the C++ source project (no compiler run) and
checks what landed:
- a copy or write that fails fails the export, naming the file (no success with files missing);
- an earlier export left inside the project is left out of the next one, with W-EXPORT-FOLDER-SKIPPED;
  an output folder inside the project is refused by MCP and the CLI alike;
- the saved shader keys of a deleted fork go at the next save (a split vert|frag fork keeps its own);
- Stop saves the variants of the objects scripts made during Play, in scenes a load replaced too;
- a never-saved scene gets its generated source (main.cpp loads it), with W-SCENE-NOT-SAVED;
- the web page step (export-settings/apply.cmake) writes index.html and always links a favicon;
- a source export in a folder named with spaces, @ and glob characters configures with every source.
"""

from __future__ import annotations

import asyncio
import os
import re
import shutil
import subprocess
from pathlib import Path

import yaml

import harness
from harness import Editor

CI = True
TIMEOUT = 1800
EDITOR = {"create": True, "name": "Pipeline Game"}

SPAWNER = """local Spawner = {}
function Spawner:init()
  -- an object no authored GameObject draws like: its variant exists only during Play
  local sphere = Primitive(self.scene)
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
    # an asset (binary: written on disk, as a user adds one), a Lua file and a header for the copy steps
    (ed.project / "ui").mkdir(parents=True, exist_ok=True)
    (ed.project / "ui" / "logo.png").write_bytes(b"not really a png")
    await ed.call("write_file", path="Game.lua", content="return {}\n")
    await ed.call("write_file", path="inc/Util.h", content="#pragma once\n")
    ed.check((ed.project / "scenes" / "Main.scene").is_file(), f"the scene file: {sorted(p.name for p in ed.project.rglob('*.scene'))}")


async def test_failed_copy_fails_the_export(ed: Editor):
    """A destination that exists as a folder where a file goes: the copy fails, and so does the
    export, naming the file (before: success, the file missing from the game)"""
    for rel in ("project/assets/ui/logo.png", "project/lua/Game.lua", "project/main.cpp"):
        out = ed.scratch.root / ("blocked-" + rel.replace("/", "_"))
        (out / rel).mkdir(parents=True)
        (out / rel / "keep").write_text("a folder where the file goes")
        job = await source_export(ed, out)
        ed.check_eq(job["status"], "failed", f"{rel}: {job.get('message')}")
        ed.check_in(Path(rel).name, job.get("message", ""), f"the failure names {rel}")


async def test_earlier_export_inside_project_is_left_out(ed: Editor):
    old = ed.project / "OldBuild"
    (old / "core").mkdir(parents=True)
    (old / "jasperengine.py").write_text("# an earlier source export\n")
    (old / "core" / "EngineThing.h").write_text("#pragma once\n")
    (old / "assets").mkdir()
    (old / "assets" / "stale.txt").write_text("stale")
    (old / "shaders").mkdir()
    (old / "shaders" / "stale.vert").write_text("void main() {}\n")
    out = ed.scratch.root / "export-skip"
    job = await source_export(ed, out)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    shipped = [p.relative_to(out / "project").as_posix() for p in (out / "project").rglob("*") if "OldBuild" in p.as_posix()]
    ed.check_eq(shipped, [], "nothing of the earlier export ships")
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
    await ed.call("close_scene", scene="Main")
    doc = yaml.safe_load(path.read_text())
    keys = doc.get("shaderKeys") or []
    mesh = next(k for k in keys if "customShader" not in k)
    doc["shaderKeys"] = keys + [dict(mesh, customShader="shaders/gone"),
                                dict(mesh, customShader="shaders/split_a.vert|shaders/split_b.frag")]
    path.write_text(yaml.safe_dump(doc, sort_keys=False))
    await ed.call("open_scene", scene=path.relative_to(ed.project).as_posix())
    await ed.call("set_transform", entity="Box", position=[0, 0.5, 0], scene="Main")
    await ed.call("save_scene", scene="Main")

    customs = [k.get("customShader") for k in saved_keys(path)]
    ed.check("shaders/gone" not in customs, f"the deleted fork's keys went: {customs}")
    ed.check_in("shaders/split_a.vert|shaders/split_b.frag", customs, "the split fork's keys stay")

    job = await source_export(ed, ed.scratch.root / "export-forks")
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")


async def test_play_created_objects_record_variants(ed: Editor):
    """Stop saves the variants of what scripts made: the played scene's (collected when a load
    replaced it) and the loaded scene's (collected at Stop, before the scripts went)"""
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
    main_before, second_before = len(saved_keys(main_file)), len(saved_keys(second_file))
    await ed.call("play", action="start", scene="Main")
    await ed.call("wait", until="secondReady == true", timeout_seconds=120)
    await ed.call("wait", frames=30)
    await ed.call("play", action="stop", keep_changes="none")
    # Stop saves the keys from a main-thread task: a few frames later
    for _ in range(100):
        if len(saved_keys(main_file)) > main_before and len(saved_keys(second_file)) > second_before:
            break
        await ed.call("editor_status")
        await asyncio.sleep(0.1)

    ed.check(len(saved_keys(main_file)) > main_before,
             f"the played scene gained the clearcoat sphere's variant: {main_before} -> {len(saved_keys(main_file))}")
    ed.check(len(saved_keys(second_file)) > second_before,
             f"the loaded scene gained the sheen sphere's variant: {second_before} -> {len(saved_keys(second_file))}")


async def test_never_saved_scene_ships(ed: Editor):
    created = await ed.call("create_scene", name="Draft")
    draft_id = created["scene"]["id"]
    out = ed.scratch.root / "export-unsaved"
    job = await source_export(ed, out)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    ed.check_in("W-SCENE-NOT-SAVED", warning_codes(job))
    sources = [p.name for p in (out / "project").glob(f"scene_*_{draft_id}.cpp")]
    ed.check_eq(len(sources), 1, f"the never-saved scene's source: {sorted(p.name for p in (out / 'project').glob('scene_*.cpp'))}")
    await ed.call("close_scene", scene=draft_id, unsaved="discard")


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
    (ed.project / "web" / "icon.png").write_bytes(b"icon bytes")
    await ed.call("set_project_settings", values={"web.favicon": "web/icon.png"})
    job = await source_export(ed, out)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    ed.check_eq((out / "project" / "export-settings" / "favicon.png").read_bytes(), b"icon bytes")
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
    ed.check(re.search(r"scene_\w+\.cpp", ninja) is not None, "the generated scene sources are compiled")
    ed.check_in("scene_scripts.cpp", ninja)
    ed.check_in("meshoptimizer", ninja)
    ed.check(re.search(r"Source/Core/\w+\.cpp", ninja) is not None, "RmlUi's sources are found")
