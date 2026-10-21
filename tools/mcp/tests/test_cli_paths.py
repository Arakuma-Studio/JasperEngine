"""Path arguments of the editor's command line: relative to the caller's working directory.

`jasperengine-editor export --project <relative path>` failed with "bad file: proj/proj/Main Scene.scene":
the project kept the relative directory it was given and joined it to its project-relative scene paths,
which already held it, so every scene was joined to the project twice and dropped (an absolute path
worked, which is what tools/smoke-test.sh passes). Every subcommand that takes --project now makes it
absolute and canonical once, against the directory the editor started in, before anything runs, and so
do --out and the other file arguments; the arguments documented as project-relative (export --assets
and --lua, check --files and import-ui documents, which fall back to the project) keep that meaning.

Each subcommand runs on scratch copies of the fixture (one in "games/My Game ü", a directory with a space
and a non-ASCII letter) from a working directory of its own, with relative project paths: plain, "./",
"../" (and "../sub/../"), a trailing separator, the project.yaml file, the space and unicode one; and with
a relative --out, which must land relative to that working directory.

- export: --list-scenes lists both scenes for every form; three full exports hold every scene (one
  generated source each), every asset and every Lua script. JASPER_SLOW_BUILD=1 builds the first one
  (relative --project and --out) with cmake and runs its player (run_build): it runs and draws.
- scene-dump: both scenes with their GameObjects, written to a relative --out.
- benchmark: the start scene and a --scene file relative to the working directory measured, the JSON
  written to a relative --out (under the harness's Xvfb).
- check, ui-effects, shaders, import-ui: the same results from the relative forms.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

import yaml

import harness
from harness import Editor, TestFailure, skip
from htmlui_pixels import decode_png

TIMEOUT = 1800
BUILD_TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))
JOBS = int(os.environ.get("JOBS", "3"))   # the host is shared

UNICODE_DIR = "games/My Game ü"
SCENES = {"Main Scene.scene": "Main_Scene.cpp", "HUD Scene.scene": "HUD_Scene.cpp"}

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


def _ignore_copy(directory: str, names: list[str]) -> set[str]:
    ignored = {".git"} & set(names)
    if Path(directory).name == ".jasperengine":
        ignored |= {"build", "export"} & set(names)
    return ignored


def layout() -> Path:
    """The scratch tree, made once:  <tmp>/work/proj, <tmp>/work/games/My Game ü (copies of the fixture)
    and <tmp>/work/sub (an empty working directory, for the ../ forms). Returns <tmp>/work."""
    if "work" in S:
        return S["work"]
    tmp = Path(tempfile.mkdtemp(prefix="jasper-cli-paths-"))
    S["tmp"] = tmp
    work = tmp / "work"
    fixture = harness.default_fixture()
    shutil.copytree(fixture, work / "proj", ignore=_ignore_copy, symlinks=True)
    shutil.copytree(fixture, work / UNICODE_DIR, ignore=_ignore_copy, symlinks=True)
    (work / "sub").mkdir()
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


def project_forms() -> list[tuple[Path, str]]:
    """(working directory, --project) pairs: every relative form names <work>/proj, or the unicode copy."""
    work = layout()
    sub = work / "sub"
    return [
        (work, "proj"),
        (work, "./proj"),
        (work, "proj/"),
        (work, "proj/project.yaml"),
        (work, "./proj/./project.yaml"),
        (sub, "../proj"),
        (sub, "../sub/../proj/"),
        (sub, "../proj/project.yaml"),
        (work, UNICODE_DIR),
        (sub, f"../{UNICODE_DIR}/project.yaml"),
        (work / "proj", "."),
    ]


def check_export(out: Path, project: Path, what: str) -> None:
    """Every scene (its generated source), every asset and every Lua script of `project` is in `out`."""
    check((out / "CMakeLists.txt").is_file(), f"{what}: no CMakeLists.txt in {out}")
    exported = out / "project"
    for scene, source in SCENES.items():
        check((project / scene).is_file(), f"the fixture has {scene}")
        check((exported / source).is_file(),
              f"{what}: scene {scene} not exported (no project/{source}): {sorted(p.name for p in exported.iterdir())}")
    assets = files_under(project / "assets")
    check(assets, "the fixture has assets")
    missing = sorted(assets - files_under(exported / "assets"))
    check(not missing, f"{what}: assets missing from {exported / 'assets'}: {missing[:20]}")
    scripts = {p.name for p in (project / "scripts").glob("*.lua")}
    check(scripts, "the fixture has Lua scripts")
    lua = {Path(p).name for p in files_under(exported / "lua")}
    check(scripts <= lua, f"{what}: Lua scripts missing: {sorted(scripts - lua)}")


def no_doubled_paths() -> None:
    work = layout()
    for doubled in (work / "proj" / "proj", work / "sub" / "proj", work / UNICODE_DIR / "games"):
        check(not doubled.exists(), f"something was written to {doubled}")


# ---------------------------------------------------------------------------- export

async def test_export_lists_scenes_for_every_project_form():
    for cwd, project in project_forms():
        r = ok(run(cwd, "export", "--project", project, "--list-scenes", timeout=300), f"export --project {project!r} --list-scenes")
        listed = {line.split("\t")[2] for line in r.stdout.splitlines() if line.count("\t") == 2}
        check(set(SCENES) <= listed, f"--project {project!r} (cwd {cwd.name}): scenes {sorted(listed)}, want {sorted(SCENES)}\n"
                                     f"stderr: {r.stderr[-2000:]}")
    no_doubled_paths()


async def test_export_relative_project_and_out():
    work = layout()
    out = work / "out-rel"
    ok(run(work, "export", "--project", "proj", "--out", "out-rel", "--backend", "opengl"), "export proj -> out-rel")
    check_export(out, work / "proj", "export --project proj --out out-rel")
    no_doubled_paths()
    S["export"] = out


async def test_export_dotdot_project_yaml_and_out():
    work = layout()
    out = work / "exports" / "dot"
    ok(run(work / "sub", "export", "--project", "../proj/project.yaml", "--out", "../exports/./dot", "--backend", "opengl"),
       "export ../proj/project.yaml -> ../exports/./dot")
    check_export(out, work / "proj", "export --project ../proj/project.yaml --out ../exports/./dot")
    check(not (work / "sub" / "exports").exists(), "--out is relative to the working directory")
    no_doubled_paths()


async def test_export_space_and_unicode_paths():
    work = layout()
    out = work / "exports" / "Ünïcode out"
    ok(run(work, "export", "--project", f"./{UNICODE_DIR}/", "--out", "exports/Ünïcode out", "--backend", "opengl"),
       "export the unicode project")
    check_export(out, work / UNICODE_DIR, f"export --project ./{UNICODE_DIR}/")
    no_doubled_paths()


# ---------------------------------------------------------------------------- scene-dump

def dumped_scenes(path: Path) -> dict[str, int]:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    return {scene["name"]: len(scene.get("entities") or {}) for scene in data.get("scenes") or []}


async def test_scene_dump_relative_paths():
    work = layout()
    sub = work / "sub"
    ok(run(sub, "scene-dump", "--project", "../proj", "--out", "dump.yaml", timeout=600), "scene-dump ../proj")
    check((sub / "dump.yaml").is_file(), "the dump is written relative to the working directory")
    scenes = dumped_scenes(sub / "dump.yaml")
    check({"Main Scene", "HUD Scene"} <= set(scenes) and all(scenes.values()),
          f"scene-dump ../proj: both scenes with their GameObjects: {scenes}")

    ok(run(sub, "scene-dump", "--project", f"../{UNICODE_DIR}/project.yaml", "--out", "../dump ü.yaml", timeout=600),
       "scene-dump of the unicode project.yaml")
    check((work / "dump ü.yaml").is_file(), "--out ../dump ü.yaml")
    check_scenes = dumped_scenes(work / "dump ü.yaml")
    check(check_scenes == scenes, f"the same dump from the unicode copy: {check_scenes} vs {scenes}")
    no_doubled_paths()


# ---------------------------------------------------------------------------- benchmark

async def test_benchmark_relative_paths():
    work = layout()
    sub = work / "sub"
    xvfb = harness.Xvfb()
    display = xvfb.start()
    try:
        extra = {"DISPLAY": display}
        r = ok(run(sub, "benchmark", "--project", "../proj", "--frames", "5", "--warmup", "2", "--out", "bench.json",
                   timeout=600, extra_env=extra), "benchmark ../proj")
        result = json.loads((sub / "bench.json").read_text())
        check(result["scene"] == "Main Scene" and result["frames"] >= 1, f"benchmark of the start scene: {result}")

        # a scene file relative to the working directory, and the project.yaml of the unicode copy
        r = ok(run(sub, "benchmark", "--project", f"../{UNICODE_DIR}/project.yaml", "--scene", f"../{UNICODE_DIR}/HUD Scene.scene",
                   "--frames", "5", "--warmup", "2", "--out", "../bench ü.json", timeout=600, extra_env=extra),
               "benchmark --scene <file relative to the working directory>")
        result = json.loads((work / "bench ü.json").read_text())
        check(result["scene"] == "HUD Scene", f"benchmark --scene ../{UNICODE_DIR}/HUD Scene.scene: {result}")
        # a project-relative scene file still works
        r = ok(run(sub, "benchmark", "--project", "../proj/", "--scene", "HUD Scene.scene", "--frames", "5", "--warmup", "2",
                   timeout=600, extra_env=extra), "benchmark --scene <project-relative file>")
        check('"scene": "HUD Scene"' in r.stdout, f"benchmark --scene 'HUD Scene.scene': {r.stdout[-1500:]}")
    finally:
        xvfb.stop()
    no_doubled_paths()


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
    for cwd, project in project_forms():
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
    for cwd, project in project_forms():
        r = ok(run(cwd, "ui-effects", "--json", "--project", project, timeout=300), f"ui-effects --project {project!r}")
        effects = {e["name"]: e for e in json.loads(r.stdout)["effects"]}
        check("path-glow" in effects, f"ui-effects --project {project!r}: the project effect is listed")
    r = ok(run(work / "sub", "ui-effects", "--validate", "--project", f"../{UNICODE_DIR}", timeout=900), "ui-effects --validate")
    check("path-glow" in r.stdout, f"ui-effects --validate lists the project effect: {r.stdout[-1500:]}")


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

def distinct_colors(png: bytes) -> int:
    w, h, ch, rows = decode_png(png)
    seen = set()
    for y in range(0, h, 7):
        row = rows[y]
        for x in range(0, w, 7):
            seen.add(tuple(row[x * ch:x * ch + 3]))
    return len(seen)


async def test_relative_export_builds_and_runs(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run the exported player (compiles the engine)")
    out = S.get("export")
    check(out is not None and (out / "CMakeLists.txt").is_file(), "the relative export (test_export_relative_project_and_out) ran")
    build = out / "build"
    t0 = time.monotonic()
    log = S["tmp"] / "player-build.log"
    with open(log, "w") as f:
        # configured and built from the export's parent with relative paths, as a user would
        r = subprocess.run(["cmake", "-S", out.name, "-B", f"{out.name}/build", "-DCMAKE_BUILD_TYPE=Release", "-DGRAPHIC_BACKEND=glcore"],
                           cwd=str(out.parent), stdout=f, stderr=subprocess.STDOUT, timeout=900)
        if r.returncode == 0:
            r = subprocess.run(["cmake", "--build", f"{out.name}/build", "-j", str(JOBS)], cwd=str(out.parent), stdout=f,
                               stderr=subprocess.STDOUT, timeout=BUILD_TIMEOUT)
    print(f"[test] player build: exit {r.returncode} in {time.monotonic() - t0:.0f}s")
    check(r.returncode == 0, f"the exported project does not build:\n{log.read_text(errors='replace')[-4000:]}")
    players = [p for p in build.iterdir() if p.is_file() and os.access(p, os.X_OK)]
    check(len(players) == 1, f"one player executable in {build}: {players}")

    raw = await ed.call_raw("run_build", {"path": str(players[0]), "seconds": 10, "screenshot_after_seconds": 8})
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    print("[test] run_build:", raw.text[:1500])
    ed.check(not r["crashed"], f"the player crashed: {r['fatal_lines']} {r['log'][-20:]}")
    ed.check(r.get("stopped_by_run_build"), "still running when stopped")
    log_text = "\n".join(r["log"])
    for bad in ("bad file", "issing shader", "Lua Error"):
        ed.check(bad not in log_text, f"the player logged '{bad}': {log_text[-3000:]}")
    ed.check(raw.images, "screenshot")
    png = raw.images[0][0]
    (ed.scratch.root / "cli-paths-player.png").write_bytes(png)
    colors = distinct_colors(png)
    ed.check(colors > 20, f"the player draws its scene ({colors} distinct colours sampled)")


async def test_cleanup():
    tmp = S.get("tmp")
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)
