"""CLI diagnostics and file filtering on small authored project inputs.

The actual C++ compiler owner checks both public adapters, then repairs, compiles and links
its script using the same configured build. Script/shader semantics live in their owners.
"""

from __future__ import annotations

import json
import shutil
import shlex
import subprocess
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path

import yaml

import harness
from harness import Editor, TestFailure, find_key, skip
from project_fixture import write_project

EDITOR = {"create": True, "name": "Project Checks"}
CI = True

TIMEOUT = 900

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
        if source is None:
            write_project(project)
        else:
            shutil.copytree(source, project, ignore=_ignore_copy, symlinks=True)
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


def has_cpp_toolchain() -> bool:
    return bool(shutil.which("cmake") and (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")))


# ---------------------------------------------------------------------------- CLI: usage

async def test_cli_usage():
    r = subprocess.run(harness.editor_command() + ["check"], capture_output=True, text=True, timeout=60)
    check_eq(r.returncode, 2, f"no --project is a usage error: {r.stderr}")
    check("--project" in r.stderr, f"says what is missing: {r.stderr}")
    r = subprocess.run(harness.editor_command() + ["check", "--help"], capture_output=True, text=True, timeout=60)
    check_eq(r.returncode, 0, "--help")
    with scratch_project() as project:
        r = run_check(project, "--nope")
        check_eq(r.returncode, 2, f"unknown option: {r.stderr}")
        r = run_check(project, "--format", "xml")
        check_eq(r.returncode, 2, f"bad --format: {r.stderr}")
        r = run_check(project / "missing", cwd=project)
        check_eq(r.returncode, 2, f"missing project: {r.stderr}")
        r = run_check(project / "project.yaml", "--lua", cwd=project)
        check_eq(r.returncode, 0, f"project.yaml is accepted as --project: {r.stdout}{r.stderr}")


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
        slow = harness.over_budget("check --lua on the fixture", {"--lua": lua_seconds}, 3.0)
        check(not slow, f"--lua on the fixture took {lua_seconds:.2f}s")

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
        check_eq([line.split(":", 1)[0] for line in lines],
                 ["scripts/BadDefault.lua", "scripts/Boom.lua", "scripts/Broken.lua", "scripts/Undefined.lua"],
                 "compiler-style lines are sorted by project-relative path")
        check(all(": error: " in line for line in lines), "diagnostics identify severity")
        check("4 errors" in r.stderr, f"summary: {r.stderr}")

        code, data = run_json(project, "--lua")
        check_eq(code, 1, "json exit code")
        check_eq((data["ok"], data["errors"], data["warnings"]), (False, 4, 0), "json counts")
        check_eq(data["checked"].get("lua"), 7, f"lua files checked (probe + 6): {data['checked']}")
        for f in data["findings"]:
            check_eq((f["tool"], f["severity"], f["col"]), ("lua", "error", 0), f"finding {f}")
        broken = next(f for f in data["findings"] if f["path"] == "scripts/Broken.lua")
        check_eq(broken["line"], 3, "syntax finding points at the authored broken line")
        bad = next(f for f in data["findings"] if f["path"] == "scripts/BadDefault.lua")
        check_eq(bad["line"], 0, "a property problem has no line")

        # a long path (Lua shortens long chunk names in its messages) keeps its line
        deep = "scripts/" + "/".join(["a_rather_long_folder_name"] * 4) + "/Deep.lua"
        write(project, deep, BROKEN_LUA)
        write(project, deep.replace("Deep", "DeepBoom"), BOOM_LUA)
        code, data = run_json(project, "--files", deep, deep.replace("Deep", "DeepBoom"))
        check_eq(sorted((f["path"], f["line"]) for f in data["findings"]),
                 [(deep, 3), (deep.replace("Deep", "DeepBoom"), 2)], "line numbers of a long path")


async def test_cli_lua_isolated_state():
    """The CLI runs each script's top level in an isolated state: no files, processes or endless loops."""
    with scratch_project() as project:
        marker = project.parent / "touched-by-lua"
        process_marker = project.parent / "spawned-by-lua"
        write(project, "scripts/Writer.lua",
              f"local f = io.open({json.dumps(str(marker))}, 'w')\nif f then f:write('x') f:close() end\n"
              f"pcall(function() return os.execute({json.dumps('touch ' + shlex.quote(str(process_marker)))}) end)\nreturn {{}}\n")
        write(project, "scripts/Forever.lua", "local T = {}\nwhile true do end\nreturn T\n")
        write(project, "scripts/Hungry.lua", "local t = {}\nfor i = 1, 1e9 do t[i] = string.rep('x', 1024) .. i end\nreturn t\n")
        code, data = run_json(project, "--files", "scripts/Writer.lua", "scripts/Forever.lua", "scripts/Hungry.lua")
        check(not marker.exists(), "a checked script cannot write files")
        check(not process_marker.exists(), "a checked script cannot spawn a process")
        by_path = {f["path"]: f for f in data["findings"]}
        check("scripts/Writer.lua" not in by_path, f"io is an inert stand-in, os.execute is gone (no crash): {data}")
        check_eq((by_path["scripts/Forever.lua"]["line"], code), (2, 1), f"an endless loop is stopped: {data}")
        check("did not finish loading" in by_path["scripts/Forever.lua"]["message"], by_path["scripts/Forever.lua"]["message"])
        check("not enough memory" in by_path["scripts/Hungry.lua"]["message"], f"memory is capped: {data}")


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
        code, data = run_json(project, "--files", str(project / "scripts/Boom.lua"), str(project / "scripts/Probe.lua"),
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
        good = project / "scripts/Probe.lua"
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
        check("scripts/Broken.lua:3:" in r.stderr and "Probe" not in r.stderr, r.stderr)

        r = run_check(project, "--hook", stdin="not json")
        check_eq(r.returncode, 1, f"unreadable hook input exits 1 (shown to the user, not the agent): {r.stderr}")


# ---------------------------------------------------------------------------- CLI: animation clips

CLIP_GUID = "0123456789abcdef0123456789abcdef"
# a finding's fields (--format json)
FINDING_KEYS = {"tool", "path", "line", "col", "severity", "message"}
GOOD_CLIP = json.dumps({"format": "jasperengine.anim", "version": 1, "guid": CLIP_GUID, "wrap": "loop",
                        "tracks": [{"path": "", "channel": "position", "keys": [[0, [0, 0, 0]], [1, [0, 1, 0]]]}]}, indent=2) + "\n"
# a JSON syntax error on line 5 (a closing brace where a value belongs)
BROKEN_CLIP = '{\n  "format": "jasperengine.anim",\n  "version": 1,\n  "tracks": [\n}\n'
WARN_CLIP = json.dumps({"format": "jasperengine.anim", "version": 1, "wrap": "sideways", "tracks": []}, indent=2) + "\n"
UNITY_CLIP = "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!74 &7400000\nAnimationClip:\n  m_Name: Walk\n"
# what a saved scene holds for Animation Players (Stream::encodeAnimationPlayerComponent), paths
# relative to the assets folder (assets/ in the scratch project; findings name files project-relative)
CLIP_SCENE = """name: Clips
entities:
  - name: Mover
    components:
      animationplayer:
        clips:
          - clip: anim/good.anim
            guid: 0123456789abcdef0123456789abcdef
          - clip: anim/missing.anim
          - clip: old/good.anim
            guid: 0123456789abcdef0123456789abcdef
          - clip: models/none.gltf#Walk
          - clip: models/hero_animated.gltf#Walk
          - clip: models/hero_animated.gltf#Wave 2
          - clip: models/hero_animated.gltf#2
          - clip: models/hero_animated.gltf#Run
  - name: Instance
    components:
      transform:
        position: [0, 0, 0]
    prefabInstance:
      overrides:
        - registryEntity: 3
          properties:
            animationplayer: [clips]
          components:
            animationplayer:
              clips:
                - clip: anim/override_missing.anim
                - clip: models/hero_animated.gltf#Jump
"""
HERO_ANIMATED = harness.REPO / "tools" / "fixtures" / "pose-animation" / "assets" / "models" / "hero_animated.gltf"


async def test_cli_anims():
    with scratch_project() as project:
        clips = {"assets/anim/good.anim": GOOD_CLIP, "assets/anim/broken.anim": BROKEN_CLIP,
                 "assets/anim/warn.anim": WARN_CLIP, "assets/anim/unity.anim": UNITY_CLIP}
        for rel, text in clips.items():
            write(project, rel, text)
        write(project, "Clips.scene", CLIP_SCENE)
        (project / "assets/models").mkdir(parents=True, exist_ok=True)
        shutil.copy(HERO_ANIMATED, project / "assets/models/hero_animated.gltf")

        # the default set first: it loads the project for the shader check, which changes no clip
        # (read-only loads: no GUID written into the clips without one), and finds what --anims finds
        code, everything = run_json(project)
        check("anims" in everything["checked"], f"the default set checks clips: {everything['checked']}")
        for rel, text in clips.items():
            check_eq((project / rel).read_text(), text, f"{rel} is unchanged by the check")
        code, data = run_json(project, "--anims")
        anim_findings = sorted(json.dumps(f, sort_keys=True) for f in everything["findings"] if f["tool"] == "anim")
        check_eq(anim_findings, sorted(json.dumps(f, sort_keys=True) for f in data["findings"]),
                 "the default set's clip findings are --anims' (whatever runs first)")
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
        check_eq(len(scene), 6, f"the scene's references: missing clip, moved clip, missing model, two unknown model "
                                f"animations, the override's missing clip: {scene}")
        check(any(s == "error" and "'anim/missing.anim'" in m and "does not exist" in m for s, m in scene), f"missing clip: {scene}")
        check(any(s == "warning" and "'old/good.anim'" in m and "moved to 'assets/anim/good.anim'" in m for s, m in scene),
              f"a moved clip found by its GUID: {scene}")
        check(any(s == "error" and "'models/none.gltf'" in m for s, m in scene), f"a model clip without its model: {scene}")
        # model#name: the names the Inspector shows (duplicates numbered, an index), read from the model file
        run = [m for s, m in scene if "#Run" in m]
        check(len(run) == 1 and "has no animation 'Run'" in run[0] and "'Walk', 'Idle', 'Animation 2', 'Wave', 'Wave 2'" in run[0],
              f"an unknown animation name lists the model's: {run}")
        check(not any("hero_animated.gltf#Walk'" in m or "#Wave 2'" in m or "#2'" in m for s, m in scene), f"known names and an index pass: {scene}")
        # the clip entries of a prefab instance's overrides are checked too
        check(any(s == "error" and "override_missing.anim" in m for s, m in scene), f"an override's missing clip: {scene}")
        check(any(s == "error" and "#Jump" in m and "has no animation 'Jump'" in m for s, m in scene), f"an override's model clip: {scene}")

        # text: compiler style; the default set includes the clips
        r = run_check(project, "--anims")
        check("assets/anim/broken.anim:5:" in r.stdout and ": error: " in r.stdout, f"text output: {r.stdout}")
        check("anims 4 clips" in r.stderr, f"summary counts the clips: {r.stderr}")

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
        for rel, text in clips.items():
            check_eq((project / rel).read_text(), text, f"{rel} is unchanged after every run")


OTHER_GUID = "fedcba9876543210fedcba9876543210"
# an entry whose path exists but whose GUID names another clip (the editor plays the GUID's file)
GUID_SCENE = """name: Guids
entities:
  - name: Mover
    components:
      animationplayer:
        clips:
          - clip: anim/other.anim
            guid: 0123456789abcdef0123456789abcdef
          - clip: anim/good.anim
            guid: 0123456789abcdef0123456789abcdef
"""


# one path twice: first with its own GUID, then with other.anim's (the editor plays other.anim there)
TWICE_SCENE = GUID_SCENE.replace("name: Guids", "name: Twice").replace(
    "          - clip: anim/other.anim\n            guid: 0123456789abcdef0123456789abcdef\n"
    "          - clip: anim/good.anim\n            guid: 0123456789abcdef0123456789abcdef\n",
    "          - clip: anim/good.anim\n            guid: 0123456789abcdef0123456789abcdef\n"
    "          - clip: anim/good.anim\n            guid: " + OTHER_GUID + "\n")


async def test_cli_anims_guid_mismatch_and_unreadable_yaml():
    with scratch_project() as project:
        write(project, "assets/anim/good.anim", GOOD_CLIP)
        write(project, "assets/anim/other.anim", GOOD_CLIP.replace(CLIP_GUID, OTHER_GUID))
        write(project, "Guids.scene", GUID_SCENE)
        write(project, "Broken.prefab", "name: Broken\nentities: [ {name: A\n")
        # two entries of one path: its own GUID (fine), then another clip's GUID (verify review minor 8)
        write(project, "Twice.scene", TWICE_SCENE)
        code, data = run_json(project, "--anims")
        found = {}
        for f in data["findings"]:
            found.setdefault(f["path"], []).append((f["severity"], f["message"]))
        scene = found.get("Guids.scene", [])
        check_eq(len(scene), 1, f"one finding: the entry whose GUID names another clip (the matching one is fine): {scene}")
        check(scene and scene[0][0] == "warning" and "'anim/other.anim' has the GUID of 'assets/anim/good.anim'" in scene[0][1],
              f"GUID/path mismatch: {scene}")
        twice = found.get("Twice.scene", [])
        check(len(twice) == 1 and "'anim/good.anim' has the GUID of 'assets/anim/other.anim'" in twice[0][1],
              f"the second entry of the same path with another clip's GUID is checked too: {twice}")
        prefab = found.get("Broken.prefab", [])
        check(len(prefab) == 1 and prefab[0][0] == "warning" and "could not read Animation Player entries" in prefab[0][1],
              f"a YAML that fails to parse is reported, not skipped: {prefab}")
        check_eq(code, 0, f"warnings only: exit 0: {data}")


async def test_cli_anims_masks_and_controllers():
    """--anims reads Avatar Masks (.mask) and Animator Controllers (.controller): their format
    (line:col, JSON paths of dropped parts) and the controller validation (anim layers spec 5.11); upstream's
    version 1 files are read converted (noted), their findings with S21's JSON paths"""
    with scratch_project() as project:
        write(project, "assets/anim/good.anim", GOOD_CLIP)
        write(project, "assets/anim/arm.mask",
              '{"format": "jasperengine.mask", "version": 1, "default": 0, "humanoid": {"right_arm": 1, "left_hand_ik": 1},\n'
              ' "transforms": [{"weight": 1}]}\n')
        write(project, "assets/anim/broken.mask", '{"format": "jasperengine.mask",\n "version": 1,,}\n')
        write(project, "assets/anim/unity.controller", "%YAML 1.1\n--- !u!91 &9100000\nAnimatorController:\n")
        write(project, "assets/anim/granjero.controller", """{"format": "jasperengine.controller", "version": 1,
  "parameters": [{"name": "Speed", "type": "float"}, {"name": "Watering", "type": "bool"}, {"name": "Unused", "type": "int"}],
  "layers": [{"name": "Base Layer", "states": [{"name": "Idle", "clip": "anim/good.anim"},
      {"name": "Watering", "clip": "anim/missing.anim"}],
    "transitions": [{"from": "Idle", "to": "Watering", "conditions": [{"parameter": "Watering", "mode": "if"}],
      "has_exit_time": false}]}, {"name": "Holding", "mask": "anim/nomask.mask", "states": []}]}""")
        code, data = run_json(project, "--anims")
        found = {}
        for f in data["findings"]:
            found.setdefault(f["path"], []).append((f["severity"], f["line"], f["message"]))
        mask = found.get("assets/anim/arm.mask", [])
        check(any("humanoid.left_hand_ik" in m for _, _, m in mask), f"an IK part is reported with its JSON path: {mask}")
        check(any("transforms[0].path" in m for _, _, m in mask), f"a dropped entry is reported with its JSON path: {mask}")
        broken = found.get("assets/anim/broken.mask", [])
        check(len(broken) == 1 and broken[0][0] == "error" and broken[0][1] == 2, f"broken JSON: an error with its line: {broken}")
        unity = found.get("assets/anim/unity.controller", [])
        check(len(unity) == 1 and "Unity .controller" in unity[0][2], f"Unity YAML is refused: {unity}")
        controller = [m for _, _, m in found.get("assets/anim/granjero.controller", [])]
        check(any("upstream's version 1 Animator Controller" in m for m in controller), f"the conversion is noted: {controller}")
        check(any("upstream's version 1 Avatar Mask" in m for _, _, m in mask), f"the conversion is noted: {mask}")
        check(any("layers[0].state_machine.states[1].motion.clip" in m and "does not load" in m for m in controller), f"a missing clip: {controller}")
        check(any("parameters[2]" in m and "unused" in m for m in controller), f"an unused parameter: {controller}")
        check(any("layers[1].mask" in m and "does not exist" in m for m in controller), f"a missing mask: {controller}")
        check(any("parameters[0]" in m and "unused" in m for m in controller), f"Speed is read by nothing: {controller}")
        check_eq(code, 1, f"errors: exit 1: {data}")
        # --files: one controller alone
        code, data = run_json(project, "--files", "assets/anim/granjero.controller")
        check_eq(data["checked"], {"anims": 1}, f"--files a controller: {data}")


MASK_GUID = "11111111111111111111111111111111"
CONTROLLER_GUID = "22222222222222222222222222222222"
# Animation Players naming masks and a controller (anim layers): one there, one missing, one moved
# (found by its GUID), a controller missing
LAYER_SCENE = """name: Layers
entities:
  - name: Hero
    components:
      animationplayer:
        clips: []
        layers:
          - name: Base Layer
          - name: Arm
            mask: anim/arm.mask
            maskGuid: 11111111111111111111111111111111
          - name: Gone
            mask: anim/gone.mask
          - name: Moved
            mask: old/arm.mask
            maskGuid: 11111111111111111111111111111111
        controller: anim/gone.controller
        controllerGuid: 33333333333333333333333333333333
  - name: Other
    components:
      animationplayer:
        clips: []
        controller: old/hero.controller
        controllerGuid: 22222222222222222222222222222222
"""


async def test_cli_anims_player_mask_and_controller_references():
    with scratch_project() as project:
        write(project, "assets/anim/arm.mask", json.dumps({"format": "jasperengine.mask", "version": 1, "guid": MASK_GUID,
                                                          "default": 0, "transforms": []}, indent=2) + "\n")
        write(project, "assets/anim/hero.controller", json.dumps({"format": "jasperengine.controller", "version": 1, "guid": CONTROLLER_GUID,
                                                                 "parameters": [], "layers": [{"name": "Base Layer", "states": []}]}, indent=2) + "\n")
        write(project, "Layers.scene", LAYER_SCENE)
        code, data = run_json(project, "--anims")
        scene = sorted((f["severity"], f["message"]) for f in data["findings"] if f["path"] == "Layers.scene")
        check_eq(code, 1, f"errors exit 1: {data}")
        check_eq(len(scene), 4, f"the missing mask and controller, the moved mask and controller: {scene}")
        check(any(s == "error" and "layers[2].mask 'anim/gone.mask'" in m and "does not exist" in m for s, m in scene), f"missing mask: {scene}")
        check(any(s == "error" and "controller 'anim/gone.controller'" in m and "does not exist" in m for s, m in scene),
              f"missing controller: {scene}")
        check(any(s == "warning" and "'old/arm.mask'" in m and "moved to 'assets/anim/arm.mask'" in m for s, m in scene),
              f"a moved mask found by its GUID: {scene}")
        check(any(s == "warning" and "'old/hero.controller'" in m and "moved to 'assets/anim/hero.controller'" in m for s, m in scene),
              f"a moved controller found by its GUID: {scene}")

        # a controller naming a moved clip and mask by GUID: checked where they are, also with --files
        write(project, "assets/anim/good.anim", GOOD_CLIP)
        write(project, "assets/anim/follow.controller", json.dumps({"format": "jasperengine.controller", "version": 1, "layers": [
            {"name": "Base Layer", "states": [{"name": "Run", "clip": "old/good.anim", "clip_guid": CLIP_GUID}]},
            {"name": "Arm", "mask": "old/arm.mask", "mask_guid": MASK_GUID, "states": []}]}, indent=2) + "\n")
        for args in (("--anims",), ("--files", "assets/anim/follow.controller")):
            code, data = run_json(project, *args)
            follow = sorted((f["severity"], f["message"]) for f in data["findings"] if f["path"] == "assets/anim/follow.controller")
            check(not any(s == "error" for s, _ in follow) and not any("does not load" in m for _, m in follow), f"{args}: found by GUID: {follow}")
            check(any("layers[0].state_machine.states[0].motion.clip: 'old/good.anim' moved to 'assets/anim/good.anim'" in m for _, m in follow),
                  f"{args}: the clip: {follow}")
            check(any("layers[1].mask: 'old/arm.mask' moved to 'assets/anim/arm.mask'" in m for _, m in follow), f"{args}: the mask: {follow}")


# ---------------------------------------------------------------------------- CLI: vertex animations

VAT_MODELS = harness.REPO / "tools" / "fixtures" / "vertex-animation" / "assets" / "models"
VAT_KEYS = FINDING_KEYS | {"code", "suggestion"}


def bake_vat(project: Path, *args: str) -> dict:
    r = subprocess.run(harness.editor_command() + ["bake-vat", "--project", str(project), *args, "--format", "json", "-q"],
                       capture_output=True, text=True, timeout=300)
    check_eq(r.returncode, 0, f"bake-vat {args}: {r.stdout[-2000:]}{r.stderr[-2000:]}")
    return json.loads(r.stdout)


def vat_findings(data: dict) -> list[tuple[str, str, str]]:
    """(path, severity, code) of the vat findings, each with the vat keys"""
    out = []
    for f in data["findings"]:
        if f["tool"] != "vat":
            continue
        check_eq(set(f), VAT_KEYS, f"a vat finding has a code (the kind) and a suggestion (the fix): {f}")
        out.append((f["path"], f["severity"], f["code"]))
    return sorted(out)


async def test_cli_vat():
    with scratch_project() as project:
        (project / "assets/models").mkdir(parents=True, exist_ok=True)
        for model in ("rigid_arm.gltf", "morph_blob.gltf"):
            shutil.copy(VAT_MODELS / model, project / "assets/models" / model)
        arm, blob = "assets/models/rigid_arm.vat", "assets/models/morph_blob.vat"
        bake_vat(project, "--source", "assets/models/rigid_arm.gltf")
        bake_vat(project, "--source", "assets/models/morph_blob.gltf", "--preset", "desktop")

        # fresh bakes are clean; the default set checks them too
        code, data = run_json(project, "--vat")
        check_eq((code, data["checked"], data["findings"]), (0, {"vat": 2}, []), "fresh bakes")
        code, data = run_json(project)
        check_eq((code, data["checked"].get("vat"), vat_findings(data)), (0, 2, []), f"the default set checks vertex animations: {data['checked']}")
        r = run_check(project, "--vat")
        check(r.returncode == 0 and r.stdout == "" and "vat 2 files" in r.stderr, f"text summary: {r.stdout}{r.stderr}")

        # a source changed since the bake: a warning (exit 0), also through --files of the source
        model = project / "assets/models/rigid_arm.gltf"
        original = model.read_text()
        model.write_text(original + "\n")
        code, data = run_json(project, "--vat")
        check_eq((code, vat_findings(data)), (0, [(arm, "warning", "vat-source-changed")]), f"source changed: {data['findings']}")
        changed = [f for f in data["findings"] if f["code"] == "vat-source-changed"][0]
        check("assets/models/rigid_arm.gltf" in changed["message"] and "rebake" in changed["suggestion"].lower(),
              f"names it, says rebake: {changed}")
        # on the command line the fix is bake-vat's rebake (not the MCP tool's arguments)
        check(f"bake-vat --rebake {arm}" in changed["suggestion"] and "bake_vertex_animation" not in changed["suggestion"],
              f"the CLI's rebake: {changed}")
        r = run_check(project, "--vat")
        check(f"{arm}: warning: " in r.stdout and "[vat-source-changed]" in r.stdout and "(fix: " in r.stdout, f"text line: {r.stdout}")
        code, data = run_json(project, "--files", "assets/models/rigid_arm.gltf")
        check_eq((code, data["checked"], vat_findings(data)), (0, {"vat": 1}, [(arm, "warning", "vat-source-changed")]),
                 "--files of a source checks the .vat baked from it")
        code, data = run_json(project, "--files", "assets/models/morph_blob.gltf")
        check_eq((code, data["checked"], data["findings"]), (0, {"vat": 1}, []), "the other bake is clean")
        code, data = run_json(project, "--files", "scripts/Probe.lua")
        check("vat" not in data["checked"], f"a file no bake read checks no .vat: {data['checked']}")
        # missing: a warning
        model.unlink()
        code, data = run_json(project, "--vat")
        check_eq((code, vat_findings(data)), (0, [(arm, "warning", "vat-source-missing")]), f"source missing: {data['findings']}")
        model.write_text(original)

        # --files: a .vat, a .vat.glb
        code, data = run_json(project, "--files", arm)
        check_eq((code, data["checked"], data["findings"]), (0, {"vat": 1}, []), "--files a .vat")
        code, data = run_json(project, "--files", arm + ".glb")
        check_eq((code, data["checked"], data["findings"]), (0, {"vat": 1}, []), "--files a .vat.glb checks its .vat")

        # a corrupted mesh: vat-mesh-mismatch, exit 1
        glb = project / (arm + ".glb")
        glb_bytes = glb.read_bytes()
        glb.write_bytes(glb_bytes[: len(glb_bytes) // 2])
        code, data = run_json(project, "--vat")
        check_eq((code, vat_findings(data)), (1, [(arm, "error", "vat-mesh-mismatch")]), f"a truncated .vat.glb: {data['findings']}")
        r = run_check(project, "--files", arm + ".glb")
        check(r.returncode == 1 and "[vat-mesh-mismatch]" in r.stdout, f"text: {r.stdout}")
        glb.write_bytes(glb_bytes)

        # a generated field changed by hand: vat-layout-mismatch, exit 1
        vat_file = project / arm
        vat_text = vat_file.read_text()
        vat = json.loads(vat_text)
        vat["layout"]["layout_hash"] = "0" * len(vat["layout"]["layout_hash"])
        vat_file.write_text(json.dumps(vat, indent=2))
        code, data = run_json(project, "--files", arm)
        check_eq((code, vat_findings(data)), (1, [(arm, "error", "vat-layout-mismatch")]), f"a changed generated field: {data['findings']}")
        check(all(f"bake-vat --rebake {arm}" in f["suggestion"] for f in data["findings"] if f["tool"] == "vat"),
              f"the CLI's rebake: {data['findings']}")
        # not a .vat at all: vat-invalid at its line:col
        vat_file.write_text('{\n  "format": "jasperengine.vat",\n  "version": 1,\n  "clips": [\n}\n')
        code, data = run_json(project, "--vat")
        bad = [f for f in data["findings"] if f["path"] == arm]
        check(code == 1 and len(bad) == 1 and bad[0]["code"] == "vat-invalid" and bad[0]["severity"] == "error" and bad[0]["line"] == 5,
              f"a .vat that doesn't parse: {bad}")
        # an editable field repaired on load: a vat-format warning with its JSON path
        vat = json.loads(vat_text)
        vat["clips"][0]["wrap"] = "sideways"
        vat_file.write_text(json.dumps(vat, indent=2))
        code, data = run_json(project, "--files", arm)
        check(code == 0 and vat_findings(data) == [(arm, "warning", "vat-format")] and "clips[0].wrap" in data["findings"][0]["message"],
              f"a repaired editable field: {data['findings']}")
        vat_file.write_text(vat_text)

        # the device limit of the recipe's preset: a desktop-wide payload on a mobile_web recipe
        blob_file = project / blob
        blob_text = blob_file.read_text()
        vat = json.loads(blob_text)
        check_eq(vat["layout"]["width"], 4096, "a desktop bake is 4096 wide")
        vat["recipe"]["preset"] = "mobile_web"
        blob_file.write_text(json.dumps(vat, indent=2))
        code, data = run_json(project, "--vat")
        check_eq((code, vat_findings(data)), (1, [(blob, "error", "vat-device-limit")]), f"over mobile_web's 2048: {data['findings']}")
        limit = [f for f in data["findings"] if f["code"] == "vat-device-limit"][0]
        check("4096x" in limit["message"] and "2048" in limit["message"] and "WebGL2" in limit["message"], f"the numbers: {limit}")
        blob_file.write_text(blob_text)

        # hook mode: an agent wrote a broken .vat
        vat_file.write_text("{ not json")
        hook = {"hook_event_name": "PostToolUse", "tool_name": "Write", "cwd": str(project),
                "tool_input": {"file_path": str(vat_file), "content": "..."}}
        r = run_check(project, "--hook", stdin=json.dumps(hook))
        check(r.returncode == 2 and f"{arm}:1:" in r.stderr and "[vat-invalid]" in r.stderr, f"hook mode: {r.returncode} {r.stderr}")
        vat_file.write_text(vat_text)
        code, data = run_json(project, "--vat")
        check_eq((code, data["findings"]), (0, []), "clean again")


# ---------------------------------------------------------------------------- CLI: shaders

# a whole-project --shaders check also counts the scenes' NavMesh Surfaces (S19 R42): none here
ONE_FORK = {"navigation": 0, "shaders": 1}


async def test_cli_shaders():
    with scratch_project() as project:
        vert, frag = shader_fork(project)
        r = run_check(project, "--shaders")
        check_eq(r.returncode, 0, f"a fresh fork (not used by any scene yet) compiles: {r.stdout}{r.stderr}")
        code, data = run_json(project, "--shaders")
        check_eq(data["checked"], ONE_FORK, "one fork")

        source = (project / frag).read_text()
        broken, line = break_main(source, "    undefined_symbol_cli = 1.0;")
        (project / frag).write_text(broken)
        r = run_check(project, "--shaders")
        check_eq(r.returncode, 1, f"broken fork: {r.stdout}{r.stderr}")
        check(r.stdout.splitlines()[0].startswith(f"{frag}:{line}: error:") and "undefined_symbol_cli" in r.stdout,
              "shader diagnostics name the authored undefined symbol and source line")

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
        # --files is not a whole-project check: no NavMesh Surface count
        check_eq((code, data["checked"]), (0, {"shaders": 1}), "fixed")
        write(project, "shaders/Lonely.vert", "#version 450\nvoid main() { nope }\n")
        code, data = run_json(project, "--shaders")
        check_eq((code, data["checked"]), (0, ONE_FORK), "a .vert without a .frag is not a fork")


async def test_cli_shaders_summary_counts_navmesh_surfaces():
    """A non-quiet whole-project --shaders check of a scene with a NavMesh Surface (S19 R42): the
    summary line names the surfaces and the check exits 0 (warnings only). Its unit was missing
    from the summary's map once: map::at aborted every such check after printing its results."""
    with scratch_project() as project:
        scene = {"id": 21, "name": "Nav", "sceneType": "scene_3d", "scene": {},
                 "entities": [{"type": "Entity", "entity": 2001, "name": "Ground Surface",
                               "components": {"transform": {"position": [0, 0, 0], "scale": [1, 1, 1],
                                                             "rotation": [1, 0, 0, 0]},
                                              "navmeshsurface": {"enabled": True}}}]}
        write(project, "Nav.scene", yaml.safe_dump(scene, sort_keys=False))
        config = yaml.safe_load((project / "project.yaml").read_text())
        config["scenes"] = [{"filepath": "Nav.scene"}]
        write(project, "project.yaml", yaml.safe_dump(config, sort_keys=False))
        r = run_check(project, "--shaders")
        check_eq(r.returncode, 0, f"an unbaked surface is a warning: exit 0 (a negative code is a crash): {r.stdout}{r.stderr}")
        check("navigation 1 surface" in r.stderr, f"the summary counts the NavMesh Surface: {r.stderr[-1000:]}")
        check("'Ground Surface' is not baked" in r.stdout, f"the unbaked surface is reported: {r.stdout[-1000:]}")


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
        check_eq((code, data["findings"]), (0, []), "the authored paint effect compiles")

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
        check_eq((code, data["findings"]), (0, []), "the authored filter effect compiles")


# ---------------------------------------------------------------------------- MCP: same findings


async def test_cpp_cli_and_compile_scripts(ed: Editor):
    if not has_cpp_toolchain():
        skip("no C++ toolchain (cmake + compiler) on PATH")
    await ed.call("set_project_settings", values={"scripting.lua_dir": "scripts"})
    made = await ed.call("create_entity", name="CompileProbe", type="empty")
    header = """#pragma once
#include "ScriptBase.h"
class CompileProbe : public jasperengine::ScriptBase {
public:
    CompileProbe(jasperengine::Scene* scene, jasperengine::Entity entity);
    void onUpdate();
};
"""
    source = """#include "CompileProbe.h"
CompileProbe::CompileProbe(jasperengine::Scene* s, jasperengine::Entity e): ScriptBase(s, e) {}
void CompileProbe::onUpdate() {
    undefined_from_check;
}
"""
    await ed.call("create_script", kind="cpp_script", name="CompileProbe", entity=find_key(made, "entity_id"),
                  header_content=header, source_content=source)
    await ed.call("save_scene", path="Compile.scene")
    code, report = run_json(ed.project, "--cpp", timeout=900)
    ed.check_eq(code, 1, "CLI C++ compilation reports an actual compiler error")
    ed.check(any(f["path"] == "scripts/CompileProbe.cpp" and f["line"] == 4
                 and f["severity"] == "error" and "undefined_from_check" in f["message"]
                 for f in report["findings"]), f"CLI preserves the compiler's file, line and symbol: {report}")
    # Reuse this project's configured build; the MCP adapter must preserve the same authored source location.
    compiled = await ed.call("compile_scripts", timeout_seconds=600, timeout=700)
    ed.check_eq(compiled["success"], False, "the MCP compiler rejects the source")
    ed.check(any(e["file"] == "scripts/CompileProbe.cpp" and e["line"] == 4
                 and "undefined_from_check" in e["message"] for e in compiled["errors"]), compiled)
    (ed.project / "scripts/CompileProbe.cpp").write_text(source.replace("undefined_from_check;", "return;"))
    await ed.call("create_script", kind="cpp_script", name="DefaultTemplate")
    repaired = await ed.call("compile_scripts", timeout_seconds=600, timeout=700)
    ed.check_eq(repaired["success"], True, f"the repaired script and shipped default template compile and link: {repaired}")


# ---------------------------------------------------------------------------- search_files

async def test_search_files(ed: Editor):
    ed.require_tools("search_files")
    await ed.call("get_project_summary")
    (ed.project / "assets").mkdir(exist_ok=True)
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
    outside.write_text("outside-only-byte-probe\n")
    link = ed.project / "docs/link.txt"
    try:
        link.symlink_to(outside)
    except OSError:
        link = None
    if link:
        r = await ed.call("search_files", pattern="outside-only-byte-probe")
        ed.check_eq(r["matches"], [], f"a symlink out of the project is not followed: {r!r}")
        link.unlink()
    outside.unlink()
    for rel in ("scripts/SearchA.lua", "scripts/Many.lua", "docs/notes.md", "assets/blob.json", "assets/huge.json",
                ".jasperengine/build/hidden.txt", "build/out.txt"):
        (ed.project / rel).unlink()


# ---------------------------------------------------------------------------- editor sources

async def test_color_fields_use_the_widget():
    """S27 AC28: every colour field of the editor goes through editor/widget's colorField (the Color
    window, its undo session, linear/sRGB/HDR handling): no ImGui::ColorEdit or ColorPicker call is
    left elsewhere in editor/."""
    editor = harness.REPO / "editor"
    offenders = []
    for path in sorted(editor.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".mm") or not path.is_file():
            continue
        rel = path.relative_to(harness.REPO).as_posix()
        if rel.startswith("editor/widget/") or rel.startswith("editor/libs/") or "/tests/" in rel:
            continue
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if "ImGui::ColorEdit" in line or "ImGui::ColorPicker" in line:
                offenders.append(f"{rel}:{number}: {line.strip()}")
    check(not offenders, "colour fields outside editor/widget/ use ui::colorField:\n" + "\n".join(offenders))
