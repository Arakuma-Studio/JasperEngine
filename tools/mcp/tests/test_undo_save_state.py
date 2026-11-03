"""An undo past a save leaves the scene (and the prefab) modified (S24, S03).

Each command puts back the modified flag it found when it ran; a save between the step and its
undo wrote the step into the file, so after the undo the editor holds what the file does not, and
the next save must write it (a save of the modified scenes skipped it, a close did not ask).

Oracles: list_scenes' modified flag, get_prefab_info's unsaved_changes, and the files on disk.
"""

from __future__ import annotations

from harness import Editor
from scene_history import SCENE_INPUTS, first_difference

TIMEOUT = 900
CI = True
EDITOR = {"project": str(SCENE_INPUTS)}


async def modified(ed: Editor, sid) -> bool:
    for entry in (await ed.call("list_scenes"))["scenes"]:
        if entry["id"] == sid:
            return entry["modified"]
    ed.fail(f"no scene {sid}")


async def test_undo_past_a_save_leaves_the_scene_modified(ed: Editor):
    """A property edit, saved, then undone: the scene is modified and save_project (which saves the
    modified scenes) writes it as before the edit; its redo is modified again. An edit undone before
    any save: not modified"""
    rel = "Undo Past Save.scene"
    path = ed.project / rel
    sid = (await ed.call("create_scene", name="Undo Past Save", type="3D", path=rel))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="cube", name="Cube", select=False)
    # drawn first, so its derived geometry (vertexCount) is in every save
    await ed.call("wait", frames=3)
    await ed.call("save_scene", scene=sid)
    before = path.read_bytes()
    await ed.call("set_transform", scene=sid, entity="Cube", position=[1, 2, 3])
    await ed.call("save_scene", scene=sid)
    after = path.read_bytes()
    ed.check(after != before, "the edit is saved")
    ed.check(not await modified(ed, sid), "saved: not modified")
    await ed.call("undo", scene=sid)
    ed.check(await modified(ed, sid), "undone past the save: modified (the file holds the edit)")
    await ed.call("save_project")
    ed.check(path.read_bytes() == before, f"save_project writes the undone scene: {first_difference(before, path.read_bytes())}")
    await ed.call("redo", scene=sid)
    ed.check(await modified(ed, sid), "redo: modified")
    await ed.call("save_scene", scene=sid)
    ed.check(path.read_bytes() == after, f"the redo saved: {first_difference(after, path.read_bytes())}")

    # undone and saved, then redone: the redo's undo gives back what the file holds
    await ed.call("undo", scene=sid)
    await ed.call("save_scene", scene=sid)
    ed.check(path.read_bytes() == before, f"the undo saved: {first_difference(before, path.read_bytes())}")
    await ed.call("redo", scene=sid)
    ed.check(await modified(ed, sid), "redone after the save of its undo: modified")
    await ed.call("undo", scene=sid)
    ed.check(not await modified(ed, sid), "undone again: the scene is what its file holds, not modified")

    # undone before any save: back to clean
    await ed.call("set_transform", scene=sid, entity="Cube", position=[4, 5, 6])
    await ed.call("undo", scene=sid)
    ed.check(not await modified(ed, sid), "an edit undone before a save: not modified")


async def test_a_grouped_step_undone_past_a_save(ed: Editor):
    """A batch of two edits as one undo step, saved, then undone: modified, and the next save writes
    the scene as before the batch"""
    rel = "Grouped Past Save.scene"
    path = ed.project / rel
    sid = (await ed.call("create_scene", name="Grouped Past Save", type="3D", path=rel))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="cube", name="Cube", select=False)
    await ed.call("wait", frames=3)
    await ed.call("save_scene", scene=sid)
    before = path.read_bytes()
    await ed.batch([("set_transform", {"scene": sid, "entity": "Cube", "position": [1, 0, 0]}),
                    ("rename_entity", {"scene": sid, "entity": "Cube", "name": "Box"})], undo_group=True)
    await ed.call("save_scene", scene=sid)
    ed.check(not await modified(ed, sid), "saved: not modified")
    await ed.call("undo", scene=sid)
    ed.check(await modified(ed, sid), "the group undone past the save: modified")
    await ed.call("save_project")
    ed.check(path.read_bytes() == before, f"the next save writes it as before the batch: {first_difference(before, path.read_bytes())}")


async def test_prefab_apply_undone_after_a_save(ed: Editor):
    """scene-inputs' figure model prefab, two instances: an apply of one instance's castShadows,
    saved (the scene save writes the prefab), then undone: the prefab shows unsaved changes and the
    scene is modified, and the next save writes both as they were before the apply"""
    rel, prefab_rel = "Prefab Past Save.scene", "Figure Past Save.prefab"
    sid = (await ed.call("create_scene", name="Prefab Past Save", type="3D", path=rel))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="model", name="Fig", model="assets/models/figure.gltf", select=False)
    await ed.call("create_prefab", scene=sid, entity="Fig", path=prefab_rel)
    await ed.call("instantiate_prefab", scene=sid, prefab=prefab_rel, position=[2, 0, 0], name="F1")
    await ed.call("set_properties", scene=sid, entity="F1", values={"Mesh": {"castShadows": False}})
    await ed.call("wait", frames=3)
    await ed.call("save_scene", scene=sid)

    def files() -> tuple:
        return (ed.project / rel).read_bytes(), (ed.project / prefab_rel).read_bytes()

    start = files()

    async def prefab_unsaved() -> bool:
        return (await ed.call("get_prefab_info", prefab=prefab_rel, usage=False))["unsaved_changes"]

    await ed.call("prefab_overrides", action="apply", scene=sid, entity="F1", component="mesh", properties=["castShadows"])
    ed.check(await prefab_unsaved(), "applied: the prefab has unsaved changes")
    await ed.call("save_scene", scene=sid)
    applied = files()
    ed.check(b"castShadows: false" in applied[1], "the save wrote the applied prefab")
    ed.check(not await prefab_unsaved(), "saved: no unsaved changes")
    await ed.call("undo", scene=sid)
    ed.check(await prefab_unsaved(), "undone past the save: the prefab has unsaved changes (its file holds the apply)")
    ed.check(await modified(ed, sid), "and the scene is modified")
    await ed.call("save_project")
    undone = files()
    ed.check(undone[1] == start[1], f"the next save writes the prefab as before the apply: {first_difference(start[1], undone[1])}")
    ed.check(undone[0] == start[0], f"and the scene: {first_difference(start[0], undone[0])}")
