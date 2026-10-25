"""CLI project/file/output arguments resolve from the caller's directory.

Authored two-scene inputs cover parent-relative and Unicode paths without a game fixture.
Export must preserve payload bytes and produce a usable CMake entry point; scene dumps and
benchmark records must identify the requested authored scenes. Other commands keep their
explicit cwd-versus-project path resolution contracts.
"""

from __future__ import annotations

import atexit
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import yaml

import harness
from harness import TestFailure
from project_fixture import write_project


TIMEOUT = 1800

UNICODE_DIR = "games/My Game ü"
SCENES = {"World.scene", "Overlay.scene"}

EFFECT_FRAG = """#version 450
// @effect     path-glow
// @kind       paint
// @category   light
// @summary    A project effect of the CLI path tests
// @cost       low
// @param      color color #ff8800 -- Colour
// @param      amount number 0.5 0 1 -- Strength

#include "effects/lib/jfx_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
    float amount;
} params;

vec4 jfx_paint(JfxContext c){
    return params.color * params.amount;
}

#include "effects/lib/main_paint.glsl"
"""

LOCAL_HTML = "<html><body><p>local only</p></body></html>\n"

S: dict = {}


# ---------------------------------------------------------------------------- helpers

def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def layout() -> Path:
    """A fresh scratch tree per case:  <tmp>/work/proj, <tmp>/work/games/My Game ü (copies of the fixture)
    and <tmp>/work/sub/deep (empty working directories, for the ../ forms). Returns <tmp>/work.

    From <tmp>/work/sub, "../proj" joined twice is "../proj/../proj", the same directory: the forms from
    <tmp>/work/sub/deep ("../../proj" twice is <tmp>/proj) are the ones a doubled join breaks."""
    S.clear()  # Each case owns new inputs; source exports/imports must not alter later tests.
    tmp = Path(tempfile.mkdtemp(prefix="jasper-cli-paths-"))
    S["tmp"] = tmp
    atexit.register(shutil.rmtree, tmp, ignore_errors=True)
    work = tmp / "work"
    fixture = write_project(tmp / "input", scenes=True)
    shutil.copytree(fixture, work / "proj")
    shutil.copytree(fixture, work / UNICODE_DIR)
    (work / "sub" / "deep").mkdir(parents=True)
    (tmp / "home").mkdir()
    S["work"] = work
    return work


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"):
        e.pop(k, None)
    e["HOME"] = str(S["tmp"] / "home")
    e["LIBGL_ALWAYS_SOFTWARE"] = "1"
    return e


def run(cwd: Path, *args: str, timeout: float = 900, extra_env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    e = env()
    e.update(extra_env or {})
    return subprocess.run(harness.editor_command() + list(args), cwd=str(cwd), env=e, capture_output=True, text=True,
                          timeout=timeout)


def ok(r: subprocess.CompletedProcess, what: str) -> subprocess.CompletedProcess:
    check(r.returncode == 0, f"{what}: exit {r.returncode}\nstdout: {r.stdout[-3000:]}\nstderr: {r.stderr[-3000:]}")
    check("bad file" not in r.stdout + r.stderr, f"{what}: a path joined twice:\n{(r.stdout + r.stderr)[-3000:]}")
    return r


def files_under(root: Path) -> set[str]:
    return {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}


def project_forms(work: Path) -> list[tuple[Path, str]]:
    """Distinct normalization cases; full exports separately cover plain relative paths."""
    return [
        (work, "./proj/"),
        (work / "sub" / "deep", "../../sub/../proj/project.yaml"),
        (work / "sub" / "deep", f"../../{UNICODE_DIR}/"),
        (work / "proj", "."),
    ]


def check_export(out: Path, project: Path, what: str) -> None:
    """Exported project files have the caller's bytes and a usable CMake entry point."""
    check((out / "CMakeLists.txt").is_file(), f"{what}: no exported CMake entry point")
    for target in ("assets/payload.txt", "scripts/Probe.lua"):
        source = project / target
        exported = out / "project" / ("lua/Probe.lua" if target.endswith(".lua") else target)
        check(exported.is_file() and exported.read_bytes() == source.read_bytes(),
              f"{what}: exported {target} preserves input bytes")
    result = subprocess.run(["cmake", "-S", str(out), "-B", str(out / "configure"),
                             "-DCMAKE_BUILD_TYPE=Release", "-DGRAPHIC_BACKEND=glcore"],
                            env=env(), capture_output=True, text=True, timeout=180)
    check(result.returncode == 0, f"{what}: exported CMake project configures: {result.stdout[-2000:]}{result.stderr[-2000:]}")


# ---------------------------------------------------------------------------- export

async def test_export_lists_scenes_for_every_project_form():
    work = layout()
    for cwd, project in project_forms(work):
        r = ok(run(cwd, "export", "--project", project, "--list-scenes", timeout=300), f"export --project {project!r} --list-scenes")
        listed = {line.split("\t")[2] for line in r.stdout.splitlines() if line.count("\t") == 2}
        check(set(SCENES) <= listed, f"--project {project!r} (cwd {cwd.name}): scenes {sorted(listed)}, want {sorted(SCENES)}\n"
                                     f"stderr: {r.stderr[-2000:]}")


async def test_export_dotdot_project_yaml_and_out():
    work = layout()
    out = work / "exports" / "Ünïcode out"
    ok(run(work / "sub" / "deep", "export", "--project", f"../../{UNICODE_DIR}/project.yaml",
           "--out", "../../exports/./Ünïcode out", "--backend", "opengl"),
       "export a Unicode project.yaml through parent-relative paths")
    check_export(out, work / UNICODE_DIR, "parent-relative Unicode export")
    check(not (work / "sub" / "exports").exists() and not (work / UNICODE_DIR / "exports").exists(),
          "--out is relative to the working directory")


# ---------------------------------------------------------------------------- scene-dump

def dumped_scenes(path: Path) -> dict[str, int]:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    return {scene["name"]: len(scene.get("entities") or {}) for scene in data.get("scenes") or []}


async def test_scene_dump_relative_paths():
    work = layout()
    sub = work / "sub"
    ok(run(sub / "deep", "scene-dump", "--project", f"../../{UNICODE_DIR}/project.yaml",
           "--out", "../dump ü.yaml", timeout=600), "scene-dump parent-relative Unicode project")
    check((sub / "dump ü.yaml").is_file(), "the dump is written relative to the working directory")
    scenes = dumped_scenes(sub / "dump ü.yaml")
    check({"World", "Overlay"} <= set(scenes) and all(scenes.values()),
          f"scene-dump preserves both scenes with their GameObjects: {scenes}")


# ---------------------------------------------------------------------------- benchmark

async def test_benchmark_relative_paths():
    work = layout()
    sub = work / "sub"
    xvfb = harness.Xvfb()
    display = xvfb.start()
    try:
        extra = {"DISPLAY": display}
        # a scene file relative to the working directory, and the project.yaml of the unicode copy
        r = ok(run(sub, "benchmark", "--project", f"../{UNICODE_DIR}/project.yaml", "--scene", f"../{UNICODE_DIR}/Overlay.scene",
                   "--frames", "5", "--warmup", "2", "--out", "../bench ü.json", timeout=600, extra_env=extra),
               "benchmark --scene <file relative to the working directory>")
        result = json.loads((work / "bench ü.json").read_text())
        check(result["scene"] == "Overlay" and result["frames"] == 5, f"benchmark --scene ../{UNICODE_DIR}/Overlay.scene: {result}")
        # a project-relative scene file still works
        r = ok(run(sub, "benchmark", "--project", "../proj/", "--scene", "Overlay.scene", "--frames", "5", "--warmup", "2", "--out", "../bench-project.json",
                   timeout=600, extra_env=extra), "benchmark --scene <project-relative file>")
        result = json.loads((work / "bench-project.json").read_text())
        check(result["scene"] == "Overlay" and result["frames"] == 5, f"project-relative --scene: {result}")
    finally:
        xvfb.stop()


# ---------------------------------------------------------------------------- check, ui-effects, shaders, import-ui

def check_json(cwd: Path, *args: str) -> dict:
    r = run(cwd, "check", *args, "--format", "json", timeout=600)
    try:
        data = json.loads(r.stdout)
    except json.JSONDecodeError:
        fail(f"check {args}: not JSON (exit {r.returncode}):\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
    check(r.returncode == 0 and data["ok"], f"check {args}: {data}")
    return data


async def test_check_relative_paths():
    work = layout()
    sub = work / "sub"
    lua_count = len(list((work / "proj" / "scripts").glob("*.lua")))
    whole = check_json(work, "--project", "proj", "--lua", "--shaders")
    check(whole["checked"].get("lua") == lua_count, f"every Lua script checked: {whole['checked']}")
    for cwd, project in project_forms(work):
        data = check_json(cwd, "--project", project, "--lua", "--shaders")
        check(data["checked"] == whole["checked"], f"check --project {project!r}: {data['checked']} vs {whole['checked']}")
    # --files: relative to the working directory, else to the project
    script = sorted(p.name for p in (work / "proj" / "scripts").glob("*.lua"))[0]
    for files in (f"../proj/scripts/{script}", f"scripts/{script}"):
        data = check_json(sub, "--project", "../proj/project.yaml", "--files", files)
        check(data["checked"] == {"lua": 1}, f"--files {files}: {data['checked']}")
    data = check_json(sub, "--project", f"../{UNICODE_DIR}/", "--files", f"../{UNICODE_DIR}/scripts/{script}")
    check(data["checked"] == {"lua": 1}, f"--files in the unicode project: {data['checked']}")


async def test_ui_effects_relative_project():
    work = layout()
    for project in (work / "proj", work / UNICODE_DIR):
        frag = project / "assets" / "ui" / "fx" / "path-glow.frag"
        frag.parent.mkdir(parents=True, exist_ok=True)
        frag.write_text(EFFECT_FRAG)
    r = ok(run(work / "sub" / "deep", "ui-effects", "--json", "--project", f"../../{UNICODE_DIR}/project.yaml"),
           "ui-effects resolves a parent-relative Unicode project")
    effects = {e["name"] for e in json.loads(r.stdout)["effects"]}
    check("path-glow" in effects, "the authored project effect is discovered")


async def test_shaders_relative_out_and_doc():
    work = layout()
    sub = work / "sub"
    ok(run(sub, "shaders", "--out", "../shaders out", "--shader", "mesh", "--backend", "opengl", timeout=600), "shaders --out ../shaders out")
    written = files_under(work / "shaders out")
    check(written, f"shaders written to {work / 'shaders out'}")
    check(not (sub / "shaders out").exists(), "--out is relative to the working directory")
    r = ok(run(sub, "shaders", "--library", "all", "--doc", "../doc ü.md", timeout=300), "shaders --library all --doc ../doc ü.md")
    check((work / "doc ü.md").is_file() and (work / "doc ü.md").stat().st_size > 0, f"--doc ../doc ü.md: {r.stdout}")


async def test_import_ui_relative_paths():
    work = layout()
    sub = work / "sub"
    for project in (work / "proj", work / UNICODE_DIR):
        doc = project / "assets" / "ui" / "local.html"
        doc.parent.mkdir(parents=True, exist_ok=True)
        doc.write_text(LOCAL_HTML)
    # the document project-relative, then relative to the working directory
    for project, document in (("../proj", "assets/ui/local.html"), (f"../{UNICODE_DIR}/project.yaml", "assets/ui/local.html"),
                              (f"../{UNICODE_DIR}/", f"../{UNICODE_DIR}/assets/ui/local.html")):
        r = ok(run(sub, "import-ui", "--project", project, document, timeout=300), f"import-ui --project {project!r} {document!r}")
        check("0 failed" in r.stderr, f"import-ui --project {project!r}: {r.stderr[-1500:]}")


# ---------------------------------------------------------------------------- the exported game runs
