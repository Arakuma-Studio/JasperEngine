"""Version control with Subversion (docs/specs/S29-version-control.md D5; code-editor-git follow-ups in
docs/superpowers/specs/2026-11-04-code-editor-git-design.md): tools/fixtures/prefab-runtime checked
out of a local file:// repository (no server, no network), with a second working copy whose commit
is newer than this one's BASE.

diff_scene and diff_file without rev compare with BASE and say so; HEAD reads BASE too, so the
other working copy's commit is no change of this one, and its revision number shows it. An
unsaved edit is diffed and reverted; a scheduled add diffs as all added; a move compares with its
source.
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

from harness import Editor, find_entities, entity_id_of

REPO = Path(__file__).resolve().parents[3]
EDITOR = {"project": REPO / "tools" / "fixtures" / "prefab-runtime"}
TIMEOUT = 600
CI = True

SCENE = "Main Scene.scene"
S: dict = {}


def scene_copy(ed: Editor, scene_id: int, name: str) -> str:
    """Main Scene's text as another scene (its own id and name)."""
    text = (ed.project / SCENE).read_text()
    ed.check(re.search(r"(?m)^id: 1$", text) and re.search(r"(?m)^name: Main Scene$", text), "the scene's id and name")
    text = re.sub(r"(?m)^id: 1$", f"id: {scene_id}", text, count=1)
    return re.sub(r"(?m)^name: Main Scene$", f"name: {name}", text, count=1)


def svn(ed: Editor, cwd: Path, *args: str) -> str:
    env = dict(os.environ, HOME=str(ed.scratch.home), LC_ALL="C")
    done = subprocess.run(["svn", "--non-interactive", *args], cwd=cwd, env=env, capture_output=True, text=True, timeout=120)
    if done.returncode != 0:
        ed.fail(f"svn {' '.join(args)} failed: {done.stderr}")
    return done.stdout


async def setup(ed: Editor) -> dict:
    if S:
        return S
    repo = ed.scratch.root / "svnrepo"
    env = dict(os.environ, HOME=str(ed.scratch.home), LC_ALL="C")
    subprocess.run(["svnadmin", "create", str(repo)], check=True, env=env, timeout=60)
    url = repo.as_uri()
    # the project folder becomes the working copy: its files are added and committed (r1)
    svn(ed, ed.scratch.root, "checkout", "-q", "--force", url, str(ed.project))
    svn(ed, ed.project, "propset", "-q", "svn:ignore", ".jasperengine", ".")
    (ed.project / "Moving.scene").write_text(scene_copy(ed, 7001, "Moving"))
    svn(ed, ed.project, "add", "-q", "--force", ".")
    svn(ed, ed.project, "commit", "-q", "-m", "initial")
    await ed.call("tool_groups", enable=["vcs"])
    status = await ed.call("get_vcs_status", refresh=True)
    ed.check_eq(status.get("state"), "active", f"after the checkout: {status}")
    ed.check_eq(status.get("provider"), "svn", f"{status}")
    await ed.call("open_scene", scene=SCENE)

    # another working copy renames Game and commits (r2): the server is newer than this BASE
    other = ed.scratch.root / "other"
    svn(ed, ed.scratch.root, "checkout", "-q", url, str(other))
    text = (other / SCENE).read_text()
    ed.check("    name: Game\n" in text, "the scene names its first GameObject Game")
    (other / SCENE).write_text(text.replace("    name: Game\n", "    name: Game Renamed\n", 1))
    svn(ed, other, "commit", "-q", "-m", "rename Game")
    S["newest"] = svn(ed, other, "info", "--show-item", "revision", "-r", "HEAD", url).strip()
    ed.check_eq(S["newest"], "2", "the other working copy's commit")
    found = await ed.call("find_entities", scene="Main Scene")
    by_name = {e.get("name"): entity_id_of(e) for e in find_entities(found)}
    ed.check("Spawner" in by_name and "Game" in by_name, f"Spawner and Game: {found}")
    S["spawner"], S["game"] = by_name["Spawner"], by_name["Game"]
    return S


def object_of(diff: dict, entity: int) -> dict | None:
    return next((o for o in diff.get("objects", []) if o.get("id") == entity), None)


async def test_diff_defaults_to_base(ed: Editor):
    await setup(ed)
    await ed.call("set_transform", scene="Main Scene", entity=S["spawner"], position=[9, 8, 7])
    for args in ({}, {"rev": "HEAD"}, {"rev": "BASE"}):
        diff = await ed.call("diff_scene", scene="Main Scene", **args)
        ed.check_eq(diff.get("rev"), "BASE", f"{args}: the revision read is named")
        ed.check(object_of(diff, S["spawner"]) is not None, f"{args}: the unsaved edit shows: {diff}")
        # the saved file is BASE's: the other working copy's rename of Game (r2) is no change of this one
        disk = await ed.call("diff_scene", scene="Main Scene", source="disk", **args)
        ed.check_eq((disk.get("rev"), disk.get("objects")), ("BASE", []), f"{args}: {disk}")
    text = await ed.call_raw("diff_scene", {"scene": "Main Scene"})
    ed.check("against BASE" in text.text, f"the text names BASE: {text.text[:200]}")

    if os.environ.get("JASPER_MCP_SHOTS_DIR"):
        # the Scene Diff window names the baseline BASE (evidence for the window's change)
        await ed.call("tool_groups", enable=["editor_ux"])
        await ed.call("run_command", id="window.vcs.sceneDiff")
        await ed.call("wait", frames=30)
        shots = Path(os.environ["JASPER_MCP_SHOTS_DIR"])
        shots.mkdir(parents=True, exist_ok=True)
        await ed.screenshot("editor", path=str(shots / "svn-scene-diff.png"), max_width=1600)

    file_diff = await ed.call("diff_file", path=SCENE)
    ed.check_eq(file_diff.get("rev"), "BASE", f"{file_diff}")
    ed.check(file_diff["diff"] == "", f"the saved file is BASE's: {file_diff['diff'][:300]}")


async def test_a_revision_number_shows_the_newer_commit(ed: Editor):
    await setup(ed)
    log = await ed.call("get_vcs_log", limit=5)
    ed.check_eq(log["entries"][0]["revision"], S["newest"], f"the history is the server's: {log}")
    diff = await ed.call("diff_scene", scene=SCENE, source="disk", rev=S["newest"])
    ed.check_eq(diff.get("rev"), S["newest"], f"{diff}")
    ed.check(object_of(diff, S["game"]) is not None, f"r{S['newest']} renamed Game: {diff}")


async def test_revert_to_base(ed: Editor):
    await setup(ed)
    await ed.call("set_transform", scene="Main Scene", entity=S["spawner"], position=[1, 2, 3])
    reverted = await ed.call("revert_to_baseline", scene="Main Scene", entity=S["spawner"], property="transform.position")
    ed.check("Revert transform.position to Baseline" in (reverted.get("undo") or {}).get("name", ""), f"{reverted}")
    after = await ed.call("diff_scene", scene="Main Scene")
    ed.check(object_of(after, S["spawner"]) is None, f"back to BASE: {after}")


async def test_added_and_moved_scenes(ed: Editor):
    await setup(ed)
    (ed.project / "Added.scene").write_text(scene_copy(ed, 7002, "Added"))
    svn(ed, ed.project, "add", "-q", "Added.scene")
    svn(ed, ed.project, "move", "-q", "Moving.scene", "Moved.scene")
    await ed.call("get_vcs_status", refresh=True)

    added = await ed.call("diff_scene", scene="Added.scene", source="disk")
    objects = [o for o in added.get("objects", []) if not o.get("settings")]
    ed.check(len(objects) >= 3 and all(o.get("state") == "added" for o in objects),
             f"a scheduled add has no BASE: every GameObject is added: {added}")

    # (an asset guid the editor gives the moved file may differ: GameObjects are what must match)
    moved = await ed.call("diff_scene", scene="Moved.scene", source="disk")
    ed.check((moved.get("added"), moved.get("removed")) == (0, 0), f"the move compares with its source, which it equals: {moved}")
