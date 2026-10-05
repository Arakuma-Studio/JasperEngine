"""Self-test of the harness against tools/mcp/fake_server.py (no editor needed).

    uv run --no-project --with mcp --with pyyaml python tools/mcp/selftest.py

Checks: stdio launch in both Xvfb modes (editor-spawned and harness-spawned), scratch layout
(cwd, HOME, settings.yaml, LIBGL_ALWAYS_SOFTWARE, stderr capture), call/ToolError/expect_error,
screenshot saving, console cursor, batch, Streamable HTTP attach, the runner (pass, -k, -j,
failure exit code and kept scratch), the stdout purity check catching a dirty server, and that
no Xvfb is left behind.
"""

import asyncio
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from harness import Editor, ToolError, fake_editor_command  # noqa: E402

checks = 0


def ok(cond, msg):
    global checks
    checks += 1
    if not cond:
        raise SystemExit(f"SELFTEST FAIL: {msg}")
    print(f"ok  {msg}")


def xvfb_pids() -> set[str]:
    r = subprocess.run(["pgrep", "-x", "Xvfb"], capture_output=True, text=True)
    return set(r.stdout.split())


async def stdio_checks():
    before = xvfb_pids()
    for mode in ("editor", "harness"):
        async with Editor(fake=True, xvfb=mode) as ed:
            st = await ed.call("editor_status")
            root = ed.scratch.root
            ok(st["cwd"] == str(root), f"[{mode}] cwd is the scratch dir")
            ok(st["home"] == str(root / "home"), f"[{mode}] HOME is scratch/home")
            ok(st["project"] == str(root / "project") and (root / "project" / "project.yaml").exists(),
               f"[{mode}] fixture copied to scratch/project and passed with --project")
            ok("last_project_path: " + str(root / "project") in st["settings_yaml"]
               and "maximized: true" in st["settings_yaml"], f"[{mode}] settings.yaml written")
            ok(st["libgl_software"] == "1", f"[{mode}] LIBGL_ALWAYS_SOFTWARE=1")
            ok(st["display"] and st["display"].startswith(":"), f"[{mode}] DISPLAY set ({st['display']})")
            ok("--headless" in st["argv"], f"[{mode}] --headless passed")
            ok("serving stdio" in ed.stderr_tail(), f"[{mode}] stderr captured to editor.stderr.log")
            try:
                await ed.call("fail_always")
                ok(False, "ToolError raised")
            except ToolError as e:
                ok("Valid choices" in e.message, f"[{mode}] ToolError carries the tool's message")
            msg = await ed.call("fail_always", expect_error=True)
            ok("Valid choices" in msg, f"[{mode}] expect_error returns the message")
            png = await ed.screenshot(view="scene", path=root / "shots" / "s.png")
            ok(png.startswith(b"\x89PNG") and (root / "shots" / "s.png").read_bytes() == png,
               f"[{mode}] screenshot saved as PNG")
            first = await ed.console_since(None)
            await ed.call("create_entity", name="C1", type="box")
            new = await ed.console_since()
            ok(len(first) >= 1 and [e["message"] for e in new] == ["Created C1"], f"[{mode}] console cursor")
            ok("stdout noise from create_entity C1" in ed.stderr_text(), f"[{mode}] server stdout noise lands in stderr log")
            kept = root
        ok(not kept.exists(), f"[{mode}] scratch removed after success")
    # a failing block keeps the scratch dir
    try:
        async with Editor(fake=True) as ed:
            kept = ed.scratch.root
            ed.check(False, "deliberate")
    except AssertionError as e:
        ok("editor stderr tail" in str(e), "check() failure carries the stderr tail")
    ok(kept.exists(), "scratch kept after failure")
    import shutil
    shutil.rmtree(kept)
    async with Editor(fake=True, create=True, name="Fresh") as ed:
        st = await ed.call("editor_status")
        ok("--create" in st["argv"] and "Fresh" in st["argv"], "create=True passes --create <dir> --name")
    time.sleep(0.5)
    ok(xvfb_pids() <= before, "no Xvfb left running")


async def http_checks():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    proc = subprocess.Popen(fake_editor_command() + ["mcp", "--http-port", str(port)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), 0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        async with Editor(url=f"http://127.0.0.1:{port}/mcp") as ed:
            ok(ed.instructions, "HTTP: initialize instructions")
            made = await ed.call("create_entity", name="H", type="box")
            ok(made["entity_id"] >= 100, "HTTP: tool call")
            ok(len(await ed.screenshot(path=Path("/tmp") / f"jasper-selftest-{port}.png")) > 8, "HTTP: screenshot")
            ok(ed.stderr_tail() == "", "HTTP: no stderr")
    finally:
        proc.terminate()
        proc.wait()


def runner_checks():
    run = [sys.executable, str(HERE / "run_tests.py"), "--fake"]
    r = subprocess.run(run + ["-j", "2"], capture_output=True, text=True)
    ok(r.returncode == 0 and "0 failed" in r.stdout, "runner: test_protocol passes on the fake with -j 2")
    r = subprocess.run(run + ["-k", "ping"], capture_output=True, text=True)
    ok(r.returncode == 0 and "1 passed, 0 failed, 0 skipped" in r.stdout, "runner: -k filters")
    env = dict(os.environ, FAKE_DIRTY_STDOUT="1")
    r = subprocess.run(run + ["-k", "stdio_raw"], capture_output=True, text=True, env=env)
    ok(r.returncode == 1 and "non-JSON-RPC stdout" in r.stdout and "Loading project" in r.stdout,
       "runner: stdout purity test catches a dirty server, exit code 1")
    ok("scratch kept:" in r.stderr, "runner: failed test keeps its scratch dir")
    for line in r.stderr.splitlines():
        if "scratch kept:" in line:
            import shutil
            shutil.rmtree(line.split("scratch kept:")[1].strip(), ignore_errors=True)


async def main():
    await stdio_checks()
    await http_checks()
    runner_checks()
    print(f"\nselftest: {checks} checks passed")


asyncio.run(main())
