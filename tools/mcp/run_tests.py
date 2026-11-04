"""Run the MCP tests: tools/mcp/tests/test_*.py.

    tools/mcp/run-tests.sh                      # every test file against the real editor
    tools/mcp/run-tests.sh -k undo              # tests whose "file::name" contains "undo"
    tools/mcp/run-tests.sh -j 4                 # 4 test files at a time (one editor each)
    tools/mcp/run-tests.sh tests/test_protocol.py
    tools/mcp/run-tests.sh --fake tools/mcp/tests/test_protocol.py   # fake-server protocol checks
    JASPER_MCP_TOKEN=<token> tools/mcp/run-tests.sh --url http://127.0.0.1:47615/mcp   # against an editor already running
    tools/mcp/run-tests.sh --suite ci -j 2      # the CI suite: the files that declare CI = True

A test is a module-level `async def test_xxx(ed)` (ed: harness.Editor) or `async def test_xxx()`.
Tests of a file run in source order and share one editor using the module's owned project setup
(module variable EDITOR = dict(...) passes Editor arguments, e.g. EDITOR = {"create": True}).
An explicit --url overrides that launch setup. If a test leaves the editor unresponsive,
the next test gets a new one.
Call harness.skip("why") or ed.require_tools(...) to skip. Module variable TIMEOUT sets the
per-test hang guard in seconds (default 600): a test fails when no tool call has been in flight
for that long (a slow but working editor, such as one waiting for game frames on a loaded machine,
never trips it; each call has its own timeout, harness.Editor.call_raw).
Module variable PREBUILT_PLAYERS = ["glcore-release", ...] names the prebuilt player runtimes the
file's tests build games with (tools/mcp/build_cache.py): with JASPER_SLOW_BUILD=1 and no -k, the
run starts building them (once, for every worker) as it starts, and -j N starts such a file once
its runtimes are built (the other files run meanwhile).
With -j N the longest files start first, by each file's wall time in the last unfiltered run
(build/test-results/mcp-file-seconds.json, JASPER_MCP_FILE_SECONDS; files it has no time for go
first, in their order): a long file never starts last and alone.

A file that declares TESTS_INDEPENDENT = "why" (each test runs in an editor of its own, in any order),
or TESTS_INDEPENDENT = ("why", ["test_x_*"]) for some of its tests, can be split: --split-seconds N
gives chunks of about N seconds, by the earlier runs' test times, a worker and editor each. --plan
FILE runs the chunks tools/mcp/shard.py planned for one server of a run across servers.

A file joins the CI suite (--suite ci, the CI workflow's e2e job) by declaring CI = True at module
level. A file that is not CI = True is left out for a reason the run prints: it needs the default
fixture (below), or it declares CI_EXCLUDED = "why" (e.g. long-running, in the nightly full suite);
a file left out for no reason stops --suite ci (exit 2). CI has no copy of the default fixture (harness.default_fixture(): JASPER_FIXTURE, else
build/fixture-unity, /fixture or ~/je/fixture-unity; the tests that need it skip without one), so
such a file must not need it:
every test that takes an editor runs on EDITOR = {"create": True, ...} or EDITOR = {"project": ...}
(e.g. a tools/fixtures project). The runner checks this and exits 2 otherwise. New test files are
written CI-capable by default (docs/mcp/README.md).

Each file runs in its own Python process. Exit code: 0 when nothing failed, 1 when a test failed or
a worker crashed, 2 when --suite ci holds a file that relies on the default fixture.
"""

from __future__ import annotations

import argparse
import ast
import asyncio
import contextlib
import importlib.util
import inspect
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESTS = HERE / "tests"
# each file's wall time in the last unfiltered run: -j N starts the longest files first
FILE_SECONDS = Path(os.environ.get("JASPER_MCP_FILE_SECONDS") or HERE.parent.parent / "build" / "test-results" / "mcp-file-seconds.json")
# each test's own time in the runs so far (--split-seconds splits independent files by it)
TEST_SECONDS = Path(os.environ.get("JASPER_MCP_TEST_SECONDS") or HERE.parent.parent / "build" / "test-results" / "mcp-test-seconds.json")
sys.path.insert(0, str(HERE))


# ---------------------------------------------------------------------------
# Worker: runs one file


def _collect(path: Path, filt: list[str], only: list[str] | None = None):
    spec = importlib.util.spec_from_file_location(path.stem, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[path.stem] = mod
    spec.loader.exec_module(mod)
    tests = []
    for name, fn in vars(mod).items():
        if name.startswith("test_") and inspect.iscoroutinefunction(fn) and fn.__module__ == mod.__name__:
            full = f"{path.name}::{name}"
            if filt and not any(f.lower() in full.lower() for f in filt):
                continue
            if only is not None and name not in only:
                continue
            tests.append((inspect.getsourcelines(fn)[1], name, fn))
    tests.sort(key=lambda t: t[0])
    return mod, [(n, f) for _, n, f in tests]


async def _run_guarded(coro, ed, timeout: float) -> None:
    """Runs a test, failing it (asyncio.TimeoutError) when it made no tool call for `timeout`
    seconds: the time spent inside tool calls does not count (they have their own timeouts)."""
    task = asyncio.ensure_future(coro)
    started = time.monotonic()
    while True:
        done, _ = await asyncio.wait({task}, timeout=min(5.0, timeout))
        if done:
            task.result()
            return
        idle = min(ed.idle_seconds(), time.monotonic() - started)
        if idle >= timeout:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await task
            raise asyncio.TimeoutError()


async def _run_file(path: Path, filt: list[str], results_path: Path, url: str | None = None,
                    only: list[str] | None = None) -> None:
    import harness

    mod, tests = _collect(path, filt, only)
    opts = dict(getattr(mod, "EDITOR", {}) or {})
    if url is not None:
        opts["url"] = url
    timeout = float(getattr(mod, "TIMEOUT", 600))
    ed: harness.Editor | None = None
    ed_error: str | None = None
    ed_skip: str | None = None
    results = []

    async def get_editor() -> harness.Editor:
        nonlocal ed, ed_error, ed_skip
        if ed is not None:
            return ed
        e = harness.Editor(label=path.stem, **opts)
        try:
            await e.__aenter__()
        except harness.Skip as ex:  # no default fixture anywhere (harness.FixtureMissing)
            ed_skip = str(ex)
            raise
        except Exception as ex:  # noqa: BLE001
            ed_error = f"editor failed to start: {ex}"
            raise
        ed = e
        return ed

    for name, fn in tests:
        full = f"{path.name}::{name}"
        harness.current_test = full
        t0 = time.monotonic()
        status, detail = "pass", ""
        wants_ed = len(inspect.signature(fn).parameters) > 0
        try:
            if wants_ed:
                if ed_skip:
                    raise harness.Skip(ed_skip)
                if ed_error:
                    raise harness.HarnessError(ed_error)
                e = await get_editor()
                await _run_guarded(fn(e), e, timeout)
            else:
                await asyncio.wait_for(fn(), timeout)
        except harness.Skip as s:
            status, detail = "skip", str(s)
        except asyncio.TimeoutError:
            status, detail = "fail", f"timed out: no tool call in flight for {timeout:.0f}s" if wants_ed else \
                f"timed out after {timeout:.0f}s"
        except Exception as ex:  # noqa: BLE001
            status = "fail"
            tb = traceback.format_exception(type(ex), ex, ex.__traceback__)
            detail = "".join(tb[-6:]).rstrip()
        if status == "fail" and ed is not None:
            ed.failed = True
            if "editor stderr tail" not in detail:
                detail += "\n" + ed._tail_block(30)
        dt = time.monotonic() - t0
        harness._log_call(path.stem, "(test)", {"status": status}, dt, status == "fail")
        print(f"{status.upper():4} {full} ({dt:.1f}s){'  ' + detail if status == 'skip' else ''}", flush=True)
        if status == "fail":
            print("     " + detail.replace("\n", "\n     "), flush=True)
        results.append({"name": full, "status": status, "detail": detail, "seconds": round(dt, 2)})
        # A test that broke the editor should not fail every test after it.
        if status == "fail" and ed is not None and ed.session is not None:
            try:
                await asyncio.wait_for(ed.session.send_ping(), 10)
            except Exception:  # noqa: BLE001
                print(f"     [runner] editor unresponsive after {name}; restarting it", flush=True)
                await ed.close()
                ed = None
    if ed is not None:
        await ed.close()
    # a chunk's test that the file no longer has: reported, never silently dropped
    for name in [n for n in (only or []) if n not in {t for t, _ in tests}]:
        detail = f"the plan names {path.name}::{name}, which the file does not have"
        print(f"FAIL {path.name}::{name} (0.0s)\n     {detail}", flush=True)
        results.append({"name": f"{path.name}::{name}", "status": "fail", "detail": detail, "seconds": 0.0})
    results_path.write_text(json.dumps(results))


def worker_main(args) -> int:
    for k, v in (("JASPER_FAKE", "1" if args.fake else None), ("JASPER_XVFB", args.xvfb),
                 ("JASPER_KEEP_SCRATCH", "1" if args.keep else None), ("JASPER_MCP_URL", args.url)):
        if v:
            os.environ[k] = v
    only = json.loads(Path(args.only).read_text()) if args.only else None
    asyncio.run(_run_file(Path(args.worker), args.k or [], Path(args.results), args.url, only))
    return 0


# ---------------------------------------------------------------------------
# CI suite


def _module_assignments(path: Path) -> dict[str, ast.expr]:
    """The module-level `NAME = value` statements of a test file, without importing it."""
    found: dict[str, ast.expr] = {}
    for node in ast.parse(path.read_text(encoding="utf-8"), str(path)).body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    found[target.id] = node.value
    return found


def _wants_editor(path: Path) -> bool:
    """Whether a test of the file takes the shared editor (a parameter: async def test_x(ed))."""
    for node in ast.parse(path.read_text(encoding="utf-8"), str(path)).body:
        if isinstance(node, ast.AsyncFunctionDef) and node.name.startswith("test_") and node.args.args:
            return True
    return False


def is_ci_file(path: Path) -> bool:
    value = _module_assignments(path).get("CI")
    return isinstance(value, ast.Constant) and value.value is True


def _own_fixture_call(path: Path) -> str | None:
    """A call in the file that copies the default fixture: Editor() or RawEditor() without
    create/project/url, or harness.default_fixture()."""
    for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"), str(path))):
        if not isinstance(node, ast.Call):
            continue
        name = node.func.id if isinstance(node.func, ast.Name) else \
            node.func.attr if isinstance(node.func, ast.Attribute) else ""
        if name == "default_fixture":
            return f"it calls default_fixture() (line {node.lineno})"
        if name in ("Editor", "RawEditor") and not node.args and \
                not any(k.arg in ("create", "project", "url") or k.arg is None for k in node.keywords):
            return f"it starts {name}() on the default fixture (line {node.lineno})"
    return None


def default_fixture_reason(path: Path) -> str | None:
    """Why the file needs the default fixture (build/fixture-unity), or None when it does not:
    its editor tests run on EDITOR["create"] or an explicit EDITOR["project"], and it starts no
    editor of its own on the default fixture."""
    own = _own_fixture_call(path)
    if own:
        return own
    if not _wants_editor(path):
        return None
    editor = _module_assignments(path).get("EDITOR")
    if isinstance(editor, ast.Dict):
        for key, value in zip(editor.keys, editor.values):
            if isinstance(key, ast.Constant) and key.value == "project":
                return None
            if isinstance(key, ast.Constant) and key.value == "create" and not (
                    isinstance(value, ast.Constant) and not value.value):
                return None
    return "its tests take the shared editor, and EDITOR has neither \"create\" nor \"project\""


def ci_excluded(path: Path) -> str | None:
    """A module-level CI_EXCLUDED = "why" (a file left out of CI for a reason of its own)."""
    value = _module_assignments(path).get("CI_EXCLUDED")
    if isinstance(value, ast.Constant) and isinstance(value.value, str) and value.value.strip():
        return value.value.strip()
    return None


def prebuilt_players(files: list[Path]) -> list[str]:
    """The prebuilt player runtimes the files declare (PREBUILT_PLAYERS = ["glcore-release", ...]:
    their tests build games with them, tools/mcp/build_cache.py), in the order the run needs them."""
    wanted: list[str] = []
    for f in files:
        value = _module_assignments(f).get("PREBUILT_PLAYERS")
        if isinstance(value, (ast.List, ast.Tuple)):
            for item in value.elts:
                if isinstance(item, ast.Constant) and isinstance(item.value, str) and item.value not in wanted:
                    wanted.append(item.value)
    return wanted


def ci_files(tests: Path = TESTS) -> tuple[list[Path], list[str], list[tuple[str, str]]]:
    """The CI = True files, the problems that stop a CI run, and the files the CI suite leaves out
    with the reason of each. No file is left out silently: one that is not CI = True either needs
    the default fixture (CI has no copy of it) or says why in CI_EXCLUDED = "..."; any other is a
    problem (mark it CI = True, or give the reason)."""
    files, problems, excluded = [], [], []
    for f in sorted(tests.glob("test_*.py")):
        fixture = default_fixture_reason(f)
        declared = ci_excluded(f)
        if is_ci_file(f):
            files.append(f)
            if fixture:
                problems.append(f"{f.name}: CI = True, but {fixture}")
            if declared:
                problems.append(f"{f.name}: CI = True and CI_EXCLUDED at once")
        elif declared:
            excluded.append((f.name, declared))
        elif fixture:
            excluded.append((f.name, f"needs the default fixture project, which CI has no copy of ({fixture})"))
        else:
            problems.append(f"{f.name}: left out of CI for no reason: it needs no default fixture; declare "
                            "CI = True, or CI_EXCLUDED = \"why\" (e.g. long-running, in the nightly full suite)")
    return files, problems, excluded


# ---------------------------------------------------------------------------
# Driver


class Chunk:
    """What one worker process runs with one editor: a file, or some of an independent file's tests."""

    def __init__(self, file: Path, tests: list[str] | None = None, seconds: float | None = None, lane: str = "",
                 label: str = ""):
        self.file, self.tests, self.seconds, self.lane = file, tests, seconds, lane
        self.label = label or file.name


def _files_matching(files: list[Path], filt: list[str]) -> list[Path]:
    """The files with a test -k selects: a worker costs an interpreter and the harness's import, which
    a file with nothing to run should not. Listed in a process of its own, since some files set the
    environment as they load; when a file cannot load, every file stays and its worker reports it."""
    cmd = [sys.executable, str(Path(__file__).resolve()), "--list"]
    for k in filt:
        cmd += ["-k", k]
    p = subprocess.run(cmd + [str(f) for f in files], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    if p.returncode != 0:
        return files
    names = {line.split("::", 1)[0] for line in p.stdout.splitlines() if "::" in line}
    return [f for f in files if f.name in names]


def _gpu_env() -> dict[str, str]:
    """JASPER_MCP_GPU_LANE=1 (a run under tools/gpu/env.sh on the GPU server): the variables that
    put an editor on the NVIDIA GPU, which only the chunks of the lane "gpu" get."""
    if os.environ.get("JASPER_MCP_GPU_LANE") != "1":
        return {}
    return {k: os.environ[k] for k in ("DISPLAY", "VK_ICD_FILENAMES", "__EGL_VENDOR_LIBRARY_FILENAMES",
                                       "__GL_SYNC_TO_VBLANK", "vblank_mode") if k in os.environ}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="test files (default: tools/mcp/tests/test_*.py)")
    ap.add_argument("-k", action="append", help="only tests whose file::name contains this (repeatable)")
    ap.add_argument("-j", type=int, default=1, help="test files (chunks) to run in parallel")
    ap.add_argument("--fake", action="store_true", help="use tools/mcp/fake_server.py instead of the editor")
    ap.add_argument("--xvfb", choices=["editor", "harness", "none"], help="who provides the X display")
    ap.add_argument("--url", help="attach to a running editor over Streamable HTTP")
    ap.add_argument("--keep", action="store_true", help="keep every scratch directory")
    ap.add_argument("--list", action="store_true", help="list the tests and exit")
    ap.add_argument("--suite", choices=["ci", "all"], default="all",
                    help="ci: only the files that declare CI = True (default: all, or the given files)")
    ap.add_argument("--json", type=Path, help="write every test result (name, status, detail, seconds) here")
    ap.add_argument("--plan", type=Path, help="run these chunks (tools/mcp/shard.py plan's plan-<shard>.json) "
                    "instead of whole files")
    ap.add_argument("--split-seconds", type=float, default=0.0,
                    help="split the files that declare TESTS_INDEPENDENT into chunks of about this many seconds "
                    "(by the earlier runs' test times), each with an editor of its own")
    ap.add_argument("--tests-dir", type=Path, default=TESTS, help=argparse.SUPPRESS)  # selftest.py
    ap.add_argument("--worker", help=argparse.SUPPRESS)
    ap.add_argument("--results", help=argparse.SUPPRESS)
    ap.add_argument("--only", help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.worker:
        return worker_main(args)

    excluded: list[tuple[str, str]] = []
    chunks: list[Chunk] = []
    if args.plan:
        if args.files or args.k or args.suite == "ci":
            print("run_tests: --plan names the tests itself (no files, -k or --suite ci with it)", file=sys.stderr)
            return 2
        plan = json.loads(args.plan.read_text())
        for c in plan["chunks"]:
            chunks.append(Chunk(args.tests_dir / c["file"], list(c["tests"]), float(c.get("seconds") or 0.0),
                                c.get("lane", ""), f"{c['file']} [chunk {c.get('chunk', '1/1')}"
                                f"{', GPU' if c.get('lane') == 'gpu' else ''}]"))
        files = list(dict.fromkeys(c.file for c in chunks))
        print(f"run_tests: plan {args.plan}: {len(chunks)} chunk(s) of {len(files)} file(s), "
              f"{sum(len(c.tests or []) for c in chunks)} test(s)", flush=True)
    elif args.suite == "ci":
        files, problems, excluded = ci_files(args.tests_dir)
        if args.files:
            wanted = {Path(f).resolve() for f in args.files}
            files = [f for f in files if f.resolve() in wanted]
            excluded = [(name, why) for name, why in excluded if (args.tests_dir / name).resolve() in wanted]
            # (every problem names its file first: another file's says nothing about this run)
            problems = [p for p in problems if (args.tests_dir / p.split(":", 1)[0]).resolve() in wanted]
        for problem in problems:
            print(f"run_tests: {problem}", file=sys.stderr)
        if problems:
            return 2
        if not files:
            print("run_tests: no CI = True test file", file=sys.stderr)
            return 2
        # what the suite filter leaves out, said every time (MCP_SUITE=all runs them)
        print(f"run_tests: --suite ci runs {len(files)} test file(s) and leaves out {len(excluded)}"
              f"{':' if excluded else ''}", flush=True)
        for name, why in excluded:
            print(f"  left out {name}: {why}", flush=True)
    else:
        files = [Path(f).resolve() for f in args.files] or sorted(TESTS.glob("test_*.py"))
    if args.list:
        for f in files:
            _, tests = _collect(f, args.k or [])
            for n, _ in tests:
                print(f"{f.name}::{n}")
        return 0
    if args.k and not chunks:
        files = _files_matching(files, args.k)

    # -j N: the longest files first (by the last unfiltered run's times; files without one first,
    # in their order), so no long file starts last and alone. Each file runs in a process and
    # scratch directory of its own: the order changes nothing a test sees.
    seconds: dict[str, float] = {}
    record_seconds = not (args.fake or args.url or args.k or args.plan or args.split_seconds)
    with contextlib.suppress(OSError, ValueError, AttributeError):
        seconds = {str(k): float(v) for k, v in json.loads(FILE_SECONDS.read_text()).items()}
    test_seconds: dict[str, float] = {}
    with contextlib.suppress(OSError, ValueError, AttributeError):
        test_seconds = {str(k): float(v) for k, v in json.loads(TEST_SECONDS.read_text()).items()}
    if not chunks:
        if args.split_seconds > 0 and not (args.k or args.url):
            import shard
            for f in files:
                if not shard.independent(f):
                    chunks.append(Chunk(f))
                    continue
                names = [f"{f.name}::{n}" for n, _ in _collect_names(f)]
                for c in shard.make_chunks(names, test_seconds, args.split_seconds, f.parent):
                    chunks.append(Chunk(f, c.tests if c.of > 1 else None, c.seconds, "",
                                        f"{c.file} [chunk {c.index + 1}/{c.of}]" if c.of > 1 else c.file))
            print(f"run_tests: {len(chunks)} chunk(s) of {len(files)} file(s) (--split-seconds {args.split_seconds:.0f}: "
                  "the TESTS_INDEPENDENT files split by the earlier runs' test times)", flush=True)
        else:
            chunks = [Chunk(f) for f in files]
    if args.j > 1 and (seconds or any(c.seconds for c in chunks)):
        def length(c: Chunk) -> float:
            if c.seconds:
                return c.seconds
            return seconds.get(c.file.name, float("inf"))
        chunks.sort(key=lambda c: -length(c))
        print(f"run_tests: the longest first, by the earlier runs' times ({FILE_SECONDS})", flush=True)

    # One build of each prebuilt player runtime per run (tools/mcp/build_cache.py), shared by every
    # worker: started now, and a file that needs one starts once it is built (the others run
    # meanwhile), so no worker waits for a build
    os.environ.setdefault("JASPER_MCP_SESSION", f"{os.getpid()}-{int(time.time())}")
    prewarm = None
    ready: dict[str, threading.Event] = {}
    if not (args.fake or args.url or args.k) and os.environ.get("JASPER_SLOW_BUILD", "") not in ("", "0"):
        wanted = prebuilt_players(files)
        if wanted:
            import build_cache
            print(f"run_tests: building the prebuilt player runtime(s) {', '.join(wanted)} as the run starts "
                  f"(tools/mcp/build_cache.py, {build_cache.shared_root() or build_cache.cache_root()})", flush=True)
            ready = {name: threading.Event() for name in wanted}
            prewarm = threading.Thread(target=build_cache.prewarm, args=(wanted, ready), daemon=True)
            prewarm.start()

    tmp = Path(tempfile.mkdtemp(prefix="jasper-mcp-results-"))
    # the editors at once, for their share of the CPUs (harness.render_threads)
    os.environ.setdefault("JASPER_MCP_WORKERS", str(max(1, args.j)))
    gpu_env = _gpu_env()
    if any(c.lane == "gpu" for c in chunks) and not gpu_env:
        print("run_tests: the plan has GPU chunks, but this run is not on the GPU lane (JASPER_MCP_GPU_LANE=1 "
              "under tools/gpu/env.sh); they run on llvmpipe", flush=True)
    # one X display of the GPU's per GPU chunk at a time (JASPER_MCP_GPU_DISPLAYS=":96,:95"): editors
    # that share a display could take each other's input
    gpu_displays = [d for d in os.environ.get("JASPER_MCP_GPU_DISPLAYS", gpu_env.get("DISPLAY", "")).split(",") if d]

    file_seconds: dict[str, float] = {}

    def run(chunk: Chunk, index: int, display: str = "") -> tuple[Chunk, int, str, list]:
        f = chunk.file
        started = time.monotonic()
        res = tmp / f"{index:04d}-{f.stem}.json"
        cmd = [sys.executable, str(Path(__file__).resolve()), "--worker", str(f), "--results", str(res)]
        for k in args.k or []:
            cmd += ["-k", k]
        if chunk.tests is not None:
            only = tmp / f"{index:04d}-{f.stem}.only.json"
            only.write_text(json.dumps(chunk.tests))
            cmd += ["--only", str(only)]
        for flag, val in (("--fake", args.fake), ("--keep", args.keep)):
            if val:
                cmd.append(flag)
        if args.xvfb:
            cmd += ["--xvfb", args.xvfb]
        if args.url:
            cmd += ["--url", args.url]
        env = None
        # JASPER_MCP_COVERAGE_DIR (tools/mcp/coverage_map.py): a coverage-instrumented editor's counts,
        # per file and chunk (GCOV_PREFIX: <dir>/<file stem>-<n>/<the objects' own paths>)
        if os.environ.get("JASPER_MCP_COVERAGE_DIR"):
            env = dict(os.environ, GCOV_PREFIX=str(Path(os.environ["JASPER_MCP_COVERAGE_DIR"]) / f"{f.stem}-{index}"),
                       GCOV_PREFIX_STRIP="0")
        if gpu_env:
            # the lane's editors on the NVIDIA GPU, every other one on llvmpipe as anywhere: each
            # editor checks it renders where its chunk says (harness.Editor._check_device)
            env = {k: v for k, v in (env or os.environ).items() if k not in gpu_env}
            env["JASPER_MCP_EXPECT_DEVICE"] = "llvmpipe"
            if chunk.lane == "gpu":
                env.update(gpu_env, JASPER_XVFB="none", DISPLAY=display or gpu_env.get("DISPLAY", ""),
                           JASPER_MCP_GPU_DISPLAY=display or gpu_env.get("DISPLAY", ""),
                           JASPER_MCP_EXPECT_DEVICE="NVIDIA")
        if args.j <= 1:
            p = subprocess.run(cmd, env=env)
            out = ""
        else:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
            out = p.stdout
        results = json.loads(res.read_text()) if res.exists() else []
        if chunk.tests is None:
            file_seconds[f.name] = round(time.monotonic() - started, 1)
        return chunk, p.returncode, out, results

    # -j N: each worker takes the first chunk whose runtimes are built (or, when only chunks that
    # wait for one are left, the first of those); a GPU chunk when one of the GPU's displays is free
    needs = {c.file: [n for n in prebuilt_players([c.file]) if n in ready] for c in chunks} if ready else {}
    queue = list(enumerate(chunks))
    queue_lock = threading.Lock()
    free_displays = list(gpu_displays)

    def next_chunk() -> tuple[int, Chunk, str] | None:
        while True:
            with queue_lock:
                if not queue:
                    return None
                eligible = [(i, item) for i, item in enumerate(queue)
                            if not (gpu_env and item[1].lane == "gpu" and gpu_displays and not free_displays)]
                for i, (index, c) in eligible:
                    if all(ready[n].is_set() for n in needs.get(c.file, ())):
                        break
                else:
                    i, (index, c) = eligible[0] if eligible else (None, (None, None))
                if c is not None:
                    queue.pop(i)
                    display = free_displays.pop(0) if gpu_env and c.lane == "gpu" and gpu_displays else ""
                    return index, c, display
            time.sleep(1.0)   # only GPU chunks left, every display taken: one comes free

    def parallel():
        done: list = []
        cond = threading.Condition()

        def worker():
            while (picked := next_chunk()) is not None:
                index, c, display = picked
                try:
                    item = run(c, index, display)
                except Exception:  # noqa: BLE001  (a crashed worker, reported as one)
                    item = (c, 1, traceback.format_exc(), [])
                finally:
                    if display:
                        with queue_lock:
                            free_displays.append(display)
                with cond:
                    done.append(item)
                    cond.notify()

        threads = [threading.Thread(target=worker, daemon=True) for _ in range(max(1, args.j))]
        for t in threads:
            t.start()
        for _ in range(len(chunks)):
            with cond:
                cond.wait_for(lambda: done)
                item = done.pop(0)
            yield item

    def serial():
        for index, c in enumerate(chunks):
            yield run(c, index, gpu_displays[0] if gpu_env and c.lane == "gpu" and gpu_displays else "")

    all_results = []
    crashed = []
    t0 = time.monotonic()
    for chunk, rc, out, results in serial() if args.j <= 1 else parallel():
        if out:
            print(f"== {chunk.label}\n{out.rstrip()}", flush=True)
        if rc != 0:
            crashed.append(f"{chunk.label} (worker exit {rc})")
        all_results += results

    if prewarm is not None:
        prewarm.join()
    if record_seconds and file_seconds:
        with contextlib.suppress(OSError):
            FILE_SECONDS.parent.mkdir(parents=True, exist_ok=True)
            FILE_SECONDS.write_text(json.dumps({**seconds, **file_seconds}, indent=1, sort_keys=True))
    # every test's own time, whatever the run's filter (what --split-seconds and shard.py plan with)
    if all_results and not (args.fake or args.url):
        with contextlib.suppress(OSError):
            TEST_SECONDS.parent.mkdir(parents=True, exist_ok=True)
            TEST_SECONDS.write_text(json.dumps({**test_seconds, **{r["name"]: r["seconds"] for r in all_results}},
                                               indent=1, sort_keys=True))
    shutil.rmtree(tmp, ignore_errors=True)
    n = {s: sum(1 for r in all_results if r["status"] == s) for s in ("pass", "fail", "skip")}
    print("\n== summary")
    for r in all_results:
        if r["status"] == "fail":
            print(f"FAIL {r['name']}")
    for c in crashed:
        print(f"CRASH {c}")
    elapsed = time.monotonic() - t0
    print(f"{n['pass']} passed, {n['fail']} failed, {n['skip']} skipped"
          f"{', ' + str(len(crashed)) + ' crashed' if crashed else ''} in {elapsed:.1f}s"
          f"{f' ({len(excluded)} test file(s) left out by --suite ci)' if args.suite == 'ci' else ''}")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({
            "suite": args.suite, "files": [f.name for f in files], "chunks": len(chunks),
            "plan": str(args.plan) if args.plan else None,
            "left_out": [{"file": name, "reason": why} for name, why in excluded], "passed": n["pass"], "failed": n["fail"],
            "skipped": n["skip"], "crashed": crashed, "seconds": round(elapsed, 1), "results": all_results,
        }, indent=1))
    if not all_results and not crashed:
        print("run_tests: no test ran", file=sys.stderr)
        return 1
    return 1 if n["fail"] or crashed else 0


def _collect_names(path: Path) -> list[tuple[str, object]]:
    """A file's tests in run order, without running anything (an import of the file)."""
    return _collect(path, [])[1]


if __name__ == "__main__":
    sys.exit(main())
