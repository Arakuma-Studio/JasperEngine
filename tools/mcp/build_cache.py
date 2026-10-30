"""The MCP suite's shared builds: one prebuilt player runtime per backend and flavour per suite run.

A test that needs a game that runs, and not the compile path itself, builds it with a prebuilt
player runtime (S30): the export cooks the project next to the runtime in about a second, where
compiling the engine for it took 20 to 60 minutes. The runtimes come from here. Each one is built
once from this checkout's engine sources with tools/players/build_linux_player.sh (what
`tools/build.sh players` and CI build), and every test and every -j worker of the run shares it:

    EDITOR = {"project": ..., "env": build_cache.player_env("glcore", "release")}   # JASPER_PLAYERS_DIR
    ...
    await build_cache.player_runtime(ed, "glcore", "release")   # waits for (or makes) the build
    r = await ed.call("build_project", mode="desktop", method="prebuilt", ...)

A file whose tests need runtimes declares them (PREBUILT_PLAYERS = ["glcore-release"]); the runner
starts building those as the run starts (JASPER_SLOW_BUILD=1, no -k filter), and starts such a file
once its runtimes are built (the other files run meanwhile).
The tests that check the compile path itself compile with method="compile" (docs/testing.md,
"Compiled and prebuilt game builds").

The cache cannot hide a bug:
- Its key covers everything that goes into a runtime: the content (not the commit or the file
  times) of every file under engine/, tools/players/, tools/performance/, tools/ci/check-notices.py,
  NOTICE.md, LICENSE and this file; the exact CMake arguments; the compiler, linker, CMake and
  Ninja versions; the compiler environment (CC, CXX, CFLAGS, ...); and the editor version the
  runtime is locked to.
- It lives under the run's build directory (build/linux/mcp-build-cache/, next to the editor
  under test; JASPER_MCP_BUILD_CACHE moves it), one entry per runtime id. An entry whose key
  differs from the current one is deleted before anything is built: a runtime of other sources
  (another commit, an uncommitted edit) is never reused. Each build starts from an empty CMake
  tree; the compiler cache below it (ccache, CCACHE_DIR, as tools/build.sh uses for the editor
  itself) keys every object on its preprocessed source, compiler and flags.
- A failed build fails every test that needs it, with the build log, and is tried again in the
  next run.

    python3 tools/mcp/build_cache.py key glcore release      # the key and what it covers
    python3 tools/mcp/build_cache.py ensure glcore release   # build it now (inside the linux image)
"""

from __future__ import annotations

import asyncio
import contextlib
import fcntl
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

SCHEMA = 1
BACKENDS = ("glcore", "gles3", "vulkan")
FLAVOURS = ("release", "development")
# what a Linux player runtime is built from (relative to the repository): engine/ holds the
# engine, its vendored libraries and engine/notices.json; the rest is the build and packaging
SOURCE_ROOTS = ("engine", "tools/players", "tools/performance", "tools/ci/check-notices.py", "NOTICE.md", "LICENSE",
                "tools/mcp/build_cache.py")
SKIP_DIRS = {".git", "__pycache__"}
BUILD_SCRIPT = REPO / "tools" / "players" / "build_linux_player.sh"
# the build options of the runtimes: those of `tools/build.sh players` without a sanitizer, unity
# build or warnings-as-errors (none of which a shipped runtime has)
CMAKE_ARGS = ("-DJASPERENGINE_SANITIZE=", "-DJASPERENGINE_UNITY_BUILD=OFF")
COMPILER_ENV = ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CMAKE_GENERATOR", "CMAKE_BUILD_PARALLEL_LEVEL",
                "CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER", "SOURCE_DATE_EPOCH")


class BuildCacheError(Exception):
    pass


def cache_root() -> Path:
    return Path(os.environ.get("JASPER_MCP_BUILD_CACHE") or REPO / "build" / "linux" / "mcp-build-cache")


def runtime_id(backend: str, flavour: str) -> str:
    if backend not in BACKENDS or flavour not in FLAVOURS:
        raise BuildCacheError(f"no Linux player runtime {backend}-{flavour} (backends {BACKENDS}, flavours {FLAVOURS})")
    return f"linux-x64-{backend}-{flavour}"


def players_dir(backend: str, flavour: str) -> Path:
    """The players directory (JASPER_PLAYERS_DIR) that holds only this runtime once it is built."""
    return cache_root() / runtime_id(backend, flavour) / "players"


def player_env(backend: str = "glcore", flavour: str = "release") -> dict[str, str]:
    """The environment an Editor needs to find the runtime: Editor(..., env=player_env(...))."""
    return {"JASPER_PLAYERS_DIR": str(players_dir(backend, flavour))}


# ---------------------------------------------------------------------------- the key

_source_digest: str | None = None


def source_digest() -> str:
    """SHA-256 of every file a runtime is built from: its path, executable bit and content."""
    global _source_digest
    if _source_digest is not None:
        return _source_digest
    files: list[Path] = []
    for root in SOURCE_ROOTS:
        path = REPO / root
        if path.is_file():
            files.append(path)
            continue
        if not path.is_dir():
            raise BuildCacheError(f"{root} is missing: the runtime's sources are incomplete")
        for directory, dirnames, filenames in os.walk(path):
            dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
            files += [Path(directory) / f for f in filenames if not f.endswith(".pyc")]
    digest = hashlib.sha256()
    for path in sorted(files, key=lambda p: p.relative_to(REPO).as_posix()):
        relative = path.relative_to(REPO).as_posix()
        if path.is_symlink():
            data, kind = os.readlink(path).encode(), "l"
        elif path.is_file():
            data, kind = path.read_bytes(), "x" if os.access(path, os.X_OK) else "f"
        else:
            continue
        digest.update(f"{relative}\0{kind}\0{len(data)}\0".encode())
        digest.update(data)
    _source_digest = digest.hexdigest()
    return _source_digest


def _version(*command: str) -> str:
    try:
        r = subprocess.run(list(command), capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as e:
        return f"unavailable: {e}"
    return (r.stdout or r.stderr).strip().splitlines()[0] if (r.stdout or r.stderr).strip() else f"exit {r.returncode}"


_toolchain: dict | None = None


def toolchain() -> dict:
    global _toolchain
    if _toolchain is None:
        _toolchain = {
            "c++": _version(os.environ.get("CXX") or "c++", "--version"),
            "cc": _version(os.environ.get("CC") or "cc", "--version"),
            "ld": _version("ld", "--version"),
            "cmake": _version("cmake", "--version"),
            "ninja": _version("ninja", "--version"),
            "env": {name: os.environ[name] for name in COMPILER_ENV if name in os.environ},
        }
    return _toolchain


_editor_version: str | None = None


def editor_version() -> str:
    """The version of the editor under test (`players list --json`): a runtime is locked to it."""
    global _editor_version
    if _editor_version is not None:
        return _editor_version
    sys.path.insert(0, str(HERE))
    try:
        import harness   # (the editor the tests start: JASPER_EDITOR, or this or the main checkout's)
        command = harness.editor_command()
    except ImportError:  # plain python3, without the harness's packages
        command = shlex.split(os.environ["JASPER_EDITOR"]) if os.environ.get("JASPER_EDITOR") else \
            [str(REPO / "build" / "linux" / "editor-install" / "bin" / "jasperengine-editor")]
    with tempfile.TemporaryDirectory(prefix="jasper-build-cache-home-") as home:
        env = dict(os.environ, HOME=home)
        env.pop("JASPER_PLAYERS_DIR", None)
        try:
            r = subprocess.run(command + ["players", "list", "--json"], capture_output=True, text=True, env=env, timeout=120)
        except OSError as e:
            raise BuildCacheError(f"the editor's version: cannot run {command}: {e}") from None
    try:
        _editor_version = json.loads(r.stdout)["editor_version"]
    except (ValueError, KeyError) as e:
        raise BuildCacheError(f"the editor's version (players list --json, exit {r.returncode}): {e}\n"
                              f"{r.stdout[-1000:]}{r.stderr[-1000:]}") from None
    return _editor_version


def _compiler_cache_args() -> list[str]:
    if shutil.which("ccache") and os.environ.get("CCACHE_DIR"):
        return ["-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"]
    return []


def key_inputs(backend: str, flavour: str) -> dict:
    return {
        "schema": SCHEMA,
        "runtime": runtime_id(backend, flavour),
        "sources": source_digest(),
        "cmake_args": list(CMAKE_ARGS) + _compiler_cache_args(),
        "editor_version": editor_version(),
        "toolchain": toolchain(),
    }


def cache_key(inputs: dict) -> str:
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()


# ---------------------------------------------------------------------------- building

def _jobs() -> int:
    if os.environ.get("JASPER_MCP_BUILD_JOBS"):
        return max(1, int(os.environ["JASPER_MCP_BUILD_JOBS"]))
    cpus = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else (os.cpu_count() or 2)
    with contextlib.suppress(OSError, ValueError):   # a container's CPU quota (docker --cpus)
        quota, period = Path("/sys/fs/cgroup/cpu.max").read_text().split()
        if quota != "max":
            cpus = min(cpus, max(1, int(quota) // int(period)))
    # the run's test editors keep two CPUs
    return max(2, cpus - 2)


def _session() -> str:
    return os.environ.get("JASPER_MCP_SESSION") or f"pid{os.getpid()}"


def _log(message: str) -> None:
    print(f"     [build-cache] {message}", flush=True)


def _tail(path: Path, lines: int = 60) -> str:
    try:
        return "\n".join(path.read_text(errors="replace").splitlines()[-lines:])
    except OSError:
        return "(no log)"


def ensure_player(backend: str = "glcore", flavour: str = "release") -> Path:
    """The runtime's directory (<players_dir>/<id>), built first unless this key's build is there.
    Blocks while another process of the run builds it (one build, every worker waits for it)."""
    rid = runtime_id(backend, flavour)
    entry = cache_root() / rid
    entry.mkdir(parents=True, exist_ok=True)
    inputs = key_inputs(backend, flavour)
    key = cache_key(inputs)
    players = entry / "players"
    runtime = players / rid
    key_file, failed_file, log_file, tree = entry / "key.json", entry / "failed.json", entry / "build.log", entry / "tree"
    with open(entry / ".lock", "w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            _log(f"{rid}: waiting for the build another worker is making")
            t0 = time.monotonic()
            fcntl.flock(lock, fcntl.LOCK_EX)
            _log(f"{rid}: waited {time.monotonic() - t0:.0f}s")
        with contextlib.suppress(OSError, ValueError):
            if json.loads(key_file.read_text())["key"] == key and (runtime / "player-info.json").is_file():
                return runtime
        with contextlib.suppress(OSError, ValueError):
            failed = json.loads(failed_file.read_text())
            if failed["key"] == key and failed["session"] == _session():
                raise BuildCacheError(f"{rid}: its build failed earlier in this run (log {log_file}):\n{_tail(log_file)}")
        # anything else here is another key's (or a broken) entry: gone before the build
        for stale in (key_file, failed_file):
            stale.unlink(missing_ok=True)
        for stale in (players, tree, entry / "players.new"):
            shutil.rmtree(stale, ignore_errors=True)
        jobs = _jobs()
        command = [str(BUILD_SCRIPT), "--jobs", str(jobs), "--engine-version", inputs["editor_version"],
                   backend, flavour, str(tree), str(entry / "players.new"), "--", *inputs["cmake_args"]]
        # at a low priority: the run's test editors (frame timing, llvmpipe) go first, and the
        # files that need the runtime wait for it anyway
        if shutil.which("nice"):
            command = ["nice", "-n", "15"] + command
        _log(f"{rid}: building (key {key[:16]}, {jobs} jobs; log {log_file})")
        t0 = time.monotonic()
        with open(log_file, "w") as log:
            log.write("$ " + " ".join(command) + "\n" + json.dumps(inputs, indent=1, sort_keys=True) + "\n")
            log.flush()
            r = subprocess.run(command, cwd=str(REPO), stdout=log, stderr=subprocess.STDOUT)
        built = entry / "players.new" / rid / "player-info.json"
        if r.returncode != 0 or not built.is_file():
            failed_file.write_text(json.dumps({"key": key, "session": _session(), "exit": r.returncode}))
            raise BuildCacheError(f"{rid}: the build failed (exit {r.returncode}, log {log_file}):\n{_tail(log_file)}")
        os.replace(entry / "players.new", players)
        shutil.rmtree(tree, ignore_errors=True)
        key_file.write_text(json.dumps({"key": key, "inputs": inputs, "seconds": round(time.monotonic() - t0, 1)},
                                       indent=1, sort_keys=True))
        _log(f"{rid}: built in {time.monotonic() - t0:.0f}s")
        return runtime


async def player_runtime(ed, backend: str = "glcore", flavour: str = "release") -> Path:
    """ensure_player for a test: off the event loop, and a call in flight for the hang guard."""
    with ed.busy():
        try:
            return await asyncio.to_thread(ensure_player, backend, flavour)
        except BuildCacheError as e:
            ed.fail(str(e))


def prewarm(wanted: list[str], ready: dict | None = None) -> None:
    """Builds the runtimes the run's files declare (PREBUILT_PLAYERS), one after another: the
    runner calls it from a thread as the run starts, and starts the files that need a runtime
    once its event in `ready` is set. A failure is the tests' to report (they fail with the log)."""
    for name in wanted:
        backend, _, flavour = name.partition("-")
        try:
            ensure_player(backend, flavour)
        except Exception as e:  # noqa: BLE001
            _log(f"{name}: {str(e).splitlines()[0]}")
        finally:
            if ready and name in ready:
                ready[name].set()


def main(argv: list[str]) -> int:
    if len(argv) != 3 or argv[0] not in ("key", "ensure"):
        print("usage: build_cache.py key|ensure <backend> <flavour>   (e.g. ensure glcore release)", file=sys.stderr)
        return 2
    action, backend, flavour = argv
    if action == "key":
        inputs = key_inputs(backend, flavour)
        print(json.dumps({"key": cache_key(inputs), "players_dir": str(players_dir(backend, flavour)), "inputs": inputs},
                         indent=1, sort_keys=True))
        return 0
    print(ensure_player(backend, flavour))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
