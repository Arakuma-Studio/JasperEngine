"""S66 crash leftovers (docs/specs/S66-crash-leftovers.md, jasper://docs/editor-tools): an editor killed
in the middle of an import or a template copy leaves staging folders the next start removes.

The copy is held by the editor's hook JASPER_TEST_STAGING_HOLD=<path> (it creates <path>.held and
waits while <path> exists) and killed with SIGKILL there, so nothing depends on timing. The editors
are real GUI ones showing the Project Hub, each started in a working folder the next can share:
the settings folder is what connects a start to the crash before it.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import shutil
import signal
import subprocess
import tempfile
import time
import zipfile
from pathlib import Path

import harness
import yaml
from harness import Editor, TestFailure, free_port, wait_for_port

CI = True
TIMEOUT = 900


async def wait_for(predicate, seconds: float, what: str) -> None:
    deadline = time.monotonic() + seconds
    while not predicate():
        if time.monotonic() > deadline:
            raise TestFailure(f"timed out waiting for {what}")
        await asyncio.sleep(0.05)


def write_project_zip(path: Path) -> None:
    """A project a few files big, one folder down: extraction stops at the hold after one entry"""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Packed/project.yaml", "name: Packed\n")
        z.writestr("Packed/scenes/One.txt", "one\n")
        z.writestr("Packed/scenes/Two.txt", "two\n")


class HubEditor:
    """The editor's GUI with --hub under a private Xvfb, reached over HTTP MCP, in the working folder
    `cwd`, which keeps its settings folder from one editor to the next."""

    def __init__(self, cwd: Path, env: dict[str, str] | None = None, settings: dict | None = None):
        self.cwd, self.extra_env, self.settings = cwd, env or {}, settings or {}
        self.proc: subprocess.Popen | None = None
        self.xvfb: harness.Xvfb | None = None
        self.ed: Editor | None = None

    async def __aenter__(self) -> "HubEditor":
        port = free_port()
        home = self.cwd / "home"
        home.mkdir(parents=True, exist_ok=True)
        # the whole X screen: the probe's window pixels are xdotool's screen pixels. create_project
        # over MCP, with no project open, goes under the default export folder
        settings = {"window": {"width": 1600, "height": 900, "maximized": True},
                    "mcp": {"http_enabled": True, "http_port": port, "http_token": harness.HTTP_TOKEN},
                    "export": {"default_dir": str(self.cwd)}}
        settings.update(self.settings)
        (self.cwd / "settings.yaml").write_text(yaml.safe_dump(settings, sort_keys=False))
        self.xvfb = harness.Xvfb()
        self.display = self.xvfb.start()
        env = {k: v for k, v in os.environ.items() if not k.startswith("XDG_") and k not in ("WAYLAND_DISPLAY", "JASPER_MCP_URL")}
        env.update({"DISPLAY": self.display, "HOME": str(home), "LIBGL_ALWAYS_SOFTWARE": "1"})
        env.update(self.extra_env)
        self.log = open(self.cwd / f"editor-{port}.log", "w")
        self.proc = subprocess.Popen(harness.editor_command() + ["--hub"], cwd=str(self.cwd), env=env,
                                     stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)
        await wait_for_port(port, timeout=180)
        self.ed = Editor(url=f"http://127.0.0.1:{port}/mcp", token=harness.HTTP_TOKEN)
        await self.ed.__aenter__()
        return self

    def kill9(self) -> None:
        """No clean-up runs; the lock is free when this returns"""
        os.kill(self.proc.pid, signal.SIGKILL)
        self.proc.wait(60)

    async def __aexit__(self, *exc) -> None:
        if self.ed:
            with contextlib.suppress(BaseException):
                await self.ed.__aexit__(None, None, None)
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                await asyncio.to_thread(self.proc.wait, 60)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                await asyncio.to_thread(self.proc.wait, 30)
        self.xvfb.stop()
        self.log.close()


class Scene:
    """A working folder, a zip to import and the hold of the editors that should stop in their copy"""

    def __init__(self):
        self.root = Path(tempfile.mkdtemp(prefix="jasper-leftovers-"))
        self.cwd = self.root / "editor"
        self.cwd.mkdir()
        self.archive = self.root / "Packed.zip"
        write_project_zip(self.archive)
        self.into = self.root / "imports"
        self.into.mkdir()
        self.hold = self.root / "staging-hold"
        self.held = Path(str(self.hold) + ".held")

    def held_editor(self) -> HubEditor:
        self.hold.write_text("hold\n")
        self.held.unlink(missing_ok=True)
        return HubEditor(self.cwd, env={"JASPER_TEST_STAGING_HOLD": str(self.hold)})

    def remove(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    async def kill9_during(self, hub: HubEditor, tool: str, args: dict) -> None:
        """Starts the tool, waits for its copy at the hold, kills the editor there"""
        call = asyncio.create_task(hub.ed.call_raw(tool, args))
        await wait_for(self.held.exists, 120, f"{tool} at the hold")
        hub.kill9()
        with contextlib.suppress(BaseException):
            await asyncio.wait_for(call, 30)


async def test_the_next_start_removes_what_a_kill_left():
    """AC1: kill -9 in the middle of an import and of a template copy, in folders the settings know
    nothing else about; the next start removes both, and the same import then takes the plain names."""
    scene = Scene()
    try:
        into = scene.into
        async with scene.held_editor() as hub:
            await scene.kill9_during(hub, "import_project", {"source": str(scene.archive), "path": str(into), "timeout_seconds": 300})
        if not (into / "Packed.importing").is_dir():
            raise TestFailure(f"kill -9 left the import's staging folder: {sorted(os.listdir(into))}")
        made = scene.cwd / "made"
        async with scene.held_editor() as hub:
            await scene.kill9_during(hub, "create_project", {"path": str(made / "New"), "name": "New", "template": "3d", "unsaved": "discard"})
        if not (made / "New.importing").is_dir():
            raise TestFailure(f"kill -9 left the copy's staging folder: {sorted(os.listdir(made))}")

        async with HubEditor(scene.cwd) as hub:
            ed = hub.ed
            await wait_for(lambda: not (into / "Packed.importing").exists() and not (made / "New.importing").exists(), 60,
                           "the start to remove the staging folders")
            ed.check_eq(sorted(os.listdir(into)), [], "nothing else stays beside the import")
            ed.check_eq(sorted(os.listdir(made)), [], "nothing else stays beside the copy")
            got = await ed.call("import_project", source=str(scene.archive), path=str(into), timeout_seconds=300)
            ed.check_eq(Path(got["path"]), into / "Packed", "the imported name has no number")
            ed.check_eq(sorted(os.listdir(into)), ["Packed"], "only the project")
    finally:
        scene.remove()


async def test_a_live_owner_in_another_editor_is_never_touched():
    """AC2: an editor at work in a folder; another one sweeps it and imports beside it. The folder
    stays, whole, until its owner dies; then the same look removes it."""
    scene = Scene()
    try:
        into = scene.into
        staging = into / "Packed.importing"
        async with Editor(create=True, name="Second") as second:
            async with scene.held_editor() as hub:
                call = asyncio.create_task(hub.ed.call_raw("import_project", {"source": str(scene.archive), "path": str(into), "timeout_seconds": 300}))
                await wait_for(scene.held.exists, 120, "the import at the hold")
                before = sorted(p.name for p in staging.rglob("*"))
                swept = await second.call("clear_project_leftovers", remove=[])
                second.check_eq(swept["removed"], [], "nothing of a running editor")
                got = await second.call_raw("clear_project_leftovers", {"remove": [str(staging)]})
                second.check(got.is_error, f"a live owner's folder cannot be removed: {got.text}")
                second.check_eq(got.structured["in_use"], [staging.as_posix()], "it is in use")
                second.check_eq(got.structured["failed"][0]["path"], staging.as_posix(), "and that is why")
                second.check_eq(sorted(p.name for p in staging.rglob("*")), before, "not a file touched")

                imported = await second.call("import_project", source=str(scene.archive), path=str(into), timeout_seconds=300)
                second.check_eq(Path(imported["path"]), into / "Packed", "the second editor's import goes through")
                second.check("leftovers" not in imported, f"a live folder is not a leftover: {imported}")
                second.check_eq(sorted(p.name for p in staging.rglob("*")), before, "not a file touched by the import's sweep")

                # the killed editor's MCP session ends with this block: nothing else awaits in it
                hub.kill9()
                with contextlib.suppress(BaseException):
                    await asyncio.wait_for(call, 30)

            # This project is under the system's temp folder, so it is no recent project and its
            # parent no known one: `remove` names the folder. Its owner is dead: the sweep takes it
            got = await second.call_raw("clear_project_leftovers", {"remove": [str(staging)]})
            second.check(not got.is_error, f"its owner died: {got.text}")
            second.check_eq(got.structured["removed"], [staging.as_posix()], "it goes")
            second.check_eq(sorted(os.listdir(into)), ["Packed"], "the folder and its marker")
    finally:
        scene.remove()


async def test_a_folder_without_a_marker_is_kept_and_reported():
    """R3, AC2: the old name without our marker may hold a user's data: every result lists it, nothing
    removes it unasked, clear_project_leftovers does when asked. Its group is hidden until enabled."""
    async with Editor(create=True, name="Old") as ed:
        listed = {t.name for t in await ed.tools(groups=None)}
        ed.check("clear_project_leftovers" not in listed, "the tool's group is off by default")
        await ed.call("tool_groups", enable=["editor_tools"])
        listed = {t.name for t in await ed.tools(groups=None)}
        ed.check("clear_project_leftovers" in listed, "tool_groups lists it")

        into = ed.scratch.root / "imports"
        into.mkdir()
        old = into / "Packed.importing"
        old.mkdir()
        (old / "my-work.txt").write_text("my work\n")
        (into / "Mine").mkdir()

        archive = ed.scratch.root / "Packed.zip"
        write_project_zip(archive)
        raw = await ed.call_raw("import_project", {"source": str(archive), "path": str(into), "timeout_seconds": 300})
        ed.check(not raw.is_error, raw.text)
        imported = raw.structured
        ed.check(old.as_posix() in raw.text, f"a client that shows only the text sees the folder: {raw.text}")
        ed.check_eq(Path(imported["path"]), into / "Packed")
        ed.check_eq([e["path"] for e in imported["leftovers"]], [old.as_posix()], "the import's result lists it")
        ed.check(imported["leftovers"][0]["reason"], "with the reason")
        ed.check("clear_project_leftovers" in imported["next"], f"and says what to do: {imported}")
        ed.check((old / "my-work.txt").is_file(), "and keeps it")

        made = await ed.call("create_project", path=str(into / "New"), name="New", template="empty", unsaved="discard")
        ed.check_eq([e["path"] for e in made["leftovers"]], [old.as_posix()], "the template copy's result lists it")

        swept = await ed.call("clear_project_leftovers")
        ed.check_eq(swept["removed"], [], "nothing removed unasked")
        ed.check_eq([e["path"] for e in swept["leftovers"]], [old.as_posix()], "the known parents' leftovers")
        ed.check((old / "my-work.txt").is_file(), "still there")

        for wrong in (into / "Mine", into / "Packed", into / "Gone.importing"):
            got = await ed.call_raw("clear_project_leftovers", {"remove": [str(wrong)]})
            ed.check(got.is_error and wrong.as_posix() in got.text, f"only a listed leftover goes: {wrong}: {got.text}")
        ed.check((into / "Mine").is_dir() and (into / "Packed" / "project.yaml").is_file(), "a folder of the user's stays")
        await ed.check_error("clear_project_leftovers", {"remove": ["relative.importing"]}, contains="absolute")

        swept = await ed.call("clear_project_leftovers", remove=[str(old)])
        ed.check_eq(swept["removed"], [old.as_posix()], "asked: it goes")
        ed.check_eq(swept["leftovers"], [], "none left")
        ed.check(not old.exists(), "the folder is gone")


async def test_the_hub_shows_a_leftover_with_its_buttons():
    """R3: in the Hub, a marker-less folder is listed with Show in Folder and Remove. Remove asks first:
    Keep changes nothing, Delete removes the folder and its row."""
    scene = Scene()
    try:
        games = scene.root / "Games"
        old = games / "Old Game.importing"
        old.mkdir(parents=True)
        (old / "my-work.txt").write_text("my work\n")
        probe = scene.root / "ui-probe.json"

        def items() -> dict:
            with contextlib.suppress(OSError, ValueError):
                return json.loads(probe.read_text()).get("items", {})
            return {}

        async with HubEditor(scene.cwd, env={"JASPER_UI_PROBE": str(probe)}, settings={"editor": {"hub_last_location": str(games)}}) as hub:
            gui = hub.ed
            env = dict(os.environ, DISPLAY=hub.display)

            async def click(name: str) -> None:
                x0, y0, x1, y1 = items()[name]
                subprocess.run(["xdotool", "mousemove", str(int((x0 + x1) / 2)), str(int((y0 + y1) / 2))], env=env, check=True, timeout=20)
                await asyncio.sleep(0.3)
                subprocess.run(["xdotool", "click", "1"], env=env, check=True, timeout=20)

            await wait_for(lambda: "hub.leftover.0.remove" in items(), 120, "the Hub to list the folder")
            gui.check("hub.leftover.0.show" in items(), f"Show in Folder: {sorted(items())}")
            gui.check("hub.leftover.1.remove" not in items(), f"one row: {sorted(items())}")

            await click("hub.leftover.0.remove")
            await wait_for(lambda: "hub.leftover.0.delete" in items(), 30, "Remove to ask first")
            gui.check((old / "my-work.txt").is_file(), "Remove alone removes nothing")
            await click("hub.leftover.0.keep")
            await wait_for(lambda: "hub.leftover.0.remove" in items(), 30, "Keep to go back")
            gui.check((old / "my-work.txt").is_file(), "Keep removes nothing")

            await click("hub.leftover.0.remove")
            await wait_for(lambda: "hub.leftover.0.delete" in items(), 30, "Remove to ask again")
            await click("hub.leftover.0.delete")
            await wait_for(lambda: not old.exists(), 30, "Delete to remove the folder")
            await wait_for(lambda: not any(name.startswith("hub.leftover") for name in items()), 30, "the row to go")
    finally:
        scene.remove()
