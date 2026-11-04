"""S46 crash leftovers (docs/specs/S46-crash-leftovers.md, jasper://docs/editor-tools): an editor killed
in the middle of an import or a template copy leaves staging folders the next start removes.

The copy is held by the editor's hook JASPER_TEST_STAGING_HOLD=<path> (it creates <path>.held and
waits while <path> exists) and killed with SIGKILL there, so nothing depends on timing.
"""

from __future__ import annotations

import asyncio
import contextlib
import io
import json
import os
import shutil
import signal
import time
import zipfile
from pathlib import Path

from harness import Editor, Scratch, TestFailure

CI = True
TIMEOUT = 900


def check(condition, message: str) -> None:
    if not condition:
        raise TestFailure(message)


async def wait_for(predicate, seconds: float, what: str) -> None:
    deadline = time.monotonic() + seconds
    while not predicate():
        if time.monotonic() > deadline:
            raise TestFailure(f"timed out waiting for {what}")
        await asyncio.sleep(0.05)


def write_project_zip(path: Path) -> None:
    """A project two scenes big, one folder down: extraction stops at the hold after one entry"""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Packed/project.yaml", "name: Packed\n")
        z.writestr("Packed/scenes/One.txt", "one\n")
        z.writestr("Packed/scenes/Two.txt", "two\n")


class Held:
    """An editor whose copies wait at the hold, in a scratch dir that outlives it"""

    def __init__(self, scratch: Scratch):
        self.hold = scratch.root / "staging-hold"
        self.held = Path(str(self.hold) + ".held")
        self.hold.write_text("hold\n")
        self.ed = Editor(create=True, name="Held", scratch=scratch, env={"JASPER_TEST_STAGING_HOLD": str(self.hold)})

    async def __aenter__(self) -> "Held":
        await self.ed.start()
        return self

    async def __aexit__(self, *exc) -> None:
        self.hold.unlink(missing_ok=True)
        await self.ed.close()

    async def kill9_during(self, tool: str, args: dict) -> None:
        """Starts the tool, waits for its copy at the hold, kills the editor there"""
        self.held.unlink(missing_ok=True)
        call = asyncio.create_task(self.ed.call_raw(tool, args))
        await wait_for(self.held.exists, 120, f"{tool} at the hold")
        owner = json.loads((self.ed.project / ".jasperengine" / "user" / "editor.lock").read_text())
        os.kill(owner["pid"], signal.SIGKILL)
        with contextlib.suppress(BaseException):
            await asyncio.wait_for(call, 30)
        await self.ed.close()


async def test_the_next_start_removes_what_a_kill_left():
    """AC1: kill -9 in the middle of an import and of a template copy, in folders the settings know
    nothing else about; the next start removes both, and the same import then takes the plain names."""
    scratch = Scratch.make(create=True)
    try:
        archive = scratch.root / "Packed.zip"
        write_project_zip(archive)
        into = scratch.root / "imports"
        into.mkdir()
        made = scratch.root / "made"
        made.mkdir()
        async with Held(scratch) as held:
            await held.kill9_during("import_project", {"source": str(archive), "path": str(into), "timeout_seconds": 300})
        check((into / "Packed.importing").is_dir(), f"kill -9 left the import's staging folder: {sorted(os.listdir(into))}")
        async with Held(scratch) as held:
            await held.kill9_during("create_project", {"path": str(made / "New"), "name": "New", "template": "3d", "unsaved": "discard"})
        check((made / "New.importing").is_dir(), f"kill -9 left the copy's staging folder: {sorted(os.listdir(made))}")

        async with Editor(create=True, name="Next", scratch=scratch) as ed:
            await wait_for(lambda: not (into / "Packed.importing").exists() and not (made / "New.importing").exists(), 60,
                           f"the start to remove the staging folders: {sorted(os.listdir(into))} {sorted(os.listdir(made))}")
            ed.check_eq(sorted(os.listdir(into)), [], "nothing else stays beside the import")
            ed.check_eq(sorted(os.listdir(made)), [], "nothing else stays beside the copy")
            got = await ed.call("import_project", source=str(archive), path=str(into), timeout_seconds=300)
            ed.check_eq(Path(got["path"]), into / "Packed", "the imported name has no number")
            ed.check_eq(sorted(os.listdir(into)), ["Packed"], "only the project")
    finally:
        scratch.remove()
