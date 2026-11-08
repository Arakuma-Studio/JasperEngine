"""Version control (docs/specs/S29-version-control.md R33, AC17): the "vcs" tool group on
tools/fixtures/prefab-runtime committed to a local temp git repository (no remote, no network).

Main Scene.scene's Spawner is edited without a save, diffed semantically (diff_scene source=editor),
reverted to the baseline and undone; then saved, listed by get_vcs_status and committed. The
refusals: commit_files without paths, with a blank message or an unchanged path; revert_files and
sync_remote push/pull without confirm; a path outside the project; an option-like revision.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

from harness import Editor, find_entities, entity_id_of, tool_meta

REPO = Path(__file__).resolve().parents[3]
EDITOR = {"project": REPO / "tools" / "fixtures" / "prefab-runtime"}
TIMEOUT = 600
CI = True

TOOLS = ("get_vcs_status", "diff_file", "get_vcs_log", "diff_scene", "revert_to_baseline", "lock_files", "unlock_files",
         "commit_files", "revert_files", "sync_remote")
SCENE = "Main Scene.scene"
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


async def setup(ed: Editor) -> dict:
    if S:
        return S
    ed.require_tools(*TOOLS)
    git(ed, "init", "-q")
    git(ed, "config", "user.name", "Agent")
    git(ed, "config", "user.email", "agent@example.com")
    git(ed, "config", "commit.gpgsign", "false")
    ignore = ed.project / ".gitignore"
    if ".jasperengine" not in (ignore.read_text() if ignore.exists() else ""):
        with ignore.open("a") as f:
            f.write("\n.jasperengine/\n")
    (ed.project / "notes.txt").write_text("unchanged\n")
    git(ed, "add", "-A")
    git(ed, "commit", "-q", "-m", "initial")
    # the project opened without a repository: a refresh finds it
    status = await ed.call("get_vcs_status", refresh=True)
    ed.check_eq(status.get("state"), "active", f"after git init: {status}")
    ed.check_eq(status.get("provider"), "git", f"{status}")
    await ed.call("open_scene", scene=SCENE)
    found = await ed.call("find_entities", scene="Main Scene", name="Spawner")
    spawner = [e for e in find_entities(found) if e.get("name") == "Spawner"]
    ed.check(spawner, f"Spawner: {found}")
    S["spawner"] = entity_id_of(spawner[0])
    S["commits"] = git(ed, "rev-list", "--count", "HEAD").strip()
    return S


def spawner_diff(diff: dict) -> dict | None:
    for item in diff.get("objects", []):
        if item.get("id") == S["spawner"]:
            return item
    return None


async def test_tools_are_in_the_vcs_group(ed: Editor):
    ed.require_tools(*TOOLS)
    for name in TOOLS:
        ed.check_eq(tool_meta(ed.tool(name)).get("jasper/group"), "vcs", f"{name}'s group")
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check(not default & set(TOOLS), f"hidden until tool_groups enables vcs: {sorted(default & set(TOOLS))}")
    await ed.call("tool_groups", enable=["vcs"])
    try:
        listed = {t.name for t in await ed.tools(groups=None)}
        ed.check(set(TOOLS) <= listed, f"listed once enabled: {sorted(set(TOOLS) - listed)}")
    finally:
        await ed.call("tool_groups", disable=["vcs"])


async def test_unsaved_edit_diff_revert_and_undo(ed: Editor):
    await setup(ed)
    await ed.call("set_transform", scene="Main Scene", entity=S["spawner"], position=[9, 8, 7])
    diff = await ed.call("diff_scene", scene="Main Scene", source="editor")
    item = spawner_diff(diff)
    ed.check(item is not None, f"the unsaved edit shows: {diff}")
    paths = [p["path"] for c in item["components"] for p in c["properties"]]
    ed.check_eq(paths, ["transform.position"], f"exactly the edited property: {item}")
    # the saved file has no change yet
    on_disk = await ed.call("diff_scene", scene="Main Scene", source="disk")
    ed.check(spawner_diff(on_disk) is None, f"disk: {on_disk}")

    reverted = await ed.call("revert_to_baseline", scene="Main Scene", entity=S["spawner"], property="transform.position")
    undo_name = (reverted.get("undo") or {}).get("name", "")
    ed.check("Revert transform.position to Baseline" in undo_name, f"{reverted}")
    after = await ed.call("diff_scene", scene="Main Scene")
    ed.check(spawner_diff(after) is None, f"reverted: {after}")
    undone = await ed.call("undo", scope="scene", scene="Main Scene")
    ed.check_eq([u["name"] for u in undone["undone"]], [undo_name], f"one step: {undone}")
    again = await ed.call("diff_scene", scene="Main Scene")
    ed.check(spawner_diff(again) is not None, f"undo brings the edit back: {again}")


async def test_status_lists_the_saved_scene(ed: Editor):
    await setup(ed)
    await ed.call("save_scene", scene="Main Scene")
    status = await ed.call("get_vcs_status", refresh=True)
    entries = {e["path"]: e for e in status["entries"]}
    ed.check(SCENE in entries, f"{SCENE} listed: {status}")
    ed.check_eq(entries[SCENE]["worktree"], "modified", f"{entries[SCENE]}")


async def test_refusals_commit_nothing(ed: Editor):
    await setup(ed)
    await ed.check_error("commit_files", {"message": "no paths"})
    await ed.check_error("commit_files", {"paths": [], "message": "empty paths"})
    await ed.check_error("commit_files", {"paths": [SCENE], "message": "  \n"}, contains="blank")
    await ed.check_error("commit_files", {"paths": ["notes.txt"], "message": "unchanged"}, contains="no changes")
    ed.check_eq(git(ed, "rev-list", "--count", "HEAD").strip(), S["commits"], "nothing was committed")
    await ed.check_error("revert_files", {"paths": [SCENE], "confirm": False}, contains="confirm")
    await ed.check_error("sync_remote", {"action": "push"}, contains="confirm")
    await ed.check_error("sync_remote", {"action": "pull", "confirm": False}, contains="confirm")
    await ed.check_error("lock_files", {"paths": ["../outside"]})
    await ed.check_error("unlock_files", {"paths": []})
    # no remote: nothing to fetch, and nothing waits on a prompt
    fetched = await ed.call_raw("sync_remote", {"action": "fetch", "wait": True, "timeout_seconds": 60})
    ed.check("Fetch" in fetched.text, f"the job answered: {fetched.text[:300]}")
    await ed.check_error("diff_file", {"path": "../outside"})
    await ed.check_error("commit_files", {"paths": ["../outside"], "message": "outside"})
    target = ed.scratch.root / "x"
    await ed.check_error("diff_file", {"path": SCENE, "rev": f"--output={target}"}, contains="not a revision")
    await ed.check_error("diff_scene", {"scene": "Main Scene", "rev": f"--output={target}"}, contains="not a revision")
    # an option-like file name is a path, never an option
    log = await ed.call("get_vcs_log", path="--output=x")
    ed.check_eq(log["entries"], [], f"no history for a file that does not exist: {log}")
    ed.check(not target.exists(), "no option reached git")
    ed.check(git(ed, "status", "--porcelain").strip() != "", "the scene is still modified")


async def test_text_diff_of_the_scene(ed: Editor):
    await setup(ed)
    diff = await ed.call("diff_file", path=SCENE)
    ed.check("--- a/" in diff["diff"] and "+++ b/" in diff["diff"], f"a unified diff: {diff['diff'][:300]}")


async def test_commit_and_log(ed: Editor):
    await setup(ed)
    committed = await ed.call("commit_files", paths=[SCENE], message="Move the spawner")
    revision = committed["revision"]
    ed.check(len(revision) >= 7, f"{committed}")
    ed.check_eq(git(ed, "rev-parse", "HEAD").strip(), revision, "HEAD is the new commit")
    ed.check_eq(git(ed, "show", "--name-only", "--format=", "HEAD").splitlines(), [SCENE], "exactly the scene")
    ed.check_eq(git(ed, "log", "-1", "--format=%B").strip(), "Move the spawner", "the message verbatim")
    log = await ed.call("get_vcs_log", limit=5)
    ed.check_eq(log["entries"][0]["revision"], revision, f"newest first: {log}")
    ed.check_eq(log["entries"][0]["message"], "Move the spawner", f"{log}")
    status = await ed.call("get_vcs_status", refresh=True)
    ed.check(SCENE not in {e["path"] for e in status["entries"]}, f"clean after the commit: {status}")


async def test_detached_and_merge_state(ed: Editor):
    """get_vcs_status names a detached HEAD by its tag and reports a merge left unfinished."""
    await setup(ed)
    branch = git(ed, "rev-parse", "--abbrev-ref", "HEAD").strip()
    git(ed, "tag", "v1")
    git(ed, "checkout", "-q", "--detach", "v1")
    try:
        status = await ed.call("get_vcs_status", refresh=True)
        ed.check_eq(status["branch"].get("detached"), True, f"{status['branch']}")
        ed.check_eq(status["branch"].get("tag"), "v1", f"{status['branch']}")
        ed.check_eq(status["branch"].get("label"), "tag v1", f"{status['branch']}")
    finally:
        git(ed, "checkout", "-q", branch)

    # a merge that stopped on a conflict
    git(ed, "checkout", "-q", "-b", "side")
    (ed.project / "notes.txt").write_text("side\n")
    git(ed, "commit", "-q", "-m", "side", "--", "notes.txt")
    git(ed, "checkout", "-q", branch)
    (ed.project / "notes.txt").write_text("ours\n")
    git(ed, "commit", "-q", "-m", "ours", "--", "notes.txt")
    env = dict(os.environ, HOME=str(ed.scratch.home), GIT_CONFIG_NOSYSTEM="1", LC_ALL="C")
    merged = subprocess.run(["git", "merge", "-q", "side"], cwd=ed.project, env=env, capture_output=True, text=True, timeout=60)
    ed.check(merged.returncode != 0, f"the merge conflicts: {merged.stdout} {merged.stderr}")
    try:
        status = await ed.call("get_vcs_status", refresh=True)
        ed.check_eq(status["branch"].get("operation"), "merge", f"{status['branch']}")
        ed.check(status["branch"]["label"].endswith("| MERGING"), f"{status['branch']}")
        ed.check(status["counts"]["conflicted"] >= 1, f"{status['counts']}")
    finally:
        git(ed, "merge", "--abort")
    status = await ed.call("get_vcs_status", refresh=True)
    ed.check("operation" not in status["branch"], f"the merge is over: {status['branch']}")


async def test_renamed_scene_diffs_against_its_source(ed: Editor):
    """A staged rename compares with the file it came from (its new path is not in HEAD)."""
    await setup(ed)
    text = (ed.project / SCENE).read_text()
    (ed.project / "Level.scene").write_text(text.replace("id: 1\nname: Main Scene\n", "id: 7001\nname: Level\n", 1))
    git(ed, "add", "Level.scene")
    git(ed, "commit", "-q", "-m", "a level", "--", "Level.scene")
    git(ed, "mv", "Level.scene", "Level Moved.scene")
    await ed.call("get_vcs_status", refresh=True)
    diff = await ed.call("diff_scene", scene="Level Moved.scene", source="disk")
    # (the asset guid the editor gives the new path may differ: GameObjects are what must match)
    ed.check((diff.get("added"), diff.get("removed")) == (0, 0), f"the same GameObjects as its source: {diff}")
