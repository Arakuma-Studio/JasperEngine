"""Run the MCP tests: tools/mcp/tests/test_*.py.

    tools/mcp/run-tests.sh                      # every test file against the real editor
    tools/mcp/run-tests.sh -k undo              # tests whose "file::name" contains "undo"
    tools/mcp/run-tests.sh -j 4                 # 4 test files at a time (one editor each)
    tools/mcp/run-tests.sh tests/test_protocol.py
    tools/mcp/run-tests.sh --fake tools/mcp/tests/test_protocol.py   # fake-server protocol checks
    JASPER_MCP_TOKEN=<token> tools/mcp/run-tests.sh --url http://127.0.0.1:47615/mcp   # against an editor already running

A test is a module-level `async def test_xxx(ed)` (ed: harness.Editor) or `async def test_xxx()`.
Tests of a file run in source order and share one editor using the module's owned project setup
(module variable EDITOR = dict(...) passes Editor arguments, e.g. EDITOR = {"create": True}).
An explicit --url overrides that launch setup. If a test leaves the editor unresponsive,
the next test gets a new one.
Call harness.skip("why") or ed.require_tools(...) to skip. Module variable TIMEOUT sets the
per-test hang guard in seconds (default 600): a test fails when no tool call has been in flight
for that long (a slow but working editor, such as one waiting for game frames on a loaded machine,
never trips it; each call has its own timeout, harness.Editor.call_raw).

Each file runs in its own Python process. Exit code: 0 when nothing failed.
"""

from __future__ import annotations

import argparse
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


async def _run_file(path: Path, filt: list[str], results_path: Path, url: str | None = None) -> None:
    import harness

    mod, tests = _collect(path, filt)
    opts = dict(getattr(mod, "EDITOR", {}) or {})
    if url is not None:
        opts["url"] = url
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
    asyncio.run(_run_file(Path(args.worker), args.k or [], Path(args.results), args.url))
    return 0


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
    ap.add_argument("--worker", help=argparse.SUPPRESS)
    ap.add_argument("--results", help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.worker:
        return worker_main(args)

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
    print(f"{n['pass']} passed, {n['fail']} failed, {n['skip']} skipped"
          f"{', ' + str(len(crashed)) + ' crashed' if crashed else ''} in {time.monotonic() - t0:.1f}s")
    return 1 if n["fail"] or crashed else 0


if __name__ == "__main__":
    sys.exit(main())
