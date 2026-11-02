"""tools/check-changed: the per-commit gate of a stream. Builds what the change touches and runs the
unit and MCP tests that can see it; the full suite stays the gate of batch landings and of main.

    tools/check-changed                 # on a build server: rb.sh <S> tools/check-changed
    tools/check-changed --dry-run       # the plan only
    tools/check-changed --base oe-port  # the diff against another base (default: the merge base
                                        # with oe-port, else main, else origin/main)
    tools/check-changed --no-cache      # run even when the same inputs passed before
    CHECK_CHANGED_VERBOSE=1 tools/check-changed   # every step's output, not only a failed one's

1. The changed files: committed (since the merge base), staged, unstaged and untracked. Through
   rb.sh the server's worktree has no history of the stream: rb.sh writes the local diff to
   build/.rb-changes.json, which this reads when its own git cannot name the base.
2. The builds: the Linux editor (SKIP_ENGINE=1 tools/build.sh linux) and the unit test binaries for
   C++ under engine/ or editor/; web for engine/ (and its CMake), Android for its own platform code;
   everything for the root CMake, docker/, tools/build.sh, the test harness, the toolchain and the
   lockfiles (ALL_TRIGGERS); nothing for a change that builds nothing (docs).
3. Unit tests, exactly: Ninja's dependency database of the test trees (`ninja -t deps`) maps every
   changed file (a changed Foo.cpp through its Foo.h too) to the test translation units that
   include it; their gtest suites run (--gtest_filter), in the core and the editor binary. A
   changed test file always runs. A change the database cannot see (CMake, a new file) runs all.
4. MCP tests, from a coverage map (tools/mcp/coverage_map.py: which sources each test file's editors
   executed, recorded by a full run of a coverage-instrumented editor): the files whose sources
   intersect the change (headers through the editor tree's dependency database), the changed test
   files, the tests of a changed MCP tool (by its tool names), of a changed fixture, Lua, shader or
   asset (by path), and the files that build games when the engine changes. Fallbacks: no map, a
   map older than MAP_MAX_AGE_DAYS or MAP_MAX_COMMITS, or a changed source the map does not know
   (new, or no test file ran it) add the CI suite; a harness change runs the whole suite.
5. A pass is cached (build/.cache/check-changed.json) by a hash of the base, the diff, the
   untracked files, the build images' Dockerfiles, the map's version and this script; the same
   inputs reprint the last summary. A run during which the files changed caches nothing.
6. Exit 1 when a step failed or a selected test could not run.
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
MAP_MAX_AGE_DAYS = 14
MAP_MAX_COMMITS = 400
GAP = ("coverage gap: this is the change's own selection; the full gate is still the full suite "
       "(batch landings and before main)")

# a change here builds and runs everything
ALL_TRIGGERS = ("CMakeLists.txt", "CMakePresets.json", "CopyEditorEngine.cmake", "docker/*", "tools/build.sh",
                "tools/with-xvfb.sh", "tools/ci/gtest-gate.py", "tools/mcp/requirements.txt", "engine/cmake/*",
                "*.lock", "uv.lock", "package-lock.json", "pnpm-lock.yaml", "tools/check_changed.py",
                "tools/check-changed")
# a change here runs the whole MCP suite (the harness every test goes through)
MCP_ALL_TRIGGERS = ("tools/mcp/harness.py", "tools/mcp/run_tests.py", "tools/mcp/run-tests.sh",
                    "tools/mcp/build_cache.py", "tools/mcp/requirements.txt")
# C/C++ and what the engine's builds read
CXX = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".mm", ".m")
HEADER = (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp")
BUILDS_NOTHING = ("docs/*", "*.md", "LICENSE", "NOTICE.md", ".claude/*", ".github/*")
WEB_DIRS = ("engine/platform/web/*", "engine/platform/emscripten/*", "tools/web/*")
ANDROID_DIRS = ("engine/platform/android/*", "engine/project/android/*", "tools/android-*", "tools/ci/android_*")
# the MCP files that build games (a prebuilt player or a compile): the engine's runtime code runs in
# the games they build, which no editor coverage sees
GAME_BUILD_FILES = ("test_build.py", "test_html_ui_effects_export.py", "test_hub.py", "test_busy.py",
                    "test_html_ui_export.py", "test_html_ui_video_export.py", "test_call_cli.py")
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
    return Changes(base, how, sorted(set(files + untracked)), sorted(deleted), sorted(untracked))


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
                           sorted(data.get("deleted", [])), sorted(data.get("untracked", [])), "rb.sh")
        try:
            return git_changes(explicit_base, cwd)
        except (LookupError, RuntimeError):
            raise LookupError(f"{rb_file} is not this checkout's diff ({len(stale)} file(s) differ, e.g. {stale[0]}): "
                              "run tools/check-changed through rb.sh, which sends it") from None
    return git_changes(explicit_base, cwd)


# ---------------------------------------------------------------------------------------- 2. the builds

@dataclass
class Plan:
    builds: list[str] = field(default_factory=list)          # linux, unit-tests, web, android, everything
    reasons: dict[str, str] = field(default_factory=dict)
    unit_all: bool = False
    unit_core: list[str] = field(default_factory=list)       # gtest suites
    unit_editor: list[str] = field(default_factory=list)
    unit_notes: list[str] = field(default_factory=list)
    mcp_all: bool = False
    mcp_ci: bool = False
    mcp_files: list[str] = field(default_factory=list)
    mcp_reasons: dict[str, str] = field(default_factory=dict)
    mcp_notes: list[str] = field(default_factory=list)

    def add_build(self, name: str, why: str) -> None:
        if name not in self.builds:
            self.builds.append(name)
            self.reasons[name] = why

    def add_mcp(self, name: str, why: str) -> None:
        if name not in self.mcp_files:
            self.mcp_files.append(name)
            self.mcp_reasons[name] = why


def plan_builds(files: list[str], plan: Plan) -> None:
    everything = [f for f in files if match(f, ALL_TRIGGERS)]
    if everything:
        for b in ("linux", "unit-tests", "web", "android"):
            plan.add_build(b, f"{everything[0]} changed (builds everything)")
        plan.unit_all = True
        plan.mcp_all = True
        plan.mcp_notes.append(f"{everything[0]} changed: the whole MCP suite")
        return
    cxx = [f for f in files if f.startswith(("engine/", "editor/")) and (f.endswith(CXX) or "CMakeLists" in f
                                                                        or f.endswith(".cmake"))]
    shaders = [f for f in files if f.startswith(("engine/shaders/", "engine/shaderlib/"))]
    engine = [f for f in files if f.startswith("engine/") and not match(f, BUILDS_NOTHING)]
    if cxx or shaders or [f for f in files if f.startswith(("editor/", "engine/"))]:
        what = (cxx or shaders or engine or [f for f in files if f.startswith("editor/")])[0]
        plan.add_build("linux", f"{what} changed")
    if cxx:
        plan.add_build("unit-tests", f"{cxx[0]} changed")
    web = [f for f in files if match(f, WEB_DIRS)] or [f for f in engine if f.endswith(CXX) or "CMakeLists" in f
                                                       or f.endswith(".cmake")]
    if web and not [f for f in web if match(f, ANDROID_DIRS)] == web:
        plan.add_build("web", f"{web[0]} changed (the engine's web build)")
    android = [f for f in files if match(f, ANDROID_DIRS)]
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


def ninja_deps(tree: Path) -> str | None:
    if not (tree / "build.ninja").is_file() or not shutil.which("ninja"):
        return None
    r = subprocess.run(["ninja", "-C", str(tree), "-t", "deps"], capture_output=True, text=True)
    return r.stdout if r.returncode == 0 and r.stdout.strip() else None


def plan_units(changes: Changes, plan: Plan, deps_core: str | None, deps_editor: str | None) -> None:
    if plan.unit_all or "unit-tests" not in plan.builds:
        return
    cxx = [f for f in changes.files if f.startswith(("engine/", "editor/"))]
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
            if is_test_source(f) and f not in known and (ROOT / f).is_file() and \
                    f.startswith("engine/tests/" if name == "core" else "editor/tests/"):
                units.append(f)
                suites = sorted(set(suites) | set(gtest_suites((ROOT / f).read_text(errors="replace"))))
        getattr(plan, f"unit_{name}").extend(suites)
        if units:
            plan.unit_notes.append(f"{name}: {len(units)} test unit(s) see the change: {', '.join(units[:6])}"
                                   f"{' ...' if len(units) > 6 else ''}")
    new = [f for f in cxx if f.endswith(CXX) and not f.startswith(("engine/tests/", "editor/tests/"))
           and f not in changes.deleted and not any(f in p for p in (deps_core, deps_editor))]
    if new:
        plan.unit_notes.append(f"{len(new)} changed source(s) no test tree compiles or includes (e.g. {new[0]}): "
                               "their behaviour is the MCP tests' to cover")


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


def test_files(tests_dir: Path = MCP_TESTS) -> dict[str, str]:
    return {p.name: p.read_text(errors="replace") for p in sorted(tests_dir.glob("test_*.py"))}


def plan_mcp(changes: Changes, plan: Plan, cmap: dict | None, sources: dict[str, str],
             header_units=None, staleness: str | None = None) -> None:
    """sources: test file -> its text. header_units(header) -> the editor tree's units including it."""
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
        elif f.startswith("tools/mcp/tests/") or (f.startswith("tools/mcp/") and f.endswith(".py")):
            # a helper module or test data: the files that import or name it
            stem = Path(f).stem
            for test, text in sources.items():
                if re.search(rf"\b{re.escape(stem)}\b", text):
                    plan.add_mcp(test, f"uses {f}")
    # the tools a changed MCP tool source defines: the files that call them
    for f in files:
        if f.startswith("editor/mcp/") and f.endswith(CXX) and (ROOT / f).is_file():
            tools = set(TOOL_NAME.findall((ROOT / f).read_text(errors="replace")))
            for test, text in sources.items():
                hit = sorted(t for t in tools if re.search(rf"[\"']{t}[\"']", text))
                if hit:
                    plan.add_mcp(test, f"calls {hit[0]} ({f})")
    # fixtures, Lua, shaders, assets: by path (the file's or its folder's name in a test file)
    for f in files:
        if f.startswith(("tools/fixtures/", "tools/test-projects/", "docs/mcp/guide/")) or \
                f.endswith((".lua", ".glsl", ".vert", ".frag", ".comp", ".html", ".css", ".scene", ".prefab", ".material")):
            parts = Path(f).parts
            keys = {Path(f).name}
            if f.startswith(("tools/fixtures/", "tools/test-projects/")) and len(parts) > 2:
                keys.add(parts[2])
            if f.startswith("docs/mcp/guide/") and len(parts) > 3:
                keys.add(parts[3])
            for test, text in sources.items():
                if any(re.search(rf"[\"'/]{re.escape(k)}[\"'/]", text) for k in keys):
                    plan.add_mcp(test, f"loads {f}")
    # the engine's and the editor's C++ (and shaders): the coverage map
    code = [f for f in files if f.startswith(("engine/", "editor/")) and (f.endswith(CXX) or
                                                                         f.startswith(("engine/shaders/", "engine/shaderlib/")))]
    if not code:
        return
    if cmap is None:
        plan.mcp_ci = True
        plan.mcp_notes.append("no coverage map (tools/mcp/coverage_map.py): the CI suite for the C++ change")
        return
    if staleness:
        plan.mcp_ci = True
        plan.mcp_notes.append(f"{staleness}: the CI suite as well")
    covered: dict[str, set[str]] = {k: set(v) for k, v in cmap.get("files", {}).items()}
    known = set().union(*covered.values()) if covered else set()
    unmapped = []
    for f in code:
        targets = {f}
        if f.endswith(HEADER) and header_units is not None:
            targets |= set(header_units(f))
        if f.endswith(CXX) and not f.endswith(HEADER):
            targets |= set(siblings(f))
        if f.startswith(("engine/shaders/", "engine/shaderlib/")):
            # shaders are built into the editor's sources; any test that renders may draw them
            targets |= {s for s in known if "shader" in s.lower()}
        # (a file the map names that the checkout no longer has is no test to run)
        hit = [t for t, srcs in covered.items() if srcs & targets and t in sources]
        for t in hit:
            plan.add_mcp(t, f"runs {f}" if f in covered[t] else f"runs code that includes {f}")
        if not hit:
            unmapped.append(f)
    if unmapped:
        plan.mcp_ci = True
        plan.mcp_notes.append(f"{len(unmapped)} changed source(s) the map does not know (new, or no test runs them; "
                              f"e.g. {unmapped[0]}): the CI suite as well")
    engine_runtime = [f for f in code if f.startswith("engine/") and not f.startswith("engine/tests/")]
    if engine_runtime:
        for t in GAME_BUILD_FILES:
            if t in sources:
                plan.add_mcp(t, f"builds a game with the engine ({engine_runtime[0]} changed)")


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
    global ROOT, CACHE_FILE, RB_CHANGES, MCP_TESTS, CORE_TREE, EDITOR_TEST_TREE, EDITOR_TREE
    ROOT = root
    CACHE_FILE = root / "build" / ".cache" / "check-changed.json"
    RB_CHANGES = root / "build" / ".rb-changes.json"
    MCP_TESTS = root / "tools" / "mcp" / "tests"
    CORE_TREE = root / "build" / "linux" / "core-tests"
    EDITOR_TEST_TREE = root / "build" / "linux" / "editor-tests"
    EDITOR_TREE = root / "build" / "linux" / "editor"


def main(argv: list[str], root: Path | None = None) -> int:
    if root is not None:
        use_root(root)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", help="the ref to diff against (default: the merge base with oe-port, main, origin/main)")
    ap.add_argument("--dry-run", action="store_true", help="print the plan only")
    ap.add_argument("--no-cache", action="store_true", help="run even when the same inputs passed before")
    ap.add_argument("-j", type=int, default=0, help="MCP editors at once (default: the CPUs / 2, at least 2)")
    ap.add_argument("--list-mcp", type=Path, help="write the selected MCP test files here (one per line)")
    args = ap.parse_args(argv)
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
    options = f"{args.base}"
    key = inputs_hash(changes, cmap_version, options, ROOT)
    if not args.no_cache and not args.dry_run:
        hit = cached(key, CACHE_FILE)
        if hit:
            print(hit["summary"])
            print(f"[check-changed] the same inputs passed before (build/.cache/check-changed.json); --no-cache runs again")
            return 0

    in_image = Path("/.dockerenv").is_file() and not shutil.which("docker")
    runner = Runner(verbose, args.j)
    build_env = {"WERROR": "1"}
    # 2. builds (the unit test trees first: their dependency database picks the suites)
    if "unit-tests" in plan.builds and not args.dry_run:
        runner.run("build the unit tests", ["tools/build.sh", "unit-tests"], dict(build_env, RUN="0"))
    staleness = map_staleness(cmap, time.time(), commits_since(ROOT)) if cmap else None
    editor_deps = parse_ninja_deps(ninja_deps(EDITOR_TREE) or "", EDITOR_TREE) if not plan.mcp_all else {}

    def header_units(header: str) -> list[str]:
        return [paths[0] for paths in editor_deps.values() if paths and header in paths]

    plan_units(changes, plan, ninja_deps(CORE_TREE), ninja_deps(EDITOR_TEST_TREE))
    plan_mcp(changes, plan, cmap, test_files(MCP_TESTS), header_units, staleness)
    text = describe(changes, plan, cmap_info)
    print(text, flush=True)
    if args.list_mcp:
        args.list_mcp.write_text("\n".join(plan.mcp_files) + "\n")
    if args.dry_run:
        print(GAP)
        return 0

    for b in plan.builds:
        if b == "unit-tests":
            continue
        if b in ("web", "android") and in_image:
            runner.steps.append((f"build {b}", "CANNOT RUN (needs the host's Docker: run tools/check-changed there)", 0.0))
            runner.failed = True
            continue
        env = dict(build_env, SKIP_ENGINE="1") if b == "linux" else dict(build_env)
        runner.run(f"build {b}", ["tools/build.sh", b], env)
    # 3. unit tests
    if not runner.failed and "unit-tests" in plan.builds:
        if plan.unit_all:
            runner.run("unit tests (all)", ["tools/build.sh", "unit-tests"], build_env)
        else:
            for suite, names in (("core", plan.unit_core), ("editor", plan.unit_editor)):
                if names:
                    runner.run(f"unit tests ({suite}: {len(names)} suite(s))", ["tools/build.sh", "unit-tests"],
                               dict(build_env, UNIT_SUITE=suite, UNIT_TEST_ARGS=f"--gtest_filter={suite_filter(names)}"))
    # 4. MCP tests
    if not runner.failed and (plan.mcp_all or plan.mcp_ci or plan.mcp_files):
        if "linux" not in plan.builds and not (ROOT / "build/linux/editor-install/bin/jasperengine-editor").exists():
            runner.run("build linux (the MCP tests' editor)", ["tools/build.sh", "linux"], dict(build_env, SKIP_ENGINE="1"))
        jobs = args.j or max(2, (os.cpu_count() or 4) // 2)
        mcp_env = {"JASPER_SLOW_BUILD": "1", "JASPER_SLOW_TESTS": "1", "MCP_SUITE": "all"}
        if plan.mcp_all:
            runner.run("MCP tests (the whole suite)", ["tools/build.sh", "mcp-tests", "-j", str(jobs), "--split-seconds", "600"], mcp_env)
            runner.mcp_results("MCP tests (the whole suite)")
        else:
            if plan.mcp_ci:
                runner.run("MCP tests (the CI suite)", ["tools/build.sh", "mcp-tests", "-j", str(jobs)],
                           {"MCP_SUITE": "ci", "JASPER_SLOW_BUILD": "1"})
                runner.mcp_results("MCP tests (the CI suite)")
            files = [f for f in plan.mcp_files if (MCP_TESTS / f).is_file()]
            missing = [f for f in plan.mcp_files if f not in files]
            if missing:
                runner.steps.append(("MCP tests", f"CANNOT RUN: {missing} (selected, not in the checkout)", 0.0))
                runner.failed = True
            if files:
                if "test_mesh_raycast.py" not in files:
                    mcp_env["MCP_SKIP_WEB_BUILD"] = "1"
                runner.run(f"MCP tests ({len(files)} file(s))", ["tools/build.sh", "mcp-tests", "-j", str(jobs),
                           "--split-seconds", "600", *[f"tools/mcp/tests/{f}" for f in files]], mcp_env)
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
    if not runner.failed:
        if inputs_hash(changes, cmap_version, options, ROOT) == key and \
                read_changes(args.base, ROOT, RB_CHANGES).files == changes.files:
            store(key, summary, CACHE_FILE)
        else:
            print("[check-changed] files changed during the run: the pass is not cached", flush=True)
    return 1 if runner.failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
