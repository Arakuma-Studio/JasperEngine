"""The undo of a move puts the GameObject back where it was (S24).

Also when the move took it out of a parent it was not the last child of, to an earlier place (into
an earlier root, into an earlier sibling, to the root before an earlier root), for several at once,
and for an instance's added GameObject or an instance root.

Oracle, independent of the code fixed: the scene file saved before the move, which the undo must
give back byte for byte (and the redo the file after it); no save warns of a split Transform order.

Each case is a test of its own, test_undo_of_a_move_<case> (TESTS_INDEPENDENT): one function running
every case took longer than a test may on a loaded server.
"""

from __future__ import annotations

import re

from harness import Editor, ToolError
from scene_history import SCENE_INPUTS, History, make_prefab

EDITOR = {"project": SCENE_INPUTS}
CI = True
TESTS_INDEPENDENT = "each case moves in a scene of its own, and makes the prefab it instantiates when the project has none"


class Moves(History):
    """A History whose saves also fail on a split Transform order (the save's warning)"""

    async def save(self) -> bytes:
        try:
            data = await super().save()
        except ToolError as e:
            self.problem(f"the save was refused: {str(e)[:500]}")
            return ("# refused: " + str(e).replace("\n", " ")[:300] + "\n").encode()
        for entry in await self.ed.console_since():
            message = entry.get("message", "")
            if "Transform order splits the branch" in message:
                self.problem(f"the save warns: {message[:300]}")
        return data


async def prefab_pa(ed: Editor) -> str:
    """Pa.prefab, made in a scene of its own the first time a test of this editor needs it"""
    if not (ed.project / "Pa.prefab").exists():
        maker = await Moves.create(ed, "Move Undo Prefabs", "prefabs")
        await make_prefab(maker, "Pa")
        await maker.save()
        await ed.call("close_scene", scene=maker.sid)
    return "Pa.prefab"


async def new_case(ed: Editor, name: str) -> Moves:
    """The case's scene: only GameObjects with a Transform (the list's own order is test_scene_order.py's)"""
    await ed.console_since(None)
    h = await Moves.create(ed, f"Move Undo {name}", name)
    await ed.call("delete_entities", scene=h.sid, entity="Skybox")
    return h


async def parent_xsy(h, z: bool = False):
    await h.entity("cube", "R0")
    await h.entity("cube", "P")
    for child in ("X", "S", "Y"):
        await h.entity("sphere", child, parent="P")
    if z:
        await h.entity("cube", "Z")


async def parent_xsy_z(h):
    await parent_xsy(h, z=True)


async def branch_before_branch(h):
    await h.entity("cube", "R0")
    await h.entity("cube", "P")
    await h.entity("sphere", "S", parent="P")
    await h.entity("sphere", "S1", parent="S")
    await h.entity("sphere", "Y", parent="P")
    await h.entity("sphere", "Y1", parent="Y")


async def leaf_before_branch(h):
    await h.entity("cube", "R0")
    await h.entity("cube", "P")
    await h.entity("sphere", "S", parent="P")
    await h.entity("sphere", "Y", parent="P")
    await h.entity("sphere", "Y1", parent="Y")


async def earlier_sibling(h):
    await h.entity("cube", "P")
    await h.entity("sphere", "X", parent="P")
    await h.entity("sphere", "X1", parent="X")
    await h.entity("sphere", "S", parent="P")
    await h.entity("sphere", "Y", parent="P")


async def two_of_four(h):
    await h.entity("cube", "R0")
    await h.entity("cube", "P")
    for child in ("X", "S", "T", "Y"):
        await h.entity("sphere", child, parent="P")


async def added_first(h):
    pa = await prefab_pa(h.ed)
    await h.entity("cube", "R0")
    await h.ed.call("instantiate_prefab", scene=h.sid, prefab=pa, name="I1")
    await h.entity("cube", "Extra", parent="I1")
    await h.ed.call("move_entity", scene=h.sid, entity="Extra", parent="I1", index=0)


async def instance_in_parent(h):
    pa = await prefab_pa(h.ed)
    await h.entity("cube", "R0")
    await h.entity("cube", "P")
    await h.ed.call("instantiate_prefab", scene=h.sid, prefab=pa, name="I2", parent="P")
    await h.entity("sphere", "Y", parent="P")


async def three_roots(h):
    for root in ("R0", "Q", "S"):
        await h.entity("cube", root)


async def into_later_child(h):
    await h.entity("cube", "P")
    await h.entity("sphere", "S", parent="P")
    await h.entity("sphere", "S1", parent="S")
    await h.entity("sphere", "Y", parent="P")
    await h.entity("cube", "Z")
    await h.entity("sphere", "Z1", parent="Z")


# (case, setup, move_entity arguments). Wrong before the fix (it read the place before putting the
# GameObject back below its old parent, which moves its branch to the end of the parent's): out of
# a parent, into an earlier root; a branch so (put back last); a GameObject so, before a sibling with
# children (put back between that sibling and its child); into an earlier sibling as its first
# child; two at once; an instance's added GameObject; an instance root; to the root, before an
# earlier root.
WRONG_BEFORE = [
    ("into an earlier root", parent_xsy, {"entity": "S", "parent": "R0"}),
    ("a branch into an earlier root", branch_before_branch, {"entity": "S", "parent": "R0"}),
    ("into an earlier root, before a sibling with children", leaf_before_branch, {"entity": "S", "parent": "R0"}),
    ("into an earlier sibling, first", earlier_sibling, {"entity": "S", "parent": "X", "index": 0}),
    ("two into an earlier root", two_of_four, {"entities": ["S", "T"], "parent": "R0"}),
    ("an added GameObject into an earlier root", added_first, {"entity": "Extra", "parent": "R0"}),
    ("an instance into an earlier root", instance_in_parent, {"entity": "I2", "parent": "R0"}),
    ("to the root, before an earlier root", parent_xsy, {"entity": "S", "before": "R0"}),
]
# right before the fix too, which must stay so: into a later root, within the parent past its old
# place, a root into an earlier root, to the root after the last root, a parent's last child into an
# earlier root, a branch into a later parent's child
RIGHT_BEFORE = [
    ("into a later root", parent_xsy_z, {"entity": "S", "parent": "Z"}),
    ("within its parent, past its place", parent_xsy, {"entity": "S", "after": "Y"}),
    ("a root into an earlier root", three_roots, {"entity": "S", "parent": "R0"}),
    ("to the root, after the last root", parent_xsy_z, {"entity": "S", "parent": None}),
    ("the last child into an earlier root", parent_xsy, {"entity": "Y", "parent": "R0"}),
    ("a branch into a later parent's child", into_later_child, {"entity": "S", "parent": "Z1"}),
]


def _make_case_test(name: str, setup, move: dict) -> None:
    """test_undo_of_a_move_<case>: the move (one move_entity, one undo step), then undo and redo
    twice; the undo saves the file before the move, the redo the file after it"""

    async def case(ed: Editor):
        h = await new_case(ed, name)
        await setup(h)

        async def step():
            await ed.call("move_entity", scene=h.sid, **move)

        await h.rounds(f"{name}: move_entity {move}", [step], rounds=2)
        await ed.call("close_scene", scene=h.sid, unsaved="discard")
        h.done()

    case.__name__ = case.__qualname__ = "test_undo_of_a_move_" + re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    case.__doc__ = f"the undo of a move {name} puts it back"
    globals()[case.__name__] = case


for _name, _setup, _move in WRONG_BEFORE + RIGHT_BEFORE:
    _make_case_test(_name, _setup, _move)


async def test_undo_of_a_move_the_split_then_a_move(ed: Editor):
    """What a split costs later: a move of the sibling leaves its child behind"""
    h = await new_case(ed, "the split, then a move")
    await leaf_before_branch(h)
    await h.save()
    await ed.call("move_entity", scene=h.sid, entity="S", parent="R0")
    await ed.call("undo", scene=h.sid)
    await ed.call("move_entity", scene=h.sid, entity="Y", parent="R0")
    if await h.below("R0", "Y1") is None:
        h.problem("Y moved into R0 without its child Y1")
    await h.save()
    await ed.call("close_scene", scene=h.sid, unsaved="discard")
    h.done()
