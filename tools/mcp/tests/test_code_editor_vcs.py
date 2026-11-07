"""The code editor's version control changes (docs/superpowers/specs/2026-11-04-code-editor-git-design.md
6.8, AC1-AC7): a committed Lua script open in the code editor lists its changes against HEAD as the gutter
marks them (code_editor status), revert_change puts one back in the buffer (unsaved, undoable), diff_file
reads the unsaved buffer (source editor) or the disk, a file with nothing to compare says why, and the
preference turns the markers off. A local temp git repository, no remote.
"""

from __future__ import annotations

import asyncio
import os
import subprocess

from harness import Editor

EDITOR = {"create": True}
TIMEOUT = 300
CI = True

SCRIPT = "scripts/Probe.lua"
BASE = "local a = 1\nlocal b = 2\nlocal c = 3\nreturn a\n"
EDITED = "local a = 1\nlocal b = 22\nreturn a\nprint(a)\n"
S: dict = {}


def git(ed: Editor, *args: str) -> str:
    env = dict(os.environ)
    env.update({"HOME": str(ed.scratch.home), "GIT_CONFIG_NOSYSTEM": "1", "GIT_TERMINAL_PROMPT": "0", "LC_ALL": "C"})
    for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        env.pop(name, None)
    done = subprocess.run(["git", *args], cwd=ed.project, env=env, capture_output=True, text=True, timeout=60)
    if done.returncode != 0:
        ed.fail(f"git {' '.join(args)} failed: {done.stderr}")
    return done.stdout


async def setup(ed: Editor) -> None:
    if S:
        return
    script = ed.project / SCRIPT
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(BASE)
    git(ed, "init", "-q")
    git(ed, "config", "user.name", "Agent")
    git(ed, "config", "user.email", "agent@example.com")
    git(ed, "config", "commit.gpgsign", "false")
    with (ed.project / ".gitignore").open("a") as f:
        f.write("\n.jasperengine/\n")
    git(ed, "add", "-A")
    git(ed, "commit", "-q", "-m", "initial")
    await ed.call("tool_groups", enable=["vcs"])
    status = await ed.call("get_vcs_status", refresh=True)
    ed.check_eq(status.get("state"), "active", f"after git init: {status}")
    S["ready"] = True


def buffer(status: dict, path: str) -> dict | None:
    for item in status.get("buffers", []):
        if item.get("path") == path:
            return item
    return None


async def buffer_when(ed: Editor, path: str, done, what: str, timeout: float = 30.0) -> dict:
    """code_editor status until the buffer satisfies done (the baseline is read off the main thread)."""
    loop = asyncio.get_running_loop()
    end = loop.time() + timeout
    last = None
    while loop.time() < end:
        last = buffer(await ed.call("code_editor", action="status"), path)
        if last is not None and done(last):
            return last
        await asyncio.sleep(0.2)
    ed.fail(f"{what}: {last}")
    return {}


def changes(item: dict) -> list:
    return (item.get("vcs") or {}).get("changes", [])


async def test_changes_revert_and_diff(ed: Editor):
    await setup(ed)
    await ed.call("code_editor", action="open", path=SCRIPT)
    clean = await buffer_when(ed, SCRIPT, lambda b: (b.get("vcs") or {}).get("state") == "ready", "the baseline arrives")
    ed.check_eq(clean["vcs"]["revision"], "HEAD", f"{clean}")
    ed.check_eq(changes(clean), [], f"no changes yet: {clean}")

    # edited on disk: the clean buffer reloads, the markers follow
    (ed.project / SCRIPT).write_text(EDITED)
    await ed.call("code_editor", action="reload", path=SCRIPT)
    edited = await buffer_when(ed, SCRIPT, lambda b: len(changes(b)) == 2, "two changes after the edit")
    ed.check_eq(changes(edited), [
        {"kind": "modified", "lines": [2, 2], "base_lines": [2, 3]},
        {"kind": "added", "lines": [4, 4], "base_lines": []},
    ], f"{edited}")
    ed.check_eq(edited.get("unsaved"), False, f"{edited}")

    # nothing to revert on an unchanged line
    await ed.check_error("code_editor", action="revert_change", path=SCRIPT, line=1, contains="no change at line 1")

    # Revert Change: the added line goes, in the buffer only
    reverted = await ed.call("code_editor", action="revert_change", path=SCRIPT, line=4)
    item = buffer(reverted, SCRIPT)
    ed.check(item is not None and item.get("unsaved") is True, f"the buffer has the revert, unsaved: {reverted}")
    ed.check_eq([c["kind"] for c in changes(item)], ["modified"], f"one change left: {item}")
    ed.check_eq((ed.project / SCRIPT).read_text(), EDITED, "the file on disk is untouched")

    # diff_file reads the buffer by default, the disk on request
    buffered = await ed.call("diff_file", path=SCRIPT)
    ed.check_eq(buffered.get("source"), "editor", f"{buffered}")
    ed.check_eq(buffered.get("unsaved"), True, f"{buffered}")
    ed.check("+local b = 22" in buffered["diff"] and "print(a)" not in buffered["diff"], f"{buffered}")
    disk = await ed.call("diff_file", path=SCRIPT, source="disk")
    ed.check_eq(disk.get("source"), "disk", f"{disk}")
    ed.check("+print(a)" in disk["diff"], f"{disk}")

    # the modified change reverts too: the buffer is the baseline again
    await ed.call("code_editor", action="revert_change", path=SCRIPT, line=2)
    same = await buffer_when(ed, SCRIPT, lambda b: changes(b) == [], "no change after both reverts")
    ed.check(same.get("unsaved") is True, f"{same}")
    empty = await ed.call("diff_file", path=SCRIPT)
    ed.check_eq(empty["diff"], "", f"{empty}")
    await ed.call("code_editor", action="reload", path=SCRIPT)
    await ed.call("code_editor", action="close", path=SCRIPT)


async def test_untracked_and_preference(ed: Editor):
    await setup(ed)
    fresh = "scripts/Fresh.lua"
    (ed.project / fresh).write_text("print('new')\n")
    await ed.call("get_vcs_status", refresh=True)
    await ed.call("code_editor", action="open", path=fresh)
    item = await buffer_when(ed, fresh, lambda b: (b.get("vcs") or {}).get("state") == "untracked", "an untracked file says so")
    ed.check_eq(changes(item), [], f"{item}")
    await ed.check_error("code_editor", action="revert_change", path=fresh, line=1, contains="Untracked")
    await ed.call("code_editor", action="close", path=fresh)

    # the preference off: no baselines, no vcs field
    await ed.call("code_editor", action="open", path=SCRIPT)
    await buffer_when(ed, SCRIPT, lambda b: "vcs" in b, "markers on by default")
    await ed.call("set_project_settings", values={"editor.vcs.code_editor_changes": False})
    try:
        await buffer_when(ed, SCRIPT, lambda b: "vcs" not in b, "no vcs field with the preference off")
    finally:
        await ed.call("set_project_settings", values={"editor.vcs.code_editor_changes": True})
    await ed.call("code_editor", action="close", path=SCRIPT)


async def test_buffer_diff_applies_to_a_renamed_file(ed: Editor):
    await setup(ed)
    old, new = "scripts/Old.lua", "scripts/New.lua"
    (ed.project / old).write_text("x\ny\nz")
    git(ed, "add", old)
    git(ed, "commit", "-q", "-m", "old")
    git(ed, "mv", old, new)
    (ed.project / new).write_text("x\nY\nz\n")
    await ed.call("get_vcs_status", refresh=True)
    try:
        # the staged rename compares with the file it came from
        await ed.call("code_editor", action="open", path=new)
        item = await buffer_when(ed, new, lambda b: len(changes(b)) == 2, "the rename's two changes")
        ed.check_eq(changes(item)[0], {"kind": "modified", "lines": [2, 2], "base_lines": [2, 2]}, f"{item}")
        await ed.call("code_editor", action="revert_change", path=new, line=2)

        # the buffer ("x\ny\nz\n") against HEAD's file without a final newline: git's patch
        buffered = await ed.call("diff_file", path=new)
        diff = buffered["diff"]
        ed.check(diff.startswith(f"--- a/{old}\n+++ b/{new}\n"), f"{buffered}")
        ed.check("-z\n\\ No newline at end of file\n+z\n" in diff, f"{buffered}")
        patch = ed.scratch.home / "buffer.patch"
        patch.write_text(diff.replace(f"--- a/{old}", f"--- a/{new}"))
        git(ed, "apply", "--cached", str(patch))
        ed.check_eq(git(ed, "show", f":{new}"), "x\ny\nz\n", "the patch on the index gives the buffer")

        # a revision the file cannot be read at is an error, never an empty file
        await ed.check_error("diff_file", path=new, rev="HEAD~5", contains="did not read")
    finally:
        await ed.call("code_editor", action="reload", path=new)
        await ed.call("code_editor", action="close", path=new)
        git(ed, "reset", "-q", "--hard")
        await ed.call("get_vcs_status", refresh=True)
