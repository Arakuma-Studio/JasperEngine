"""The undo of a move puts the GameObject back where it was (S24).

Also when the move took it out of a parent it was not the last child of, to an earlier place (into
an earlier root, into an earlier sibling, to the root before an earlier root), for several at once,
and for an instance's added GameObject or an instance root.

Oracle, independent of the code fixed: the scene file saved before the move, which the undo must
give back byte for byte (and the redo the file after it); no save warns of a split Transform order.
"""

from __future__ import annotations

from harness import Editor, ToolError
from scene_history import SCENE_INPUTS, History, make_prefab

TIMEOUT = 1800
CI = True


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


async def test_undo_of_a_move_puts_it_back():
    """Each case in a scene of its own: the move (one move_entity, one undo step), then undo and redo
    twice; the undo saves the file before the move, the redo the file after it. The cases the undo
    got wrong (it read the place before putting the GameObject back below its old parent, which
    moves its branch to the end of the parent's): out of a parent, into an earlier root; a branch so
    (put back last); a GameObject so, before a sibling with children (put back between that sibling
    and its child, and a later move of the sibling leaves the child behind); into an earlier sibling
    as its first child; two at once; an instance's added GameObject; an instance root; to the root,
    before an earlier root. And the cases it got right, which must stay so: into a later root, within
    the parent past its old place, a root into an earlier root, to the root after the last root, a
    parent's last child into an earlier root, a branch into a later parent's child"""
    async with Editor(project=str(SCENE_INPUTS), label="move-undo") as ed:
        maker = await Moves.create(ed, "Move Undo Prefabs", "prefabs")
        pa = await make_prefab(maker, "Pa")
        await maker.save()
        await ed.call("close_scene", scene=maker.sid)
        await ed.console_since(None)
        problems: list = []

        async def case(name: str, setup, move: dict) -> None:
            h = await Moves.create(ed, f"Move Undo {name}", name)
            # only GameObjects with a Transform: the list's own order is test_scene_order.py's
            await ed.call("delete_entities", scene=h.sid, entity="Skybox")
            await setup(h)

            async def step():
                await ed.call("move_entity", scene=h.sid, **move)

            await h.rounds(f"{name}: move_entity {move}", [step], rounds=2)
            problems.extend(h.problems)
            await ed.call("close_scene", scene=h.sid, unsaved="discard")

        async def parent_xsy(h, z: bool = False):
            await h.entity("cube", "R0")
            await h.entity("cube", "P")
            for child in ("X", "S", "Y"):
                await h.entity("sphere", child, parent="P")
            if z:
                await h.entity("cube", "Z")

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
            await h.entity("cube", "R0")
            await ed.call("instantiate_prefab", scene=h.sid, prefab=pa, name="I1")
            await h.entity("cube", "Extra", parent="I1")
            await ed.call("move_entity", scene=h.sid, entity="Extra", parent="I1", index=0)

        async def instance_in_parent(h):
            await h.entity("cube", "R0")
            await h.entity("cube", "P")
            await ed.call("instantiate_prefab", scene=h.sid, prefab=pa, name="I2", parent="P")
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

        # wrong before the fix
        await case("into an earlier root", parent_xsy, {"entity": "S", "parent": "R0"})
        await case("a branch into an earlier root", branch_before_branch, {"entity": "S", "parent": "R0"})
        await case("into an earlier root, before a sibling with children", leaf_before_branch, {"entity": "S", "parent": "R0"})
        # what a split costs later: a move of the sibling leaves its child behind
        h = await Moves.create(ed, "Move Undo the split, then a move", "the split, then a move")
        await ed.call("delete_entities", scene=h.sid, entity="Skybox")
        await leaf_before_branch(h)
        await h.save()
        await ed.call("move_entity", scene=h.sid, entity="S", parent="R0")
        await ed.call("undo", scene=h.sid)
        await ed.call("move_entity", scene=h.sid, entity="Y", parent="R0")
        if await h.below("R0", "Y1") is None:
            h.problem("Y moved into R0 without its child Y1")
        await h.save()
        problems.extend(h.problems)
        await ed.call("close_scene", scene=h.sid, unsaved="discard")
        await case("into an earlier sibling, first", earlier_sibling, {"entity": "S", "parent": "X", "index": 0})
        await case("two into an earlier root", two_of_four, {"entities": ["S", "T"], "parent": "R0"})
        await case("an added GameObject into an earlier root", added_first, {"entity": "Extra", "parent": "R0"})
        await case("an instance into an earlier root", instance_in_parent, {"entity": "I2", "parent": "R0"})
        await case("to the root, before an earlier root", parent_xsy, {"entity": "S", "before": "R0"})
        # right before the fix too
        await case("into a later root", lambda h: parent_xsy(h, z=True), {"entity": "S", "parent": "Z"})
        await case("within its parent, past its place", parent_xsy, {"entity": "S", "after": "Y"})
        await case("a root into an earlier root", three_roots, {"entity": "S", "parent": "R0"})
        await case("to the root, after the last root", lambda h: parent_xsy(h, z=True), {"entity": "S", "parent": None})
        await case("the last child into an earlier root", parent_xsy, {"entity": "Y", "parent": "R0"})
        await case("a branch into a later parent's child", into_later_child, {"entity": "S", "parent": "Z1"})

        ed.check(not problems, f"{len(problems)} problem(s):\n" + "\n".join(problems))
