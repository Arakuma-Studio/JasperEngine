"""tools/check-changed: the per-commit gate of a stream. Builds what the change touches and runs the
unit and MCP tests that can see it; the full suite is for pushes to origin main that need it.

    tools/check-changed                 # build and test what the change touches
    tools/check-changed --dry-run       # the plan only
    tools/check-changed --base oe-port  # the diff against another base (default: the merge base
                                        # with oe-port, else main, else origin/main)
    tools/check-changed --no-cache      # run even when the same inputs passed before
    tools/check-changed --no-mcp        # builds and unit tests only: the MCP selection stays the integrator's
    CHECK_CHANGED_VERBOSE=1 tools/check-changed   # every step's output, not only a failed one's

1. The changed files: committed (since the merge base), staged, unstaged and untracked. Through
   rb.sh the server's worktree has no history of the stream: rb.sh writes the local diff to
   build/.rb-changes.json, which this reads when its own git cannot name the base.
2. The builds: the Linux editor (SKIP_ENGINE=1 tools/build.sh linux) for engine/, editor/ and what
   the editor embeds (EDITOR_INPUTS), the unit test binaries for their C++ and shaders, the HTML UI
   tests for its code, web for engine/ code and its CMake, Android for its own platform code;
   everything for the root CMake, docker/, tools/build.sh, the toolchain and the lockfiles
   (ALL_TRIGGERS); nothing for a change that builds nothing (docs). This tool's and the harness's
   own Python run their self-tests (SELF_TESTS).
3. Unit tests, exactly: Ninja's dependency database of the test trees (`ninja -t deps`) maps every
   changed file (a changed Foo.cpp through its Foo.h too) to the test translation units that
   include it; their gtest suites run (--gtest_filter), in the core and the editor binary. A
   changed test file always runs. A change the database cannot see (CMake, no tree yet) runs all;
   a shader runs the core tests' *Gpu suites.
4. MCP tests, from a coverage map (tools/mcp/coverage_map.py: which source files, headers included,
   each test file's editors executed a function of, recorded by a full run of a coverage-instrumented
   editor): the files that executed the functions the diff's hunks touch (the whole file when a changed
   line is outside every function, a header then also through its .cpp; only new lines outside them
   touch nothing old; a function nearly every editor runs, the 10 quickest files that run it), the changed test
   files, the tests of a changed MCP tool (by its tool names), of a changed fixture, Lua, shader or
   asset (by path; a shader also test_shader_library.py and the CI suite), the files that run a
   prebuilt game when the engine changes, and those that compile a game when what the compile
   path reads changes (COMPILE_PATHS), and the guard of what a save writes when serialization or
   code generation changes (SAVED_OUTPUT_PATHS, spec S63). Fallbacks: no map, a
   map older than MAP_MAX_AGE_DAYS or MAP_MAX_COMMITS, or a changed source the map does not know
   (new) or file no test names add the CI suite; a harness change runs the whole suite.
5. A pass is cached (build/.cache/check-changed.json) by a hash of the base, the diff, the
   untracked files, the build images' Dockerfiles, the map's version and this script; the same
   inputs reprint the last summary. A run during which the files changed caches nothing.
6. Exit 1 when a step failed or a selected test could not run. Inside the Linux image the web and
   Android builds are left out, and said to be (their images are the host's to run).
7. After the Linux editor's build: every header the change edits still compiles alone
   (tools/check_headers.py) and no component header passes the fan-out limits (tools/check_fanout.py).
8. When a header under engine/core, engine/CMakeLists.txt or what ALL_TRIGGERS names changes: the
   engine builds with every optional module off (tools/check-physics-off.sh all-off). The editor's
   engine has every module, so nothing else here sees a source that lost a header only a module's
   code gave it; a game's Compile build and the smaller prebuilt players do.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE_FILE = ROOT / "build" / ".cache" / "check-changed.json"
RB_CHANGES = ROOT / "build" / ".rb-changes.json"
MCP_TESTS = ROOT / "tools" / "mcp" / "tests"
CORE_TREE = ROOT / "build" / "linux" / "core-tests"
EDITOR_TEST_TREE = ROOT / "build" / "linux" / "editor-tests"
EDITOR_TREE = ROOT / "build" / "linux" / "editor"
HTMLUI_TREE = ROOT / "build" / "linux" / "htmlui-tests-glcore"
COMMON_SHARE = 0.8      # of the files with an editor: a function that many execute is any editor's
COMMON_SAMPLE = 10      # the quickest of those that a change of such a function selects
MAP_MAX_AGE_DAYS = 14
MAP_MAX_COMMITS = 400
GAP = ("coverage gap: this is the change's own selection; the full gate is still the full suite "
       "(before pushing to origin main, when the push needs it)")

# a change here builds and runs everything
ALL_TRIGGERS = ("CMakeLists.txt", "CMakePresets.json", "CopyEditorEngine.cmake", "docker/*", "tools/build.sh",
                "tools/with-xvfb.sh", "tools/ci/gtest-gate.py", "tools/mcp/requirements.txt", "engine/cmake/*",
                "*.lock", "uv.lock", "package-lock.json", "pnpm-lock.yaml")
# a change here runs the whole MCP suite (the harness every test goes through)
MCP_ALL_TRIGGERS = ("tools/mcp/harness.py", "tools/mcp/run_tests.py", "tools/mcp/run-tests.sh",
                    "tools/mcp/build_cache.py", "tools/mcp/requirements.txt")
# the test harness's and this tool's own Python: their self-tests (python3 -I <test>, no image)
SELF_TESTS = {"tools/check_changed.py": "tools/test_check_changed.py", "tools/check-changed": "tools/test_check_changed.py",
              "tools/test_check_changed.py": "tools/test_check_changed.py", "tools/mcp/coverage_map.py": "tools/test_check_changed.py",
              "tools/mcp/shard.py": "tools/mcp/test_shard.py", "tools/mcp/test_shard.py": "tools/mcp/test_shard.py",
              "tools/mcp/build_cache.py": "tools/mcp/test_build_cache.py", "tools/mcp/test_build_cache.py": "tools/mcp/test_build_cache.py",
              "tools/check_fanout.py": "tools/test_check_fanout.py", "tools/test_check_fanout.py": "tools/test_check_fanout.py"}
# an MCP test file, or what classifies it, changes: every file must stay classified for the CI suite
CI_SUITE_GUARD = "tools/ci/check-mcp-ci-suite.py"
CI_SUITE_GUARD_TRIGGERS = ("tools/mcp/tests/test_*.py", "tools/mcp/run_tests.py", CI_SUITE_GUARD)
# the engine with every optional module off: a header's include can reach a source only through a
# module's code, and the editor's engine, which has them all, never shows it
MODULES_OFF_CHECK = "tools/check-physics-off.sh"
# the required-reason APIs of engine code against the engine's Apple privacy manifest
APPLE_PRIVACY_CHECK = "tools/check_apple_privacy.py"
# C/C++ and what the engine's builds read
CXX = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".mm", ".m")
HEADER = (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp")
BUILDS_NOTHING = ("docs/*", "*.md", "LICENSE", "NOTICE.md", ".claude/*")
# outside engine/ and editor/, compiled into the editor or embedded in it (CMakeLists.txt): the
# vendored libraries, the MCP guide and the HTML UI example library, the Lua API tools
EDITOR_INPUTS = ("libs/*", "docs/mcp/guide/*", "docs/unity-glossary.md", "tools/lua_api/*")
HTMLUI_PATHS = ("engine/tests/htmlui/*", "engine/core/htmlui/*", "engine/libs/rmlui/*", "engine/libs/lexbor/*")
WEB_DIRS = ("engine/platform/web/*", "engine/platform/emscripten/*", "tools/web/*")
ANDROID_DIRS = ("engine/platform/android/*", "engine/project/android/*", "tools/android-*", "tools/ci/android_*")
# the MCP files that run a game built with a prebuilt player: the engine's runtime code runs in the
# game, which no editor coverage sees; and those that compile one, for what the compile path reads
# (the full gate compiles for every change)
GAME_BUILD_FILES = ("test_html_ui_export.py", "test_html_ui_video_export.py", "test_call_cli.py")
COMPILE_FILES = ("test_build.py", "test_compile_without_modules.py", "test_html_ui_effects_export.py", "test_hub.py", "test_busy.py")
COMPILE_PATHS = ("engine/CMakeLists.txt", "engine/project/*", "engine/player/*", "engine/platform/*", "engine/core/player/*",
                 "engine/core/diag/*", "editor/Exporter*", "editor/Generator*", "editor/players/*", "tools/players/*")
# what a save writes (the scene files, the generated code): the guard that it does not depend on
# what was drawn or loaded (spec S63); it runs the code the map sees through every other test file
SAVED_OUTPUT_FILES = ("test_saved_output_determinism.py",)
SAVED_OUTPUT_PATHS = ("editor/Factory*", "editor/Generator*", "editor/Stream*", "editor/stream/*", "editor/reflect/*",
                      "engine/core/serialization/*")
TEST_DEF = re.compile(r"^\s*(TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*\(\s*(\w+)\s*,", re.M)
TOOL_NAME = re.compile(r"\b\w+\.name\s*=\s*\"([a-z][a-z0-9_]*)\"")


def match(path: str, patterns) -> bool:
    return any(fnmatch.fnmatchcase(path, p) for p in patterns)


def git(*args: str, cwd: Path = ROOT, check: bool = True) -> str:
    r = subprocess.run(["git", *args], cwd=str(cwd), capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


# ---------------------------------------------------------------------------------------- 1. the diff

@dataclass
class Changes:
    base: str                      # the base commit
    base_ref: str                  # what it is (the merge base with oe-port, ...)
    files: list[str]               # changed, staged, unstaged, untracked, deleted
    deleted: list[str] = field(default_factory=list)
    untracked: list[str] = field(default_factory=list)
    source: str = "git"            # git | rb.sh (build/.rb-changes.json)
    # `git diff -U0` of the change, and of the coverage map's commit to this tree: file -> [[first line,
    # lines in the old, first line, lines in the new], ...]
    hunks: dict[str, list[list[int]]] = field(default_factory=dict)
    map_commit: str = ""
    map_diff: dict[str, list[list[int]]] = field(default_factory=dict)


def find_base(explicit: str | None, cwd: Path = ROOT) -> tuple[str, str]:
    candidates = [explicit] if explicit else ["oe-port", "main", "origin/main"]
    for ref in candidates:
        r = subprocess.run(["git", "merge-base", "HEAD", ref], cwd=str(cwd), capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip(), f"merge base of HEAD and {ref}"
    raise LookupError(f"no merge base of HEAD and {' / '.join(candidates)}")


def git_changes(explicit_base: str | None, cwd: Path = ROOT) -> Changes:
    base, how = find_base(explicit_base, cwd)
    status = git("diff", "--name-status", "--no-renames", base, cwd=cwd)   # committed + staged + unstaged
    files, deleted = [], []
    for line in status.splitlines():
        if not line.strip():
            continue
        kind, _, path = line.partition("\t")
        files.append(path)
        if kind.startswith("D"):
            deleted.append(path)
    untracked = [p for p in git("ls-files", "--others", "--exclude-standard", cwd=cwd).splitlines() if p]
    return Changes(base, how, sorted(set(files + untracked)), sorted(deleted), sorted(untracked), "git",
                   parse_hunks(git("diff", "-U0", "--no-renames", base, cwd=cwd)))


def parse_hunks(diff: str) -> dict[str, list[list[int]]]:
    """`git diff -U0`: file -> [[first line, lines in the old side, first line, lines in the new side], ...]."""
    hunks: dict[str, list[list[int]]] = {}
    current = None
    for line in diff.splitlines():
        if line.startswith("diff --git "):
            current = None
        elif line.startswith("--- a/"):
            current = line[6:]
        elif line.startswith("+++ b/") and current is None:
            current = line[6:]
        elif line.startswith("@@ ") and current:
            m = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? ", line + " ")
            if m:
                hunks.setdefault(current, []).append([int(m.group(1)), int(m.group(2) or 1), int(m.group(3)), int(m.group(4) or 1)])
    return hunks


def to_map_lines(hunks: list[list[int]], map_diff: list[list[int]]) -> list[tuple[int, int, bool]]:
    """The change's lines in the coverage map's lines: (first, last, only new lines). The change's new
    side is this tree; the map's diff takes this tree's lines to the map's commit: a line outside its
    hunks moves by their offset, one inside is that hunk's old lines. A deletion is the place it was."""
    def back(line: int) -> tuple[int, int]:
        offset = 0
        for old, old_count, new, new_count in map_diff:
            first_new = new if new_count else new + 1   # (no new lines: it sits after that line)
            if line < first_new:
                break
            if new_count and line < new + new_count:
                return (old, old + max(old_count - 1, 0)) if old_count else (old, old + 1)
            offset = (old + old_count - 1 if old_count else old) - (new + new_count - 1 if new_count else new)
        return line + offset, line + offset
    ranges = []
    for old, old_count, new, new_count in hunks:
        pure_addition = old_count == 0
        lines = range(new, new + new_count) if new_count else range(new, new + 2)
        points = [back(line) for line in (lines[0], lines[-1])]
        ranges.append((min(p[0] for p in points), max(p[1] for p in points), pure_addition))
    return ranges


def read_changes(explicit_base: str | None, cwd: Path = ROOT, rb_file: Path = RB_CHANGES) -> Changes:
    """The diff rb.sh sent with this sync (a build server's worktree has no history of the stream),
    when every file it names is as it hashed it; else git's diff here."""
    if rb_file.is_file():
        data = json.loads(rb_file.read_text())
        stale = [f for f, digest in data.get("sha256", {}).items()
                 if not (cwd / f).is_file() or hashlib.sha256((cwd / f).read_bytes()).hexdigest() != digest]
        if not stale:
            if explicit_base and explicit_base not in data.get("base_ref", "") and explicit_base != data.get("base"):
                raise LookupError(f"--base {explicit_base}: the diff rb.sh sent is the {data.get('base_ref')}")
            return Changes(data["base"], data.get("base_ref", "rb.sh"), sorted(set(data["files"])),
                           sorted(data.get("deleted", [])), sorted(data.get("untracked", [])), "rb.sh",
                           data.get("hunks", {}), data.get("map_commit", ""), data.get("map_diff", {}))
        try:
            return git_changes(explicit_base, cwd)
        except (LookupError, RuntimeError):
            raise LookupError(f"{rb_file} is not this checkout's diff ({len(stale)} file(s) differ, e.g. {stale[0]}): "
                              "delete it to use git's diff") from None
    return git_changes(explicit_base, cwd)


# ---------------------------------------------------------------------------------------- 2. the builds

@dataclass
class Plan:
    builds: list[str] = field(default_factory=list)          # linux, unit-tests, web, android, everything
    reasons: dict[str, str] = field(default_factory=dict)
    unit_all: bool = False
    unit_core: list[str] = field(default_factory=list)       # gtest suites
    unit_editor: list[str] = field(default_factory=list)
    unit_htmlui: list[str] = field(default_factory=list)
    htmlui_all: bool = False
    self_tests: list[str] = field(default_factory=list)      # tools' own Python tests
    unit_notes: list[str] = field(default_factory=list)
    mcp_all: bool = False
    mcp_ci: bool = False
    mcp_files: list[str] = field(default_factory=list)
    mcp_reasons: dict[str, str] = field(default_factory=dict)
    mcp_notes: list[str] = field(default_factory=list)
    modules_off: str = ""                                    # the file that asks for MODULES_OFF_CHECK

    def add_build(self, name: str, why: str) -> None:
        if name not in self.builds:
            self.builds.append(name)
            self.reasons[name] = why

    def add_mcp(self, name: str, why: str) -> None:
        if name not in self.mcp_files:
            self.mcp_files.append(name)
            self.mcp_reasons[name] = why


def plan_builds(files: list[str], plan: Plan) -> None:
    for f in files:
        test = SELF_TESTS.get(f)
        if test and test not in plan.self_tests:
            plan.self_tests.append(test)
        if match(f, CI_SUITE_GUARD_TRIGGERS) and CI_SUITE_GUARD not in plan.self_tests:
            plan.self_tests.append(CI_SUITE_GUARD)
    # engine code may call a required-reason API the Apple privacy manifest must declare
    if any(f.startswith("engine/") and f.endswith(CXX) for f in files) or "engine/workspaces/xcode/ios/PrivacyInfo.xcprivacy" in files:
        if APPLE_PRIVACY_CHECK not in plan.self_tests:
            plan.self_tests.append(APPLE_PRIVACY_CHECK)
    everything = [f for f in files if match(f, ALL_TRIGGERS)]
    plan.modules_off = next((f for f in files if f in ("engine/CMakeLists.txt", MODULES_OFF_CHECK)
                             or (f.startswith("engine/core/") and f.endswith(HEADER))), everything[0] if everything else "")
    if everything:
        for b in ("linux", "unit-tests", "web", "android"):
            plan.add_build(b, f"{everything[0]} changed (builds everything)")
        plan.unit_all = True
        plan.mcp_all = True
        plan.mcp_notes.append(f"{everything[0]} changed: the whole MCP suite")
        return
    cxx = [f for f in files if f.startswith(("engine/", "editor/", "libs/")) and (f.endswith(CXX) or "CMakeLists" in f
                                                                                 or f.endswith(".cmake"))]
    engine = [f for f in files if f.startswith("engine/")]
    editor = [f for f in files if f.startswith("editor/") or match(f, EDITOR_INPUTS)]
    if engine or editor:
        plan.add_build("linux", f"{(cxx or engine or editor)[0]} changed")
    shaders = [f for f in engine if f.startswith(("engine/shaders/", "engine/shaderlib/"))]
    # (a test's data: a golden, a fixture; no dependency database sees it)
    test_data = [f for f in files if f.startswith(("engine/tests/", "editor/tests/")) and not f.endswith(CXX)
                 and not f.startswith("engine/tests/htmlui/")]
    if cxx or shaders or test_data:
        plan.add_build("unit-tests", f"{(cxx or shaders or test_data)[0]} changed")
    if test_data:
        plan.unit_all = True
        plan.unit_notes.append(f"{test_data[0]} changed (a unit test's data): every unit test")
    if [f for f in files if match(f, HTMLUI_PATHS) and f.endswith(CXX)] or \
            [f for f in files if f.startswith("engine/tests/htmlui/")]:
        plan.add_build("htmlui-tests", f"{[f for f in files if match(f, HTMLUI_PATHS)][0]} changed")
    android = [f for f in files if match(f, ANDROID_DIRS)]
    web = [f for f in files if match(f, WEB_DIRS)] + \
        [f for f in engine if f not in android and (f.endswith(CXX) or "CMakeLists" in f or f.endswith(".cmake"))
         and not f.startswith("engine/tests/")]
    if web:
        plan.add_build("web", f"{web[0]} changed (the engine's web build)")
    if android:
        plan.add_build("android", f"{android[0]} changed (Android's own code)")


# ---------------------------------------------------------------------------------------- 3. unit tests

def parse_ninja_deps(text: str, tree: Path, root: Path | None = None) -> dict[str, list[str]]:
    """`ninja -t deps` output: object -> its dependencies (repository-relative where they are in it;
    the first one is the object's source)."""
    deps: dict[str, list[str]] = {}
    current = None
    for line in text.splitlines():
        if not line.strip():
            current = None
            continue
        if not line.startswith((" ", "\t")):
            current = line.split(":", 1)[0].strip()
            deps[current] = []
        elif current is not None:
            deps[current].append(normalize(line.strip(), tree, root))
    return deps


def normalize(path: str, tree: Path, root: Path | None = None) -> str:
    root = root or ROOT
    p = Path(path)
    if not p.is_absolute():
        p = tree / p
    text = os.path.normpath(str(p))
    for prefix in (str(root), "/src"):   # this checkout, or the image's mount of it
        if text == prefix or text.startswith(prefix + os.sep):
            return os.path.relpath(text, prefix)
    return text


def siblings(path: str) -> list[str]:
    """A changed Foo.cpp also counts as its Foo.h (what the other units see of it)."""
    stem, ext = os.path.splitext(path)
    if ext in HEADER:
        return [path]
    return [path] + [stem + h for h in (".h", ".hpp", ".hh", ".inl")]


def is_test_source(path: str) -> bool:
    return path.startswith(("engine/tests/", "editor/tests/")) and path.endswith((".cpp", ".cc", ".cxx"))


def gtest_suites(source_text: str) -> list[str]:
    return sorted({m.group(2) for m in TEST_DEF.finditer(source_text)})


def suite_filter(suites: list[str]) -> str:
    """--gtest_filter for whole suites, value- and type-parameterised ones too (Prefix/Suite.x/0)."""
    return ":".join(f"{s}.*:*/{s}.*:{s}/*.*" for s in suites)


def select_units(changed: list[str], deps: dict[str, list[str]], read=lambda p: (ROOT / p).read_text(errors="replace")):
    """The test TUs of a tree that see the change, and their gtest suites."""
    effective = set()
    for f in changed:
        effective.update(siblings(f))
    units = []
    for obj, paths in deps.items():
        if not paths or not is_test_source(paths[0]):
            continue
        if paths[0] in changed or effective.intersection(paths):
            units.append(paths[0])
    suites = set()
    for unit in units:
        try:
            suites.update(gtest_suites(read(unit)))
        except OSError:
            pass
    return sorted(set(units)), sorted(suites)


def in_image() -> bool:
    return Path("/.dockerenv").is_file() and not shutil.which("docker")


def ninja_deps(tree: Path) -> str | None:
    """`ninja -t deps` of a tree, by the image's ninja: another version of it (a host's) would
    delete the tree's dependency log as unreadable."""
    if not (tree / "build.ninja").is_file():
        return None
    rel = os.path.relpath(tree, ROOT)
    command = ["ninja", "-C", rel, "-t", "deps"]
    if not in_image():
        command = ["tools/build.sh", "exec", "linux"] + command
    r = subprocess.run(command, cwd=str(ROOT), capture_output=True, text=True, errors="replace")
    return r.stdout if r.returncode == 0 and r.stdout.strip() else None


def plan_units(changes: Changes, plan: Plan, deps_core: str | None, deps_editor: str | None,
               deps_htmlui: str | None = None) -> None:
    if "htmlui-tests" in plan.builds:
        if deps_htmlui is None or [f for f in changes.files if match(f, HTMLUI_PATHS) and
                                   (f.endswith(("CMakeLists.txt", ".cmake")) or
                                    (f.startswith("engine/tests/htmlui/") and not f.endswith(CXX)))]:
            plan.htmlui_all = True
            plan.unit_notes.append("HTML UI tests: all (no dependency database of their tree yet, their CMake or a test's data changed)")
        else:
            units, suites = select_units(_code(changes.files), parse_ninja_deps(deps_htmlui, HTMLUI_TREE))
            plan.unit_htmlui.extend(suites)
            plan.unit_notes.append(f"htmlui: {len(units)} test unit(s) see the change")
    if plan.unit_all or "unit-tests" not in plan.builds:
        return
    cxx = _code(changes.files)
    unseen = [f for f in cxx if f.endswith(("CMakeLists.txt", ".cmake"))]
    if unseen:
        plan.unit_all = True
        plan.unit_notes.append(f"{unseen[0]} changed (CMake): every unit test")
        return
    if deps_core is None or deps_editor is None:
        plan.unit_all = True
        plan.unit_notes.append("no dependency database of the test trees yet (their first build): every unit test")
        return
    for name, text, tree in (("core", deps_core, CORE_TREE), ("editor", deps_editor, EDITOR_TEST_TREE)):
        deps = parse_ninja_deps(text, tree)
        known = {p for paths in deps.values() for p in paths}
        units, suites = select_units(cxx, deps)
        # a new test source no database has yet: its suites run anyway
        for f in changes.files:
            if is_test_source(f) and f not in known and (ROOT / f).is_file() and not f.startswith("engine/tests/htmlui/") and \
                    f.startswith("engine/tests/" if name == "core" else "editor/tests/"):
                units.append(f)
                suites = sorted(set(suites) | set(gtest_suites((ROOT / f).read_text(errors="replace"))))
        getattr(plan, f"unit_{name}").extend(suites)
        if units:
            plan.unit_notes.append(f"{name}: {len(units)} test unit(s) see the change: {', '.join(units[:6])}"
                                   f"{' ...' if len(units) > 6 else ''}")
    # a shader is compiled when the test runs, no C++ unit includes it: the suites that draw
    if [f for f in changes.files if f.startswith(("engine/shaders/", "engine/shaderlib/"))]:
        drawing = gpu_suites(ROOT / "engine" / "tests" / "core")
        plan.unit_core = sorted(set(plan.unit_core) | set(drawing))
        plan.unit_notes.append(f"a shader changed: the {len(drawing)} *Gpu suites of the core tests")
    new = [f for f in cxx if f.endswith(CXX) and not f.startswith(("engine/tests/", "editor/tests/"))
           and f not in changes.deleted and not any(f in p for p in (deps_core, deps_editor))]
    if new:
        plan.unit_notes.append(f"{len(new)} changed source(s) no test tree compiles or includes (e.g. {new[0]}): "
                               "their behaviour is the MCP tests' to cover")


def _code(files: list[str]) -> list[str]:
    return [f for f in files if f.startswith(("engine/", "editor/", "libs/"))]


def gpu_suites(tests: Path) -> list[str]:
    """The core tests' suites that render into a device (their name says it: docs/testing.md)."""
    found: set[str] = set()
    for directory, _, names in os.walk(tests):
        for name in names:
            if name.endswith(".cpp"):
                found.update(s for s in gtest_suites((Path(directory) / name).read_text(errors="replace")) if "Gpu" in s)
    return sorted(found)


# ---------------------------------------------------------------------------------------- 4. MCP tests

def load_map(path: Path) -> dict | None:
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


def map_path() -> Path:
    """The coverage map: JASPER_MCP_COVERAGE_MAP, else the machine's shared one, else the checkout's."""
    if os.environ.get("JASPER_MCP_COVERAGE_MAP"):
        return Path(os.environ["JASPER_MCP_COVERAGE_MAP"])
    for candidate in (Path(os.environ.get("JASPER_MCP_SHARED_BUILD_CACHE", "/nonexistent")) / "coverage-map.json",
                      Path.home() / "je" / "mcp-build-cache" / "coverage-map.json",
                      ROOT / "build" / "coverage" / "coverage-map.json"):
        if candidate.is_file():
            return candidate
    return ROOT / "build" / "coverage" / "coverage-map.json"


def map_staleness(cmap: dict, now: float, commits_since=None) -> str | None:
    age = (now - float(cmap.get("created", 0))) / 86400
    if age > MAP_MAX_AGE_DAYS:
        return f"the coverage map is {age:.0f} days old (limit {MAP_MAX_AGE_DAYS})"
    if commits_since is not None:
        n = commits_since(cmap.get("commit", ""))
        if n is not None and n > MAP_MAX_COMMITS:
            return f"the coverage map is {n} commits behind (limit {MAP_MAX_COMMITS})"
    return None


SHADER_DIRS = ("engine/shaders/", "engine/shaderlib/")
GENERIC_NAMES = {"assets", "scenes", "scripts", "shaders", "images", "textures", "golden", "tests", "core", "project",
                 "models", "ui", "data", "fixtures", "guide", "mcp", "lua", "engine", "editor", "tools", "docs", "includes"}


def path_mapped(f: str) -> bool:
    """A file tests load, not one they execute or one that is only prose or code of another kind."""
    if f.endswith(CXX + (".py", ".sh", ".md", ".cmake", ".txt", ".json")) and not f.startswith(("tools/fixtures/", "tools/test-projects/", "docs/mcp/guide/")):
        return False
    return f.startswith(("engine/", "editor/", "tools/fixtures/", "tools/test-projects/", "docs/mcp/guide/", "tools/mcp/tests/golden/"))


def path_keys(f: str) -> set[str]:
    parts = Path(f).parts
    keys = {Path(f).name, Path(f).stem}
    if f.startswith(("tools/fixtures/", "tools/test-projects/")) and len(parts) > 2:
        keys.add(parts[2])
    elif len(parts) > 1:
        keys.add(parts[-2])
    return {k for k in keys if k.lower() not in GENERIC_NAMES and len(k) > 2}


def test_files(tests_dir: Path = MCP_TESTS) -> dict[str, str]:
    return {p.name: p.read_text(errors="replace") for p in sorted(tests_dir.glob("test_*.py"))}


def plan_mcp(changes: Changes, plan: Plan, cmap: dict | None, sources: dict[str, str],
             staleness: str | None = None) -> None:
    """sources: test file -> its text."""
    if plan.mcp_all:
        return
    files = changes.files
    harness = [f for f in files if f in MCP_ALL_TRIGGERS]
    if harness:
        plan.mcp_all = True
        plan.mcp_notes.append(f"{harness[0]} changed (the harness): the whole MCP suite")
        return
    for f in files:
        name = Path(f).name
        if f.startswith("tools/mcp/tests/") and name.startswith("test_") and name.endswith(".py"):
            if f not in changes.deleted:
                plan.add_mcp(name, "the test file changed")
        elif f.startswith("tools/mcp/tests/golden/"):
            pass   # (by path, below)
        elif f.startswith("tools/mcp/tests/") or (f.startswith("tools/mcp/") and f.endswith(".py")):
            # a helper module or test data: the files that import or name it
            stem = Path(f).stem
            for test, text in sources.items():
                if re.search(rf"\b{re.escape(stem)}\b", text):
                    plan.add_mcp(test, f"uses {f}")
    # the contributor docs have a drift check of their own
    agent_docs = [f for f in files if f in ("AGENTS.md", "CLAUDE.md") or f.startswith(".claude/rules/")]
    if agent_docs and "test_agent_docs.py" in sources:
        plan.add_mcp("test_agent_docs.py", f"checks {agent_docs[0]}")
    # the tools a changed MCP tool source defines: the files that call them, unless the map says
    # exactly which functions the change touches and who runs them
    for f in files:
        entry = (cmap or {}).get("sources", {}).get(f)
        exact = entry and changes.map_commit == cmap.get("commit") and changes.hunks.get(f) and \
            not touched_functions(entry, to_map_lines(changes.hunks[f], changes.map_diff.get(f, [])))[1]
        if f.startswith("editor/mcp/") and f.endswith(CXX) and (ROOT / f).is_file() and not exact:
            tools = set(TOOL_NAME.findall((ROOT / f).read_text(errors="replace")))
            for test, text in sources.items():
                hit = sorted(t for t in tools if re.search(rf"[\"']{t}[\"']", text))
                if hit:
                    plan.add_mcp(test, f"calls {hit[0]} ({f})")
    # everything else a test can load (fixtures, test projects, the guide, Lua, shaders, HTML, images,
    # scenes...): by path, the file's, stem's, folder's or fixture's name in a test file; a file
    # no test names is unmapped: the CI suite
    unmapped_paths = []
    for f in files:
        if not path_mapped(f) or f in changes.deleted:
            continue
        keys = path_keys(f)
        hit = False
        for test, text in sources.items():
            if any(re.search(rf"[\"'/\s]{re.escape(k)}[\"'/.\s]", text) for k in keys):
                plan.add_mcp(test, f"loads {f}")
                hit = True
        if f.startswith("docs/mcp/guide/") and "test_docs.py" in sources:
            plan.add_mcp("test_docs.py", f"the guide changed ({f})")
            hit = True
        if f.startswith(SHADER_DIRS) and "test_shader_library.py" in sources:
            # any shader: its compile matrix, and the CI suite for what draws with it
            plan.add_mcp("test_shader_library.py", f"compiles {f}")
            plan.mcp_ci = True
            hit = True
        if not hit:
            unmapped_paths.append(f)
    if unmapped_paths:
        plan.mcp_ci = True
        plan.mcp_notes.append(f"{len(unmapped_paths)} changed file(s) no test file names (e.g. {unmapped_paths[0]}): the CI suite as well")
    compile_path = [f for f in files if match(f, COMPILE_PATHS)]
    for t in COMPILE_FILES if compile_path else ():
        if t in sources:
            plan.add_mcp(t, f"compiles a game ({compile_path[0]} changed)")
    saved_output = [f for f in files if match(f, SAVED_OUTPUT_PATHS)]
    for t in SAVED_OUTPUT_FILES if saved_output else ():
        if t in sources:
            plan.add_mcp(t, f"guards what a save writes ({saved_output[0]} changed)")
    # the engine's and the editor's C++: the coverage map
    # (the unit tests' own sources are no editor code: nothing an MCP test could run)
    code = [f for f in files if f.startswith(("engine/", "editor/", "libs/")) and f.endswith(CXX)
            and not f.startswith(("engine/tests/", "editor/tests/"))]
    if not code:
        return
    if cmap is None or "sources" not in cmap:
        plan.mcp_ci = True
        plan.mcp_notes.append("no coverage map (tools/mcp/coverage_map.py): the CI suite for the C++ change")
        return
    if staleness:
        plan.mcp_ci = True
        plan.mcp_notes.append(f"{staleness}: the CI suite as well")
    select_by_map(changes, code, cmap, sources, plan)
    engine_runtime = [f for f in code if f.startswith("engine/")]
    if engine_runtime:
        for t in GAME_BUILD_FILES:
            if t in sources:
                plan.add_mcp(t, f"runs a game of the engine ({engine_runtime[0]} changed)")


def touched_functions(entry: dict, ranges: list[tuple[int, int, bool]] | None) -> tuple[list[list], bool]:
    """The map's functions of a file a diff touches, and whether that is the whole file: no lines
    (the map's commit is not this tree's history), or a changed line outside every function the map
    knows (a declaration, a type, a macro: anything may depend on it). Only new lines outside the
    functions touch nothing old: a new function counts through its callers' changes."""
    functions = entry["functions"]
    if not ranges:
        return functions, True
    touched = []
    for first, last, only_new in ranges:
        hit = [f for f in functions if f[0] <= last and first <= f[1]]
        inside = all(any(f[0] <= line <= f[1] for f in hit) for line in (first, last))
        if not inside and not only_new:
            return functions, True
        touched += hit
    return touched, False


def select_by_map(changes: Changes, code: list[str], cmap: dict, sources: dict[str, str], plan: Plan) -> None:
    """The test files that executed what the change touches. A function nearly every editor runs
    (COMMON_SHARE of the files with an editor: start-up, the frame, registrations) selects the
    COMMON_SAMPLE quickest files that run it, not all of them: that is the full gate's to cover.
    Code no test executes selects nothing, and is said to; a file the map does not know, the CI suite."""
    tests = cmap["tests"]
    seconds = cmap.get("seconds", {})
    limit = max(2, COMMON_SHARE * cmap.get("editor_tests", len(tests)))
    known = cmap["sources"]
    unmapped, unrun = [], []
    for f in code:
        entry = known.get(f)
        if f in changes.deleted and not entry:
            continue
        sibling = None
        if f.endswith(HEADER):
            sibling = next((known[c] for c in (os.path.splitext(f)[0] + e for e in (".cpp", ".cc", ".mm")) if c in known), None)
        if not entry and not sibling:
            unmapped.append(f)
            continue
        groups = []
        whole = True
        if entry:
            ranges = None
            if changes.map_commit and changes.map_commit == cmap.get("commit") and changes.hunks.get(f):
                ranges = to_map_lines(changes.hunks[f], changes.map_diff.get(f, []))
            functions, whole = touched_functions(entry, ranges)
            groups.append((f, functions))
        if sibling and (whole or not entry):
            groups.append((f, sibling["functions"]))
        if not whole and not any(functions for _, functions in groups):
            continue   # (only new code: its callers' changes select)
        hit, common = 0, 0
        for _, functions in groups:
            for _first, _last, mask in functions:
                value = int(mask, 16)
                if value and bin(value).count("1") >= limit:
                    common |= value
                else:
                    hit |= value
        names = [t for i, t in enumerate(tests) if hit >> i & 1 and t in sources]
        how = "the whole file" if whole else "the changed functions"
        for t in names:
            plan.add_mcp(t, f"runs {f} ({how})")
        if common:
            rest = sorted((t for i, t in enumerate(tests) if common >> i & 1 and t in sources),
                          key=lambda t: (seconds.get(t, 1e9), t))
            for t in rest[:COMMON_SAMPLE]:
                plan.add_mcp(t, f"a quick file that runs {f}'s common code")
            plan.mcp_notes.append(f"{f}: code nearly every editor runs: the {COMMON_SAMPLE} quickest files of those that run it")
        if not names and not common:
            unrun.append(f)
    if unmapped:
        plan.mcp_ci = True
        plan.mcp_notes.append(f"{len(unmapped)} changed source(s) the map does not know (new; e.g. {unmapped[0]}): the CI suite as well")
    if unrun:
        plan.mcp_notes.append(f"{len(unrun)} changed source(s) no MCP test executes (e.g. {unrun[0]}): the unit tests' to cover")


# ---------------------------------------------------------------------------------------- 5. cache

def inputs_hash(changes: Changes, cmap_version: str, options: str, root: Path | None = None) -> str:
    root = root or ROOT
    h = hashlib.sha256()
    h.update(f"base {changes.base}\noptions {options}\nmap {cmap_version}\n".encode())
    for f in changes.files:
        p = root / f
        h.update(f"\0{f}\0".encode())
        if p.is_file():
            h.update(p.read_bytes())
        elif f in changes.deleted:
            h.update(b"(deleted)")
    for d in sorted((root / "docker").glob("*.Dockerfile")):
        h.update(d.name.encode() + d.read_bytes())
    h.update(Path(__file__).read_bytes())
    return h.hexdigest()


def cached(key: str, cache_file: Path = CACHE_FILE) -> dict | None:
    try:
        data = json.loads(cache_file.read_text())
    except (OSError, ValueError):
        return None
    return data if data.get("key") == key and data.get("passed") else None


def store(key: str, summary: str, cache_file: Path = CACHE_FILE) -> None:
    cache_file.parent.mkdir(parents=True, exist_ok=True)
    tmp = cache_file.with_suffix(".tmp")
    tmp.write_text(json.dumps({"key": key, "passed": True, "summary": summary, "at": time.time()}, indent=1))
    os.replace(tmp, cache_file)


# ---------------------------------------------------------------------------------------- running

class Runner:
    def __init__(self, verbose: bool, jobs: int | None):
        self.verbose = verbose
        self.steps: list[tuple[str, str, float]] = []
        self.failed = False
        self.unrunnable: list[str] = []
        self.partial = False

    def mcp_results(self, label: str) -> None:
        """A selected MCP test that skipped could not run: a failure of the step (the full suite
        skips none on a build server)."""
        try:
            data = json.loads((ROOT / "build" / "test-results" / "mcp.json").read_text())
        except (OSError, ValueError):
            self.failed = True
            self.unrunnable.append(f"{label}: no results (build/test-results/mcp.json)")
            return
        for r in data.get("results", []):
            if r.get("status") == "skip":
                self.failed = True
                self.unrunnable.append(f"{r['name']}: {r.get('detail', '')[:200]}")

    def run(self, label: str, command: list[str], env: dict | None = None) -> bool:
        print(f"[check-changed] {label}: {' '.join(shlex.quote(c) for c in command)}", flush=True)
        t0 = time.monotonic()
        full_env = dict(os.environ, **(env or {}))
        if self.verbose:
            r = subprocess.run(command, cwd=str(ROOT), env=full_env)
            out = ""
        else:
            r = subprocess.run(command, cwd=str(ROOT), env=full_env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors="replace")
            out = r.stdout or ""
        dt = time.monotonic() - t0
        ok = r.returncode == 0
        self.steps.append((label, "PASS" if ok else f"FAIL (exit {r.returncode})", dt))
        if not ok:
            self.failed = True
            print(f"[check-changed] {label} FAILED (exit {r.returncode}, {dt:.0f}s); its output:", flush=True)
            if out:
                print(out[-20000:], flush=True)
        else:
            summary = [l for l in out.splitlines() if re.search(r"\d+ passed|tests? ran|\[  PASSED  \]", l)]
            print(f"[check-changed] {label}: ok in {dt:.0f}s{' (' + summary[-1].strip() + ')' if summary else ''}",
                  flush=True)
        return ok


def describe(changes: Changes, plan: Plan, cmap_info: str) -> str:
    lines = [f"changes: {len(changes.files)} file(s) against {changes.base[:12]} ({changes.base_ref}; from {changes.source})"]
    for f in changes.files[:12]:
        lines.append(f"  {'D' if f in changes.deleted else '?' if f in changes.untracked else 'M'} {f}")
    if len(changes.files) > 12:
        lines.append(f"  ... and {len(changes.files) - 12} more")
    lines.append("builds: " + (", ".join(f"{b} ({plan.reasons[b]})" for b in plan.builds) or "none (nothing buildable changed)"))
    if plan.unit_all:
        lines.append("unit tests: all (" + "; ".join(plan.unit_notes) + ")" if plan.unit_notes else "unit tests: all")
    elif plan.unit_core or plan.unit_editor:
        lines.append(f"unit tests: core suites {plan.unit_core or '-'}; editor suites {plan.unit_editor or '-'}")
    else:
        lines.append("unit tests: none see the change")
    lines += [f"  {n}" for n in plan.unit_notes if not plan.unit_all]
    if plan.self_tests:
        lines.append("tool tests: " + ", ".join(plan.self_tests))
    if plan.modules_off:
        lines.append(f"the engine with every module off ({plan.modules_off} changed)")
    if plan.htmlui_all or plan.unit_htmlui:
        lines.append("HTML UI tests: " + ("all" if plan.htmlui_all else f"suites {plan.unit_htmlui}"))
    if plan.mcp_all:
        lines.append("MCP tests: the whole suite")
    else:
        lines.append(f"MCP tests: {len(plan.mcp_files)} file(s){' + the CI suite' if plan.mcp_ci else ''} ({cmap_info})")
        for t in plan.mcp_files:
            lines.append(f"  {t}: {plan.mcp_reasons[t]}")
    lines += [f"  {n}" for n in plan.mcp_notes]
    return "\n".join(lines)


def commits_since(root: Path):
    def count(commit: str):
        if not commit:
            return None
        r = subprocess.run(["git", "rev-list", "--count", f"{commit}..HEAD"], cwd=str(root), capture_output=True, text=True)
        return int(r.stdout.strip()) if r.returncode == 0 and r.stdout.strip().isdigit() else None
    return count


def use_root(root: Path) -> None:
    """The checkout the run works in (tools/test_check_changed.py points it at a scratch repository)."""
    global ROOT, CACHE_FILE, RB_CHANGES, MCP_TESTS, CORE_TREE, EDITOR_TEST_TREE, EDITOR_TREE, HTMLUI_TREE
    ROOT = root
    CACHE_FILE = root / "build" / ".cache" / "check-changed.json"
    RB_CHANGES = root / "build" / ".rb-changes.json"
    MCP_TESTS = root / "tools" / "mcp" / "tests"
    CORE_TREE = root / "build" / "linux" / "core-tests"
    EDITOR_TEST_TREE = root / "build" / "linux" / "editor-tests"
    EDITOR_TREE = root / "build" / "linux" / "editor"
    HTMLUI_TREE = root / "build" / "linux" / "htmlui-tests-glcore"


def main(argv: list[str], root: Path | None = None) -> int:
    if root is not None:
        use_root(root)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", help="the ref to diff against (default: the merge base with oe-port, main, origin/main)")
    ap.add_argument("--dry-run", action="store_true", help="print the plan only")
    ap.add_argument("--no-cache", action="store_true", help="run even when the same inputs passed before")
    ap.add_argument("-j", type=int, default=0, help="MCP editors at once (default: the CPUs / 2, at least 2)")
    ap.add_argument("--keep-going", action="store_true", help="run the tests after a failed step (the exit code stays 1)")
    ap.add_argument("--no-mcp", action="store_true", help="leave the MCP tests out, and say so: for a change that selects "
                    "the whole suite, which the integrator runs once per wave (the pass is not cached)")
    ap.add_argument("--list-mcp", type=Path, help="write the selected MCP test files here (one per line)")
    ap.add_argument("--root", type=Path, help="check this checkout instead of the one holding this script "
                    "(e.g. an older branch's worktree, with this check-changed)")
    args = ap.parse_args(argv)
    if args.root is not None:
        use_root(args.root.resolve())
    verbose = os.environ.get("CHECK_CHANGED_VERBOSE", "") not in ("", "0")
    t0 = time.monotonic()

    try:
        changes = read_changes(args.base, ROOT, RB_CHANGES)
    except (LookupError, RuntimeError) as e:
        print(f"check-changed: {e}", file=sys.stderr)
        return 1
    plan = Plan()
    plan_builds(changes.files, plan)
    cpath = map_path()
    cmap = load_map(cpath)
    cmap_info = f"map {cpath} of {cmap.get('commit', '?')[:12]}" if cmap else f"no map at {cpath}"
    cmap_version = f"{cmap.get('commit', '')}:{cmap.get('created', '')}" if cmap else "none"
    if cmap and changes.source == "git" and cmap.get("commit"):
        # (a checkout with the history: the map's lines from git; through rb.sh, it sends them)
        r = subprocess.run(["git", "diff", "-U0", "--no-renames", cmap["commit"], "--", *changes.files], cwd=str(ROOT),
                           capture_output=True, text=True, errors="replace")
        if r.returncode == 0:
            changes.map_commit, changes.map_diff = cmap["commit"], parse_hunks(r.stdout)
    options = f"{args.base}"
    key = inputs_hash(changes, cmap_version, options, ROOT)
    if not args.no_cache and not args.dry_run:
        hit = cached(key, CACHE_FILE)
        if hit:
            print(hit["summary"])
            print(f"[check-changed] the same inputs passed before (build/.cache/check-changed.json); --no-cache runs again")
            return 0

    runner = Runner(verbose, args.j)
    build_env = {"WERROR": "1"}
    # 2. builds (the unit test trees first: their dependency database picks the suites)
    if "unit-tests" in plan.builds and not args.dry_run:
        runner.run("build the unit tests", ["tools/build.sh", "unit-tests"], dict(build_env, RUN="0"))
    if "htmlui-tests" in plan.builds and not args.dry_run:
        runner.run("build the HTML UI tests", ["tools/build.sh", "htmlui-tests"], dict(build_env, RUN="0"))
    staleness = map_staleness(cmap, time.time(), commits_since(ROOT)) if cmap else None
    plan_units(changes, plan, ninja_deps(CORE_TREE), ninja_deps(EDITOR_TEST_TREE),
               ninja_deps(HTMLUI_TREE) if "htmlui-tests" in plan.builds else None)
    plan_mcp(changes, plan, cmap, test_files(MCP_TESTS), staleness)
    text = describe(changes, plan, cmap_info)
    print(text, flush=True)
    if args.list_mcp:
        args.list_mcp.write_text("\n".join(plan.mcp_files) + "\n")
    if args.dry_run:
        print(GAP)
        return 0

    for test in plan.self_tests:
        if (ROOT / test).is_file():
            runner.run(f"tool tests ({test})", [sys.executable, "-I", test])
    for b in plan.builds:
        if b in ("unit-tests", "htmlui-tests"):
            continue
        if b in ("web", "android") and in_image():
            # (their images are not this one: the check goes on, its pass is not cached, and the summary says so)
            runner.steps.append((f"build {b}", "SKIP (not in the Linux image: tools/check-changed outside it builds it)", 0.0))
            runner.partial = True
            continue
        env = dict(build_env, SKIP_ENGINE="1") if b == "linux" else dict(build_env)
        runner.run(f"build {b}", ["tools/build.sh", b], env)
    # the header gates: they read the Linux editor's tree, whose paths are the image's
    # (a checkout without them: an older branch's, --root)
    in_tree = (lambda *c: list(c)) if in_image() else (lambda *c: ["tools/build.sh", "exec", "linux", *c])
    if "linux" in plan.builds and (ROOT / "tools/check_fanout.py").is_file() and (args.keep_going or not runner.failed):
        headers = sorted(f for f in changes.files if f.endswith(HEADER) and f not in changes.deleted)
        if headers:
            runner.run(f"headers compile alone ({len(headers)} edited)", in_tree("python3", "-I", "tools/check_headers.py", *headers))
        runner.run("header fan-out limits", in_tree("python3", "-I", "tools/check_fanout.py"))
    if plan.modules_off and (ROOT / MODULES_OFF_CHECK).is_file() and (args.keep_going or not runner.failed):
        jobs = os.environ["JOBS"] if os.environ.get("JOBS", "").isdigit() else str(os.cpu_count() or 4)
        runner.run("the engine with every module off", in_tree("env", "WERROR=1", "bash", MODULES_OFF_CHECK, "-j", jobs, "all-off"))
    # 3. unit tests
    if (args.keep_going or not runner.failed) and "unit-tests" in plan.builds:
        if plan.unit_all:
            runner.run("unit tests (all)", ["tools/build.sh", "unit-tests"], build_env)
        else:
            for suite, names in (("core", plan.unit_core), ("editor", plan.unit_editor)):
                if names:
                    runner.run(f"unit tests ({suite}: {len(names)} suite(s))", ["tools/build.sh", "unit-tests"],
                               dict(build_env, UNIT_SUITE=suite, UNIT_TEST_ARGS=f"--gtest_filter={suite_filter(names)}"))
    if (args.keep_going or not runner.failed) and "htmlui-tests" in plan.builds:
        if plan.htmlui_all:
            runner.run("HTML UI tests (all)", ["tools/build.sh", "htmlui-tests"], build_env)
        elif plan.unit_htmlui:
            runner.run(f"HTML UI tests ({len(plan.unit_htmlui)} suite(s))", ["tools/build.sh", "htmlui-tests"],
                       dict(build_env, HTMLUI_TEST_ARGS=f"--gtest_filter={suite_filter(plan.unit_htmlui)}"))
    # 4. MCP tests
    if args.no_mcp and (plan.mcp_all or plan.mcp_ci or plan.mcp_files):
        runner.steps.append(("MCP tests", "SKIP (--no-mcp)", 0.0))
        runner.partial = True
    elif (args.keep_going or not runner.failed) and (plan.mcp_all or plan.mcp_ci or plan.mcp_files):
        if "linux" not in plan.builds and not (ROOT / "build/linux/editor-install/bin/jasperengine-editor").exists():
            runner.run("build linux (the MCP tests' editor)", ["tools/build.sh", "linux"], dict(build_env, SKIP_ENGINE="1"))
        # (the container's CPUs, rb.sh's JOBS, for two each: LP_NUM_THREADS 2, docs/testing.md)
        cpus = int(os.environ["JOBS"]) if os.environ.get("JOBS", "").isdigit() else (os.cpu_count() or 4)
        jobs = args.j or max(2, cpus // 2)
        # (a runner without --split-seconds: an older checkout's, --root)
        runner_source = ROOT / "tools/mcp/run_tests.py"
        split = ["--split-seconds", "600"] if runner_source.is_file() and "--split-seconds" in runner_source.read_text() else []
        mcp_env = {"JASPER_SLOW_BUILD": "1", "JASPER_SLOW_TESTS": "1", "MCP_SUITE": "all"}
        if plan.mcp_all:
            runner.run("MCP tests (the whole suite)", ["tools/build.sh", "mcp-tests", "-j", str(jobs), *split], mcp_env)
            runner.mcp_results("MCP tests (the whole suite)")
        else:
            if plan.mcp_ci:
                runner.run("MCP tests (the CI suite)", ["tools/build.sh", "mcp-tests", "-j", str(jobs)],
                           {"MCP_SUITE": "ci", "JASPER_SLOW_BUILD": "1", "JASPER_SLOW_TESTS": "1"})
                runner.mcp_results("MCP tests (the CI suite)")
            files = [f for f in plan.mcp_files if (MCP_TESTS / f).is_file()]
            missing = [f for f in plan.mcp_files if f not in files]
            if plan.mcp_ci:   # (the CI suite above ran those)
                files = [f for f in files if not re.search(r"^CI = True", (MCP_TESTS / f).read_text(errors="replace"), re.M)]
            if missing:
                runner.steps.append(("MCP tests", f"CANNOT RUN: {missing} (selected, not in the checkout)", 0.0))
                runner.failed = True
            if files:
                if "test_mesh_raycast.py" not in files:
                    mcp_env["MCP_SKIP_WEB_BUILD"] = "1"
                elif in_image():
                    # (its latency gate loads a web build, which this image cannot make)
                    runner.unrunnable.append("test_mesh_raycast.py: needs the web image: run on the host")
                    runner.failed = True
                    files = [f for f in files if f != "test_mesh_raycast.py"]
                    mcp_env["MCP_SKIP_WEB_BUILD"] = "1"
                runner.run(f"MCP tests ({len(files)} file(s))", ["tools/build.sh", "mcp-tests", "-j", str(jobs),
                           *split, *[f"tools/mcp/tests/{f}" for f in files]], mcp_env)
                runner.mcp_results(f"MCP tests ({len(files)} file(s))")
    wall = time.monotonic() - t0
    lines = ["", "== check-changed summary", text]
    lines += [f"  could not run: {s}" for s in runner.unrunnable]
    for label, status, dt in runner.steps:
        lines.append(f"{status:4} {label} ({dt:.0f}s)")
    if not runner.steps:
        lines.append("nothing to build or test")
    lines.append(f"{'FAILED' if runner.failed else 'passed'} in {wall:.0f}s")
    lines.append(GAP)
    summary = "\n".join(lines)
    print(summary, flush=True)
    if runner.partial:
        print("[check-changed] a step was left out (above): the pass is not cached", flush=True)
    elif not runner.failed:
        if inputs_hash(changes, cmap_version, options, ROOT) == key and \
                read_changes(args.base, ROOT, RB_CHANGES).files == changes.files:
            store(key, summary, CACHE_FILE)
        else:
            print("[check-changed] files changed during the run: the pass is not cached", flush=True)
    return 1 if runner.failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
