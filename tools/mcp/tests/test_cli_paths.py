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
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

import yaml

import harness
from harness import TestFailure
from project_fixture import write_project


TIMEOUT = 1800
CI = True

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


async def test_export_leaves_recent_projects_alone():
    """A command-line export saves the project, which records nothing in the person's editor
    settings: their last project and recent projects stay (an editor opened later reopens theirs)"""
    work = layout()
    cwd = work / "sub" / "deep"
    other = work / "Other"
    other.mkdir()
    settings = cwd / "settings.yaml"
    settings.write_text(yaml.safe_dump({"last_project_path": str(other), "recent_projects": [str(other)]}))
    # the temporary folder elsewhere: a project inside it is the editor's temporary project, never recorded
    temp = S["tmp"] / "systemp"
    temp.mkdir()
    ok(run(cwd, "export", "--project", "../../proj", "--out", "../../exports/recent", "--backend", "opengl",
           extra_env={"TMPDIR": str(temp)}),
       "export with the person's settings in the working directory")
    data = yaml.safe_load(settings.read_text()) or {}
    recent = [entry["path"] if isinstance(entry, dict) else entry for entry in data.get("recent_projects") or []]
    check(Path(data.get("last_project_path", "")).resolve() == other.resolve(), f"the last project is still theirs: {data}")
    check([Path(p).resolve() for p in recent] == [other.resolve()], f"the recent projects are unchanged: {data}")


TITLE = 'Pirates; Revenge #2 "Gold" \\ $HOME ${X} 100% & | ^ < > , \u30b2\u30fc\u30e0 \U0001F3AE'


def c_literal_bytes(literal: str) -> bytes:
    """The bytes of a C string literal as the preprocessor prints it (escapes \\ooo \\x \\\\ \\" \\')"""
    raw = literal.strip().encode("utf-8")
    check(raw[:1] == b'"' and raw[-1:] == b'"', f"a string literal: {literal!r}")
    raw, out, i = raw[1:-1], bytearray(), 0
    while i < len(raw):
        if raw[i:i + 1] == b"\\":
            digits = raw[i + 1:i + 4]
            if len(digits) == 3 and all(48 <= d <= 55 for d in digits):
                out.append(int(digits, 8))
                i += 4
                continue
            out += raw[i + 1:i + 2]
            i += 2
        else:
            out += raw[i:i + 1]
            i += 1
    return bytes(out)


async def test_window_title_with_every_awkward_character():
    """The window title reaches the game's compile as a C string of exactly its bytes, whatever it
    holds: CMake splits a definition at ; and drops one holding #, and several build tools break on
    % & | ^ < > , and quotes, so the export carries them as escapes"""
    work = layout()
    project = work / "proj"
    data = yaml.safe_load((project / "project.yaml").read_text(encoding="utf-8"))
    data["windowTitle"] = TITLE
    (project / "project.yaml").write_text(yaml.safe_dump(data, allow_unicode=True, sort_keys=False), encoding="utf-8")
    out = work / "exports" / "title"
    ok(run(work, "export", "--project", "proj", "--out", "exports/title", "--backend", "opengl"), "export a project with an awkward title")
    configured = out / "configure"
    result = subprocess.run(["cmake", "-S", str(out), "-B", str(configured), "-DCMAKE_BUILD_TYPE=Release",
                             "-DGRAPHIC_BACKEND=glcore", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"],
                            env=env(), capture_output=True, text=True, timeout=300)
    check(result.returncode == 0, f"the export configures: {result.stdout[-2000:]}{result.stderr[-2000:]}")
    commands = json.loads((configured / "compile_commands.json").read_text())
    game = [c for c in commands if "JASPERENGINE_WINDOW_TITLE" in c.get("command", " ".join(c.get("arguments", [])))]
    check(game, "a compile of the game has the title macro (CMake drops a definition holding #)")
    entry = game[0]
    argv = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
    # the same compiler and flags, preprocessing a probe instead of compiling the source
    probe = work / "title_probe.cpp"
    probe.write_text("JASPERENGINE_WINDOW_TITLE\n")
    kept, skip_next = [], False
    for arg in argv:
        if skip_next:
            skip_next = False
            continue
        # (-include: a precompiled header that is not built yet)
        if arg in ("-o", "-c", "-include"):
            skip_next = True
            continue
        if arg == entry["file"] or arg.endswith(".cpp"):
            continue
        kept.append(arg)
    result = subprocess.run(kept + ["-E", "-P", "-x", "c++", str(probe)], cwd=entry["directory"], env=env(),
                            capture_output=True, text=True, timeout=120)
    check(result.returncode == 0, f"the probe preprocesses: {result.stderr[-2000:]}")
    printed = [line for line in result.stdout.splitlines() if line.strip()][-1]
    check(c_literal_bytes(printed) == TITLE.encode("utf-8"), f"the title's bytes: {printed!r}, want {TITLE!r}")


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
        check(result["play"] is False, f"without --play the scene is measured in edit mode: {result}")
    finally:
        xvfb.stop()


async def test_benchmark_play():
    # --play measures the scene playing: the benchmark starts Play and only measures once the
    # scene plays (it fails when Play doesn't start)
    work = layout()
    xvfb = harness.Xvfb()
    display = xvfb.start()
    try:
        ok(run(work, "benchmark", "--project", "proj", "--scene", "World.scene", "--play", "--frames", "5", "--warmup", "2",
               "--out", "bench-play.json", timeout=600, extra_env={"DISPLAY": display}), "benchmark --play")
        result = json.loads((work / "bench-play.json").read_text())
        check(result["scene"] == "World" and result["frames"] == 5 and result["play"] is True, f"benchmark --play: {result}")
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
