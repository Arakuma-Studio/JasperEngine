"""A project whose project.yaml does not parse is a clean error for every opener.

Project::loadProject's error path refreshes the Project window, whose thumbnail cleanup walked the
project directory with a throwing iterator. For the first project of a run there is no project path
yet, so it threw from inside the catch: the GUI aborted at startup when its last project was broken,
and `mcp --project` reported a filesystem error instead of the parse error.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

import harness
from harness import Editor
from scene_history import SCENE_INPUTS

TIMEOUT = 600
CI = True
BROKEN = "name: [unclosed\n"
PARSE_ERROR = "Failed to load project YAML"


def check(cond, message: str) -> None:
    if not cond:
        raise AssertionError(message)


def broken_copy(root: Path) -> Path:
    project = root / "broken"
    shutil.copytree(SCENE_INPUTS, project, ignore=shutil.ignore_patterns(".git", ".jasperengine"))
    (project / "project.yaml").write_text(BROKEN)
    return project


async def test_gui_start_with_a_broken_last_project():
    """The GUI editor whose last project's project.yaml does not parse logs the parse error and keeps
    running (on a temporary project), instead of aborting"""
    scratch = harness.Scratch.make(SCENE_INPUTS)
    xvfb = harness.Xvfb()
    proc = None
    try:
        (scratch.project / "project.yaml").write_text(BROKEN)
        env = dict(os.environ)
        for key in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "WAYLAND_DISPLAY"):
            env.pop(key, None)
        env["DISPLAY"] = xvfb.start()
        env["HOME"] = str(scratch.home)
        env["LIBGL_ALWAYS_SOFTWARE"] = "1"
        out_log, err_log = scratch.root / "editor.stdout.log", scratch.root / "editor.stderr.log"
        # the editor's log: editor.log beside the settings.yaml it uses (the working directory's)
        editor_log = scratch.root / "editor.log"

        def logged() -> str:
            return editor_log.read_text(errors="replace") if editor_log.exists() else ""

        with open(out_log, "wb") as out, open(err_log, "wb") as err:
            # the GUI with no argument opens settings.yaml's last project (Scratch.make wrote it)
            proc = subprocess.Popen(harness.editor_command(), cwd=scratch.root, env=env, stdout=out, stderr=err)
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline and proc.poll() is None and PARSE_ERROR not in logged():
                await asyncio.sleep(0.5)
            # it has to be still running a while after the error (the abort came right after it)
            await asyncio.sleep(5)
            log, stderr = logged(), err_log.read_text(errors="replace")
            print(f"[open] GUI: running {proc.poll() is None}; editor.log tail:\n" + "\n".join(log.splitlines()[-6:]) +
                  "\nstderr tail:\n" + "\n".join(stderr.splitlines()[-6:]))
            check(PARSE_ERROR in log + stderr, "the parse error was logged: " + log[-2000:] + stderr[-2000:])
            check(proc.poll() is None, f"the editor still runs after the error (exit {proc.returncode}): " + stderr[-2000:])
            check("terminate called" not in stderr and "filesystem error" not in log + stderr, "no exception escaped: " + stderr[-2000:])
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        xvfb.stop()
        scratch.remove()


async def test_mcp_project_that_does_not_parse():
    """`mcp --project` on a project.yaml that does not parse: the session starts (on a temporary
    project) and the editor's message is the parse error"""
    root = Path(tempfile.mkdtemp(prefix="jasper-open-"))
    try:
        async with Editor(project=broken_copy(root)) as ed:
            status = await ed.call("editor_status")
            log = ed.scratch.stderr_log.read_text(errors="replace")
            lines = [line for line in log.splitlines() if line.startswith("[mcp] could not open the project at")]
            print(f"[open] mcp: {lines}")
            check(len(lines) == 1 and PARSE_ERROR in lines[0] and "filesystem" not in lines[0], f"the open error: {lines}")
            check(Path(status["project"]["path"]) != ed.project, f"a temporary project instead: {status['project']}")
    finally:
        shutil.rmtree(root, ignore_errors=True)
