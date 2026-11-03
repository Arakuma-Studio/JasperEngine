"""A Missing Prefab instance is relinked once its file is back, whenever the asset database looks
(S04).

A scene opened while a prefab file is gone (deleted, or moved out of the project, outside the
editor) makes its instances Missing Prefab ones. The read that found the file gone tells the
database, so the file put back is new to it (an Added, which relinks) whether or not the database
looked at the disk while the file was gone: a delete and a put-back in one watcher batch (a git
checkout and back), or a file moved out and back with its stamp.

The editors run with the file watcher off (JASPER_FILE_WATCHER=off), so only the test's syncs make
the database look at the disk, and no outcome depends on where a watcher batch falls.

Oracles: the scene files on disk (bytes), get_entity, get_project_summary's unsaved scenes.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

from harness import Editor
from scene_history import SCENE_INPUTS, History, first_difference, two_level_prefab

TIMEOUT = 1200
CI = True
WATCHER_OFF = {"JASPER_FILE_WATCHER": "off"}
MEMBER = "I1/Pa Child"


async def present(ed: Editor, sid, path: str = MEMBER) -> bool:
    got = await ed.call_raw("get_entity", {"scene": sid, "entity": path, "detail": "summary"})
    return not got.is_error


async def relinked(ed: Editor, sid, frames: int = 300) -> bool:
    """The asset database walks the disk on a Background job after a sync: the relink lands a few
    frames later"""
    for _ in range(frames // 10):
        if await present(ed, sid):
            return True
        await ed.call("wait", frames=10)
    return await present(ed, sid)


async def member_id(ed: Editor, sid, path: str = MEMBER):
    return (await ed.call("get_entity", scene=sid, entity=path, detail="summary"))["id"]


async def unsaved_scenes(ed: Editor) -> list:
    return (await ed.call("get_project_summary"))["unsaved"]["scenes"]


async def instance_scene(ed: Editor, name: str):
    """An instance I1 of Pa (a cube with a sphere child), a cube added below its child, a Tail
    after it"""
    h = await History.create(ed, name, name)
    pa = await two_level_prefab(h, "Pa", "cube", "sphere")
    await ed.call("instantiate_prefab", scene=h.sid, prefab=pa, name="I1")
    await h.entity("cube", "Added", parent="I1/Pa Child")
    await h.entity("cube", "Tail")
    return h, pa


async def open_missing(ed: Editor, h: History, label: str) -> bytes:
    """Opens the scene (its prefab is gone): the instance is Missing; saves it"""
    h.sid = (await ed.call("open_scene", scene=h.rel))["scene"]["id"]
    ed.check(not await present(ed, h.sid), f"{label}: {MEMBER} is not there while the prefab is missing")
    return await h.save()


async def reopened(ed: Editor, h: History) -> bytes:
    await ed.call("close_scene", scene=h.sid)
    h.sid = (await ed.call("open_scene", scene=h.rel))["scene"]["id"]
    return await h.save()


async def test_a_prefab_put_back_relinks_on_the_next_sync():
    """The scene saved, closed; Pa.prefab deleted; the scene opened (Missing), saved; the file
    written back, one sync of it: the instance is back with its child's id, and the scene saves the
    bytes it saved before, also after reopening. A second round gives the same."""
    async with Editor(project=str(SCENE_INPUTS), label="relink-put-back", env=WATCHER_OFF) as ed:
        h, pa = await instance_scene(ed, "Put Back")
        prefab_file = ed.project / pa
        for round_ in (1, 2):
            saves = {"before": await h.save()}
            child = await member_id(ed, h.sid)
            await ed.call("save_project")
            await ed.call("close_scene", scene=h.sid)
            kept = prefab_file.read_bytes()
            prefab_file.unlink()
            saves["missing"] = await open_missing(ed, h, f"round {round_}")
            prefab_file.write_bytes(kept)
            await ed.call("sync_files", paths=[pa])
            back = await relinked(ed, h.sid)
            print(f"[relink] round {round_}: relinked after the sync {back}")
            ed.check(back, f"round {round_}: the sync relinks the instance: {MEMBER} is there again")
            ed.check_eq(await member_id(ed, h.sid), child, f"round {round_}: the child has its id again")
            saves["found"] = await h.save()
            saves["reopened"] = await reopened(ed, h)
            for name in ("missing", "found", "reopened"):
                ed.check(saves[name] == saves["before"],
                         f"round {round_}, {name}: the scene as before:\n{first_difference(saves['before'], saves[name])}")


async def test_a_prefab_moved_out_and_back_relinks_on_the_next_sync():
    """Pa.prefab moved out of the project and back (the same stamp and size) while the scene opened
    without it: a sync of it relinks the instance, the scene saves as before"""
    async with Editor(project=str(SCENE_INPUTS), label="relink-moved-back", env=WATCHER_OFF) as ed:
        h, pa = await instance_scene(ed, "Moved Back")
        prefab_file = ed.project / pa
        before = await h.save()
        await ed.call("save_project")
        await ed.call("close_scene", scene=h.sid)
        stamp = os.stat(prefab_file)
        away = Path(tempfile.mkdtemp(dir=ed.scratch.root)) / "Pa.prefab"
        os.rename(prefab_file, away)
        missing = await open_missing(ed, h, "moved out")
        os.rename(away, prefab_file)
        back = os.stat(prefab_file)
        ed.check_eq((back.st_mtime_ns, back.st_size), (stamp.st_mtime_ns, stamp.st_size), "the file is back with its stamp")
        await ed.call("sync_files", paths=[pa])
        again = await relinked(ed, h.sid)
        print(f"[relink] relinked after the sync {again}")
        ed.check(again, f"the sync relinks the instance: {MEMBER} is there again")
        found = await h.save()
        for name, data in (("missing", missing), ("found", found)):
            ed.check(data == before, f"{name}: the scene as before:\n{first_difference(before, data)}")


async def test_a_scene_opened_from_the_file_put_back_is_left_as_it_is():
    """Two scenes with an instance of Pa, saved and closed. Pa.prefab deleted, the first opened
    (Missing); the file put back, the second opened (a live instance, read from the file). The sync
    relinks the first; the second is not touched: no unsaved changes, its child keeps its id, and
    both save the bytes they saved before"""
    async with Editor(project=str(SCENE_INPUTS), label="relink-second-scene", env=WATCHER_OFF) as ed:
        first, pa = await instance_scene(ed, "First")
        second = await History.create(ed, "Second", "second")
        await ed.call("instantiate_prefab", scene=second.sid, prefab=pa, name="I1")
        first_before = await first.save()
        second_before = await second.save()
        second_child = await member_id(ed, second.sid)
        await ed.call("save_project")
        await ed.call("close_scene", scene=first.sid)
        await ed.call("close_scene", scene=second.sid)
        prefab_file = ed.project / pa
        kept = prefab_file.read_bytes()
        prefab_file.unlink()
        first_missing = await open_missing(ed, first, "first")
        prefab_file.write_bytes(kept)
        second.sid = (await ed.call("open_scene", scene=second.rel))["scene"]["id"]
        ed.check(await present(ed, second.sid), f"the second scene, opened from the file put back, has {MEMBER}")
        ed.check_eq(await member_id(ed, second.sid), second_child, "the second scene's child has its id")
        ed.check(second.sid not in await unsaved_scenes(ed), "the second scene opens without unsaved changes")
        await ed.call("sync_files", paths=[pa])
        again = await relinked(ed, first.sid)
        unsaved = await unsaved_scenes(ed)
        print(f"[relink] first relinked {again}; unsaved scenes {unsaved} (second {second.sid})")
        ed.check(again, f"the sync relinks the first scene's instance: {MEMBER} is there again")
        ed.check(second.sid not in unsaved, f"the sync leaves the second scene unchanged: unsaved {unsaved}")
        ed.check_eq(await member_id(ed, second.sid), second_child, "the second scene's child keeps its id")
        first_found = await first.save()
        second_after = await second.save()
        ed.check(first_missing == first_before, f"first, missing: as before:\n{first_difference(first_before, first_missing)}")
        ed.check(first_found == first_before, f"first, found: as before:\n{first_difference(first_before, first_found)}")
        ed.check(second_after == second_before, f"second: as before:\n{first_difference(second_before, second_after)}")
