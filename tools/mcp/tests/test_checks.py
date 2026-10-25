"""Project checks shared by the CLI and the MCP server: `jasperengine-editor check` (Lua, shaders,
C++ scripts; text/JSON output, --files, --hook) run as a subprocess, the same findings through
check_script / check_shader / compile_scripts, and search_files.

The C++ build tests (CLI --cpp and its comparison with compile_scripts) build the scripts with the
host compiler; they run when cmake and a C++ compiler are on PATH (about a minute)."""

from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path

import harness
from harness import Editor, TestFailure, find_key, skip

TIMEOUT = 900

FINDING_KEYS = {"tool", "path", "line", "col", "severity", "message"}

BROKEN_LUA = "local T = {}\n\nlocal x = = 1\n\nreturn T\n"            # syntax error at line 3
BOOM_LUA = "local T = {}\nerror(\"boom\")\nreturn T\n"                # runtime error at load, line 2
UNDEFINED_LUA = "local T = {}\nlocal v = Vector3(1, 2, 3)\nundefinedThing()\nreturn T\n"   # line 3
BAD_DEFAULT_LUA = ("local T = {\n    properties = {\n        { name = \"speed\", type = \"float\", default = \"fast\" },\n"
                   "    }\n}\nreturn T\n")
GOOD_LUA = ("local Good = {\n    properties = {\n        { name = \"speed\", type = \"float\", default = 5.0 },\n"
            "        { name = \"tint\", type = \"color3\", default = { 1, 0, 0 } },\n    }\n}\n"
            "local Utils = require(\"GoodUtils\")\nlocal up = Vector3(0, 1, 0)\n"
            "function Good:init() RegisterEngineEvent(self, \"onUpdate\") end\nreturn Good\n")
GOOD_UTILS_LUA = "local M = {}\nfunction M.twice(x) return x * 2 end\nreturn M\n"


# ---------------------------------------------------------------------------- helpers

def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def check_eq(actual, expected, message: str = "") -> None:
    if actual != expected:
        fail(f"{message + ': ' if message else ''}expected {expected!r}, got {actual!r}")


def _ignore_copy(directory: str, names: list[str]) -> set[str]:
    ignored = {".git"} & set(names)
    if Path(directory).name == ".jasperengine" and "build" in names:
        ignored.add("build")
    return ignored


@contextmanager
def scratch_project(source: Path | None = None):
    """A temporary copy of the fixture (or of `source`), without .git and .jasperengine/build."""
    tmp = Path(tempfile.mkdtemp(prefix="jasper-check-"))
    try:
        project = tmp / "project"
        shutil.copytree(source or harness.default_fixture(), project, ignore=_ignore_copy, symlinks=True)
        yield project
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run_check(project: Path, *args: str, cwd: Path | None = None, stdin: str | None = None,
              timeout: float = 120) -> subprocess.CompletedProcess:
    cmd = harness.editor_command() + ["check", "--project", str(project), *args]
    return subprocess.run(cmd, cwd=str(cwd or project), input=stdin if stdin is not None else "",
                          capture_output=True, text=True, timeout=timeout)


def run_json(project: Path, *args: str, **kw) -> tuple[int, dict]:
    r = run_check(project, *args, "--format", "json", **kw)
    try:
        data = json.loads(r.stdout)
    except json.JSONDecodeError:
        fail(f"--format json printed something that is not JSON (exit {r.returncode}):\n"
             f"stdout: {r.stdout[-2000:]}\nstderr: {r.stderr[-2000:]}")
    return r.returncode, data


def write(project: Path, rel: str, text: str) -> Path:
    path = project / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def listing(project: Path) -> dict[str, float]:
    out = {}
    for p in project.rglob("*"):
        rel = p.relative_to(project).as_posix()
        if rel.startswith(".jasperengine") or not p.is_file():
            continue
        out[rel] = p.stat().st_mtime_ns
    return out


def shader_fork(project: Path, base: str = "shaders/Glow") -> tuple[str, str]:
    """A mesh shader fork as fork_shader writes it (copies of the engine's mesh shader)."""
    lib = harness.REPO / "engine" / "shaderlib"
    vert = write(project, base + ".vert", (lib / "mesh.vert").read_text())
    frag = write(project, base + ".frag", (lib / "mesh.frag").read_text())
    return vert.relative_to(project).as_posix(), frag.relative_to(project).as_posix()


def break_main(source: str, statement: str) -> tuple[str, int]:
    """Inserts `statement` on its own line just inside main(); returns the source and that line (1-based)."""
    lines = source.split("\n")
    index = next(i for i, line in enumerate(lines) if line.startswith("void main()"))
    if "{" not in lines[index]:
        index += 1   # the opening brace on the next line
    lines.insert(index + 1, statement)
    return "\n".join(lines), index + 2


def strip_findings(findings: list[dict]) -> list[tuple]:
    return sorted((f["tool"], f["path"], f["line"], f["col"], f["severity"], f["message"]) for f in findings)


def has_cpp_toolchain() -> bool:
    return bool(shutil.which("cmake") and (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")))


# ---------------------------------------------------------------------------- CLI: usage

async def test_cli_usage():
    r = subprocess.run(harness.editor_command() + ["check"], capture_output=True, text=True, timeout=60)
    check_eq(r.returncode, 2, f"no --project is a usage error: {r.stderr}")
    check("--project" in r.stderr, f"says what is missing: {r.stderr}")
    r = subprocess.run(harness.editor_command() + ["check", "--help"], capture_output=True, text=True, timeout=60)
    check_eq(r.returncode, 0, "--help")
    for option in ("--lua", "--shaders", "--cpp", "--anims", "--all", "--files", "--format", "--quiet", "--hook"):
        check(option in r.stdout, f"--help documents {option}: {r.stdout}")
    with scratch_project() as project:
        r = run_check(project, "--nope")
        check_eq(r.returncode, 2, f"unknown option: {r.stderr}")
        r = run_check(project, "--format", "xml")
        check_eq(r.returncode, 2, f"bad --format: {r.stderr}")
        r = run_check(project / "missing", cwd=project)
        check_eq(r.returncode, 2, f"missing project: {r.stderr}")
        r = run_check(project / "project.yaml", "--lua", cwd=project)
        check_eq(r.returncode, 0, f"project.yaml is accepted as --project: {r.stdout}{r.stderr}")
    r = subprocess.run(harness.editor_command() + ["--help"], capture_output=True, text=True, timeout=60)
    check(" check " in r.stdout, f"the main help lists check: {r.stdout}")


# ---------------------------------------------------------------------------- CLI: Lua

async def test_cli_clean_fixture_is_fast_and_read_only():
    with scratch_project() as project:
        before = listing(project)
        t0 = time.monotonic()
        r = run_check(project, "--lua")
        lua_seconds = time.monotonic() - t0
        check_eq(r.returncode, 0, f"clean --lua: {r.stdout}{r.stderr}")
        check_eq(r.stdout, "", "no findings on stdout")
        check("0 errors" in r.stderr, f"summary on stderr: {r.stderr}")
        check(lua_seconds < 3.0, f"--lua on the fixture took {lua_seconds:.2f}s")
        print(f"     [timing] check --lua on the fixture: {lua_seconds:.2f}s")

        t0 = time.monotonic()
        r = run_check(project, "--shaders")
        check_eq(r.returncode, 0, f"clean --shaders: {r.stdout}{r.stderr}")
        print(f"     [timing] check --shaders on the fixture (no forks): {time.monotonic() - t0:.2f}s")
        r = run_check(project)
        check_eq(r.returncode, 0, f"clean default (lua + shaders): {r.stdout}{r.stderr}")
        r = run_check(project, "--quiet")
        check_eq((r.returncode, r.stdout, r.stderr), (0, "", ""), "--quiet prints nothing when clean")
        check_eq(listing(project), before, "check does not write project files")


async def test_cli_broken_lua_text_and_json():
    with scratch_project() as project:
        write(project, "scripts/Broken.lua", BROKEN_LUA)
        write(project, "scripts/Boom.lua", BOOM_LUA)
        write(project, "scripts/Undefined.lua", UNDEFINED_LUA)
        write(project, "scripts/BadDefault.lua", BAD_DEFAULT_LUA)
        write(project, "scripts/Good.lua", GOOD_LUA)
        write(project, "scripts/GoodUtils.lua", GOOD_UTILS_LUA)

        r = run_check(project, "--lua")
        check_eq(r.returncode, 1, f"problems exit 1: {r.stdout}{r.stderr}")
        lines = r.stdout.splitlines()
        expected = [
            "scripts/BadDefault.lua: error: property 'speed' (float): default must be a number, got string",
            "scripts/Boom.lua:2: error: runtime error at load: boom",
            "scripts/Broken.lua:3: error: unexpected symbol near '='",
            "scripts/Undefined.lua:3: error: runtime error at load: attempt to call a nil value (global 'undefinedThing')",
        ]
        check_eq(lines, expected, "compiler-style lines, sorted by path")
        check("4 errors" in r.stderr, f"summary: {r.stderr}")

        code, data = run_json(project, "--lua")
        check_eq(code, 1, "json exit code")
        check_eq(set(data), {"ok", "errors", "warnings", "checked", "findings", "seconds"}, "json top-level keys")
        check_eq((data["ok"], data["errors"], data["warnings"]), (False, 4, 0), "json counts")
        check_eq(data["checked"].get("lua"), 7, f"lua files checked (fixture + 6): {data['checked']}")
        for f in data["findings"]:
            check_eq(set(f), FINDING_KEYS, "finding keys")
            check_eq((f["tool"], f["severity"], f["col"]), ("lua", "error", 0), f"finding {f}")
        broken = next(f for f in data["findings"] if f["path"] == "scripts/Broken.lua")
        check_eq((broken["line"], broken["message"]), (3, "unexpected symbol near '='"), "syntax finding")
        bad = next(f for f in data["findings"] if f["path"] == "scripts/BadDefault.lua")
        check_eq(bad["line"], 0, "a property problem has no line")

        # a long path (Lua shortens long chunk names in its messages) keeps its line
        deep = "scripts/" + "/".join(["a_rather_long_folder_name"] * 4) + "/Deep.lua"
        write(project, deep, BROKEN_LUA)
        write(project, deep.replace("Deep", "DeepBoom"), BOOM_LUA)
        code, data = run_json(project, "--files", deep, deep.replace("Deep", "DeepBoom"))
        check_eq(sorted((f["path"], f["line"], f["message"]) for f in data["findings"]),
                 [(deep, 3, "unexpected symbol near '='"), (deep.replace("Deep", "DeepBoom"), 2, "runtime error at load: boom")],
                 "line numbers of a long path")


async def test_cli_lua_isolated_state():
    """The CLI runs each script's top level in an isolated state: no files, processes or endless loops."""
    with scratch_project() as project:
        marker = project.parent / "touched-by-lua"
        write(project, "scripts/Writer.lua",
              f"local f = io.open({json.dumps(str(marker))}, 'w')\nif f then f:write('x') f:close() end\n"
              "local ok = pcall(function() return os.execute('true') end)\nreturn {}\n")
        write(project, "scripts/Forever.lua", "local T = {}\nwhile true do end\nreturn T\n")
        write(project, "scripts/Hungry.lua", "local t = {}\nfor i = 1, 1e9 do t[i] = string.rep('x', 1024) .. i end\nreturn t\n")
        t0 = time.monotonic()
        code, data = run_json(project, "--files", "scripts/Writer.lua", "scripts/Forever.lua", "scripts/Hungry.lua")
        seconds = time.monotonic() - t0
        check(not marker.exists(), "a script can't write files from the check")
        by_path = {f["path"]: f for f in data["findings"]}
        check("scripts/Writer.lua" not in by_path, f"io is an inert stand-in, os.execute is gone (no crash): {data}")
        check_eq((by_path["scripts/Forever.lua"]["line"], code), (2, 1), f"an endless loop is stopped: {data}")
        check("did not finish loading" in by_path["scripts/Forever.lua"]["message"], by_path["scripts/Forever.lua"]["message"])
        check("not enough memory" in by_path["scripts/Hungry.lua"]["message"], f"memory is capped: {data}")
        check(seconds < 20, f"the limits keep it quick: {seconds:.1f}s")


async def test_cli_files_filter():
    with scratch_project() as project:
        write(project, "scripts/Broken.lua", BROKEN_LUA)
        write(project, "scripts/Boom.lua", BOOM_LUA)
        code, data = run_json(project, "--files", "scripts/Broken.lua")
        check_eq(code, 1, "one broken file")
        check_eq([f["path"] for f in data["findings"]], ["scripts/Broken.lua"], "only the given file")
        check_eq(data["checked"], {"lua": 1}, "one file checked")

        # absolute paths, several files, cwd elsewhere
        elsewhere = project.parent
        code, data = run_json(project, "--files", str(project / "scripts/Boom.lua"), str(project / "scripts/GameController.lua"),
                              cwd=elsewhere)
        check_eq([f["path"] for f in data["findings"]], ["scripts/Boom.lua"], "absolute paths")
        check_eq(data["checked"], {"lua": 2}, "two files")
        # cwd-relative path from inside a subdirectory
        code, data = run_json(project, "--files", "Boom.lua", cwd=project / "scripts")
        check_eq([f["path"] for f in data["findings"]], ["scripts/Boom.lua"], "cwd-relative path")

        # files nothing checks, outside the project, deleted: nothing to do, fast, exit 0
        outside = project.parent / "outside.lua"
        outside.write_text(BROKEN_LUA)
        t0 = time.monotonic()
        r = run_check(project, "--files", "README.md", str(outside), "scripts/Deleted.lua", "--quiet")
        check_eq((r.returncode, r.stdout, r.stderr), (0, "", ""), "nothing to check")
        check(time.monotonic() - t0 < 2.0, "nothing to check is instant")
        # a C++ file is skipped unless --cpp is asked for
        write(project, "scripts/Thing.cpp", "int broken( {\n")
        code, data = run_json(project, "--files", "scripts/Thing.cpp")
        check_eq((code, data["findings"], data["checked"]), (0, [], {}), "C++ files need --cpp")
        # --files and a category: only that category of the given files
        code, data = run_json(project, "--shaders", "--files", "scripts/Broken.lua")
        check_eq((code, data["checked"]), (0, {}), "--shaders ignores a Lua file")


async def test_cli_hook_mode():
    with scratch_project() as project:
        broken = write(project, "scripts/Broken.lua", BROKEN_LUA)
        good = project / "scripts/GameController.lua"
        claude = {"hook_event_name": "PostToolUse", "tool_name": "Edit", "cwd": str(project),
                  "tool_input": {"file_path": str(broken), "old_string": "a", "new_string": "b"}}
        r = run_check(project, "--hook", stdin=json.dumps(claude))
        check_eq(r.returncode, 2, f"errors exit 2 in hook mode (the agent reads stderr): {r.stdout}{r.stderr}")
        check_eq(r.stdout, "", "hook mode writes to stderr only")
        check("scripts/Broken.lua:3: error: unexpected symbol near '='" in r.stderr, r.stderr)

        claude["tool_input"] = {"file_path": str(good), "content": "..."}
        r = run_check(project, "--hook", stdin=json.dumps(claude))
        check_eq((r.returncode, r.stdout, r.stderr), (0, "", ""), "clean file: silent")
        claude["tool_input"] = {"file_path": str(project / "README.md")}
        r = run_check(project, "--hook", stdin=json.dumps(claude))
        check_eq((r.returncode, r.stdout, r.stderr), (0, "", ""), "unchecked file: silent")

        # Codex: apply_patch, the paths are in the patch text
        patch = (f"*** Begin Patch\n*** Update File: {good}\n@@\n-a\n+b\n*** Add File: scripts/Broken.lua\n+x\n"
                 f"*** Delete File: scripts/Gone.lua\n*** End Patch")
        codex = {"hook_event_name": "PostToolUse", "tool_name": "apply_patch", "cwd": str(project),
                 "tool_input": {"command": patch}}
        r = run_check(project, "--hook", stdin=json.dumps(codex))
        check_eq(r.returncode, 2, f"Codex apply_patch paths: {r.stderr}")
        check("scripts/Broken.lua:3:" in r.stderr and "GameController" not in r.stderr, r.stderr)

        r = run_check(project, "--hook", stdin="not json")
        check_eq(r.returncode, 1, f"unreadable hook input exits 1 (shown to the user, not the agent): {r.stderr}")


# ---------------------------------------------------------------------------- CLI: animation clips

CLIP_GUID = "0123456789abcdef0123456789abcdef"
GOOD_CLIP = json.dumps({"format": "jasperengine.anim", "version": 1, "guid": CLIP_GUID, "wrap": "loop",
                        "tracks": [{"path": "", "channel": "position", "keys": [[0, [0, 0, 0]], [1, [0, 1, 0]]]}]}, indent=2) + "\n"
# a JSON syntax error on line 5 (a closing brace where a value belongs)
BROKEN_CLIP = '{\n  "format": "jasperengine.anim",\n  "version": 1,\n  "tracks": [\n}\n'
WARN_CLIP = json.dumps({"format": "jasperengine.anim", "version": 1, "wrap": "sideways", "tracks": []}, indent=2) + "\n"
UNITY_CLIP = "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!74 &7400000\nAnimationClip:\n  m_Name: Walk\n"
# what a saved scene holds for Animation Players (Stream::encodeAnimationPlayerComponent), paths
# relative to the assets folder (the project root in the default fixture)
CLIP_SCENE = """name: Clips
entities:
  - name: Mover
    components:
      animationplayer:
        clips:
          - clip: assets/anim/good.anim
            guid: 0123456789abcdef0123456789abcdef
          - clip: assets/anim/missing.anim
          - clip: assets/old/good.anim
            guid: 0123456789abcdef0123456789abcdef
          - clip: assets/models/none.gltf#Walk
"""


async def test_cli_anims():
    with scratch_project() as project:
        write(project, "assets/anim/good.anim", GOOD_CLIP)
        write(project, "assets/anim/broken.anim", BROKEN_CLIP)
        write(project, "assets/anim/warn.anim", WARN_CLIP)
        write(project, "assets/anim/unity.anim", UNITY_CLIP)
        write(project, "Clips.scene", CLIP_SCENE)
        code, data = run_json(project, "--anims")
        check_eq(code, 1, f"errors exit 1: {data}")
        check_eq(data["checked"], {"anims": 4}, "the four clips are read (anims only)")
        found = {}
        for f in data["findings"]:
            check_eq(set(f), FINDING_KEYS, "six keys")
            check_eq(f["tool"], "anim", "tool")
            found.setdefault(f["path"], []).append(f)
        broken = found["assets/anim/broken.anim"]
        check(len(broken) == 1 and broken[0]["severity"] == "error" and broken[0]["line"] == 5 and broken[0]["col"] > 0,
              f"a JSON syntax error with line:col: {broken}")
        warn = found["assets/anim/warn.anim"]
        check(len(warn) == 1 and warn[0]["severity"] == "warning" and warn[0]["message"].startswith("wrap:"),
              f"a lenient warning with its JSON path: {warn}")
        unity = found["assets/anim/unity.anim"]
        check(unity[0]["severity"] == "error" and "Unity .anim (YAML)" in unity[0]["message"], f"Unity YAML refused: {unity}")
        check("assets/anim/good.anim" not in found, f"a valid clip has no findings: {found.get('assets/anim/good.anim')}")
        scene = sorted((f["severity"], f["message"]) for f in found.get("Clips.scene", []))
        check_eq(len(scene), 3, f"the scene's references: missing clip, moved clip, missing model: {scene}")
        check(any(s == "error" and "assets/anim/missing.anim" in m and "does not exist" in m for s, m in scene), f"missing clip: {scene}")
        check(any(s == "warning" and "assets/old/good.anim" in m and "moved to 'assets/anim/good.anim'" in m for s, m in scene),
              f"a moved clip found by its GUID: {scene}")
        check(any(s == "error" and "assets/models/none.gltf" in m for s, m in scene), f"a model clip without its model: {scene}")

        # text: compiler style; the default set includes the clips
        r = run_check(project, "--anims")
        check("assets/anim/broken.anim:5:" in r.stdout and ": error: " in r.stdout, f"text output: {r.stdout}")
        check("anims 4 clips" in r.stderr, f"summary counts the clips: {r.stderr}")
        code, data = run_json(project)
        check("anims" in data["checked"], f"the default set checks clips: {data['checked']}")

        # --files: only the named clips, no reference scan
        code, data = run_json(project, "--files", "assets/anim/good.anim")
        check_eq((code, data["checked"], data["findings"]), (0, {"anims": 1}, []), "--files a valid clip")
        code, data = run_json(project, "--files", "assets/anim/broken.anim", "assets/anim/warn.anim")
        check_eq((code, data["checked"]["anims"], len(data["findings"])), (1, 2, 2), f"--files two clips: {data}")

        # hook mode: an agent wrote a broken clip
        hook = {"hook_event_name": "PostToolUse", "tool_name": "Write", "cwd": str(project),
                "tool_input": {"file_path": str(project / "assets/anim/broken.anim"), "content": "..."}}
        r = run_check(project, "--hook", stdin=json.dumps(hook))
        check_eq(r.returncode, 2, f"a broken clip exits 2 in hook mode: {r.stderr}")
        check("assets/anim/broken.anim:5:" in r.stderr, r.stderr)


# ---------------------------------------------------------------------------- CLI: shaders

async def test_cli_shaders():
    with scratch_project() as project:
        vert, frag = shader_fork(project)
        r = run_check(project, "--shaders")
        check_eq(r.returncode, 0, f"a fresh fork (not used by any scene yet) compiles: {r.stdout}{r.stderr}")
        code, data = run_json(project, "--shaders")
        check_eq(data["checked"], {"shaders": 1}, "one fork")

        source = (project / frag).read_text()
        broken, line = break_main(source, "    undefined_symbol_cli = 1.0;")
        (project / frag).write_text(broken)
        r = run_check(project, "--shaders")
        check_eq(r.returncode, 1, f"broken fork: {r.stdout}{r.stderr}")
        check_eq(r.stdout.splitlines()[0], f"{frag}:{line}: error: 'undefined_symbol_cli' : undeclared identifier",
                 "compiler-style shader error at the fork's line")

        # an error in a project include, reported in the include
        (project / frag).write_text(source.replace("void main()", '#include "inc/extra.glsl"\nvoid main()', 1))
        write(project, "shaders/inc/extra.glsl", "// extra\nfloat extraValue() {\n    return missing_include_symbol;\n}\n")
        code, data = run_json(project, "--files", "shaders/inc/extra.glsl")
        check_eq(code, 1, f"include error: {data}")
        include = [f for f in data["findings"] if f["path"] == "shaders/inc/extra.glsl"]
        check(include, f"an error in the include is reported in it: {data['findings']}")
        check_eq((include[0]["tool"], include[0]["line"], include[0]["severity"]), ("shader", 3, "error"), "include finding")
        check("missing_include_symbol" in include[0]["message"], include[0]["message"])

        # --files with the .frag only checks its fork; a lone .vert without its pair is ignored
        (project / frag).write_text(source)
        code, data = run_json(project, "--files", frag)
        check_eq((code, data["checked"]), (0, {"shaders": 1}), "fixed")
        write(project, "shaders/Lonely.vert", "#version 450\nvoid main() { nope }\n")
        code, data = run_json(project, "--shaders")
        check_eq((code, data["checked"]), (0, {"shaders": 1}), "a .vert without a .frag is not a fork")


EFFECT_FRAG = """#version 450
// @effect     check-glow
// @kind       paint
// @category   light
// @summary    A project effect of the check tests
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


async def test_cli_ui_effects():
    """Project UI effects (phase 5): every .frag whose manifest starts with // @effect is compiled
    in its forms by `check`; a GLSL error is effect-shader-compile at the .frag line, a manifest
    error effect-manifest at its line, a parameter block that does not match the manifest too."""
    with scratch_project() as project:
        frag = "assets/ui/fx/check-glow.frag"
        write(project, frag, EFFECT_FRAG)
        code, data = run_json(project, "--shaders")
        check_eq((code, data["checked"]), (0, {"shaders": 2}), f"a paint effect: both forms compile: {data}")

        broken = EFFECT_FRAG.replace("return params.color * params.amount;", "return params.color * undefined_amount;")
        line = broken.split("\n").index("    return params.color * undefined_amount;") + 1
        write(project, frag, broken)
        r = run_check(project, "--files", frag)
        check_eq(r.returncode, 1, f"broken effect: {r.stdout}{r.stderr}")
        first = r.stdout.splitlines()[0]
        check(first.startswith(f"{frag}:{line}: error: ") and "undefined_amount" in first and first.endswith("[HTMLUI-FX-0015]"),
              f"compiler-style effect-shader-compile at the .frag line: {first}")
        code, data = run_json(project, "--files", frag)
        check(all(f["code"] == "HTMLUI-FX-0015" and f["path"] == frag for f in data["findings"]), f"codes: {data['findings']}")

        bad = EFFECT_FRAG.replace("// @param      amount number 0.5 0 1", "// @param      amount number 3 0 1")
        line = bad.split("\n").index("// @param      amount number 3 0 1 -- Strength") + 1
        write(project, frag, bad)
        code, data = run_json(project, "--files", frag)
        check_eq(code, 1, f"manifest error: {data}")
        manifest = [f for f in data["findings"] if f["code"] == "HTMLUI-FX-0014"]
        check(manifest and manifest[0]["path"] == frag and manifest[0]["line"] == line, f"effect-manifest at line {line}: {data['findings']}")

        mismatch = EFFECT_FRAG.replace("    float amount;\n} params;", "} params;").replace("params.color * params.amount", "params.color")
        write(project, frag, mismatch)
        code, data = run_json(project, "--files", frag)
        check(code == 1 and any(f["code"] == "HTMLUI-FX-0014" and "amount" in f["message"] for f in data["findings"]),
              f"the parameter block does not declare amount: {data['findings']}")

        # a filter effect has the filter form only; an unrelated .frag is not an effect
        write(project, frag, EFFECT_FRAG.replace("// @kind       paint", "// @kind       filter")
              .replace("main_paint", "main_filter").replace("vec4 jfx_paint(JfxContext c){", "vec4 jfx_filter(JfxContext c){"))
        code, data = run_json(project, "--shaders")
        check_eq((code, data["checked"]), (0, {"shaders": 1}), f"a filter effect: one form: {data}")


# ---------------------------------------------------------------------------- MCP: same findings

async def test_check_script_findings_match_cli(ed: Editor):
    ed.require_tools("check_script")
    cases = {"scripts/Broken.lua": BROKEN_LUA, "scripts/Boom.lua": BOOM_LUA, "scripts/Undefined.lua": UNDEFINED_LUA,
             "scripts/BadDefault.lua": BAD_DEFAULT_LUA, "scripts/Good.lua": GOOD_LUA, "scripts/GoodUtils.lua": GOOD_UTILS_LUA}
    for rel, text in cases.items():
        write(ed.project, rel, text)
    mcp = []
    results = {}
    for rel in cases:
        r = await ed.call("check_script", path=rel)
        ed.check(isinstance(r.get("findings"), list), f"check_script returns findings: {r!r}")
        ed.check_eq(r["ok"], not r["findings"], f"ok iff no findings: {r!r}")
        mcp += r["findings"]
        results[rel] = r
    good = results["scripts/Good.lua"]
    ed.check_eq([p["name"] for p in good["properties"]], ["speed", "tint"], "properties of a clean script")
    boom = results["scripts/Boom.lua"]
    ed.check_eq(boom["errors"], [{"line": 2, "message": "runtime error at load: boom"}], "errors carry the load error")
    # the editor survives a property default it can't read (it used to abort)
    bad = results["scripts/BadDefault.lua"]
    ed.check_eq(bad["ok"], False, f"bad default: {bad!r}")
    await ed.check_alive()

    with scratch_project(ed.project) as copy:
        code, data = run_json(copy, "--files", *cases)
    ed.check_eq(code, 1, "CLI exit code")
    ed.check_eq(strip_findings(data["findings"]), strip_findings(mcp), "CLI and check_script find the same")
    for rel in cases:
        (ed.project / rel).unlink()


async def test_check_shader_findings_match_cli(ed: Editor):
    ed.require_tools("fork_shader", "check_shader", "save_scene")
    made = await ed.call("create_entity", name="CheckGlow", type="box")
    eid = find_key(made, "entity_id", "id")
    fork = await ed.call("fork_shader", entity=eid, directory="shaders")
    frag = fork["frag_path"]
    await ed.call("save_scene")
    source = (ed.project / frag).read_text()
    broken, line = break_main(source, "    undefined_symbol_mcp = 1.0;")
    (ed.project / frag).write_text(broken)
    r = await ed.call("check_shader", shader=fork["custom_shader"], timeout=300)
    ed.check_eq(r["failed"], 1, f"broken: {r!r}")
    ed.check(r["findings"], f"check_shader returns findings: {r!r}")
    ed.check_eq((r["findings"][0]["path"], r["findings"][0]["line"]), (frag, line), "file:line of the error")
    with scratch_project(ed.project) as copy:
        code, data = run_json(copy, "--files", frag)
    ed.check_eq(code, 1, "CLI exit code")
    ed.check_eq(strip_findings(data["findings"]), strip_findings(r["findings"]), "CLI and check_shader find the same")
    (ed.project / frag).write_text(source)
    ok = await ed.call("check_shader", shader=fork["custom_shader"], timeout=300)
    ed.check_eq((ok["failed"], ok["findings"]), (0, []), "fixed")


CPP_BAD_LINE = "    counter += undefinedThing;  // compile error\n"


async def test_cpp_cli_and_compile_scripts(ed: Editor):
    ed.require_tools("create_script", "compile_scripts", "save_scene")
    if not has_cpp_toolchain():
        skip("no C++ toolchain (cmake + compiler) on PATH")
    made = await ed.call("create_entity", name="CheckSpinner", type="box")
    eid = find_key(made, "entity_id", "id")
    await ed.call("create_script", kind="cpp_script", name="CheckSpinner", entity=eid)
    await ed.call("save_scene")
    with scratch_project(ed.project) as copy:
        t0 = time.monotonic()
        code, data = run_json(copy, "--cpp", timeout=900)
        print(f"     [timing] check --cpp, first (configure + build): {time.monotonic() - t0:.1f}s")
        if code != 0 and any("ABI" in f["message"] or "compatible" in f["message"] for f in data["findings"]):
            skip(f"the C++ build can't run in this environment: {data['findings']}")
        ed.check_eq((code, data["errors"]), (0, 0), f"clean C++ build: {data}")
        ed.check_eq(data["checked"], {"cpp": 1}, "one script")

        source = (copy / "scripts/CheckSpinner.cpp").read_text()
        lines = source.splitlines(keepends=True)
        anchor = next(i for i, line in enumerate(lines) if "counter++" in line)
        lines.insert(anchor + 1, CPP_BAD_LINE)
        broken = "".join(lines)
        (copy / "scripts/CheckSpinner.cpp").write_text(broken)
        t0 = time.monotonic()
        r = run_check(copy, "--cpp", "--files", "scripts/CheckSpinner.cpp", timeout=900)
        print(f"     [timing] check --cpp, incremental: {time.monotonic() - t0:.1f}s")
        ed.check_eq(r.returncode, 1, f"broken C++: {r.stdout}{r.stderr}")
        first = r.stdout.splitlines()[0]
        ed.check(first.startswith(f"scripts/CheckSpinner.cpp:{anchor + 2}:") and ": error: " in first and "undefinedThing" in first,
                 f"compiler-style C++ error: {r.stdout}")
        code, cli = run_json(copy, "--cpp", timeout=900)

    (ed.project / "scripts/CheckSpinner.cpp").write_text(broken)
    mcp = await ed.call("compile_scripts", timeout_seconds=600, timeout=700)
    ed.check_eq(mcp["success"], False, "compile_scripts fails too")
    as_findings = [{"tool": "cpp", "path": e["file"], "line": e["line"], "col": e["column"], "severity": sev,
                    "message": e["message"]} for sev, key in (("error", "errors"), ("warning", "warnings")) for e in mcp[key]]
    ed.check_eq(strip_findings(cli["findings"]), strip_findings(as_findings), "CLI --cpp and compile_scripts find the same")
    (ed.project / "scripts/CheckSpinner.cpp").write_text(source)


# ---------------------------------------------------------------------------- search_files

async def test_search_files(ed: Editor):
    ed.require_tools("search_files")
    tool = ed.tool("search_files")
    size = len(tool.name) + len(tool.description or "") + len(json.dumps(tool.input_schema, separators=(",", ":")))
    ed.check(size <= 600, f"search_files definition is {size} chars (budget 600)")

    write(ed.project, "scripts/SearchA.lua", "local A = {}\n-- needle one\nfunction A:Speed() end\nlocal speed = 1\nreturn A\n")
    write(ed.project, "docs/notes.md", "first\nsecond NEEDLE two\nthird\n")
    write(ed.project, ".jasperengine/build/hidden.txt", "needle hidden\n")
    write(ed.project, "build/out.txt", "needle build\n")
    (ed.project / "assets/blob.json").write_bytes(b'{"x": "needle"}\x00\x01\x02 binary')
    (ed.project / "assets/huge.json").write_text("needle\n" + "x" * (3 * 1024 * 1024))

    r = await ed.call("search_files", pattern="needle")
    ed.check_eq(sorted((m["path"], m["line"]) for m in r["matches"]), [("scripts/SearchA.lua", 2)],
                f"literal, case-sensitive by default; build dirs, binary and huge files skipped: {r!r}")
    ed.check_eq(r["matches"][0]["text"], "-- needle one", "the line text")

    r = await ed.call("search_files", pattern="needle", case_sensitive=False)
    ed.check_eq(sorted((m["path"], m["line"]) for m in r["matches"]),
                [("docs/notes.md", 2), ("scripts/SearchA.lua", 2)], f"case-insensitive: {r!r}")

    r = await ed.call("search_files", pattern=r"function \w+:\w+\(", regex=True)
    ed.check((("scripts/SearchA.lua", 3) in [(m["path"], m["line"]) for m in r["matches"]]), f"regex: {r!r}")
    r = await ed.call("search_files", pattern="(", regex=False, glob="scripts/SearchA.lua")
    ed.check_eq([m["line"] for m in r["matches"]], [3], "a literal pattern is not a regex")
    await ed.check_error("search_files", pattern="(", regex=True, contains="regex")

    r = await ed.call("search_files", pattern="speed", case_sensitive=False, glob="*.lua")
    ed.check(r["matches"] and all(m["path"].endswith(".lua") for m in r["matches"]), f"glob on names: {r!r}")
    r = await ed.call("search_files", pattern="needle", case_sensitive=False, glob="docs")
    ed.check_eq([m["path"] for m in r["matches"]], ["docs/notes.md"], "a folder as the filter")

    r = await ed.call("search_files", pattern="NEEDLE", glob="docs/*.md", context=1)
    m = r["matches"][0]
    ed.check_eq(m["context"], {"before": ["first"], "after": ["third"]}, f"context lines: {r!r}")

    write(ed.project, "scripts/Many.lua", "".join(f"-- hit {i}\n" for i in range(30)))
    r = await ed.call("search_files", pattern="hit", glob="scripts/Many.lua", max_results=5)
    ed.check_eq((len(r["matches"]), r["total"], r["truncated"]), (5, 30, True), f"max_results: {r!r}")

    for escape in ("../*", "../../etc/passwd", "/etc/passwd", "scripts/../../x"):
        await ed.check_error("search_files", pattern="root", glob=escape, contains="project")

    outside = ed.project.parent / "outside-secret.txt"
    outside.write_text("needle secret\n")
    link = ed.project / "docs/link.txt"
    try:
        link.symlink_to(outside)
    except OSError:
        link = None
    if link:
        r = await ed.call("search_files", pattern="secret")
        ed.check_eq(r["matches"], [], f"a symlink out of the project is not followed: {r!r}")
        link.unlink()
    outside.unlink()
    for rel in ("scripts/SearchA.lua", "scripts/Many.lua", "docs/notes.md", "assets/blob.json", "assets/huge.json",
                ".jasperengine/build/hidden.txt", "build/out.txt"):
        (ed.project / rel).unlink()
