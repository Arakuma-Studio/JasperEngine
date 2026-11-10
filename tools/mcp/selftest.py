"""Self-test of the harness against tools/mcp/fake_server.py (no editor needed).

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/mcp/selftest.py

Checks: stdio launch in both Xvfb modes (editor-spawned and harness-spawned), scratch layout
(cwd, HOME, settings.yaml, LIBGL_ALWAYS_SOFTWARE, stderr capture), call/ToolError/expect_error,
screenshot saving, console cursor, batch, Streamable HTTP attach, the runner (pass, -k, -j,
--suite ci and its exit 2, failure exit code and kept scratch), the stdout purity check catching a
dirty server, and that no Xvfb is left behind.
"""

import asyncio
import json
import os
import socket
import subprocess
import sys
import tempfile
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


def xvfb_pid(ed: Editor, status: dict) -> int:
    if ed._xvfb:
        return ed._xvfb.proc.pid
    # -displayfd does not create an X lock file; find the fake editor's own child.
    return int(subprocess.check_output(["pgrep", "-P", str(status["pid"]), "-x", "Xvfb"], text=True))


async def stdio_checks(project):
    owned_xvfb = set()
    async with Editor(fake=True, xvfb="editor", project=project) as ed:
        st = await ed.call("editor_status")
        root = ed.scratch.root
        ok(st["cwd"] == str(root), "cwd is the scratch dir")
        ok(st["home"] == str(root / "home"), "HOME is scratch/home")
        ok(st["project"] == str(root / "project")
           and (root / "project" / "project.yaml").read_bytes() == (project / "project.yaml").read_bytes(),
           "fixture copied to scratch/project and passed with --project")
        ok("last_project_path: " + str(root / "project") in st["settings_yaml"]
           and "maximized: true" in st["settings_yaml"], "settings.yaml written")
        ok(st["libgl_software"] == "1", "LIBGL_ALWAYS_SOFTWARE=1")
        ok(st["display"] and st["display"].startswith(":"), "editor-spawned DISPLAY set")
        owned_xvfb.add(xvfb_pid(ed, st))
        ok("--headless" in st["argv"], "--headless passed")
        ok("serving stdio" in ed.stderr_tail(), "stderr captured to editor.stderr.log")
        try:
            await ed.call("fail_always")
            ok(False, "ToolError raised")
        except ToolError as e:
            ok("Valid choices" in e.message, "ToolError carries the tool's message")
        msg = await ed.call("fail_always", expect_error=True)
        ok("Valid choices" in msg, "expect_error returns the message")
        png = await ed.screenshot(view="scene", path=root / "shots" / "s.png")
        ok(png.startswith(b"\x89PNG") and (root / "shots" / "s.png").read_bytes() == png,
           "screenshot saved as PNG")
        first = await ed.console_since(None)
        await ed.call("create_entity", name="C1", type="box")
        new = await ed.console_since()
        ok(len(first) >= 1 and [e["message"] for e in new] == ["Created C1"], "console cursor")
        ok("stdout noise from create_entity C1" in ed.stderr_text(), "server stdout noise lands in stderr log")
        kept = root
    ok(not kept.exists(), "scratch removed after success")
    async with Editor(fake=True, xvfb="harness", create=True, name="Fresh") as ed:
        st = await ed.call("editor_status")
        owned_xvfb.add(xvfb_pid(ed, st))
        ok(st["display"] == ed._xvfb.display, "harness-spawned DISPLAY reaches server")
        ok("--create" in st["argv"] and "Fresh" in st["argv"], "create=True passes --create <dir> --name")
    # a failing block keeps the scratch dir
    try:
        async with Editor(fake=True, project=project) as ed:
            kept = ed.scratch.root
            owned_xvfb.add(xvfb_pid(ed, await ed.call("editor_status")))
            ed.check(False, "deliberate")
    except AssertionError as e:
        ok("editor stderr tail" in str(e), "check() failure carries the stderr tail")
    ok(kept.exists(), "scratch kept after failure")
    import shutil
    shutil.rmtree(kept)
    ok(all(not Path(f"/proc/{pid}").exists() for pid in owned_xvfb), "no self-test Xvfb left running")


async def http_checks():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    editor = Editor(url=f"http://127.0.0.1:{port}/mcp")
    proc = subprocess.Popen(fake_editor_command() + ["mcp", "--http-port", str(port)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + editor.startup_timeout
        while True:
            if proc.poll() is not None:
                raise RuntimeError(f"fake HTTP server exited with {proc.returncode} before listening")
            try:
                socket.create_connection(("127.0.0.1", port), 0.2).close()
                break
            except OSError as error:
                if time.monotonic() >= deadline:
                    raise TimeoutError("fake HTTP server did not start listening") from error
                await asyncio.sleep(0.1)
        with tempfile.TemporaryDirectory(prefix="jasper-selftest-") as shots:
            async with editor as ed:
                ok(ed.instructions, "HTTP: initialize instructions")
                made = await ed.call("create_entity", name="H", type="box")
                ok(made["entity_id"] >= 100, "HTTP: tool call")
                ok(len(await ed.screenshot(path=Path(shots) / "http.png")) > 8, "HTTP: screenshot")
                ok(ed.stderr_tail() == "", "HTTP: no stderr")
            remote = subprocess.run(
                [sys.executable, str(HERE / "run_tests.py"), "--url", f"http://127.0.0.1:{port}/mcp",
                 "-k", "test_initialize", str(HERE / "tests" / "test_protocol.py")],
                capture_output=True, text=True,
                env=dict(os.environ, JASPER_EDITOR=str(Path(shots) / "must-not-launch-editor")),
            )
            ok(remote.returncode == 0 and "1 passed, 0 failed, 0 skipped" in remote.stdout,
               "runner: explicit --url overrides a test module's owned project")
    finally:
        proc.terminate()
        proc.wait()


def runner_checks():
    runner = [sys.executable, str(HERE / "run_tests.py"), "--fake"]
    run = runner + [str(HERE / "tests" / "test_protocol.py")]
    r = subprocess.run(run + ["-k", "test_protocol.py::test_initialize",
                              "-k", "test_protocol.py::test_batch"], capture_output=True, text=True)
    ok(r.returncode == 0 and "0 failed" in r.stdout, "runner: selected tests pass on the fake")
    for test in ("test_initialize", "test_batch"):
        ok(f"PASS test_protocol.py::{test} (" in r.stdout, f"runner: {test} exercises the fake")
    with tempfile.TemporaryDirectory(prefix="jasper-runner-selftest-") as root:
        files = [Path(root) / f"parallel_{name}.py" for name in ("a", "b")]
        for path in files:
            path.write_text("""import asyncio
from pathlib import Path

TIMEOUT = 30

async def test_parallel():
    marker = Path(__file__).with_suffix('.ready')
    marker.touch()
    while len(list(marker.parent.glob('*.ready'))) < 2:
        await asyncio.sleep(0.01)
""")
        r = subprocess.run(runner + ["-j", "2", *map(str, files)], capture_output=True, text=True)
        ok(r.returncode == 0 and "2 passed, 0 failed, 0 skipped" in r.stdout,
           "runner: -j 2 runs two files concurrently")
        # the CI suite: CI = True files only; one that needs the default fixture stops the run (exit 2)
        tests = Path(root) / "ci"
        tests.mkdir()
        (tests / "test_ci_created.py").write_text('CI = True\nEDITOR = {"create": True}\n\nasync def test_ping(ed):\n    await ed.ping()\n')
        (tests / "test_ci_editorless.py").write_text("CI = True\n\nasync def test_nothing():\n    pass\n")
        (tests / "test_not_ci.py").write_text("async def test_ping(ed):\n    await ed.ping()\n")
        (tests / "test_long.py").write_text('CI_EXCLUDED = "long-running"\n\nasync def test_nothing():\n    pass\n')
        suite = runner + ["--suite", "ci", "--tests-dir", str(tests)]
        r = subprocess.run(suite, capture_output=True, text=True)
        ok(r.returncode == 0 and "2 passed, 0 failed, 0 skipped" in r.stdout and "PASS test_not_ci" not in r.stdout
           and "PASS test_long" not in r.stdout, "runner: --suite ci runs the CI = True files only")
        ok("--suite ci runs 2 test file(s) and leaves out 2:" in r.stdout and
           "left out test_not_ci.py: needs the default fixture" in r.stdout and
           "left out test_long.py: long-running" in r.stdout and "(2 test file(s) left out by --suite ci)" in r.stdout,
           "runner: --suite ci says which files it leaves out, and why")
        (tests / "test_unexplained.py").write_text("async def test_nothing():\n    pass\n")
        r = subprocess.run(suite, capture_output=True, text=True)
        ok(r.returncode == 2 and "test_unexplained.py: left out of CI for no reason" in r.stderr,
           "runner: --suite ci exits 2 on a file left out for no reason")
        # a run of named files answers for those files, not for the rest of the directory
        r = subprocess.run(suite + [str(tests / "test_ci_editorless.py")], capture_output=True, text=True)
        ok(r.returncode == 0 and "1 passed, 0 failed, 0 skipped" in r.stdout,
           f"runner: another file's problem does not stop a run of named files: {r.stderr[-200:]}")
        r = subprocess.run(suite + [str(tests / "test_unexplained.py")], capture_output=True, text=True)
        ok(r.returncode == 2 and "test_unexplained.py: left out of CI for no reason" in r.stderr,
           "runner: a named file left out for no reason still stops the run")
        (tests / "test_unexplained.py").unlink()
        (tests / "test_ci_default_fixture.py").write_text("CI = True\n\nasync def test_ping(ed):\n    await ed.ping()\n")
        r = subprocess.run(suite, capture_output=True, text=True)
        ok(r.returncode == 2 and "test_ci_default_fixture.py: CI = True" in r.stderr,
           "runner: --suite ci exits 2 on a CI = True file that relies on the default fixture")
        # chunks: a plan's tests, each chunk its own worker process; a planned test the file does not
        # have fails; --split-seconds splits a TESTS_INDEPENDENT file by the earlier test times
        chunked = Path(root) / "chunks"
        chunked.mkdir()
        (chunked / "test_free.py").write_text(
            'import os\nfrom pathlib import Path\nTESTS_INDEPENDENT = "each its own"\n'
            + "".join(f'\nasync def test_{n}():\n    print("PID", os.getpid())\n' for n in "abc"))
        plan = Path(root) / "plan.json"
        plan.write_text(json.dumps({"chunks": [{"file": "test_free.py", "tests": ["test_a", "test_c"], "seconds": 2},
                                               {"file": "test_free.py", "tests": ["test_b"], "seconds": 1}]}))
        planned = runner + ["--tests-dir", str(chunked), "--plan", str(plan), "-j", "2", "--json", str(Path(root) / "plan-results.json")]
        r = subprocess.run(planned, capture_output=True, text=True)
        pids = {line.split()[1] for line in r.stdout.splitlines() if line.startswith("PID")}
        ok(r.returncode == 0 and "3 passed, 0 failed, 0 skipped" in r.stdout and len(pids) == 2 and "[chunk" in r.stdout,
           f"runner: --plan runs each chunk's tests in a process of its own: {r.stdout[-300:]}")
        results = json.loads((Path(root) / "plan-results.json").read_text())
        ok(sorted(x["name"] for x in results["results"]) == [f"test_free.py::test_{n}" for n in "abc"],
           "runner: --plan's results name every planned test once")
        plan.write_text(json.dumps({"chunks": [{"file": "test_free.py", "tests": ["test_a", "test_gone"], "seconds": 2}]}))
        r = subprocess.run(planned, capture_output=True, text=True)
        ok(r.returncode == 1 and "FAIL test_free.py::test_gone" in r.stdout and "PASS test_free.py::test_b" not in r.stdout,
           "runner: a planned test the file does not have fails, and an unplanned one does not run")
        seconds = Path(root) / "test-seconds.json"
        seconds.write_text(json.dumps({f"test_free.py::test_{n}": 100.0 for n in "abc"}))
        env = dict(os.environ, JASPER_MCP_TEST_SECONDS=str(seconds))
        r = subprocess.run(runner + ["--split-seconds", "150", "-j", "2", str(chunked / "test_free.py")],
                           capture_output=True, text=True, env=env)
        ok(r.returncode == 0 and "3 passed, 0 failed, 0 skipped" in r.stdout and "[chunk 3/3]" in r.stdout,
           f"runner: --split-seconds splits a TESTS_INDEPENDENT file: {r.stdout[-300:]}")
    r = subprocess.run(run + ["-k", "test_protocol.py::test_initialize"], capture_output=True, text=True)
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
    # no CI runs --suite ci on every push, and one undeclared file stops the suite check-changed runs
    r = subprocess.run(runner + ["--suite", "ci", "--list"], capture_output=True, text=True)
    ok(r.returncode == 0, f"runner: every real test file declares its place in the CI suite: {r.stderr[-400:]}")


async def main():
    with tempfile.TemporaryDirectory(prefix="jasper-harness-fixture-") as fixture:
        project = Path(fixture)
        (project / "project.yaml").write_text("name: Harness\n")
        os.environ["JASPER_FIXTURE"] = fixture
        await stdio_checks(project)
        await http_checks()
        runner_checks()
    print(f"\nselftest: {checks} checks passed")


asyncio.run(main())
