"""S46 crash leftovers (docs/specs/S46-crash-leftovers.md, jasper://docs/editor-tools): an editor killed
in the middle of an import or a template copy leaves staging folders the next start removes.

The copy is held by the editor's hook JASPER_TEST_STAGING_HOLD=<path> (it creates <path>.held and
waits while <path> exists) and killed with SIGKILL there, so nothing depends on timing.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
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
        await self.ed.__aenter__()
        return self

    async def __aexit__(self, *exc) -> None:
        self.hold.unlink(missing_ok=True)
        await self.ed.__aexit__(*exc)

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


async def test_a_live_owner_in_another_editor_is_never_touched():
    """AC2: an editor at work in a folder; another one sweeps it and imports beside it. The folder
    stays, whole, until its owner dies; then the same look removes it."""
    scratch = Scratch.make(create=True)
    try:
        archive = scratch.root / "Packed.zip"
        write_project_zip(archive)
        into = scratch.root / "imports"
        into.mkdir()
        staging = into / "Packed.importing"
        async with Held(scratch) as held:
            call = asyncio.create_task(held.ed.call_raw("import_project", {"source": str(archive), "path": str(into), "timeout_seconds": 300}))
            await wait_for(held.held.exists, 120, "the import at the hold")
            before = sorted(p.name for p in staging.rglob("*"))
            async with Editor(create=True, name="Second") as second:
                swept = await second.call("clean_project_leftovers", remove=[])
                second.check_eq(swept["removed"], [], "nothing of a running editor")
                got = await second.call_raw("clean_project_leftovers", {"remove": [str(staging)]})
                second.check(got.is_error, f"a live owner's folder cannot be removed: {got.text}")
                second.check_eq(got.structured["in_use"], [staging.as_posix()], "it is in use")
                second.check_eq(got.structured["failed"][0]["path"], staging.as_posix(), "and that is why")
                second.check_eq(sorted(p.name for p in staging.rglob("*")), before, "not a file touched")

                imported = await second.call("import_project", source=str(archive), path=str(into), timeout_seconds=300)
                second.check_eq(Path(imported["path"]), into / "Packed", "the second editor's import goes through")
                second.check("leftovers" not in imported, f"a live folder is not a leftover: {imported}")
                second.check_eq(sorted(p.name for p in staging.rglob("*")), before, "not a file touched by the import's sweep")

                owner = json.loads((held.ed.project / ".jasperengine" / "user" / "editor.lock").read_text())
                os.kill(owner["pid"], signal.SIGKILL)
                with contextlib.suppress(BaseException):
                    await asyncio.wait_for(call, 30)

                swept = await second.call("clean_project_leftovers")
                second.check_eq(swept["removed"], [staging.as_posix()], "its owner died: it goes")
                second.check_eq(sorted(os.listdir(into)), ["Packed"], "the folder and its marker")
    finally:
        scratch.remove()


async def test_a_folder_without_a_marker_is_kept_and_reported():
    """R3, AC2: the old name without our marker may hold a user's data: every result lists it, nothing
    removes it unasked, clean_project_leftovers does when asked. Its group is hidden until enabled."""
    async with Editor(create=True, name="Old") as ed:
        listed = {t.name for t in await ed.tools(groups=None)}
        ed.check("clean_project_leftovers" not in listed, "the tool's group is off by default")
        await ed.call("tool_groups", enable=["editor_tools"])
        listed = {t.name for t in await ed.tools(groups=None)}
        ed.check("clean_project_leftovers" in listed, "tool_groups lists it")

        into = ed.scratch.root / "imports"
        into.mkdir()
        old = into / "Packed.importing"
        old.mkdir()
        (old / "my-work.txt").write_text("my work\n")
        (into / "Mine").mkdir()

        archive = ed.scratch.root / "Packed.zip"
        write_project_zip(archive)
        imported = await ed.call("import_project", source=str(archive), path=str(into), timeout_seconds=300)
        ed.check_eq(Path(imported["path"]), into / "Packed")
        ed.check_eq([e["path"] for e in imported["leftovers"]], [old.as_posix()], "the import's result lists it")
        ed.check(imported["leftovers"][0]["reason"], "with the reason")
        ed.check((old / "my-work.txt").is_file(), "and keeps it")

        made = await ed.call("create_project", path=str(into / "New"), name="New", template="empty", unsaved="discard")
        ed.check_eq([e["path"] for e in made["leftovers"]], [old.as_posix()], "the template copy's result lists it")

        swept = await ed.call("clean_project_leftovers")
        ed.check_eq(swept["removed"], [], "nothing removed unasked")
        ed.check_eq([e["path"] for e in swept["leftovers"]], [old.as_posix()], "the known parents' leftovers")
        ed.check((old / "my-work.txt").is_file(), "still there")

        for wrong in (into / "Mine", into / "Packed", into / "Gone.importing"):
            got = await ed.call_raw("clean_project_leftovers", {"remove": [str(wrong)]})
            ed.check(got.is_error and wrong.as_posix() in got.text, f"only a listed leftover goes: {wrong}: {got.text}")
        ed.check((into / "Mine").is_dir() and (into / "Packed" / "project.yaml").is_file(), "a folder of the user's stays")
        await ed.check_error("clean_project_leftovers", {"remove": ["relative.importing"]}, contains="absolute")

        swept = await ed.call("clean_project_leftovers", remove=[str(old)])
        ed.check_eq(swept["removed"], [old.as_posix()], "asked: it goes")
        ed.check_eq(swept["leftovers"], [], "none left")
        ed.check(not old.exists(), "the folder is gone")
