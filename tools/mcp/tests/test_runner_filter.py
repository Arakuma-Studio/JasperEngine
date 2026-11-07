"""run_tests.py -k starts a worker only for the files with a matching test (a worker costs seconds,
and the suite has hundreds of files), without loading those files into its own process, whose
environment every worker inherits. A file that cannot load still gets reported, and a filter that
matches nothing still fails.
"""

from __future__ import annotations

import asyncio
import shutil
import sys
import tempfile
from pathlib import Path

import harness
from harness import TestFailure

CI = True

RUNNER = harness.REPO / "tools" / "mcp" / "run_tests.py"
LEAK = "JASPER_RUNNER_FILTER_TEST_LEAK"
MATCHING = f"""
import os
async def test_wanted():
    assert "{LEAK}" not in os.environ, "another file's environment reached this worker"
"""
OTHER = """
import os, sys
from pathlib import Path
os.environ["{leak}"] = "1"
if "--worker" in sys.argv:
    (Path(__file__).parent / "worker-started").write_text("a worker ran a file with nothing to run")
async def test_other():
    pass
"""


async def run(root: Path, *args: str) -> tuple[int, str]:
    files = [str(f) for f in sorted(root.glob("test_*.py"))]
    proc = await asyncio.create_subprocess_exec(sys.executable, str(RUNNER), *args, *files,
                                                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    out, _ = await proc.communicate()
    return proc.returncode, out.decode(errors="replace")


def scratch() -> Path:
    root = Path(tempfile.mkdtemp(prefix="jasper-runner-filter-"))
    (root / "test_a_matching.py").write_text(MATCHING)
    (root / "test_b_other.py").write_text(OTHER.format(leak=LEAK))
    return root


async def test_only_matching_files_get_a_worker():
    root = scratch()
    try:
        code, out = await run(root, "-k", "test_wanted")
        if code != 0 or "PASS test_a_matching.py::test_wanted" not in out:
            raise TestFailure(f"the matching test should pass: exit {code}\n{out}")
        if (root / "worker-started").exists():
            raise TestFailure(f"a file with no matching test got a worker:\n{out}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


async def test_a_file_that_cannot_load_is_reported():
    root = scratch()
    try:
        (root / "test_c_broken.py").write_text("raise RuntimeError('this file cannot load')\n")
        code, out = await run(root, "-k", "test_wanted")
        if code == 0 or "CRASH" not in out or "test_c_broken.py" not in out:
            raise TestFailure(f"the broken file must fail the run: exit {code}\n{out}")
    finally:
        shutil.rmtree(root, ignore_errors=True)


async def test_a_filter_matching_nothing_fails():
    root = scratch()
    try:
        code, out = await run(root, "-k", "no_such_test")
        if code != 1 or "no test ran" not in out:
            raise TestFailure(f"a filter that matches nothing is not a pass: exit {code}\n{out}")
    finally:
        shutil.rmtree(root, ignore_errors=True)
