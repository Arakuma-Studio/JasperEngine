"""tools/check-changed: the per-commit gate of a stream. Builds what the change touches and runs the
unit and MCP tests that can see it; the full gate is CI's (the merge queue and the nightly).

    tools/check-changed                 # build and test what the change touches
    tools/check-changed --dry-run       # the plan only
    tools/check-changed --base oe-port  # the diff against another base (default: the merge base
                                        # with oe-port, else main, else origin/main)
    tools/check-changed --no-cache      # run even when the same inputs passed before
    tools/check-changed --no-mcp        # builds and unit tests only: the MCP selection stays the integrator's
    tools/check-changed --json          # the plan, the step results, logs and fix commands as one JSON document
    tools/check-changed --red-control   # then the same selected tests without the change: some must fail
    tools/check-changed --red-control --restore   # the tree an interrupted red control left reverted
    CHECK_CHANGED_VERBOSE=1 tools/check-changed   # every step's output on the terminal too
    tools/check-changed --plan-json plan.json --deps-from DIR   # CI's `changes` job (tools/ci/plan_lanes.py):
                                        # the plan as JSON, nothing run; the test trees' dependency
                                        # databases from DIR (tools/ci/dump_deps.sh) instead of Ninja

1. The changed files: committed (since the merge base), staged, unstaged and untracked. Through
   rb.sh the server's worktree has no history of the stream: rb.sh writes the local diff to
   build/.rb-changes.json, which this reads when its own git cannot name the base.
2. The builds: the Linux editor (SKIP_ENGINE=1 tools/build.sh linux) for engine/, editor/ and what
   the editor embeds (EDITOR_INPUTS), the unit test binaries for their C++ and shaders, the HTML UI
   tests for its code, web for engine/ code and its CMake, Android for its own platform code;
   everything for the root CMake, docker/, tools/build.sh, the toolchain and the lockfiles
   (ALL_TRIGGERS); nothing for a change that builds nothing (docs). This tool's and the harness's
   own Python run their self-tests (SELF_TESTS). Navigation's runtime, Recast/Detour and the
   engine's CMake also build the engine with navigation OFF and run its stub tests
   (NAVIGATION_OFF_PATHS: tools/check-navigation-off.sh).
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
6. Exit 1 when a step failed or a selected test could not run. Independent steps go on after a
   failure; a step whose build failed is skipped, and said to be. Each step's whole output is in
   build/check-changed/<step>.log; a failed step shows its parsed failures (googletest, unittest,
   pytest, the MCP results, build diagnostics and crashes through tools/ci/report.py, a lint's
   findings; at most FAILURE_LINES) and its log. The run ends with one line per step, ok, FAIL,
   skip or cached, a FAIL with the command that reruns it (narrowed to the tests that failed).
   Inside the Linux image the web and Android builds are left out, and said to be (their images
   are the host's to run).
7. After the Linux editor's build: every header the change edits still compiles alone
   (tools/check_headers.py) and no component header passes the fan-out limits (tools/check_fanout.py).
8. When a header under engine/core, engine/CMakeLists.txt or what ALL_TRIGGERS names changes: the
   engine builds with every optional module off (tools/check-physics-off.sh all-off). The editor's
   engine has every module, so nothing else here sees a source that lost a header only a module's
   code gave it; a game's Compile build and the smaller prebuilt players do.
9. The area lints (AREA_LINTS), before the builds: each when a path it reads changes, with its
   --self-test when the lint or its fixtures change.
10. --red-control (a clean tree): after the pass, or its cached one, the branch's change outside the
   tests (RED_CONTROL_KEEPS) is saved to build/red-control/patch.diff and reverse-applied in place;
   the same selected tests are rebuilt and rerun, and the tree is restored (finally) and verified
   byte-identical (status, diff, untracked files). It prints which tests failed without the change
   (one whose build failed: red (build)) and exits 1 when none did; never cached. A marker
   (build/red-control/in-progress.json) lives from the revert to the verified restore: a run killed
   in between leaves it, and every later run refuses until --red-control --restore.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import importlib.util
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE_FILE = ROOT / "build" / ".cache" / "check-changed.json"
LOG_DIR = ROOT / "build" / "check-changed"      # each step's whole output
RED_DIR = ROOT / "build" / "red-control"        # the red control's patch, logs and in-progress marker
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
GAP = ("coverage gap: this is the change's own selection; the full gate is CI's: a pull request's Check, "
       "the merge queue (every build and unit test) and the nightly (the whole MCP CI suite)")

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
              "tools/ci/report.py": "tools/test_check_changed.py",   # (its parsers read check-changed's logs)
              "tools/mcp/shard.py": "tools/mcp/test_shard.py", "tools/mcp/test_shard.py": "tools/mcp/test_shard.py",
              "tools/mcp/build_cache.py": "tools/mcp/test_build_cache.py", "tools/mcp/test_build_cache.py": "tools/mcp/test_build_cache.py",
              "tools/mcp/call_profile.py": "tools/mcp/test_call_profile.py", "tools/mcp/test_call_profile.py": "tools/mcp/test_call_profile.py",
              "tools/check_fanout.py": "tools/test_check_fanout.py", "tools/test_check_fanout.py": "tools/test_check_fanout.py",
              "tools/xcode_sync_sources.py": "tools/test_xcode_sync_sources.py",
              "tools/test_xcode_sync_sources.py": "tools/test_xcode_sync_sources.py",
              "tools/check_shader_lod.py": "tools/ci/test_check_shader_lod.py",
              "tools/players/write_hashes.py": "tools/players/test_write_hashes.py",
              "tools/players/test_write_hashes.py": "tools/players/test_write_hashes.py",
              "tools/ci/release.sh": "tools/ci/test_release.py", "tools/ci/test_release.py": "tools/ci/test_release.py",
              "tools/ci/test_check_shader_lod.py": "tools/ci/test_check_shader_lod.py",
              "tools/ci/plan_lanes.py": "tools/ci/test_plan_lanes.py", "tools/ci/test_plan_lanes.py": "tools/ci/test_plan_lanes.py",
              "tools/ci/plan_inputs.py": "tools/ci/test_plan_inputs.py", "tools/ci/test_plan_inputs.py": "tools/ci/test_plan_inputs.py",
              "tools/ci/shadow.py": "tools/ci/test_shadow.py", "tools/ci/test_shadow.py": "tools/ci/test_shadow.py",
              "tools/ci/mcp_gate.py": "tools/ci/test_mcp_gate.py", "tools/ci/test_mcp_gate.py": "tools/ci/test_mcp_gate.py",
              "tools/ci/pr-evidence.sh": "tools/ci/test_pr_evidence.py", "tools/ci/test_pr_evidence.py": "tools/ci/test_pr_evidence.py"}
# tools/<dir>/test_<x>.py beside a changed tools/<dir>/<x>.py is its owner test, unless it needs what
# only its lane has (it takes arguments)
SELF_TESTS_ELSEWHERE = {"tools/crash/test_symbolicate.py": "takes the built jasperengine-crash-test",
                        "tools/ci/test_player_cli.py": "needs a built editor and the runtime image (the smoke lane)"}
# the shader lint, with its own tests, when an engine shader changes (.claude/rules/rendering.md)
SHADER_LINT_TEST = "tools/ci/test_check_shader_lod.py"
# an MCP test file, or what classifies it, changes: every file must stay classified for the CI suite
CI_SUITE_GUARD = "tools/ci/check-mcp-ci-suite.py"
CI_SUITE_GUARD_TRIGGERS = ("tools/mcp/tests/test_*.py", "tools/mcp/run_tests.py", CI_SUITE_GUARD)
# an engine source changes (any can hold an event or walk one): every event must stay cleared at Stop.
# The lint reads every engine source but the vendored libraries, the tests and the IDE projects
EVENT_CLEARS_LINT = "tools/check_event_clears.py"
EVENT_CLEARS_SKIPPED = ("engine/libs/", "engine/tests/", "engine/workspaces/")
# render code changes: no string literal names a GPU vendor or renderer (a workaround is a GPU quirk)
GPU_VENDOR_LINT = "tools/check_gpu_vendor_branches.py"
GPU_VENDOR_TRIGGERS = ("engine/renders/*", "engine/core/render/*", "engine/shaderlib/*", "engine/core/device/gpu_tiers.json",
                       GPU_VENDOR_LINT, "tools/gpu_vendor_branches_allowlist.txt")
# the area lints (AGENTS.md, Test), each run when a path it reads changes, before the builds: the
# paths it scans (its own skips: tests, libraries), its allow-list. lint -> (the paths it reads, the
# fixtures of its --self-test, None when it has none): a change of the lint or of those fixtures also
# runs its --self-test
LINT_SKIPPED_DIRS = {"tests", "libs", "external"}


def _scans(path: str, tops: tuple[str, ...], extensions: tuple[str, ...]) -> bool:
    return path.startswith(tops) and path.endswith(extensions) and not LINT_SKIPPED_DIRS & set(path.split("/")[:-1])


AREA_LINTS = {
    "tools/check_raw_writes.py": (
        lambda f: _scans(f, ("editor/", "engine/core/"), (".cpp", ".h", ".hpp", ".cc", ".mm"))
        or f == "tools/raw_writes_allowlist.txt", ()),
    "tools/check_stream_keys.py": (lambda f: match(f, ("editor/Stream*.cpp", "editor/reflect/components/*")), None),
    EVENT_CLEARS_LINT: (lambda f: f.startswith("engine/") and f.endswith(CXX) and not f.startswith(EVENT_CLEARS_SKIPPED), ()),
    "tools/check_text_encoding.py": (
        lambda f: _scans(f, ("editor/", "engine/"), (".cpp", ".h", ".hpp", ".cc", ".mm", ".m", ".c", ".inl"))
        or f == "tools/text_encoding_allowlist.txt", ()),
    "tools/ci/check-hardcoded-shortcuts.py": (
        lambda f: f.startswith("editor/") and f.endswith((".cpp", ".h", ".mm")) and not f.startswith("editor/tests/"),
        ("tools/ci/fixtures/hardcoded-shortcuts/",)),
}
# the engine with every optional module off: a header's include can reach a source only through a
# module's code, and the editor's engine, which has them all, never shows it
MODULES_OFF_CHECK = "tools/check-physics-off.sh"
# the required-reason APIs of engine code against the engine's Apple privacy manifest
APPLE_PRIVACY_CHECK = "tools/check_apple_privacy.py"
# C/C++ and what the engine's builds read
CXX = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".mm", ".m")
HEADER = (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp")
BUILDS_NOTHING = ("docs/*", "*.md", "LICENSE", "NOTICE.md", ".claude/*")
# what neither a build nor any test reads: a CI run whose every path is here runs no lane
# (tools/ci/plan_lanes.py). A path joins only with a note saying nothing reads it:
#   docs/specs, docs/superpowers, docs/engine-gaps, docs/openengine-analysis, docs/test-cleanup:
#     design prose; no CMake, test or lint opens them (tools/rebrand.py only allow-lists superpowers);
#   the root *-LEDGER.md and README.md: status prose; no CMake install, test or lint opens them;
#   tools/mcp/ci-timings.json: only the MCP shard planner reads it, to balance the shards, and every
#     plan runs the union check, so no value in it can lose or break a test (its daily refit's PR
#     would otherwise build every platform).
# Not here: .claude/ (tools/mcp/tests/test_agent_docs.py and tools/check_agent_docs.py read its
# rules), docs/mcp/guide and docs/unity-glossary.md (the editor embeds them), the rest of docs/
# (tests read keyboard-shortcuts.md, the golden ledgers under docs/html-ui and docs/titleart, ...)
CI_NOTHING_READS = ("docs/specs/*", "docs/superpowers/*", "docs/engine-gaps/*", "docs/openengine-analysis/*",
                    "docs/test-cleanup/*", "tools/mcp/ci-timings.json")
CI_NOTHING_READS_ROOT = ("*-LEDGER.md", "README.md")
# what --deps-from reads: `ninja -t deps` of build/linux/<name> (tools/ci/dump_deps.sh writes them)
DEPS_DUMPS = ("core-tests", "editor-tests", "htmlui-tests")
# outside engine/ and editor/, compiled into the editor or embedded in it (CMakeLists.txt): the
# vendored libraries, the MCP guide and the HTML UI example library, the Lua API tools
EDITOR_INPUTS = ("libs/*", "docs/mcp/guide/*", "docs/unity-glossary.md", "tools/lua_api/*")
HTMLUI_PATHS = ("engine/tests/htmlui/*", "engine/core/htmlui/*", "engine/libs/rmlui/*", "engine/libs/lexbor/*")
WEB_DIRS = ("engine/platform/web/*", "engine/platform/emscripten/*", "tools/web/*")
ANDROID_DIRS = ("engine/platform/android/*", "engine/project/android/*", "tools/android-*", "tools/ci/android_*")
# what a JASPERENGINE_NAVIGATION=OFF build compiles differently: tools/check-navigation-off.sh
# builds the engine that way and runs NavStubTest (compiled only then) and NavGridTest (S74 R28)
NAVIGATION_OFF_PATHS = ("engine/core/navigation/*", "engine/libs/recastnavigation/*", "engine/core/subsystem/NavigationSystem.*",
                        "engine/core/object/navigation/*", "engine/core/component/Nav*", "engine/CMakeLists.txt",
                        "engine/tests/core/navigation/*", "tools/check-navigation-off.sh")
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
# why a test was selected: its own source changed (owner); it includes, runs, calls or loads what
# changed (dependency); a rule watches the changed path (policy); a fixed quick sample of what nearly
# everything runs (smoke). A whole suite run by a widening is a fallback (tools/ci/plan_lanes.py)
REASON_KINDS = ("owner", "dependency", "policy", "smoke")
TEST_DEF = re.compile(r"^\s*(TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*\(\s*(\w+)\s*,", re.M)
TOOL_NAME = re.compile(r"\b\w+\.name\s*=\s*\"([a-z][a-z0-9_]*)\"")


def match(path: str, patterns) -> bool:
    return any(fnmatch.fnmatchcase(path, p) for p in patterns)


def ci_reads_nothing(path: str) -> bool:
    return match(path, CI_NOTHING_READS) or ("/" not in path and match(path, CI_NOTHING_READS_ROOT))


def git(*args: str, cwd: Path = ROOT, check: bool = True) -> str:
    r = subprocess.run(["git", *args], cwd=str(cwd), capture_output=True, text=True, errors="replace")
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
        r = subprocess.run(["git", "merge-base", "HEAD", ref], cwd=str(cwd), capture_output=True, text=True, errors="replace")
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
    builds: list[str] = field(default_factory=list)          # linux, unit-tests, web, android, navigation-off
    reasons: dict[str, str] = field(default_factory=dict)
    unit_all: bool = False
    unit_core: list[str] = field(default_factory=list)       # gtest suites
    unit_editor: list[str] = field(default_factory=list)
    unit_htmlui: list[str] = field(default_factory=list)
    htmlui_all: bool = False
    self_tests: list[str] = field(default_factory=list)      # tools' own Python tests
    lints: list[str] = field(default_factory=list)           # area lints: "<script> [--self-test]"
    unit_notes: list[str] = field(default_factory=list)
    mcp_all: bool = False
    mcp_ci: bool = False
    mcp_files: list[str] = field(default_factory=list)
    mcp_reasons: dict[str, str] = field(default_factory=dict)
    # why each was selected, by kind (REASON_KINDS); a unit suite's per test binary: suite -> (kind, why)
    mcp_kinds: dict[str, str] = field(default_factory=dict)
    unit_reasons: dict[str, dict[str, tuple[str, str]]] = field(default_factory=lambda: {"core": {}, "editor": {}, "htmlui": {}})
    mcp_notes: list[str] = field(default_factory=list)
    modules_off: str = ""                                    # the file that asks for MODULES_OFF_CHECK
    fallbacks: list[str] = field(default_factory=list)       # why a selection widened (--plan-json)

    def widen(self, notes: list[str], note: str) -> None:
        notes.append(note)
        self.fallbacks.append(note)

    def add_build(self, name: str, why: str) -> None:
        if name not in self.builds:
            self.builds.append(name)
            self.reasons[name] = why

    def add_mcp(self, name: str, why: str, kind: str = "dependency") -> None:
        if name not in self.mcp_files:
            self.mcp_files.append(name)
            self.mcp_reasons[name] = why
            self.mcp_kinds[name] = kind

    def add_suites(self, binary: str, suites, kind: str, why: str) -> None:
        for suite in suites:
            self.unit_reasons[binary].setdefault(suite, (kind, why))


def owner_test(path: str) -> str | None:
    """The test beside a tool's Python (tools/<dir>/test_<x>.py of tools/<dir>/<x>.py, or itself)."""
    if not (path.startswith("tools/") and path.endswith(".py")) or path.startswith("tools/mcp/tests/"):
        return None
    p = Path(path)
    test = p if p.name.startswith("test_") else p.with_name(f"test_{p.name}")
    rel = test.as_posix()
    return rel if (ROOT / rel).is_file() and rel not in SELF_TESTS_ELSEWHERE else None


def plan_builds(files: list[str], plan: Plan) -> None:
    for f in files:
        test = SELF_TESTS.get(f) or owner_test(f)
        if test and test not in plan.self_tests:
            plan.self_tests.append(test)
        if match(f, CI_SUITE_GUARD_TRIGGERS) and CI_SUITE_GUARD not in plan.self_tests:
            plan.self_tests.append(CI_SUITE_GUARD)
        if match(f, GPU_VENDOR_TRIGGERS) and GPU_VENDOR_LINT not in plan.self_tests:
            plan.self_tests.append(GPU_VENDOR_LINT)
    for lint, (reads, fixtures) in AREA_LINTS.items():
        if any(reads(f) or f == lint for f in files):
            plan.lints.append(lint)
        if fixtures is not None and any(f == lint or f.startswith(fixtures) for f in files):
            plan.lints.append(f"{lint} --self-test")
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
        plan.widen(plan.mcp_notes, f"{everything[0]} changed: the whole MCP suite")
        return
    cxx = [f for f in files if f.startswith(("engine/", "editor/", "libs/")) and (f.endswith(CXX) or "CMakeLists" in f
                                                                                 or f.endswith(".cmake"))]
    engine = [f for f in files if f.startswith("engine/")]
    editor = [f for f in files if f.startswith("editor/") or match(f, EDITOR_INPUTS)]
    if engine or editor:
        plan.add_build("linux", f"{(cxx or engine or editor)[0]} changed")
    shaders = [f for f in engine if f.startswith(("engine/shaders/", "engine/shaderlib/"))]
    if [f for f in shaders if f.startswith("engine/shaderlib/")] and SHADER_LINT_TEST not in plan.self_tests:
        plan.self_tests.append(SHADER_LINT_TEST)
    # (a test's data: a golden, a fixture; no dependency database sees it)
    test_data = [f for f in files if f.startswith(("engine/tests/", "editor/tests/")) and not f.endswith(CXX)
                 and not f.startswith("engine/tests/htmlui/")]
    if cxx or shaders or test_data:
        plan.add_build("unit-tests", f"{(cxx or shaders or test_data)[0]} changed")
    if test_data:
        plan.unit_all = True
        plan.widen(plan.unit_notes, f"{test_data[0]} changed (a unit test's data): every unit test")
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
    navigation = [f for f in files if match(f, NAVIGATION_OFF_PATHS)]
    if navigation:
        plan.add_build("navigation-off", f"{navigation[0]} changed (the engine without Recast/Detour: NavStubTest)")


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


def select_units(changed: list[str], deps: dict[str, list[str]], read=lambda p: (ROOT / p).read_text(errors="replace"),
                 why: dict[str, str] | None = None):
    """The test TUs of a tree that see the change, and their gtest suites; why (when given) gets each
    unit's changed path, or the sibling header of one, that it is or includes."""
    effective = set()
    for f in changed:
        effective.update(siblings(f))
    units = []
    for obj, paths in deps.items():
        if not paths or not is_test_source(paths[0]):
            continue
        if paths[0] in changed or effective.intersection(paths):
            units.append(paths[0])
            if why is not None:
                why.setdefault(paths[0], paths[0] if paths[0] in changed else next(p for p in paths if p in effective))
    suites = set()
    for unit in units:
        try:
            suites.update(gtest_suites(read(unit)))
        except OSError:
            pass
    return sorted(set(units)), sorted(suites)


CONSUMER_STEPS = 6   # include steps nearest_consumers walks before it gives up
# the vendored libraries: no walk through their insides (CI's compacted deps dumps keep only their sources)
VENDORED = ("engine/libs/", "editor/libs/", "libs/")


def nearest_consumers(path: str, deps: dict[str, list[str]]) -> tuple[list[str], dict[str, str], int]:
    """A changed source the tree links but no test unit includes: the test units that include its
    nearest consumers (a source of the tree that includes it, through that source's own header, then
    theirs, a step at a time until a test unit includes one), each with the consumer it includes, and
    the steps taken. Nothing when the tree does not compile or include it, or no chain reaches a test."""
    libs = [paths for paths in deps.values() if paths and not is_test_source(paths[0])]
    tests = [(paths[0], set(paths)) for paths in deps.values() if paths and is_test_source(paths[0])]
    seen = set(siblings(path))
    if not any(seen.intersection(paths) for paths in libs):
        return [], {}, 0
    frontier = set(seen)
    for step in range(1, CONSUMER_STEPS + 1):
        found = set()
        for paths in libs:
            if frontier.intersection(paths[1:]):
                found.update(s for s in siblings(paths[0]) if s not in seen)
        if not found:
            break
        seen |= found
        hits = {unit: min(found & included) for unit, included in tests if found & included}
        if hits:
            return sorted(hits), hits, step
        frontier = found
    return [], {}, 0


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


def dumped_deps(directory: Path, name: str, plan: Plan) -> str | None:
    """--deps-from: build/linux/<name>'s `ninja -t deps` as tools/ci/dump_deps.sh wrote it (None, and
    said to be, when the dump is missing or empty: as a tree without a database)."""
    try:
        text = (directory / f"{name}.deps").read_text(errors="replace")
    except OSError:
        text = ""
    if not text.strip():
        plan.widen(plan.unit_notes, f"--deps-from {directory} has no {name}.deps")
        return None
    return text


def plan_units(changes: Changes, plan: Plan, deps_core: str | None, deps_editor: str | None,
               deps_htmlui: str | None = None) -> None:
    if "htmlui-tests" in plan.builds:
        if deps_htmlui is None or [f for f in changes.files if match(f, HTMLUI_PATHS) and
                                   (f.endswith(("CMakeLists.txt", ".cmake")) or
                                    (f.startswith("engine/tests/htmlui/") and not f.endswith(CXX)))]:
            plan.htmlui_all = True
            plan.widen(plan.unit_notes, "HTML UI tests: all (no dependency database of their tree yet, their CMake or a test's data changed)")
        else:
            why: dict[str, str] = {}
            units, suites = select_units(_code(changes.files), parse_ninja_deps(deps_htmlui, HTMLUI_TREE), why=why)
            plan.unit_htmlui.extend(suites)
            record_units(plan, "htmlui", why)
            plan.unit_notes.append(f"htmlui: {len(units)} test unit(s) see the change")
    if plan.unit_all or "unit-tests" not in plan.builds:
        return
    cxx = _code(changes.files)
    unseen = [f for f in cxx if f.endswith(("CMakeLists.txt", ".cmake"))]
    if unseen:
        plan.unit_all = True
        plan.widen(plan.unit_notes, f"{unseen[0]} changed (CMake): every unit test")
        return
    if deps_core is None or deps_editor is None:
        plan.unit_all = True
        plan.widen(plan.unit_notes, "no dependency database of the test trees yet (their first build): every unit test")
        return
    for name, text, tree in (("core", deps_core, CORE_TREE), ("editor", deps_editor, EDITOR_TEST_TREE)):
        deps = parse_ninja_deps(text, tree)
        known = {p for paths in deps.values() for p in paths}
        why = {}
        units, suites = select_units(cxx, deps, why=why)
        # a new test source no database has yet: its suites run anyway
        for f in changes.files:
            if is_test_source(f) and f not in known and (ROOT / f).is_file() and not f.startswith("engine/tests/htmlui/") and \
                    f.startswith("engine/tests/" if name == "core" else "editor/tests/"):
                units.append(f)
                why[f] = f
                suites = sorted(set(suites) | set(gtest_suites((ROOT / f).read_text(errors="replace"))))
        # a linked source no test unit includes (a .cpp behind another one's header): its nearest consumers
        reached = set(why.values())
        for f in cxx:
            if not f.endswith(CXX) or is_test_source(f) or f in changes.deleted or f.startswith(VENDORED) \
                    or reached.intersection(siblings(f)):
                continue
            near, via, steps = nearest_consumers(f, deps)
            for unit in near:
                if unit in why:
                    continue
                why[unit] = via[unit]
                text = (ROOT / unit).read_text(errors="replace") if (ROOT / unit).is_file() else ""
                found = gtest_suites(text)
                suites = sorted(set(suites) | set(found))
                plan.add_suites(name, found, "dependency", f"{unit} includes {via[unit]}, the nearest consumer of {f} "
                                f"({steps} include step{'s' if steps > 1 else ''})")
            if near:
                units += [u for u in near if u not in units]
                plan.unit_notes.append(f"{name}: no test unit includes {f}: {len(near)} that include its nearest consumers "
                                       f"({steps} include step{'s' if steps > 1 else ''})")
        getattr(plan, f"unit_{name}").extend(suites)
        record_units(plan, name, why)
        if units:
            plan.unit_notes.append(f"{name}: {len(units)} test unit(s) see the change: {', '.join(units[:6])}"
                                   f"{' ...' if len(units) > 6 else ''}")
    # a shader is compiled when the test runs, no C++ unit includes it: the suites that draw
    if [f for f in changes.files if f.startswith(("engine/shaders/", "engine/shaderlib/"))]:
        drawing = gpu_suites(ROOT / "engine" / "tests" / "core")
        plan.unit_core = sorted(set(plan.unit_core) | set(drawing))
        plan.add_suites("core", drawing, "policy", "a shader changed: the suites that draw (*Gpu)")
        plan.unit_notes.append(f"a shader changed: the {len(drawing)} *Gpu suites of the core tests")
    new = [f for f in cxx if f.endswith(CXX) and not f.startswith(("engine/tests/", "editor/tests/"))
           and f not in changes.deleted and not any(f in p for p in (deps_core, deps_editor))]
    if new:
        plan.unit_notes.append(f"{len(new)} changed source(s) no test tree compiles or includes (e.g. {new[0]}): "
                               "their behaviour is the MCP tests' to cover")


def record_units(plan: Plan, binary: str, why: dict[str, str]) -> None:
    """The reason of each suite of the units select_units chose: its unit changed (owner), or includes
    what changed (dependency)."""
    for unit, path in sorted(why.items()):
        try:
            suites = gtest_suites((ROOT / unit).read_text(errors="replace"))
        except OSError:
            continue
        if path == unit:
            plan.add_suites(binary, suites, "owner", f"{unit} changed")
        else:
            plan.add_suites(binary, suites, "dependency", f"{unit} includes {path}")


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
        plan.widen(plan.mcp_notes, f"{harness[0]} changed (the harness): the whole MCP suite")
        return
    for f in files:
        name = Path(f).name
        if f.startswith("tools/mcp/tests/") and name.startswith("test_") and name.endswith(".py"):
            if f not in changes.deleted:
                plan.add_mcp(name, "the test file changed", "owner")
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
        plan.add_mcp("test_agent_docs.py", f"checks {agent_docs[0]}", "policy")
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
            plan.add_mcp("test_docs.py", f"the guide changed ({f})", "policy")
            hit = True
        if f.startswith(SHADER_DIRS) and "test_shader_library.py" in sources:
            # any shader: its compile matrix, and the CI suite for what draws with it
            plan.add_mcp("test_shader_library.py", f"compiles {f}", "policy")
            if not plan.mcp_ci:
                plan.fallbacks.append(f"a shader changed ({f}): the CI suite as well")
            plan.mcp_ci = True
            hit = True
        if not hit:
            unmapped_paths.append(f)
    if unmapped_paths:
        plan.mcp_ci = True
        plan.widen(plan.mcp_notes, f"{len(unmapped_paths)} changed file(s) no test file names (e.g. {unmapped_paths[0]}): the CI suite as well")
    compile_path = [f for f in files if match(f, COMPILE_PATHS)]
    for t in COMPILE_FILES if compile_path else ():
        if t in sources:
            plan.add_mcp(t, f"compiles a game ({compile_path[0]} changed)", "policy")
    saved_output = [f for f in files if match(f, SAVED_OUTPUT_PATHS)]
    for t in SAVED_OUTPUT_FILES if saved_output else ():
        if t in sources:
            plan.add_mcp(t, f"guards what a save writes ({saved_output[0]} changed)", "policy")
    # the engine's and the editor's C++: the coverage map
    # (the unit tests' own sources are no editor code: nothing an MCP test could run)
    code = [f for f in files if f.startswith(("engine/", "editor/", "libs/")) and f.endswith(CXX)
            and not f.startswith(("engine/tests/", "editor/tests/"))]
    if not code:
        return
    if cmap is None or "sources" not in cmap:
        plan.mcp_ci = True
        plan.widen(plan.mcp_notes, "no coverage map (tools/mcp/coverage_map.py): the CI suite for the C++ change")
        return
    if staleness:
        plan.mcp_ci = True
        plan.widen(plan.mcp_notes, f"{staleness}: the CI suite as well")
    select_by_map(changes, code, cmap, sources, plan)
    engine_runtime = [f for f in code if f.startswith("engine/")]
    if engine_runtime:
        for t in GAME_BUILD_FILES:
            if t in sources:
                plan.add_mcp(t, f"runs a game of the engine ({engine_runtime[0]} changed)", "policy")


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
                plan.add_mcp(t, f"a quick file that runs {f}'s common code", "smoke")
            plan.mcp_notes.append(f"{f}: code nearly every editor runs: the {COMMON_SAMPLE} quickest files of those that run it")
        if not names and not common:
            unrun.append(f)
    if unmapped:
        plan.mcp_ci = True
        plan.widen(plan.mcp_notes, f"{len(unmapped)} changed source(s) the map does not know (new; e.g. {unmapped[0]}): the CI suite as well")
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


def store(key: str, summary: str, cache_file: Path = CACHE_FILE, text: str = "", steps: list[dict] | None = None,
          plan: dict | None = None) -> None:
    """A pass: its summary, its plan (text and --json's) and its steps, which a hit reprints as cached."""
    cache_file.parent.mkdir(parents=True, exist_ok=True)
    tmp = cache_file.with_suffix(".tmp")
    tmp.write_text(json.dumps({"key": key, "passed": True, "summary": summary, "text": text or summary, "steps": steps or [],
                               "plan": plan or {}, "at": time.time()}, indent=1))
    os.replace(tmp, cache_file)


# ---------------------------------------------------------------------------------------- running

FAILURE_LINES = 40      # a failed step's parsed failures on the terminal; the rest is in its log
UNITTEST_FAIL = re.compile(r"^(?:FAIL|ERROR): (\w+) \(([^)]+)\)")
PYTEST_FAIL = re.compile(r"^FAILED (\S+::\S+)|^(\S+\.py::\S+) FAILED")
LINT_FINDING = re.compile(r"^[\w./+-]+:\d+[:) ]")
TEST_KINDS = ("unit", "htmlui", "mcp", "tool", "navigation")   # the steps that run tests (the red control's)
_REPORT = None


def ci_report():
    """tools/ci/report.py, whose parsers read a log as CI's failure report does (loaded by path:
    python3 -I keeps this script's directory off sys.path)."""
    global _REPORT
    if _REPORT is None:
        path = Path(__file__).resolve().parent / "ci" / "report.py"
        spec = importlib.util.spec_from_file_location("jasper_ci_report", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        _REPORT = module
    return _REPORT


def command_text(command: list[str], env: dict | None = None) -> str:
    """A command as the shell line that reruns it, its own environment first (this interpreter as python3)."""
    words = ["python3" if c == sys.executable else shlex.quote(c) for c in command]
    return " ".join([f"{k}={shlex.quote(v)}" for k, v in (env or {}).items()] + words)


def slug(label: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", label.lower()).strip("-") or "step"


def parse_failures(lines: list[str], log: Path, repro: str) -> tuple[list[str], list[str]]:
    """A failed step's output: its failures as terminal lines (at most FAILURE_LINES), and the names of
    the tests that failed. googletest, unittest and pytest results; build diagnostics, crashes,
    sanitizer reports and gate verdicts through tools/ci/report.py; else a lint's file:line findings;
    else the output's tail."""
    report = ci_report()
    clean = [report.clean_line(line) for line in lines]
    shown: list[str] = []
    tests: list[str] = []
    for run in report.gtest_runs(clean):
        if run["outcome"] == "failed":
            tests.append(run["name"])
            body = [l for l in clean[run["start"] + 1:run["end"]] if l.strip() and not report.END_RE.search(l)]
            shown += [f"FAILED {run['name']}"] + [f"    {report.relative_text(l.strip())}" for l in body[:6]]
        elif run["outcome"] == "unfinished" and run["died"]:
            tests.append(run["name"])   # (report.parse_log shows the crash)
    for i, line in enumerate(clean):
        m = UNITTEST_FAIL.match(line)
        if m:
            where = m.group(2).removeprefix("__main__.")
            name = where if where.endswith("." + m.group(1)) else f"{where}.{m.group(1)}"
            tests.append(name)
            end = next((k for k in range(i + 2, len(clean)) if clean[k].startswith(("=" * 20, "-" * 20))), len(clean))
            body = [l.strip() for l in clean[i + 2:end] if l.strip()]
            shown += [f"FAILED {name}"] + [f"    {l}" for l in body[-3:]]
            continue
        m = PYTEST_FAIL.match(line)
        if m:
            tests.append(m.group(1) or m.group(2))
            shown.append(line.strip())
    for f in report.parse_log(log, repro):
        shown.append(f["title"] + (f"  ({f['location']})" if f["location"] else ""))
        shown += [f"    {l}" for l in f["excerpt"].splitlines()[:4] if l.strip() and l.strip() != f["title"]]
    if not shown:
        shown = [l for l in clean if LINT_FINDING.match(l)]
    if not shown:
        shown = [l for l in clean if l.strip()][-15:] or ["(no output)"]
    if len(shown) > FAILURE_LINES:
        shown = shown[:FAILURE_LINES - 1] + [f"... {len(shown) - FAILURE_LINES + 1} more line(s): see the log"]
    return shown, list(dict.fromkeys(tests))


def narrowed(kind: str, command: list[str], env: dict | None, tests: list[str]) -> str:
    """The command that reruns a failed step, narrowed to the tests that failed (one binary's
    googletest tests, the MCP files)."""
    env = dict(env or {})
    if tests and kind == "unit" and "UNIT_SUITE" in env:
        env["UNIT_TEST_ARGS"] = "--gtest_filter=" + ":".join(tests)
    elif tests and kind == "htmlui":
        env["HTMLUI_TEST_ARGS"] = "--gtest_filter=" + ":".join(tests)
    elif tests and kind == "mcp":
        files = sorted({Path(t.split("::", 1)[0]).name for t in tests})
        command = [c for c in command if not c.startswith("tools/mcp/tests/")] + [f"tools/mcp/tests/{f}" for f in files]
    return command_text(command, env)


@dataclass
class Step:
    label: str
    status: str                    # ok | FAIL | skip | cached
    kind: str = "test"             # build | lint | tool | unit | htmlui | mcp | navigation | test
    seconds: float = 0.0
    log: str = ""                  # its whole output (repository-relative)
    fix: str = ""                  # the command that reruns it, narrowed to what failed
    reason: str = ""               # FAIL: the exit code or what could not run; skip: why
    blocked_by: str = ""           # skip: the step it needed, which failed or was left out
    failures: list[str] = field(default_factory=list)
    tests: list[str] = field(default_factory=list)   # the tests that failed

    def lines(self, status: str | None = None) -> list[str]:
        """Its summary line, `ok|FAIL|skip|cached  <step>  <seconds>`, and a failure's fix and log."""
        status = status or self.status
        out = [f"{status:<6}  {self.label}  {self.seconds:.0f}s" + (f"  ({self.reason})" if self.reason and status != "cached" else "")]
        if status == "FAIL":
            out.append(f"        fix: {self.fix}")
            if self.log:
                out.append(f"        log: {self.log}")
        return out


class Runner:
    """Runs the steps: each one's whole output in log_dir/<step>.log, a failed one's parsed failures on
    the terminal, a step whose prerequisite failed or was left out reported as skipped."""

    def __init__(self, verbose: bool, log_dir: Path, out=None):
        self.verbose = verbose
        self.log_dir = log_dir
        self.out = out or sys.stdout
        self.steps: list[Step] = []
        self.failed = False
        self.unrunnable: list[str] = []
        self.partial = False
        self.logs: set[str] = set()

    def say(self, text: str) -> None:
        print(text, file=self.out, flush=True)

    def step(self, label: str) -> Step | None:
        return next((s for s in reversed(self.steps) if s.label == label), None)

    def skip(self, label: str, reason: str, kind: str = "test", partial: bool = False, blocked_by: str = "") -> None:
        self.steps.append(Step(label, "skip", kind, reason=reason, blocked_by=blocked_by))
        self.partial |= partial
        self.say(f"[check-changed] {label}: skipped ({reason})")

    def fail(self, label: str, reason: str, fix: str, kind: str = "test") -> None:
        """A selected step that cannot run here."""
        self.steps.append(Step(label, "FAIL", kind, reason=reason, fix=fix))
        self.failed = True
        self.say(f"[check-changed] {label}: FAILED ({reason})")

    def log_path(self, label: str) -> Path:
        name = slug(label)
        n = 2
        while name in self.logs:
            name = f"{slug(label)}-{n}"
            n += 1
        self.logs.add(name)
        return self.log_dir / f"{name}.log"

    def run(self, label: str, command: list[str], env: dict | None = None, kind: str = "test", needs=()) -> bool:
        for need in needs:
            before = self.step(need)
            if before is not None and before.status not in ("ok", "cached"):
                self.skip(label, f"{need}: {'failed' if before.status == 'FAIL' else 'skipped'}", kind, blocked_by=need)
                return False
        fix = command_text(command, env)
        self.say(f"[check-changed] {label}: {fix}")
        self.log_dir.mkdir(parents=True, exist_ok=True)
        log = self.log_path(label)
        results = ROOT / "build" / "test-results" / "mcp.json"
        if kind == "mcp":
            results.unlink(missing_ok=True)   # (a stale one is no result of this run)
        t0 = time.monotonic()
        lines: list[str] = []
        with open(log, "w", encoding="utf-8", errors="replace") as f:
            f.write(f"$ {fix}\n")
            with subprocess.Popen(command, cwd=str(ROOT), env=dict(os.environ, **(env or {})), stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, errors="replace") as p:
                try:
                    for line in p.stdout:
                        f.write(line)
                        lines.append(line.rstrip("\n"))
                        if self.verbose:
                            self.out.write(line)
                            self.out.flush()
                    code = p.wait()
                except BaseException:
                    p.kill()
                    raise
        step = Step(label, "ok" if code == 0 else "FAIL", kind, time.monotonic() - t0, os.path.relpath(log, ROOT), fix,
                    "" if code == 0 else f"exit {code}")
        self.steps.append(step)
        if kind == "mcp":
            self.mcp_results(step, results)
        if step.status == "FAIL":
            self.failed = True
            failures, tests = parse_failures(lines, log, fix)
            step.failures = step.failures or failures
            step.tests = step.tests or tests
            step.fix = narrowed(kind, command, env, step.tests)
            self.say(f"[check-changed] {label} FAILED ({step.reason}, {step.seconds:.0f}s):")
            for line in step.failures:
                self.say(f"  {line}")
            self.say(f"  log: {step.log}")
        else:
            summary = [l for l in lines if re.search(r"\d+ passed|tests? ran|\[  PASSED  \]", l)]
            self.say(f"[check-changed] {label}: ok in {step.seconds:.0f}s{' (' + summary[-1].strip() + ')' if summary else ''}")
        return step.status == "ok"

    def mcp_results(self, step: Step, path: Path) -> None:
        """The MCP run's results: its failed tests, and a selected test that skipped, which could not
        run: a failure of the step (the full suite skips none on a build server)."""
        try:
            data = json.loads(path.read_text())
        except (OSError, ValueError):
            self.unrunnable.append(f"{step.label}: no results (build/test-results/mcp.json)")
            step.reason = "; ".join(r for r in (step.reason, "could not run: no results (build/test-results/mcp.json)") if r)
            step.status = "FAIL"
            return
        skipped = [r for r in data.get("results", []) if r.get("status") == "skip"]
        for r in skipped:
            self.unrunnable.append(f"{r['name']}: {r.get('detail', '')[:200]}")
        failed = [r for r in data.get("results", []) if r.get("status") == "fail"]
        crashed = [m.group(1) for m in (re.search(r"(test_[\w-]+\.py)", str(c)) for c in data.get("crashed") or []) if m]
        step.tests = [str(r.get("name", "?")) for r in failed] + crashed
        _, found, _ = ci_report().parse_mcp_json(path, step.fix)
        for f in found:
            step.failures += [f"FAILED {f['title']}"] + [f"    {l}" for l in f["excerpt"].splitlines()[-4:] if l.strip()]
        if len(step.failures) > FAILURE_LINES:
            step.failures = step.failures[:FAILURE_LINES - 1] + [f"... {len(step.failures) - FAILURE_LINES + 1} more line(s): see the log"]
        if skipped and step.status == "ok":
            step.status, step.reason = "FAIL", f"could not run: {len(skipped)} selected test(s) skipped"
            step.failures = [f"skipped {r['name']}: {r.get('detail', '')[:200]}" for r in skipped][:FAILURE_LINES]


def run_steps(runner: Runner, plan: Plan, changes: Changes, args, finish_plan=None, red: bool = False) -> None:
    """The plan's steps. Independent ones go on after a failure; one whose build failed is skipped.
    finish_plan: the unit and MCP selection, once the unit test trees are built (their dependency
    database). red: the red control's rerun, only what builds and runs the selected tests."""
    build_env = {"WERROR": "1"}
    # 1. the area lints: seconds, and a finding needs no build
    for lint in plan.lints if not red else ():
        if (ROOT / lint.split()[0]).is_file():   # (a checkout without it: an older branch's, --root)
            runner.run(f"lint {lint}", [sys.executable, "-I", *lint.split()], kind="lint")
    unit_selected = plan.unit_all or plan.unit_core or plan.unit_editor
    # 2. builds (the unit test trees first: their dependency database picks the suites)
    if "unit-tests" in plan.builds and (not red or unit_selected):
        runner.run("build the unit tests", ["tools/build.sh", "unit-tests"], dict(build_env, RUN="0"), "build")
    if "htmlui-tests" in plan.builds and (not red or plan.htmlui_all or plan.unit_htmlui):
        runner.run("build the HTML UI tests", ["tools/build.sh", "htmlui-tests"], dict(build_env, RUN="0"), "build")
    if finish_plan:
        finish_plan()
    for test in plan.self_tests:
        if (ROOT / test).is_file():
            runner.run(f"tool tests ({test})", [sys.executable, "-I", test], kind="tool")
    mcp_selected = plan.mcp_all or plan.mcp_ci or plan.mcp_files
    for b in plan.builds:
        if b in ("unit-tests", "htmlui-tests"):
            continue
        if red and b != "navigation-off" and not (b == "linux" and mcp_selected and not args.no_mcp):
            continue   # (no selected test runs in it)
        if b in ("web", "android") and in_image():
            # (their images are not this one: the check goes on, its pass is not cached, and the summary says so)
            runner.skip(f"build {b}", "not in the Linux image: tools/check-changed outside it builds it", "build", partial=True)
            continue
        if b == "navigation-off":
            # (a script of the Linux image, not a build.sh target)
            script = ["tools/check-navigation-off.sh"]
            runner.run("navigation OFF (NavStubTest, NavGridTest)", script if in_image() else ["tools/build.sh", "exec", "linux", *script],
                       kind="navigation")
            continue
        env = dict(build_env, SKIP_ENGINE="1") if b == "linux" else dict(build_env)
        runner.run(f"build {b}", ["tools/build.sh", b], env, "build")
    # the header gates: they read the Linux editor's tree, whose paths are the image's
    # (a checkout without them: an older branch's, --root)
    in_tree = (lambda *c: list(c)) if in_image() else (lambda *c: ["tools/build.sh", "exec", "linux", *c])
    if not red and "linux" in plan.builds and (ROOT / "tools/check_fanout.py").is_file():
        headers = sorted(f for f in changes.files if f.endswith(HEADER) and f not in changes.deleted)
        if headers:
            runner.run(f"headers compile alone ({len(headers)} edited)", in_tree("python3", "-I", "tools/check_headers.py", *headers),
                       kind="build", needs=("build linux",))
        runner.run("header fan-out limits", in_tree("python3", "-I", "tools/check_fanout.py"), kind="lint", needs=("build linux",))
    if not red and plan.modules_off and (ROOT / MODULES_OFF_CHECK).is_file():
        jobs = os.environ["JOBS"] if os.environ.get("JOBS", "").isdigit() else str(os.cpu_count() or 4)
        runner.run("the engine with every module off", in_tree("env", "WERROR=1", "bash", MODULES_OFF_CHECK, "-j", jobs, "all-off"),
                   kind="build")
    # 3. unit tests
    unit_build = ("build the unit tests",)
    if "unit-tests" in plan.builds:
        if plan.unit_all:
            runner.run("unit tests (all)", ["tools/build.sh", "unit-tests"], build_env, "unit", unit_build)
        else:
            for suite, names in (("core", plan.unit_core), ("editor", plan.unit_editor)):
                if names:
                    runner.run(f"unit tests ({suite}: {len(names)} suite(s))", ["tools/build.sh", "unit-tests"],
                               dict(build_env, UNIT_SUITE=suite, UNIT_TEST_ARGS=f"--gtest_filter={suite_filter(names)}"),
                               "unit", unit_build)
    if "htmlui-tests" in plan.builds:
        htmlui_build = ("build the HTML UI tests",)
        if plan.htmlui_all:
            runner.run("HTML UI tests (all)", ["tools/build.sh", "htmlui-tests"], build_env, "htmlui", htmlui_build)
        elif plan.unit_htmlui:
            runner.run(f"HTML UI tests ({len(plan.unit_htmlui)} suite(s))", ["tools/build.sh", "htmlui-tests"],
                       dict(build_env, HTMLUI_TEST_ARGS=f"--gtest_filter={suite_filter(plan.unit_htmlui)}"), "htmlui", htmlui_build)
    # 4. MCP tests
    if args.no_mcp and mcp_selected:
        runner.skip("MCP tests", "--no-mcp", "mcp", partial=True)
        return
    if not mcp_selected:
        return
    editor = "build linux"
    if "linux" not in plan.builds and not (ROOT / "build/linux/editor-install/bin/jasperengine-editor").exists():
        editor = "build linux (the MCP tests' editor)"
        runner.run(editor, ["tools/build.sh", "linux"], dict(build_env, SKIP_ENGINE="1"), "build")
    needs = (editor,) if runner.step(editor) else ()
    # (the container's CPUs, rb.sh's JOBS, for two each: LP_NUM_THREADS 2, docs/testing.md)
    cpus = int(os.environ["JOBS"]) if os.environ.get("JOBS", "").isdigit() else (os.cpu_count() or 4)
    jobs = args.j or max(2, cpus // 2)
    # (a runner without --split-seconds: an older checkout's, --root)
    runner_source = ROOT / "tools/mcp/run_tests.py"
    split = ["--split-seconds", "600"] if runner_source.is_file() and "--split-seconds" in runner_source.read_text() else []
    mcp_env = {"JASPER_SLOW_BUILD": "1", "JASPER_SLOW_TESTS": "1", "MCP_SUITE": "all"}
    if plan.mcp_all:
        runner.run("MCP tests (the whole suite)", ["tools/build.sh", "mcp-tests", "-j", str(jobs), *split], mcp_env, "mcp", needs)
        return
    if plan.mcp_ci:
        runner.run("MCP tests (the CI suite)", ["tools/build.sh", "mcp-tests", "-j", str(jobs)],
                   {"MCP_SUITE": "ci", "JASPER_SLOW_BUILD": "1", "JASPER_SLOW_TESTS": "1"}, "mcp", needs)
    files = [f for f in plan.mcp_files if (MCP_TESTS / f).is_file()]
    missing = [f for f in plan.mcp_files if f not in files]
    if plan.mcp_ci:   # (the CI suite above ran those)
        files = [f for f in files if not re.search(r"^CI = True", (MCP_TESTS / f).read_text(errors="replace"), re.M)]
    if missing:
        runner.fail("MCP tests (selected, not in the checkout)", f"cannot run: {missing}", "git status", "mcp")
    if not files:
        return
    if "test_mesh_raycast.py" not in files:
        mcp_env["MCP_SKIP_WEB_BUILD"] = "1"
    elif in_image():
        # (its latency gate loads a web build, which this image cannot make)
        runner.unrunnable.append("test_mesh_raycast.py: needs the web image: run on the host")
        runner.fail("MCP tests (test_mesh_raycast.py)", "cannot run: needs the web image",
                    "tools/build.sh mcp-tests tools/mcp/tests/test_mesh_raycast.py   # on the host", "mcp")
        files = [f for f in files if f != "test_mesh_raycast.py"]
        mcp_env["MCP_SKIP_WEB_BUILD"] = "1"
    if files:
        runner.run(f"MCP tests ({len(files)} file(s))", ["tools/build.sh", "mcp-tests", "-j", str(jobs),
                   *split, *[f"tools/mcp/tests/{f}" for f in files]], mcp_env, "mcp", needs)


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
    if plan.lints:
        lines.append("lints: " + ", ".join(plan.lints))
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
        r = subprocess.run(["git", "rev-list", "--count", f"{commit}..HEAD"], cwd=str(root), capture_output=True, text=True, errors="replace")
        return int(r.stdout.strip()) if r.returncode == 0 and r.stdout.strip().isdigit() else None
    return count


def use_root(root: Path) -> None:
    """The checkout the run works in (tools/test_check_changed.py points it at a scratch repository)."""
    global ROOT, CACHE_FILE, RB_CHANGES, MCP_TESTS, CORE_TREE, EDITOR_TEST_TREE, EDITOR_TREE, HTMLUI_TREE, LOG_DIR, RED_DIR
    ROOT = root
    CACHE_FILE = root / "build" / ".cache" / "check-changed.json"
    LOG_DIR = root / "build" / "check-changed"
    RED_DIR = root / "build" / "red-control"
    RB_CHANGES = root / "build" / ".rb-changes.json"
    MCP_TESTS = root / "tools" / "mcp" / "tests"
    CORE_TREE = root / "build" / "linux" / "core-tests"
    EDITOR_TEST_TREE = root / "build" / "linux" / "editor-tests"
    EDITOR_TREE = root / "build" / "linux" / "editor"
    HTMLUI_TREE = root / "build" / "linux" / "htmlui-tests-glcore"


def plan_document(changes: Changes, plan: Plan, cmap_info: str, deps_from: Path | None) -> dict:
    """--plan-json: the plan as data (tools/ci/plan_lanes.py turns it into CI lanes)."""
    def suites(all_: bool, names: list[str]) -> str | list[str]:
        return "all" if all_ else sorted(names)
    unit_built = "unit-tests" in plan.builds
    unit = {"core": suites(plan.unit_all, plan.unit_core) if unit_built else [],
            "editor": suites(plan.unit_all, plan.unit_editor) if unit_built else [],
            "htmlui": suites(plan.htmlui_all, plan.unit_htmlui) if "htmlui-tests" in plan.builds else []}
    selected = [{"file": f, "reason": plan.mcp_reasons[f], "kind": plan.mcp_kinds.get(f, "dependency")} for f in plan.mcp_files]
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(ROOT), capture_output=True, text=True, errors="replace")
    return {
        "base": changes.base, "base_ref": changes.base_ref, "head": head.stdout.strip() if head.returncode == 0 else "",
        "source": changes.source, "files": changes.files, "deleted": changes.deleted, "untracked": changes.untracked,
        "builds": plan.builds, "build_reasons": plan.reasons,
        "unit": "all" if all(v == "all" for v in unit.values()) else unit,
        "mcp": "all" if plan.mcp_all else "ci" if plan.mcp_ci else selected,
        "mcp_selected": selected,
        # suite -> [kind, why] per test binary (REASON_KINDS)
        "unit_reasons": {b: {s: list(r) for s, r in sorted(plan.unit_reasons[b].items())} for b in ("core", "editor", "htmlui")},
        "self_tests": plan.self_tests, "lints": plan.lints, "modules_off": plan.modules_off, "navigation_off": "navigation-off" in plan.builds,
        "fallbacks": plan.fallbacks, "notes": plan.unit_notes + plan.mcp_notes,
        "map": cmap_info, "deps_from": str(deps_from) if deps_from else None,
    }




# ---------------------------------------------------------------------------------------- red control

# what the red control keeps: the tests and their data (the tools' own tests and fixtures too, so a
# tool's change has one); everything else the branch changed it reverts
RED_CONTROL_KEEPS = ("engine/tests/*", "editor/tests/*", "tools/mcp/tests/*", "tools/fixtures/*", "docs/*", "*/golden/*",
                     "*/goldens/*", "tools/*test_*.py", "tools/tests/*", "tools/*/tests/*", "tools/*/fixtures/*")
RESTORE = "tools/check-changed --red-control --restore"


def marker_file() -> Path:
    """Written before the revert, removed once the tree is verified restored: a run killed in between
    leaves it, and every later run refuses until --restore."""
    return RED_DIR / "in-progress.json"


def tree_state(root: Path) -> str:
    """The working tree as git sees it: its status (untracked files too), its diff against HEAD and
    the untracked files' bytes."""
    h = hashlib.sha256()
    for args in (["status", "--porcelain=v1", "-z", "--untracked-files=all"], ["diff", "--binary", "HEAD"]):
        h.update(subprocess.run(["git", *args], cwd=str(root), capture_output=True, check=True).stdout + b"\0")
    for name in git("ls-files", "--others", "--exclude-standard", "-z", cwd=root).split("\0"):
        if name and (root / name).is_file():
            h.update(name.encode() + b"\0" + (root / name).read_bytes())
    return h.hexdigest()


def dirty_paths(root: Path) -> list[str]:
    return [l[3:] for l in git("status", "--porcelain=v1", "--untracked-files=all", cwd=root).splitlines() if l.strip()]


def restore_paths(head: str, paths: list[str], root: Path) -> None:
    """The reverted paths as HEAD has them: checked out where HEAD has them, removed where it has none
    (a file the branch deleted, which the revert brought back)."""
    in_head: set[str] = set()
    for i in range(0, len(paths), 200):
        chunk = paths[i:i + 200]
        in_head.update(git("ls-tree", "-r", "--name-only", head, "--", *chunk, cwd=root).splitlines())
    present = [p for p in paths if p in in_head]
    for i in range(0, len(present), 200):
        git("checkout", head, "--", *present[i:i + 200], cwd=root)
    for p in paths:
        if p in in_head:
            continue
        target = root / p
        if target.is_file() or target.is_symlink():
            target.unlink()
        parent = target.parent
        while parent != root and parent.is_dir() and not any(parent.iterdir()):
            parent.rmdir()
            parent = parent.parent


def restore_command(out) -> int:
    """--restore: the tree an interrupted red control left reverted, back as it was."""
    marker = marker_file()
    if not marker.is_file():
        print("check-changed: nothing to restore (no interrupted --red-control)", file=out, flush=True)
        return 0
    data = json.loads(marker.read_text())
    head = git("rev-parse", "HEAD", cwd=ROOT).strip()
    if head != data["head"]:
        print(f"check-changed: HEAD moved since the interrupted red control (it ran on {data['head'][:12]}): "
              f"`git checkout {data['head']}`, then `{RESTORE}`", file=out, flush=True)
        return 1
    restore_paths(head, data["paths"], ROOT)
    if tree_state(ROOT) != data["state"]:
        print(f"check-changed: the {len(data['paths'])} reverted path(s) are restored, but the tree differs from before the red "
              f"control elsewhere (`git status`); when it is as you want it, delete {os.path.relpath(marker, ROOT)}",
              file=out, flush=True)
        return 2
    marker.unlink()
    print(f"check-changed: restored: the tree is byte-identical to before the red control ({len(data['paths'])} path(s))",
          file=out, flush=True)
    return 0


def interrupt(signum, _frame):
    raise KeyboardInterrupt(f"signal {signum}")


def red_control(plan: Plan, changes: Changes, args, verbose: bool, out) -> tuple[int, dict]:
    """--red-control after a pass: the branch's change outside the tests reverted in place, the same
    selected tests rebuilt and rerun, the tree restored (finally) and verified byte-identical. Exit 1
    when no selected test went red."""
    def say(text: str) -> None:
        print(text, file=out, flush=True)
    patch = RED_DIR / "patch.diff"
    head = git("rev-parse", "HEAD", cwd=ROOT).strip()
    changed = [p for p in git("diff", "--name-only", "--no-renames", changes.base, "HEAD", cwd=ROOT).splitlines() if p]
    reverted = [p for p in changed if not match(p, RED_CONTROL_KEEPS)]
    doc: dict = {"patch": os.path.relpath(patch, ROOT), "reverted": reverted, "steps": [], "red": 0, "restored": None}
    if not reverted:
        say(f"red control: the branch changes no path outside the tests ({len(changed)} path(s)): nothing to revert")
        return 1, doc
    RED_DIR.mkdir(parents=True, exist_ok=True)
    diff = subprocess.run(["git", "diff", "--binary", "--no-renames", changes.base, "HEAD", "--", *reverted], cwd=str(ROOT),
                          capture_output=True, check=True).stdout
    patch.write_bytes(diff)
    before = tree_state(ROOT)
    marker = marker_file()
    tmp = marker.with_suffix(".tmp")
    tmp.write_text(json.dumps({"head": head, "base": changes.base, "paths": reverted, "state": before, "pid": os.getpid(),
                               "patch": doc["patch"], "at": time.time()}, indent=1))
    os.replace(tmp, marker)
    say(f"\n== red control: the change outside the tests reverted in place ({len(reverted)} path(s), {doc['patch']}); "
        f"rebuilding and rerunning the same selected tests")
    runner = Runner(verbose, RED_DIR / "logs", out)
    previous = {s: signal.signal(s, interrupt) for s in (signal.SIGTERM, signal.SIGHUP)}
    try:
        r = subprocess.run(["git", "apply", "-R", "--binary", "--whitespace=nowarn", str(patch)], cwd=str(ROOT),
                           capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            say(f"red control: the patch does not reverse-apply ({r.stderr.strip()[:500]}): nothing was reverted")
        else:
            run_steps(runner, plan, changes, args, red=True)
    finally:
        restore_paths(head, reverted, ROOT)
        doc["restored"] = tree_state(ROOT) == before
        if doc["restored"]:
            marker.unlink()
        for s, handler in previous.items():
            signal.signal(s, handler)
    if not doc["restored"]:
        say(f"red control: the tree is NOT as it was before (`git status`); {os.path.relpath(marker, ROOT)} stays: `{RESTORE}`")
        return 2, doc
    if r.returncode != 0:
        return 1, doc
    rows = []
    for step in runner.steps:
        if step.kind not in TEST_KINDS:
            continue
        blocker = runner.step(step.blocked_by) if step.blocked_by else None
        if step.status == "skip":
            verdict = "red (build)" if blocker is not None and blocker.status == "FAIL" else "not run"
        elif step.status == "FAIL":
            verdict = "not run" if "could not run" in step.reason or step.reason.startswith("cannot run") else "red"
        else:
            verdict = "green"
        rows.append({"step": step.label, "verdict": verdict, "tests": step.tests, "log": step.log, "reason": step.reason})
    doc["steps"] = rows
    doc["red"] = sum(1 for row in rows if row["verdict"].startswith("red"))
    say("\n== red control: the selected tests without the change")
    for row in rows:
        detail = {"red": ": " + ", ".join(row["tests"][:20]) if row["tests"] else "",
                  "green": ": passes without the change, so it does not pin it",
                  "not run": f": {row['reason']}" if row["reason"] else ""}.get(row["verdict"], "")
        say(f"{row['verdict']:<12} {row['step']}{detail}")
    if not rows:
        say("(no selected test: nothing can go red)")
    say(f"the tree is restored: byte-identical to before (status and diff); the patch stays in {doc['patch']}")
    if not doc["red"]:
        say("red control FAILED: no selected test failed without the change: they do not pin it")
        return 1, doc
    say(f"red control: {doc['red']} of {len(rows)} selected test step(s) went red without the change (never cached)")
    return 0, doc


def run_document(plan_doc: dict, steps: list[Step], runner: Runner | None, exit_code: int, seconds: float,
                 cached_pass: bool = False, red: dict | None = None) -> dict:
    """--json: the plan, every step's result, log and fix command, and the red control's verdicts."""
    status = "cached" if cached_pass else None
    return {"plan": plan_doc,
            "steps": [dict(name=s.label, status=status or s.status, kind=s.kind, seconds=round(s.seconds, 1), log=s.log, fix=s.fix,
                           reason=s.reason, failures=s.failures, tests=s.tests) for s in steps],
            "could_not_run": runner.unrunnable if runner else [], "partial": bool(runner and runner.partial),
            "cached": cached_pass, "passed": not (runner and runner.failed) and (exit_code == 0 or red is not None), "exit": exit_code,
            "seconds": round(seconds, 1), "red_control": red}


def main(argv: list[str], root: Path | None = None) -> int:
    if root is not None:
        use_root(root)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", help="the ref to diff against (default: the merge base with oe-port, main, origin/main)")
    ap.add_argument("--dry-run", action="store_true", help="print the plan only")
    ap.add_argument("--no-cache", action="store_true", help="run even when the same inputs passed before")
    ap.add_argument("-j", type=int, default=0, help="MCP editors at once (default: the CPUs / 2, at least 2)")
    ap.add_argument("--no-mcp", action="store_true", help="leave the MCP tests out, and say so: for a change that selects "
                    "the whole suite, which the integrator runs once per wave (the pass is not cached)")
    ap.add_argument("--json", action="store_true", help="print the plan, the step results, their logs and fix commands as "
                    "one JSON document on stdout (the progress goes to stderr)")
    ap.add_argument("--red-control", action="store_true", help="after a pass (or its cached one), revert the branch's change "
                    "outside the tests in place, rerun the same selected tests and restore the tree: exit 1 when none "
                    "fails without the change. Needs a clean tree")
    ap.add_argument("--restore", action="store_true", help="with --red-control: restore the tree an interrupted one left reverted")
    ap.add_argument("--list-mcp", type=Path, help="write the selected MCP test files here (one per line)")
    ap.add_argument("--root", type=Path, help="check this checkout instead of the one holding this script "
                    "(e.g. an older branch's worktree, with this check-changed)")
    ap.add_argument("--plan-json", type=Path, help="write the plan here as JSON and run nothing (implies --dry-run; "
                    "exit 0 on any plan, 2 when there is none)")
    ap.add_argument("--deps-from", type=Path, help="read the test trees' `ninja -t deps` from <name>.deps files here "
                    "(core-tests, editor-tests, htmlui-tests: tools/ci/dump_deps.sh) instead of asking Ninja")
    args = ap.parse_args(argv)
    if args.plan_json:
        args.dry_run = True
    if args.restore and not args.red_control:
        ap.error("--restore goes with --red-control")
    if args.red_control and args.dry_run:
        ap.error("--red-control runs the tests: not with --dry-run or --plan-json")
    if args.root is not None:
        use_root(args.root.resolve())
    verbose = os.environ.get("CHECK_CHANGED_VERBOSE", "") not in ("", "0")
    out = sys.stderr if args.json else sys.stdout

    def say(text: str) -> None:
        print(text, file=out, flush=True)
    t0 = time.monotonic()

    if args.restore:
        return restore_command(out)
    if marker_file().is_file():
        say(f"check-changed: an interrupted --red-control left the change outside the tests reverted "
            f"({os.path.relpath(marker_file(), ROOT)}): restore the tree first: {RESTORE}")
        return 1
    if args.red_control:
        dirty = dirty_paths(ROOT)
        if dirty:
            say(f"check-changed: --red-control reverts the tree in place and needs it clean: {len(dirty)} uncommitted "
                f"path(s) (e.g. {dirty[0]}): commit them first")
            return 1
    try:
        changes = read_changes(args.base, ROOT, RB_CHANGES)
    except (LookupError, RuntimeError) as e:
        print(f"check-changed: {e}", file=sys.stderr)
        return 2 if args.plan_json else 1
    if args.red_control and changes.source != "git":
        say("check-changed: --red-control needs the branch's history (this diff came through rb.sh)")
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
    planned = {"text": ""}

    def finish_plan() -> None:
        """The unit and MCP selection (the unit test trees' dependency database: after their build)."""
        staleness = map_staleness(cmap, time.time(), commits_since(ROOT)) if cmap else None
        if args.deps_from:
            deps = [dumped_deps(args.deps_from, name, plan) if name != "htmlui-tests" or "htmlui-tests" in plan.builds else None
                    for name in DEPS_DUMPS]
            plan_units(changes, plan, *deps)
        else:
            plan_units(changes, plan, ninja_deps(CORE_TREE), ninja_deps(EDITOR_TEST_TREE),
                       ninja_deps(HTMLUI_TREE) if "htmlui-tests" in plan.builds else None)
        plan_mcp(changes, plan, cmap, test_files(MCP_TESTS), staleness)
        planned["text"] = describe(changes, plan, cmap_info)
        say(planned["text"])
        if args.list_mcp:
            args.list_mcp.write_text("\n".join(plan.mcp_files) + "\n")
        if args.plan_json:
            args.plan_json.parent.mkdir(parents=True, exist_ok=True)
            args.plan_json.write_text(json.dumps(plan_document(changes, plan, cmap_info, args.deps_from), indent=1) + "\n")

    def emit(document: dict) -> None:
        if args.json:
            print(json.dumps(document, indent=1), flush=True)

    hit = None if args.no_cache or args.dry_run else cached(key, CACHE_FILE)
    if args.dry_run:
        finish_plan()
        say(GAP)
        emit(run_document(plan_document(changes, plan, cmap_info, args.deps_from), [], None, 0, time.monotonic() - t0))
        return 0
    runner = Runner(verbose, LOG_DIR, out)
    if hit:
        steps = [Step(**s) for s in hit.get("steps", [])]
        say(hit["text"])
        say(GAP)
        for step in steps:
            for line in step.lines("cached"):
                say(line)
        say("[check-changed] the same inputs passed before (build/.cache/check-changed.json); --no-cache runs again")
        if not args.red_control:
            emit(run_document(hit.get("plan", {}), steps, None, 0, time.monotonic() - t0, cached_pass=True))
            return 0
        finish_plan()   # (the trees the pass built: the same selection)
    else:
        for old in LOG_DIR.glob("*.log"):
            old.unlink()
        run_steps(runner, plan, changes, args, finish_plan)
        steps = runner.steps
        wall = time.monotonic() - t0
        lines = ["", "== check-changed summary", planned["text"], GAP]
        lines += [f"  could not run: {s}" for s in runner.unrunnable]
        for step in steps:
            lines += step.lines()
        if not steps:
            lines.append("nothing to build or test")
        lines.append(f"{'FAILED' if runner.failed else 'passed'} in {wall:.0f}s")
        summary = "\n".join(lines)
        say(summary)
        if runner.partial:
            say("[check-changed] a step was left out (above): the pass is not cached")
        elif not runner.failed:
            if inputs_hash(changes, cmap_version, options, ROOT) == key and \
                    read_changes(args.base, ROOT, RB_CHANGES).files == changes.files:
                store(key, summary, CACHE_FILE, planned["text"], [asdict(s) for s in steps],
                      plan_document(changes, plan, cmap_info, args.deps_from))
            else:
                say("[check-changed] files changed during the run: the pass is not cached")
        if runner.failed or not args.red_control:
            code = 1 if runner.failed else 0
            if args.red_control:
                say("red control: not run: the change's own selection failed (above); fix that first")
            emit(run_document(plan_document(changes, plan, cmap_info, args.deps_from), steps, runner, code, wall))
            return code
    try:
        code, red = red_control(plan, changes, args, verbose, out)
    except KeyboardInterrupt:
        say(f"red control: interrupted{'; the tree is restored' if not marker_file().is_file() else ': ' + RESTORE}")
        return 130
    emit(run_document(plan_document(changes, plan, cmap_info, args.deps_from), steps, runner, code, time.monotonic() - t0,
                      cached_pass=bool(hit), red=red))
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
