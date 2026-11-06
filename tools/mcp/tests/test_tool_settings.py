"""Agents' editors leave the person's editor settings alone (docs/superpowers/specs/
2026-11-04-build-toolchain-design.md 6.1).

A stdio MCP editor (`jasperengine-editor mcp`, headless here) works on scratch copies: opening,
creating and saving them must not make one the person's last project or add it to the recent
projects, and the temporary project it falls back to when `--project` fails must not clear the
person's last project. The settings file is the scratch working directory's settings.yaml, which a
stdio editor uses when it exists (AppSettings::initialize). The editor's temporary folder (TMPDIR)
is moved away from the scratch project: a project inside it is the editor's temporary project,
which is never recorded anyway (Project::isTempProject).
"""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

import yaml

from harness import Editor, TestFailure

CI = True
TIMEOUT = 600


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def settings_of(ed: Editor) -> dict:
    return yaml.safe_load((ed.scratch.root / "settings.yaml").read_text()) or {}


def recent_paths(settings: dict) -> list[str]:
    return [entry["path"] if isinstance(entry, dict) else entry for entry in settings.get("recent_projects") or []]


def same(a: str | None, b: Path) -> bool:
    return bool(a) and Path(a).resolve() == b.resolve()


def check_untouched(settings: dict, other: Path, scratch_project: Path, what: str) -> None:
    check(same(settings.get("last_project_path"), other), f"{what}: the last project is still the person's: {settings}")
    check(any(same(p, other) for p in recent_paths(settings)), f"{what}: the person's recent project is kept: {settings}")
    check(not any(same(p, scratch_project) for p in recent_paths(settings)), f"{what}: the scratch project is not recorded: {settings}")


async def test_created_and_saved_scratch_project_is_not_recorded():
    other = Path(tempfile.mkdtemp(prefix="jasper-other-project-"))
    temp = Path(tempfile.mkdtemp(prefix="jasper-editor-tmp-"))
    person = {"last_project_path": str(other), "recent_projects": [str(other)]}
    ed = Editor(create=True, name="Scratch Recent", settings=person, keep=True, env={"TMPDIR": str(temp)})
    try:
        async with ed:
            await ed.call("save_project")
            check_untouched(settings_of(ed), other, ed.project, "after save_project")
        # and after the editor quit (it saves its settings then)
        check_untouched(settings_of(ed), other, ed.project, "after quitting")
    finally:
        if ed.scratch and not ed.failed:
            ed.scratch.remove()
        shutil.rmtree(other, ignore_errors=True)
        shutil.rmtree(temp, ignore_errors=True)


async def test_failed_project_open_keeps_the_last_project():
    root = Path(tempfile.mkdtemp(prefix="jasper-broken-open-"))
    other = root / "Other"
    other.mkdir()
    broken = root / "broken"
    broken.mkdir()
    (broken / "project.yaml").write_text("name: [unclosed\n")
    person = {"last_project_path": str(other), "recent_projects": [str(other)]}
    temp = root / "tmp"
    temp.mkdir()
    ed = Editor(project=broken, settings=person, keep=True, env={"TMPDIR": str(temp)})
    try:
        async with ed:
            status = await ed.call("editor_status")
            check(Path(status["project"]["path"]) != ed.project, f"a temporary project instead: {status['project']}")
        check_untouched(settings_of(ed), other, ed.project, "after the temporary-project fallback")
    finally:
        if ed.scratch and not ed.failed:
            ed.scratch.remove()
        shutil.rmtree(root, ignore_errors=True)
