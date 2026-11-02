"""The MCP suite's shared builds: one prebuilt player runtime per backend, flavour and content key.

A test that needs a game that runs, and not the compile path itself, builds it with a prebuilt
player runtime (S30): the export cooks the project next to the runtime in about a second, where
compiling the engine for it took 20 to 60 minutes. The runtimes come from here. Each one is built
from this checkout's engine sources with tools/players/build_linux_player.sh (what
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
  NOTICE.md, LICENSE, the Linux image's docker/linux.Dockerfile and this file; the exact CMake
  arguments; the compiler, linker, CMake and Ninja versions; the compiler environment (CC, CXX,
  CFLAGS, ...); and the editor version the runtime is locked to.
- The runtimes are a test's, not a shipped one's: no GCC link-time optimisation
  (-DJASPERENGINE_RELEASE_LTO=OFF; the rest of the release link, sections, hidden visibility and
  split symbols, as shipped). LTO costs a release runtime's link most of its build time and
  changes nothing a test of the game checks; the suite's compiled release game
  (test_build.py::test_desktop_build_and_run) and CI's `tools/build.sh players` keep it.
- Local cache (the default): under the run's build directory (build/linux/mcp-build-cache/, next
  to the editor under test; JASPER_MCP_BUILD_CACHE moves it), one entry per runtime id. An entry
  whose key differs from the current one is deleted before anything is built: a runtime of other
  sources (another commit, an uncommitted edit) is never reused.
- Shared cache (JASPER_MCP_SHARED_BUILD_CACHE=<dir>, e.g. a build server's ~/je/mcp-build-cache
  that tools/build.sh mounts for MCP_BUILD_CACHE): one entry per runtime id AND key,
  <dir>/v1/<runtime id>/<key>/, shared by every worktree, stream and run of the machine. A run
  takes the entry of its key or builds it: one process builds a key (an exclusive lock), every
  other one needing it waits for that build; the build goes to a temporary directory that is
  renamed into place complete (no reader ever sees half an entry). A run holds a shared lock on
  each entry it uses for as long as it runs, and the size limit (JASPER_MCP_BUILD_CACHE_MAX_GB,
  default 15) evicts the least recently used entries that nobody holds. The run's
  build/linux/mcp-build-cache/<runtime id>/players is then a link to the entry's players/.
- Each build starts from an empty CMake tree at a fixed path (the run's
  build/linux/mcp-build-cache/<runtime id>/tree, /src/... in the Linux image whatever the
  worktree); the compiler cache below it (ccache, CCACHE_DIR, as tools/build.sh uses for the editor
  itself) keys every object on its preprocessed source, compiler, flags and that path. The build's
  ccache hits and misses go in its key.json and the run's output.
- A failed build fails every test that needs it, with the build log, and is tried again in the
  next run.

    python3 tools/mcp/build_cache.py key glcore release      # the key and what it covers
    python3 tools/mcp/build_cache.py ensure glcore release   # build it now (inside the linux image)
    python3 tools/mcp/build_cache.py status [backend-flavour ...]   # built for this checkout? (exit 1: not all)
    python3 tools/mcp/build_cache.py list                    # the shared cache's entries
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

SCHEMA = 2
BACKENDS = ("glcore", "gles3", "vulkan")
FLAVOURS = ("release", "development")
# what a Linux player runtime is built from (relative to the repository): engine/ holds the
# engine, its vendored libraries and engine/notices.json; the rest is the build and packaging,
# and the recipe of the Linux image it is built in (its compiler, its system headers)
SOURCE_ROOTS = ("engine", "tools/players", "tools/performance", "tools/ci/check-notices.py", "NOTICE.md", "LICENSE",
                "docker/linux.Dockerfile", "tools/mcp/build_cache.py")
SKIP_DIRS = {".git", "__pycache__"}
BUILD_SCRIPT = REPO / "tools" / "players" / "build_linux_player.sh"
# the build options of the runtimes: those of `tools/build.sh players` without a sanitizer, unity
# build or warnings-as-errors (none of which a shipped runtime has), and without GCC's link-time
# optimisation (a test's runtime; the module docstring says which tests keep it)
CMAKE_ARGS = ("-DJASPERENGINE_SANITIZE=", "-DJASPERENGINE_UNITY_BUILD=OFF", "-DJASPERENGINE_RELEASE_LTO=OFF")
COMPILER_ENV = ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "CMAKE_GENERATOR", "CMAKE_BUILD_PARALLEL_LEVEL",
                "CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER", "SOURCE_DATE_EPOCH")
# the shared cache's layout version (a directory of its own: another layout never reads this one)
SHARED_LAYOUT = "v1"
DEFAULT_MAX_GB = 15.0


class BuildCacheError(Exception):
    pass


def cache_root() -> Path:
    """The run's own cache directory (in the shared mode: its links into the shared cache, and the
    build trees)."""
    return Path(os.environ.get("JASPER_MCP_BUILD_CACHE") or REPO / "build" / "linux" / "mcp-build-cache")


def shared_root() -> Path | None:
    """The machine's shared cache (JASPER_MCP_SHARED_BUILD_CACHE), or None: the local cache."""
    value = os.environ.get("JASPER_MCP_SHARED_BUILD_CACHE", "")
    return Path(value) / SHARED_LAYOUT if value else None


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
    sys.path.insert(0, str(HERE))
    import harness   # (its build_jobs: JOBS, else the container's CPU quota)
    # the run's test editors keep two CPUs (the build runs at nice 15 besides)
    return max(2, harness.build_jobs() - 2)


def _session() -> str:
    return os.environ.get("JASPER_MCP_SESSION") or f"pid{os.getpid()}"


def _log(message: str) -> None:
    print(f"     [build-cache] {message}", flush=True)


def _tail(path: Path, lines: int = 60) -> str:
    try:
        return "\n".join(path.read_text(errors="replace").splitlines()[-lines:])
    except OSError:
        return "(no log)"


def _ccache_counts(stats_log: Path) -> dict[str, int]:
    """Hits and misses of the compiles a build ran (ccache's stats_log, CCACHE_STATSLOG)."""
    counts = {"hits": 0, "misses": 0}
    with contextlib.suppress(OSError):
        for line in stats_log.read_text(errors="replace").splitlines():
            if line in ("direct_cache_hit", "preprocessed_cache_hit"):
                counts["hits"] += 1
            elif line == "cache_miss":
                counts["misses"] += 1
    return counts


def _build(backend: str, flavour: str, inputs: dict, key: str, out: Path, log_file: Path) -> dict:
    """Builds the runtime into out/<id> from an empty tree at the fixed path; its ccache counts."""
    rid = runtime_id(backend, flavour)
    local = cache_root() / rid
    local.mkdir(parents=True, exist_ok=True)
    tree = local / "tree"
    stats_log = local / "ccache-stats.log"
    # (one build in this tree at a time, whoever makes it: another run of this worktree waits)
    with open(local / ".tree.lock", "w") as tree_lock:
        fcntl.flock(tree_lock, fcntl.LOCK_EX)
        shutil.rmtree(tree, ignore_errors=True)
        shutil.rmtree(out, ignore_errors=True)
        stats_log.unlink(missing_ok=True)
        jobs = _jobs()
        command = [str(BUILD_SCRIPT), "--jobs", str(jobs), "--engine-version", inputs["editor_version"],
                   backend, flavour, str(tree), str(out), "--", *inputs["cmake_args"]]
        # at a low priority: the run's test editors (frame timing, llvmpipe) go first, and the
        # files that need the runtime wait for it anyway
        if shutil.which("nice"):
            command = ["nice", "-n", "15"] + command
        _log(f"{rid}: building (key {key[:16]}, {jobs} jobs; log {log_file})")
        t0 = time.monotonic()
        env = dict(os.environ, CCACHE_STATSLOG=str(stats_log))
        with open(log_file, "w") as log:
            log.write("$ " + " ".join(command) + "\n" + json.dumps(inputs, indent=1, sort_keys=True) + "\n")
            log.flush()
            r = subprocess.run(command, cwd=str(REPO), stdout=log, stderr=subprocess.STDOUT, env=env)
        seconds = round(time.monotonic() - t0, 1)
        counts = _ccache_counts(stats_log)
        shutil.rmtree(tree, ignore_errors=True)
    built = out / rid / "player-info.json"
    if r.returncode != 0 or not built.is_file():
        raise _BuildFailed(r.returncode)
    _log(f"{rid}: built in {seconds:.0f}s (ccache {counts['hits']} hits, {counts['misses']} misses)")
    return {"seconds": seconds, "ccache": counts, "jobs": jobs}


class _BuildFailed(Exception):
    def __init__(self, exit_code: int):
        super().__init__(exit_code)
        self.exit_code = exit_code


def _ensure_local(backend: str, flavour: str, inputs: dict, key: str) -> Path:
    rid = runtime_id(backend, flavour)
    entry = cache_root() / rid
    entry.mkdir(parents=True, exist_ok=True)
    players = entry / "players"
    runtime = players / rid
    key_file, failed_file, log_file = entry / "key.json", entry / "failed.json", entry / "build.log"
    with open(entry / ".lock", "w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            _log(f"{rid}: waiting for the build another worker is making")
            t0 = time.monotonic()
            fcntl.flock(lock, fcntl.LOCK_EX)
            _log(f"{rid}: waited {time.monotonic() - t0:.0f}s")
        with contextlib.suppress(OSError, ValueError):
            if json.loads(key_file.read_text())["key"] == key and not players.is_symlink() and \
                    (runtime / "player-info.json").is_file():
                return runtime
        with contextlib.suppress(OSError, ValueError):
            failed = json.loads(failed_file.read_text())
            if failed["key"] == key and failed["session"] == _session():
                raise BuildCacheError(f"{rid}: its build failed earlier in this run (log {log_file}):\n{_tail(log_file)}")
        # anything else here is another key's (or a broken) entry, or a link into a shared cache:
        # gone before the build
        for stale in (key_file, failed_file):
            stale.unlink(missing_ok=True)
        _remove(players)
        _remove(entry / "players.new")
        try:
            result = _build(backend, flavour, inputs, key, entry / "players.new", log_file)
        except _BuildFailed as e:
            failed_file.write_text(json.dumps({"key": key, "session": _session(), "exit": e.exit_code}))
            raise BuildCacheError(f"{rid}: the build failed (exit {e.exit_code}, log {log_file}):\n{_tail(log_file)}") from None
        os.replace(entry / "players.new", players)
        key_file.write_text(json.dumps({"key": key, "inputs": inputs, **result}, indent=1, sort_keys=True))
        return runtime


def _remove(path: Path) -> None:
    """A file, a link (not what it points to) or a directory tree."""
    if path.is_symlink() or path.is_file():
        path.unlink(missing_ok=True)
    elif path.exists():
        shutil.rmtree(path, ignore_errors=True)


# the shared locks this process holds on the shared entries it uses (released when it exits)
_held: dict[Path, object] = {}


def _hold(use_file: Path) -> None:
    """A shared lock on an entry for the rest of this process: eviction never takes it away."""
    if use_file in _held:
        return
    handle = open(use_file, "a")
    fcntl.flock(handle, fcntl.LOCK_SH)
    _held[use_file] = handle


def _entry_complete(entry: Path, key: str, rid: str) -> bool:
    with contextlib.suppress(OSError, ValueError):
        return json.loads((entry / "key.json").read_text())["key"] == key and \
            (entry / "players" / rid / "player-info.json").is_file()
    return False


def _link(players: Path, target: Path) -> None:
    """Points the run's players directory at the shared entry's (an atomic replace)."""
    players.parent.mkdir(parents=True, exist_ok=True)
    if players.is_symlink() and os.readlink(players) == str(target):
        return
    if players.exists() and not players.is_symlink():
        shutil.rmtree(players, ignore_errors=True)   # a local cache's entry of earlier runs
    tmp = players.with_name(f".players.{os.getpid()}.link")
    tmp.unlink(missing_ok=True)
    os.symlink(target, tmp)
    os.replace(tmp, players)


def _ensure_shared(store: Path, backend: str, flavour: str, inputs: dict, key: str) -> Path:
    rid = runtime_id(backend, flavour)
    group = store / rid
    group.mkdir(parents=True, exist_ok=True)
    entry = group / key
    use_file, lock_file = group / f"{key}.use", group / f"{key}.lock"
    failed_file, log_file = group / f"{key}.failed.json", group / f"{key}.build.log"
    local_players = cache_root() / rid / "players"
    # held before the entry is looked at: from here on no eviction removes it under this run
    _hold(use_file)
    if not _entry_complete(entry, key, rid):
        with open(lock_file, "a") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                _log(f"{rid}: waiting for the build another run or worker is making (shared cache {store})")
                t0 = time.monotonic()
                fcntl.flock(lock, fcntl.LOCK_EX)
                _log(f"{rid}: waited {time.monotonic() - t0:.0f}s")
            if not _entry_complete(entry, key, rid):
                with contextlib.suppress(OSError, ValueError):
                    failed = json.loads(failed_file.read_text())
                    if failed["key"] == key and failed["session"] == _session():
                        raise BuildCacheError(f"{rid}: its build failed earlier in this run (log {log_file}):\n"
                                              f"{_tail(log_file)}")
                failed_file.unlink(missing_ok=True)
                _remove(entry)   # (an incomplete entry: impossible but by hand)
                staging = group / f".new-{key[:16]}-{os.uname().nodename}-{os.getpid()}"
                _remove(staging)
                staging.mkdir()
                try:
                    result = _build(backend, flavour, inputs, key, staging / "players", log_file)
                except _BuildFailed as e:
                    _remove(staging)
                    failed_file.write_text(json.dumps({"key": key, "session": _session(), "exit": e.exit_code}))
                    raise BuildCacheError(f"{rid}: the build failed (exit {e.exit_code}, log {log_file}):\n"
                                          f"{_tail(log_file)}") from None
                shutil.copyfile(log_file, staging / "build.log")
                (staging / "key.json").write_text(json.dumps(
                    {"key": key, "inputs": inputs, "built_by": str(REPO), "built_at": time.time(), **result},
                    indent=1, sort_keys=True))
                os.rename(staging, entry)   # complete, or not there at all
                evict(store, keep={entry})
            else:
                _log(f"{rid}: built by another run meanwhile (key {key[:16]}, shared cache {store})")
    else:
        built = {}
        with contextlib.suppress(OSError, ValueError):
            built = json.loads((entry / "key.json").read_text())
        _log(f"{rid}: reused from the shared cache (key {key[:16]}, built {built.get('seconds', '?')}s by "
             f"{built.get('built_by', '?')}; {entry})")
    with contextlib.suppress(OSError):
        os.utime(entry)   # last use, for the eviction
    _link(local_players, entry / "players")
    return local_players / rid


def _size(path: Path) -> int:
    total = 0
    for directory, _, files in os.walk(path):
        for f in files:
            with contextlib.suppress(OSError):
                total += os.lstat(os.path.join(directory, f)).st_blocks * 512
    return total


def max_bytes() -> int:
    with contextlib.suppress(ValueError):
        return int(float(os.environ.get("JASPER_MCP_BUILD_CACHE_MAX_GB") or DEFAULT_MAX_GB) * (1 << 30))
    return int(DEFAULT_MAX_GB * (1 << 30))


def entries(store: Path) -> list[dict]:
    """The shared cache's complete entries, least recently used first."""
    found = []
    for group in sorted(p for p in store.iterdir() if p.is_dir()) if store.is_dir() else []:
        for entry in group.iterdir():
            if entry.is_dir() and not entry.name.startswith(".") and (entry / "key.json").is_file():
                with contextlib.suppress(OSError):
                    found.append({"runtime": group.name, "key": entry.name, "path": entry,
                                  "used": entry.stat().st_mtime, "bytes": _size(entry)})
    return sorted(found, key=lambda e: e["used"])


def evict(store: Path, keep: set[Path] = frozenset()) -> list[Path]:
    """Removes the least recently used entries nobody holds until the cache fits max_bytes(); and
    staging directories of builds that died (no process holds their key's build lock)."""
    removed: list[Path] = []
    store.mkdir(parents=True, exist_ok=True)
    with open(store / ".evict.lock", "a") as evict_lock:
        fcntl.flock(evict_lock, fcntl.LOCK_EX)
        for group in (p for p in store.iterdir() if p.is_dir()):
            for staging in group.glob(".new-*"):
                key16 = staging.name.split("-")[1]
                locks = [f for f in group.glob(f"{key16}*.lock")]
                with contextlib.ExitStack() as stack:
                    free = True
                    for f in locks:
                        handle = stack.enter_context(open(f, "a"))
                        try:
                            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        except BlockingIOError:
                            free = False
                    if free:
                        _remove(staging)
        listed = entries(store)
        total = sum(e["bytes"] for e in listed)
        limit = max_bytes()
        for e in listed:
            if total <= limit:
                break
            if e["path"] in keep:
                continue
            with open(e["path"].parent / f"{e['key']}.use", "a") as use:
                try:
                    fcntl.flock(use, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    continue   # a run uses it
                gone = e["path"].with_name(f".old-{e['key'][:16]}-{os.getpid()}")
                os.rename(e["path"], gone)
                _remove(gone)
            total -= e["bytes"]
            removed.append(e["path"])
            _log(f"evicted {e['runtime']} {e['key'][:16]} ({e['bytes'] / (1 << 20):.0f} MiB, the cache's limit "
                 f"{limit / (1 << 30):.1f} GiB)")
    return removed


def ensure_player(backend: str = "glcore", flavour: str = "release") -> Path:
    """The runtime's directory (<players_dir>/<id>), built first unless this key's build is there.
    Blocks while another process builds it (one build; every worker, and in the shared mode every
    run of the machine, waits for it)."""
    rid = runtime_id(backend, flavour)
    inputs = key_inputs(backend, flavour)
    key = cache_key(inputs)
    store = shared_root()
    if store is None:
        return _ensure_local(backend, flavour, inputs, key)
    return _ensure_shared(store, backend, flavour, inputs, key)


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


def status(names: list[str]) -> dict[str, bool]:
    """Whether each runtime (backend-flavour) of this checkout's key is built, in the cache this
    run would use (no build, no lock)."""
    found = {}
    store = shared_root()
    for name in names:
        backend, _, flavour = name.partition("-")
        rid = runtime_id(backend, flavour)
        key = cache_key(key_inputs(backend, flavour))
        if store is not None:
            found[name] = _entry_complete(store / rid / key, key, rid)
        else:
            entry = cache_root() / rid
            with contextlib.suppress(OSError, ValueError):
                found[name] = json.loads((entry / "key.json").read_text())["key"] == key and \
                    (entry / "players" / rid / "player-info.json").is_file()
            found.setdefault(name, False)
    return found


def main(argv: list[str]) -> int:
    usage = ("usage: build_cache.py key|ensure <backend> <flavour>   (e.g. ensure glcore release)\n"
             "       build_cache.py status [backend-flavour ...]     (default: glcore-release glcore-development)\n"
             "       build_cache.py list | evict                     (the shared cache, JASPER_MCP_SHARED_BUILD_CACHE)")
    if argv and argv[0] == "status":
        found = status(argv[1:] or ["glcore-release", "glcore-development"])
        print(json.dumps({"cache": str(shared_root() or cache_root()), "built": found}, indent=1, sort_keys=True))
        return 0 if all(found.values()) else 1
    if argv in (["list"], ["evict"]):
        store = shared_root()
        if store is None:
            print("build_cache.py: no shared cache (JASPER_MCP_SHARED_BUILD_CACHE is not set)", file=sys.stderr)
            return 2
        if argv == ["evict"]:
            evict(store)
        listed = entries(store)
        for e in listed:
            print(f"{e['runtime']:32} {e['key'][:16]} {e['bytes'] / (1 << 20):8.0f} MiB  last used "
                  f"{time.strftime('%Y-%m-%d %H:%M', time.gmtime(e['used']))} UTC")
        print(f"{len(listed)} entries, {sum(e['bytes'] for e in listed) / (1 << 30):.2f} GiB of "
              f"{max_bytes() / (1 << 30):.1f} GiB ({store})")
        return 0
    if len(argv) != 3 or argv[0] not in ("key", "ensure"):
        print(usage, file=sys.stderr)
        return 2
    action, backend, flavour = argv
    if action == "key":
        inputs = key_inputs(backend, flavour)
        print(json.dumps({"key": cache_key(inputs), "players_dir": str(players_dir(backend, flavour)),
                          "shared": str(shared_root() or ""), "inputs": inputs}, indent=1, sort_keys=True))
        return 0
    print(ensure_player(backend, flavour))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
