"""Regression tests for editor bugs found through MCP (one test per bug, newest last).

Runs in a new empty project with a 3D scene of its own.
"""

from harness import Editor

EDITOR = {"create": True, "name": "Regressions"}
TIMEOUT = 900

SCENE = {}


# -- helpers --------------------------------------------------------------------


def close(a, b, tol=1e-3):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return a == b


async def scene3d(ed: Editor) -> int:
    if "id" not in SCENE:
        made = await ed.call("create_scene", name="Regress3D", type="3D")
        SCENE["id"] = made["scene"]["id"]
    await ed.call("select_scene", scene=SCENE["id"])
    return SCENE["id"]


async def create(ed: Editor, type_: str, name: str, **args) -> int:
    made = await ed.call("create_entity", type=type_, name=name, scene=SCENE["id"], **args)
    return made["entity_id"]


async def world_position(ed: Editor, entity) -> list:
    got = await ed.call("get_entity", entity=entity, scene=SCENE["id"], detail="summary")
    return got["world"]["position"]


async def local_position(ed: Editor, entity) -> list:
    got = await ed.call("get_entity", entity=entity, scene=SCENE["id"], components=["transform"],
                        properties=["position"])
    return got["components"]["transform"]["position"]


async def tree_names(ed: Editor) -> list:
    got = await ed.call("get_scene_tree", scene=SCENE["id"], format="json")
    names = []

    def walk(nodes):
        for n in nodes:
            names.append(n["name"])
            walk(n.get("children", []))

    walk(got["nodes"])
    return names


async def snapshot(ed: Editor, ids: dict) -> dict:
    out = {}
    for name, eid in ids.items():
        out[name] = (await world_position(ed, eid), await local_position(ed, eid))
    return out


def check_snapshot(ed: Editor, got: dict, want: dict, when: str) -> None:
    bad = [f"{name}: {got.get(name)} != {want[name]}" for name in want
           if name not in got or not (close(got[name][0], want[name][0]) and close(got[name][1], want[name][1]))]
    ed.check(not bad, f"{when}: transforms changed:\n  " + "\n  ".join(bad))


async def undo(ed: Editor) -> None:
    await ed.call("undo", scene=SCENE["id"])


async def redo(ed: Editor) -> None:
    await ed.call("redo", scene=SCENE["id"])


# -- DeleteEntityCmd: several entities in one delete, then undo / redo -------------
# Undo put each restored GameObject back at the raw Transform-array index recorded at
# delete time. The camera's editor helpers (icon, frustum lines) are removed from that
# array after the delete, so the recorded indices pointed past its end: the reorder
# rotated past end() (heap corruption, a later abort in free()) and new GameObjects then
# shared another one's Transform slot.


async def test_delete_camera_and_empty_undo_redo(ed: Editor):
    await scene3d(ed)
    ids = {}
    ids["KeepA"] = await create(ed, "empty", "KeepA", position=[1, 2, 3])
    cam = await create(ed, "camera", "DelCam", position=[0, 5, -10])
    empty = await create(ed, "empty", "DelEmpty", position=[4, 0, 0])
    ids["KeepB"] = await create(ed, "empty", "KeepB", position=[-7, 1, 0])
    ids["KeepBChild"] = await create(ed, "empty", "KeepBChild", parent=ids["KeepB"], position=[0, 3, 0])
    ids["DelCam"] = cam
    ids["DelEmpty"] = empty
    names0 = await tree_names(ed)
    snap0 = await snapshot(ed, ids)
    ed.check(close(snap0["KeepBChild"][0], [-7, 4, 0]), f"child world position: {snap0['KeepBChild']}")

    # camera first, as the report did
    await ed.call("delete_entities", entities=[cam, empty], scene=SCENE["id"])
    await undo(ed)
    ed.check_eq(await tree_names(ed), names0, "hierarchy after delete + undo")
    check_snapshot(ed, await snapshot(ed, ids), snap0, "after delete + undo")

    for cycle in range(2):
        await redo(ed)
        await undo(ed)
        ed.check_eq(await tree_names(ed), names0, f"hierarchy after redo + undo #{cycle + 1}")
        check_snapshot(ed, await snapshot(ed, ids), snap0, f"after redo + undo #{cycle + 1}")

    # delete again (a new command) in the other order
    await ed.call("delete_entities", entities=[empty, cam], scene=SCENE["id"])
    await undo(ed)
    ed.check_eq(await tree_names(ed), names0, "hierarchy after reversed delete + undo")
    check_snapshot(ed, await snapshot(ed, ids), snap0, "after reversed delete + undo")

    # new GameObjects must get Transform slots of their own
    f = await create(ed, "empty", "NewF", position=[9, 9, 9])
    g = await create(ed, "empty", "NewG", position=[11, 0, 0])
    ed.check(close(await world_position(ed, f), [9, 9, 9]), "NewF reads another GameObject's transform")
    ed.check(close(await world_position(ed, g), [11, 0, 0]), "NewG reads another GameObject's transform")
    await ed.call("set_transform", entity=g, position=[50, 50, 50], scene=SCENE["id"])
    check_snapshot(ed, await snapshot(ed, ids), snap0, "after moving a new GameObject")
    await ed.call("delete_entities", entities=[f, g], scene=SCENE["id"])
    check_snapshot(ed, await snapshot(ed, ids), snap0, "after deleting the new GameObjects")
    await ed.check_alive()


async def test_delete_two_last_empties_reversed_undo(ed: Editor):
    """No camera: two GameObjects last in the array, deleted later-first, then undone."""
    await scene3d(ed)
    x1 = await create(ed, "empty", "TailX1", position=[2, 0, 0])
    x2 = await create(ed, "empty", "TailX2", position=[3, 0, 0])
    names0 = await tree_names(ed)
    await ed.call("delete_entities", entities=[x2, x1], scene=SCENE["id"])
    await undo(ed)
    ed.check_eq(await tree_names(ed), names0, "hierarchy after reversed delete + undo")
    ed.check(close(await world_position(ed, x1), [2, 0, 0]), "TailX1 position")
    ed.check(close(await world_position(ed, x2), [3, 0, 0]), "TailX2 position")
    n = await create(ed, "empty", "AfterTail", position=[-5, -5, -5])
    ed.check(close(await world_position(ed, n), [-5, -5, -5]), "a new GameObject reads another's transform")
    ed.check(close(await world_position(ed, x2), [3, 0, 0]), "TailX2 changed when a GameObject was created")
    await ed.call("delete_entities", entities=[n, x1, x2], scene=SCENE["id"])
    await ed.check_alive()
