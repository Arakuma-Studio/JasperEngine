"""The engine's shader library (library:<name> references): the compile matrix of
`jasperengine-editor shaders --library`, check_shader and `check --shaders` with library
references, and manifest validation of a project fork."""

from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
from contextlib import contextmanager
from pathlib import Path

import harness
from harness import Editor, TestFailure, find_key

TIMEOUT = 900

# a mesh fork with a manifest whose @property names no member and whose @texture is undeclared
BAD_MANIFEST_FRAG = """#version 450

// @shader mesh
// @name "Bad"
// @property tint color (1, 0, 0, 1) "Tint"
// @property missingMember float 1.0 "Missing"
// @texture u_missingTexture white "Missing"
// @bogus directive

uniform u_fs_customParams {
    vec4 tint;
} customParams;

out vec4 g_finalColor;

#include "includes/output.glsl"

void main() {
    g_finalColor = finalizeColor(customParams.tint);
}
"""


def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def check_eq(actual, expected, message: str = "") -> None:
    if actual != expected:
        fail(f"{message + ': ' if message else ''}expected {expected!r}, got {actual!r}")


def run_editor(*args: str, timeout: float = 600) -> subprocess.CompletedProcess:
    return subprocess.run(harness.editor_command() + list(args), capture_output=True, text=True, timeout=timeout)


@contextmanager
def scratch_project(source: Path | None = None):
    tmp = Path(tempfile.mkdtemp(prefix="jasper-library-"))
    try:
        project = tmp / "project"
        shutil.copytree(source or harness.default_fixture(), project,
                        ignore=shutil.ignore_patterns(".git", "build"), symlinks=True)
        yield project
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run_check_json(project: Path, *args: str) -> tuple[int, dict]:
    r = subprocess.run(harness.editor_command() + ["check", "--project", str(project), *args, "--format", "json"],
                       cwd=str(project), input="", capture_output=True, text=True, timeout=600)
    try:
        return r.returncode, json.loads(r.stdout)
    except json.JSONDecodeError:
        fail(f"not JSON (exit {r.returncode}):\nstdout: {r.stdout[-2000:]}\nstderr: {r.stderr[-2000:]}")


# ---------------------------------------------------------------------------- CLI: compile matrix

async def test_cli_library_matrix():
    r = run_editor("shaders", "--library", "unlit", "--backend", "opengl,vulkan")
    check_eq(r.returncode, 0, f"library:unlit compiles: {r.stdout}{r.stderr}")
    line = next((l for l in r.stdout.splitlines() if l.startswith("library:unlit:")), "")
    check("OK" in line and "0 failed" in line, f"summary line: {r.stdout}")
    # base, features alone and together, the mesh variants and the depth program
    variants = int(line.split(":")[2].split("variants")[0])
    check(variants >= 20, f"a matrix, not one variant: {line}")

    r = run_editor("shaders", "--library", "no_such_shader", "--backend", "opengl")
    check_eq(r.returncode, 1, f"an unknown library shader fails: {r.stdout}{r.stderr}")
    check("unknown library shader 'no_such_shader'" in r.stderr, f"says why: {r.stderr}")

    r = run_editor("shaders", "--help")
    check("--library" in r.stdout, "usage documents --library")


async def test_cli_library_matrix_all_backends():
    r = run_editor("shaders", "--library", "all", "--backend", "all", timeout=1800)
    check_eq(r.returncode, 0, f"every library shader on every backend: {r.stdout[-3000:]}{r.stderr[-3000:]}")
    check("library:unlit:" in r.stdout, f"unlit is in the library: {r.stdout}")


# ---------------------------------------------------------------------------- MCP and project checks

async def test_check_shader_library_reference(ed: Editor):
    ed.require_tools("check_shader")
    ok = await ed.call("check_shader", shader="library:unlit", timeout=300)
    ed.check_eq(ok["failed"], 0, f"library:unlit compiles: {ok!r}")
    ed.check_eq(ok["results"][0]["type"], "mesh", "the type comes from the manifest")
    ok = await ed.call("check_shader", shader="library:unlit#ALPHA_CLIP,VERTEX_COLOR", timeout=300)
    ed.check_eq(ok["failed"], 0, f"with its features: {ok!r}")
    # the reference resolves to the variant the engine draws: triplanar needs its default
    # SPACE keyword, matcap's default REPLACE has no base map (feature=!MATCAP_MODE_REPLACE)
    for ref in ("library:triplanar", "library:matcap", "library:hologram#BLEND_MODE_ALPHA"):
        ok = await ed.call("check_shader", shader=ref, timeout=300)
        ed.check_eq((ok["failed"], ok["findings"]), (0, []), f"{ref}: {ok!r}")
    ok = await ed.call("check_shader", shader="library:triplanar", timeout=300)
    ed.check_eq(ok["results"][0]["shader"], "library:triplanar#BLEND_NONE,SPACE_WORLD", "defaults and the blend define")
    ok = await ed.call("check_shader", shader="library:hologram#BLEND_MODE_ALPHA", timeout=300)
    ed.check_eq(ok["results"][0]["shader"], "library:hologram#BLEND_ALPHA,BLEND_MODE_ALPHA", "the keyword's @state[...] blend")
    msg = await ed.call("check_shader", shader="library:no_such_shader", expect_error=True)
    ed.check_in("library:unlit", msg, "lists the library shaders")


async def test_check_library_reference_in_scene(ed: Editor):
    ed.require_tools("set_properties", "save_scene", "check_shader")
    made = await ed.call("create_entity", name="LibraryUnlit", type="box")
    eid = find_key(made, "entity_id", "id")
    await ed.call("set_properties", entity=eid, values={"mesh": {"customShader": "library:unlit"}})
    await ed.call("save_scene")
    r = await ed.call("check_shader", timeout=300)
    shaders = [item.get("shader") for item in r["results"]]
    ed.check("library:unlit" in shaders, f"check_shader without arguments checks library references in use: {r!r}")
    ed.check_eq(r["failed"], 0, f"{r!r}")

    with scratch_project(ed.project) as copy:
        code, data = run_check_json(copy, "--shaders")
    ed.check_eq(code, 0, f"check --shaders accepts a library reference: {data}")
    ed.check(data["checked"].get("shaders", 0) >= 1, f"the reference is checked: {data}")

    # an unknown library shader is refused, with the ones there are
    msg = await ed.call("set_properties", entity=eid, values={"mesh": {"customShader": "library:no_such_shader"}},
                        expect_error=True)
    ed.check_in("library:unlit", msg, "lists the library shaders")

    # one a scene file names anyway is a check error
    scene_file = next((p for p in ed.project.rglob("*.scene") if "library:unlit" in p.read_text()), None)
    ed.check(scene_file is not None, "the scene saved the reference")
    with scratch_project(ed.project) as copy:
        copied = copy / scene_file.relative_to(ed.project)
        copied.write_text(copied.read_text().replace("library:unlit", "library:no_such_shader"))
        code, data = run_check_json(copy, "--shaders")
    ed.check_eq(code, 1, f"an unknown library shader is an error: {data}")
    ed.check(any("no_such_shader" in f["message"] for f in data["findings"]), f"named: {data['findings']}")

    await ed.call("set_properties", entity=eid, values={"mesh": {"customShader": ""}})
    await ed.call("save_scene")


async def test_check_fork_manifest(ed: Editor):
    ed.require_tools("write_file", "check_shader")
    vert = (harness.REPO / "engine" / "shaderlib" / "library" / "surface.vert").read_text()
    await ed.call("write_file", path="shaders/BadManifest.vert", content=vert, timeout=300)
    await ed.call("write_file", path="shaders/BadManifest.frag", content=BAD_MANIFEST_FRAG, timeout=300)
    r = await ed.call("check_shader", shader="shaders/BadManifest", shader_type="mesh", timeout=300)
    ed.check_eq(r["failed"], 1, f"manifest errors fail the check: {r!r}")
    findings = {(f["line"], f["severity"]): f["message"] for f in r["findings"]}
    ed.check(any("missingMember" in m for (line, sev), m in findings.items() if sev == "error" and line == 6),
             f"@property without a member, at its line: {r['findings']}")
    ed.check(any("u_missingTexture" in m for (line, sev), m in findings.items() if sev == "error" and line == 7),
             f"undeclared @texture, at its line: {r['findings']}")
    ed.check(any("@bogus" in m for (line, sev), m in findings.items() if sev == "warning" and line == 8),
             f"an unknown directive is a warning: {r['findings']}")
    for rel in ("shaders/BadManifest.vert", "shaders/BadManifest.frag"):
        (ed.project / rel).unlink()
