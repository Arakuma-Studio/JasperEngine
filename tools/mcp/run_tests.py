"""Run the MCP tests: tools/mcp/tests/test_*.py.

    tools/mcp/run-tests.sh                      # every test file against the real editor
    tools/mcp/run-tests.sh -k undo              # tests whose "file::name" contains "undo"
    tools/mcp/run-tests.sh -j 4                 # 4 test files at a time (one editor each)
    tools/mcp/run-tests.sh tests/test_protocol.py
    tools/mcp/run-tests.sh --fake tools/mcp/tests/test_protocol.py   # fake-server protocol checks
    tools/mcp/run-tests.sh --url http://127.0.0.1:47615/mcp   # against an editor already running
    tools/mcp/run-tests.sh --suite ci -j 2      # the CI suite: the files that declare CI = True

A test is a module-level `async def test_xxx(ed)` (ed: harness.Editor) or `async def test_xxx()`.
Tests of a file run in source order and share one editor, started on a fresh scratch copy of the
fixture project (module variable EDITOR = dict(...) passes Editor arguments, e.g.
EDITOR = {"create": True}). If a test leaves the editor unresponsive, the next test gets a new one.
Call harness.skip("why") or ed.require_tools(...) to skip. Module variable TIMEOUT sets the
per-test hang guard in seconds (default 600): a test fails when no tool call has been in flight
for that long (a slow but working editor, such as one waiting for game frames on a loaded machine,
never trips it; each call has its own timeout, harness.Editor.call_raw).

A file joins the CI suite (--suite ci, the CI workflow's e2e job) by declaring CI = True at module
level. CI has no copy of the default fixture (build/fixture-unity), so such a file must not need it:
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
import concurrent.futures
import contextlib
import importlib.util
import inspect
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESTS = HERE / "tests"
sys.path.insert(0, str(HERE))


# ---------------------------------------------------------------------------
# Worker: runs one file


def _collect(path: Path, filt: list[str]):
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


async def _run_file(path: Path, filt: list[str], results_path: Path) -> None:
    import harness

    mod, tests = _collect(path, filt)
    opts = dict(getattr(mod, "EDITOR", {}) or {})
    timeout = float(getattr(mod, "TIMEOUT", 600))
    ed: harness.Editor | None = None
    ed_error: str | None = None
    results = []

    async def get_editor() -> harness.Editor:
        nonlocal ed, ed_error
        if ed is not None:
            return ed
        e = harness.Editor(label=path.stem, **opts)
        try:
            await e.__aenter__()
        except Exception as ex:  # noqa: BLE001
            ed_error = f"editor failed to start: {ex}"
            raise
        ed = e
        return ed

    for name, fn in tests:
        full = f"{path.name}::{name}"
        t0 = time.monotonic()
        status, detail = "pass", ""
        wants_ed = len(inspect.signature(fn).parameters) > 0
        try:
            if wants_ed:
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
    results_path.write_text(json.dumps(results))


def worker_main(args) -> int:
    for k, v in (("JASPER_FAKE", "1" if args.fake else None), ("JASPER_XVFB", args.xvfb),
                 ("JASPER_KEEP_SCRATCH", "1" if args.keep else None), ("JASPER_MCP_URL", args.url)):
        if v:
            os.environ[k] = v
    asyncio.run(_run_file(Path(args.worker), args.k or [], Path(args.results)))
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


def ci_files(tests: Path = TESTS) -> tuple[list[Path], list[str]]:
    """The CI = True files, and the problems that keep them out of CI."""
    files = [f for f in sorted(tests.glob("test_*.py")) if is_ci_file(f)]
    problems = [f"{f.name}: CI = True, but {reason}" for f in files if (reason := default_fixture_reason(f))]
    return files, problems


# ---------------------------------------------------------------------------
# Driver


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="test files (default: tools/mcp/tests/test_*.py)")
    ap.add_argument("-k", action="append", help="only tests whose file::name contains this (repeatable)")
    ap.add_argument("-j", type=int, default=1, help="test files to run in parallel")
    ap.add_argument("--fake", action="store_true", help="use tools/mcp/fake_server.py instead of the editor")
    ap.add_argument("--xvfb", choices=["editor", "harness", "none"], help="who provides the X display")
    ap.add_argument("--url", help="attach to a running editor over Streamable HTTP")
    ap.add_argument("--keep", action="store_true", help="keep every scratch directory")
    ap.add_argument("--list", action="store_true", help="list the tests and exit")
    ap.add_argument("--suite", choices=["ci", "all"], default="all",
                    help="ci: only the files that declare CI = True (default: all, or the given files)")
    ap.add_argument("--json", type=Path, help="write every test result (name, status, detail, seconds) here")
    ap.add_argument("--tests-dir", type=Path, default=TESTS, help=argparse.SUPPRESS)  # selftest.py
    ap.add_argument("--worker", help=argparse.SUPPRESS)
    ap.add_argument("--results", help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.worker:
        return worker_main(args)

    if args.suite == "ci":
        files, problems = ci_files(args.tests_dir)
        if args.files:
            wanted = {Path(f).resolve() for f in args.files}
            files = [f for f in files if f.resolve() in wanted]
        for problem in problems:
            print(f"run_tests: {problem}", file=sys.stderr)
        if problems:
            return 2
        if not files:
            print("run_tests: no CI = True test file", file=sys.stderr)
            return 2
    else:
        files = [Path(f).resolve() for f in args.files] or sorted(TESTS.glob("test_*.py"))
    if args.list:
        for f in files:
            _, tests = _collect(f, args.k or [])
            for n, _ in tests:
                print(f"{f.name}::{n}")
        return 0

    tmp = Path(tempfile.mkdtemp(prefix="jasper-mcp-results-"))

    def run(f: Path) -> tuple[Path, int, str, list]:
        res = tmp / (f.stem + ".json")
        cmd = [sys.executable, str(Path(__file__).resolve()), "--worker", str(f), "--results", str(res)]
        for k in args.k or []:
            cmd += ["-k", k]
        for flag, val in (("--fake", args.fake), ("--keep", args.keep)):
            if val:
                cmd.append(flag)
        if args.xvfb:
            cmd += ["--xvfb", args.xvfb]
        if args.url:
            cmd += ["--url", args.url]
        if args.j <= 1:
            p = subprocess.run(cmd)
            out = ""
        else:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            out = p.stdout
        results = json.loads(res.read_text()) if res.exists() else []
        return f, p.returncode, out, results

    all_results = []
    crashed = []
    t0 = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max(1, args.j)) as pool:
        for f, rc, out, results in pool.map(run, files) if args.j <= 1 else \
                (fut.result() for fut in concurrent.futures.as_completed([pool.submit(run, f) for f in files])):
            if out:
                print(f"== {f.name}\n{out.rstrip()}", flush=True)
            if rc != 0:
                crashed.append(f"{f.name} (worker exit {rc})")
            all_results += results

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
          f"{', ' + str(len(crashed)) + ' crashed' if crashed else ''} in {elapsed:.1f}s")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({
            "suite": args.suite, "files": [f.name for f in files], "passed": n["pass"], "failed": n["fail"],
            "skipped": n["skip"], "crashed": crashed, "seconds": round(elapsed, 1), "results": all_results,
        }, indent=1))
    if not all_results and not crashed:
        print("run_tests: no test ran", file=sys.stderr)
        return 1
    return 1 if n["fail"] or crashed else 0


if __name__ == "__main__":
    sys.exit(main())
