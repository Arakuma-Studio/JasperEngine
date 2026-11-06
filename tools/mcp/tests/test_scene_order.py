"""A scene's GameObject list keeps the GameObjects without a Transform in their place.

Every path that sorts a scene's GameObject list by Transform order leaves a GameObject without a
Transform (the Skybox, Fog, a bare GameObject) in its place among the others: a model reload and a
model clear (with a GameObject hung on a model node), a prefab instantiated below a parent, Replace
Prefab, a Missing Prefab found again, a Hierarchy move, a paste, a Transform added or removed, a
GameObject added to a prefab, a removed GameObject reverted, a prefab apply, and the scene-dump CLI's
addObject. Each operation keeps the other GameObjects' order in the scene file, its undo gives the
file back byte for byte; get_scene_tree lists the roots as the Hierarchy window does (the
GameObjects without a Transform first, in the scene's order, then the Transform roots).

Oracles: the scene files on disk (bytes, PyYAML), get_scene_tree.
"""

from __future__ import annotations

import shutil
import subprocess

import yaml

from harness import Editor, editor_command
from scene_history import (LOOSE, MODEL, MODEL_SOURCE, SCENE_INPUTS, History, common_order, first_difference,
                           hierarchy_roots, top_level, two_level_prefab)

TIMEOUT = 1800
CI = True


class Order(History):
    """A scene saved and compared by bytes around one operation"""

    @classmethod
    async def make(cls, ed: Editor, name: str) -> "Order":
        return await cls.create(ed, name, name)

    def loose(self, names: list, moved: tuple = ()) -> list:
        return [n for n in names if n in LOOSE and n not in moved]

    async def check(self, label: str, operation, *, undos: int = 1, moved: tuple = (), expect_after=None) -> bytes:
        """Saves, runs the operation, saves; then undo and redo. The GameObjects the operation does
        not move keep their order in the file and in the Hierarchy, the ones without a Transform
        keep their own order; undo saves the file back byte for byte, redo the operation's order"""
        ed = self.ed
        start = await self.save()
        start_roots = await self.roots()
        await operation()
        after = await self.save()
        after_roots = await self.roots()
        names_before = [n for n in top_level(start) if n not in moved]
        names_after = [n for n in top_level(after) if n not in moved]
        print(f"[{label}] before {top_level(start)}\n[{label}] after  {top_level(after)}")
        # the file first (the list's order), then the Hierarchy's view of it (get_scene_tree)
        kept_before, kept_after = common_order(names_before, names_after)
        ed.check_eq(kept_after, kept_before, f"{label}: the other GameObjects keep their order in the file")
        if expect_after is not None:
            ed.check_eq(top_level(after), expect_after, f"{label}: the file's GameObjects")
        for _ in range(undos):
            await ed.call("undo", scene=self.sid)
        undone = await self.save()
        ed.check(undone == start, f"{label}: undo gives back the file byte for byte:\n{first_difference(start, undone)}")
        ed.check_eq(start_roots, hierarchy_roots(start), f"{label}: the Hierarchy lists the file's order")
        ed.check_eq(after_roots, hierarchy_roots(after), f"{label}: the Hierarchy lists the file's order after it")
        ed.check_eq(self.loose(after_roots, moved), self.loose(start_roots, moved), f"{label}: the GameObjects without a Transform keep their order")
        ed.check_eq(await self.roots(), start_roots, f"{label}: undo gives back the Hierarchy's order")
        for _ in range(undos):
            await ed.call("redo", scene=self.sid)
        # the order only: a redo may make an imported GameObject with another id
        redone = await self.save()
        ed.check_eq(top_level(redone), top_level(after), f"{label}: redo gives back the operation's order")
        ed.check_eq(await self.roots(), after_roots, f"{label}: redo gives back the Hierarchy's order")
        for _ in range(undos):
            await ed.call("undo", scene=self.sid)
        return after


async def model_with_held(order: Order) -> None:
    """Fig (a skinned column), a cube hung on its bone Root, the Skybox right after Fig, a cube and
    a Fog after it: the reload or clear of Fig parks the cube on Fig and sorts"""
    shutil.copy(MODEL_SOURCE, order.ed.project / MODEL)
    await order.entity("model", "Fig", model=MODEL)
    await order.ed.call("wait", frames=3)
    bone = await order.below("Fig", "Root")
    order.ed.check(bone is not None, f"the model has its bone Root: {await order.ed.call('get_scene_tree', scene=order.sid)}")
    await order.entity("cube", "Held", parent=bone)
    await order.entity("cube", "Tail")
    await order.entity("fog", "Fog")
    await order.ed.call("move_entity", scene=order.sid, entity="Skybox", after="Fig")


# -- model load and clear -------------------------------------------------------------------------------


async def test_model_reload_keeps_the_scene_order():
    """set_model of Fig's own file (the cube on a node is parked on Fig, then put on the new node):
    the Skybox stays after Fig"""
    async with Editor(project=str(SCENE_INPUTS), label="order-reload") as ed:
        order = await Order.make(ed, "Order Reload")
        await model_with_held(order)

        async def reload():
            await ed.call("set_model", scene=order.sid, entity="Fig", path=MODEL)

        await order.check("model reload", reload, expect_after=["Directional Light", "Fig", "Skybox", "Tail", "Fog"])


async def test_model_clear_keeps_the_scene_order():
    """set_model clear (the cube on a node stays, on Fig): the Skybox stays after Fig; the undo puts
    the cube back on its node"""
    async with Editor(project=str(SCENE_INPUTS), label="order-clear") as ed:
        order = await Order.make(ed, "Order Clear")
        await model_with_held(order)

        async def clear():
            await ed.call("set_model", scene=order.sid, entity="Fig", clear=True)

        await order.check("model clear", clear, expect_after=["Directional Light", "Fig", "Skybox", "Tail", "Fog"])


# -- prefab import, replace, relink and apply ---------------------------------------------------------------


async def test_instantiate_keeps_the_scene_order():
    """instantiate_prefab at the scene root and below a GameObject listed before the Skybox"""
    async with Editor(project=str(SCENE_INPUTS), label="order-import") as ed:
        order = await Order.make(ed, "Order Import")
        prefab = await two_level_prefab(order, "Pa", "cube", "sphere")
        await order.entity("cube", "Parent")
        # a sphere, as the prefab has: its instance then brings no shader variant the scene lacks (a
        # scene's shaderKeys keep what any save had, Project::collectSavedShaderKeys, so an undo
        # cannot take one back; a sphere is GPU instanced, a cube dynamically batched, S55)
        await order.entity("sphere", "Tail")
        await order.entity("fog", "Fog")
        await ed.call("move_entity", scene=order.sid, entity="Skybox", after="Parent")

        async def at_root():
            await ed.call("instantiate_prefab", scene=order.sid, prefab=prefab, name="I1")

        await order.check("instantiate at the root", at_root, moved=("I1",),
                          expect_after=["Directional Light", "Parent", "Skybox", "Tail", "Fog", "I1"])

        async def below_parent():
            await ed.call("instantiate_prefab", scene=order.sid, prefab=prefab, name="I2", parent="Parent")

        await order.check("instantiate below a parent", below_parent,
                          expect_after=["Directional Light", "Parent", "Skybox", "Tail", "Fog"])


async def test_replace_prefab_keeps_the_scene_order():
    """replace_prefab (the instance's GameObjects made again from the other prefab) of an instance
    the Skybox follows"""
    async with Editor(project=str(SCENE_INPUTS), label="order-replace") as ed:
        order = await Order.make(ed, "Order Replace")
        pa = await two_level_prefab(order, "Pa", "cube", "sphere")
        pb = await two_level_prefab(order, "Pb", "sphere", "cube")
        await ed.call("instantiate_prefab", scene=order.sid, prefab=pa, name="I1")
        await order.entity("cube", "Tail")
        await order.entity("fog", "Fog")
        await ed.call("move_entity", scene=order.sid, entity="Skybox", after="I1")

        async def replace():
            await ed.call("replace_prefab", scene=order.sid, entity="I1", prefab=pb)

        await order.check("replace prefab", replace, expect_after=["Directional Light", "I1", "Skybox", "Tail", "Fog"])


async def test_missing_prefab_found_again_keeps_the_scene_order():
    """An instance whose prefab is missing when its scene opens, then found again (the file comes
    back): the scene saves as before the prefab went"""
    async with Editor(project=str(SCENE_INPUTS), label="order-missing") as ed:
        order = await Order.make(ed, "Order Missing")
        pa = await two_level_prefab(order, "Pa", "cube", "sphere")
        await ed.call("instantiate_prefab", scene=order.sid, prefab=pa, name="I1")
        await order.entity("cube", "Tail")
        await order.entity("fog", "Fog")
        await ed.call("move_entity", scene=order.sid, entity="Skybox", after="I1")
        start = await order.save()
        await ed.call("save_project")
        await ed.call("close_scene", scene=order.sid)
        prefab_file = ed.project / pa
        kept = prefab_file.read_bytes()
        prefab_file.unlink()
        # the database sees the file gone before the scene opens
        await ed.call("sync_files")
        order.sid = (await ed.call("open_scene", scene=order.rel))["scene"]["id"]
        prefab_file.write_bytes(kept)
        await ed.call("sync_files")
        for _ in range(30):
            if not (await ed.call("get_prefab_info", scene=order.sid, entity="I1", usage=False)).get("missing_prefab"):
                break
            await ed.call("wait", frames=10)
        found = await order.save()
        print(f"[order-missing] before {top_level(start)}\n[order-missing] found  {top_level(found)}")
        ed.check_eq(top_level(found), top_level(start), "found again: the file's order")
        ed.check(found == start, f"found again: the file as before:\n{first_difference(start, found)}")
        ed.check_eq(await order.roots(), hierarchy_roots(start), "found again: the Hierarchy's order")


async def test_prefab_apply_and_undo_keep_the_scene_order():
    """scene-inputs' figure model as a prefab with two instances, and the Skybox moved among them.
    An apply of an instance's castShadows to the prefab, then its undo: the scene saves the
    GameObjects in the same order each time, and after the undo byte for byte as before the apply"""
    async with Editor(project=str(SCENE_INPUTS), label="order-apply") as ed:
        order = await Order.make(ed, "Order Apply")
        sid = order.sid
        await order.entity("model", "Fig", model="assets/models/figure.gltf")
        await ed.call("create_prefab", scene=sid, entity="Fig", path="Figure.prefab")
        for i in (1, 2):
            await ed.call("instantiate_prefab", scene=sid, prefab="Figure.prefab", position=[i * 2, 0, 0], name=f"F{i}")
        await ed.call("move_entity", scene=sid, entity="Skybox", after="F1")
        start = await order.save()
        names = top_level(start)
        ed.check(names.index("Skybox") > names.index("F1"), f"the Skybox after F1: {names}")
        await ed.call("set_properties", scene=sid, entity="F2", values={"Mesh": {"castShadows": False}})
        overridden = await order.save()
        await ed.call("prefab_overrides", action="apply", scene=sid, entity="F2", component="mesh", properties=["castShadows"])
        applied = await order.save()
        ed.check_eq(top_level(applied), names, "the apply keeps the scene's order")
        await ed.call("undo", scene=sid)
        undone = await order.save()
        ed.check_eq(top_level(undone), names, "its undo too")
        ed.check(undone == overridden, f"undo gives back the scene byte for byte: {first_difference(overridden, undone)}")
        await ed.call("redo", scene=sid)
        redone = await order.save()
        ed.check(redone == applied, f"redo: {first_difference(applied, redone)}")


# -- moves, paste, Transform ------------------------------------------------------------------------------


async def test_moves_keep_the_scene_order():
    """move_entity of GameObjects with a Transform: a root moved before another, a child moved to
    another root; and a root the Skybox follows moved past a GameObject a Fog follows (both keep
    their order; the undo puts each back)"""
    async with Editor(project=str(SCENE_INPUTS), label="order-move") as ed:
        order = await Order.make(ed, "Order Move")
        await order.entity("cube", "A")
        b = await order.entity("cube", "B")
        await order.entity("sphere", "B Child", parent=b)
        await order.entity("cube", "C")
        await order.entity("fog", "Fog")
        await ed.call("move_entity", scene=order.sid, entity="Skybox", after="A")

        async def root_before():
            await ed.call("move_entity", scene=order.sid, entity="C", before="A")

        await order.check("a root moved", root_before, moved=("C",),
                          expect_after=["Directional Light", "C", "A", "Skybox", "B", "Fog"])

        async def child_to_another_root():
            await ed.call("move_entity", scene=order.sid, entity="B Child", parent="A")

        await order.check("a child moved to another root", child_to_another_root,
                          expect_after=["Directional Light", "A", "Skybox", "B", "C", "Fog"])

        await ed.call("move_entity", scene=order.sid, entity="Skybox", after="B")
        await ed.call("move_entity", scene=order.sid, entity="Fog", after="C")

        async def past_another():
            await ed.call("move_entity", scene=order.sid, entity="B", after="C")

        await order.check("a root the Skybox follows moved", past_another, moved=("B", "Skybox", "Fog"))


async def test_paste_keeps_the_scene_order():
    """paste_entities after a GameObject the Skybox follows, and into one"""
    async with Editor(project=str(SCENE_INPUTS), label="order-paste") as ed:
        order = await Order.make(ed, "Order Paste")
        await order.entity("cube", "A")
        await order.entity("cube", "B")
        await order.entity("fog", "Fog")
        await ed.call("move_entity", scene=order.sid, entity="Skybox", after="A")
        await ed.call("copy_entities", scene=order.sid, entity="B")

        async def after_a():
            await ed.call("paste_entities", scene=order.sid, after="A")

        await order.check("paste after", after_a, moved=("B",))

        async def into_a():
            await ed.call("paste_entities", scene=order.sid, parent="A")

        await order.check("paste into", into_a, expect_after=["Directional Light", "A", "Skybox", "B", "Fog"])


async def test_transform_added_and_removed_keep_the_scene_order():
    """add_component / remove_component Transform: a bare GameObject listed just before the Skybox
    gets one, a GameObject the Skybox follows loses its own (its undo puts the Transform back at its
    place in Transform order)"""
    async with Editor(project=str(SCENE_INPUTS), label="order-transform") as ed:
        order = await Order.make(ed, "Order Transform")
        await order.entity("cube", "A")
        await order.entity("bare", "Loose")
        await order.entity("cube", "B")
        await order.entity("fog", "Fog")
        await ed.call("move_entity", scene=order.sid, entity="Loose", after="A")
        await ed.call("move_entity", scene=order.sid, entity="Skybox", after="Loose")

        async def add():
            await ed.call("add_component", scene=order.sid, entity="Loose", component="Transform")

        # its Transform comes last in Transform order, so it goes last; the Skybox it was
        # listed before stays after A
        await order.check("a Transform added", add, moved=("Loose",),
                          expect_after=["Directional Light", "A", "Skybox", "B", "Fog", "Loose"])

        async def remove():
            await ed.call("remove_component", scene=order.sid, entity="A", component="Transform")

        await order.check("a Transform removed", remove, moved=("A",))


# -- prefab edits -----------------------------------------------------------------------------------------


async def test_add_to_prefab_and_revert_keep_the_scene_order():
    """add_to_prefab (a root GameObject moved below an instance into its prefab) and the revert of a
    removed GameObject (deleted from an instance, then put back), with the Skybox after the instance"""
    async with Editor(project=str(SCENE_INPUTS), label="order-prefab-edit") as ed:
        order = await Order.make(ed, "Order Prefab Edit")
        pa = await two_level_prefab(order, "Pa", "cube", "sphere")
        await ed.call("instantiate_prefab", scene=order.sid, prefab=pa, name="I1")
        await order.entity("cube", "Extra")
        await order.entity("fog", "Fog")
        await ed.call("move_entity", scene=order.sid, entity="Extra", before="I1")
        await ed.call("move_entity", scene=order.sid, entity="Skybox", after="I1")

        async def add():
            await ed.call("add_to_prefab", scene=order.sid, entity="Extra", parent="I1")

        await order.check("add to prefab", add, moved=("Extra",), expect_after=["Directional Light", "I1", "Skybox", "Fog"])

        await ed.call("delete_entities", scene=order.sid, entity="I1/Pa Child")
        await ed.call("move_entity", scene=order.sid, entity="Extra", before="I1")

        async def revert():
            await ed.call("prefab_overrides", action="revert", scene=order.sid, entity="I1", kind="removed_gameobject", name="Pa Child")

        await order.check("revert a removed GameObject", revert,
                          expect_after=["Directional Light", "Extra", "I1", "Skybox", "Fog"])


# -- the scene-dump CLI -----------------------------------------------------------------------------------


async def test_cli_add_object_keeps_the_scene_order():
    """scene-dump --ops addObject (a plain child below a GameObject listed before the Skybox), then
    save: the Skybox stays where it was in the file"""
    async with Editor(project=str(SCENE_INPUTS), label="order-cli") as ed:
        order = await Order.make(ed, "Order CLI")
        await order.entity("cube", "A")
        await order.entity("cube", "B")
        await order.entity("fog", "Fog")
        await ed.call("move_entity", scene=order.sid, entity="Skybox", after="A")
        start = await order.save()
        await ed.call("save_project")
        copy = ed.scratch.root / "cli" / "project"
        shutil.copytree(ed.project, copy, ignore=shutil.ignore_patterns("editor.lock"))
        ops = copy.parent / "ops.yaml"
        ops.write_text(yaml.safe_dump([
            {"op": "addObject", "scene": "Order CLI", "entity": "A", "name": "Added"},
            {"op": "save"},
        ]))
        res = subprocess.run(editor_command() + ["scene-dump", "--project", str(copy), "--ops", str(ops), "--out", str(copy.parent / "dump.txt"),
                                                 "--ignore-lock"],
                             capture_output=True, text=True, timeout=900)
        ed.check(res.returncode == 0, f"scene-dump ran: {res.stdout[-800:]}\n{res.stderr[-800:]}")
        saved = (copy / order.rel).read_bytes()
        print(f"[order-cli] before {top_level(start)}\n[order-cli] after  {top_level(saved)}")
        ed.check_eq(top_level(saved), top_level(start), "addObject: the file's order")
        added = yaml.safe_load(saved)["entities"][top_level(saved).index("A")]
        ed.check("Added" in [child.get("name") for child in added.get("children", [])], "addObject: the child is saved below A")


# -- get_scene_tree's roots --------------------------------------------------------------------------------


async def test_scene_tree_lists_the_roots_as_the_hierarchy_does():
    """A scene listing a Directional Light, A, a Skybox after A, B, a bare Loose after B and a Fog:
    get_scene_tree (json and text) lists Skybox, Loose, Fog, then Directional Light, A, B, the
    Hierarchy window's order, before and after a move of B before A and after its undo"""
    async with Editor(project=str(SCENE_INPUTS), label="order-tree") as ed:
        order = await Order.make(ed, "Order Tree")
        sid = order.sid
        await order.entity("cube", "A")
        await order.entity("cube", "B")
        await order.entity("bare", "Loose")
        await order.entity("fog", "Fog")
        await ed.call("move_entity", scene=sid, entity="Skybox", after="A")
        await ed.call("move_entity", scene=sid, entity="Loose", after="B")

        async def check(label: str, expected: list) -> None:
            data = await order.save()
            print(f"[order-tree] {label}: file {top_level(data)}")
            ed.check_eq(hierarchy_roots(data), expected, f"{label}: the Hierarchy's order of the file")
            ed.check_eq(await order.roots(), expected, f"{label}: get_scene_tree json lists the Hierarchy's order")
            text = (await ed.call_raw("get_scene_tree", {"scene": sid, "depth": 0, "components": False})).text
            lines = [line.split(" [")[0].strip() for line in text.splitlines()[1:] if " [" in line]
            ed.check_eq(lines, expected, f"{label}: get_scene_tree text lists the Hierarchy's order")

        await check("start", ["Skybox", "Loose", "Fog", "Directional Light", "A", "B"])
        await ed.call("move_entity", scene=sid, entity="B", before="A")
        await check("B moved before A", ["Skybox", "Loose", "Fog", "Directional Light", "B", "A"])
        await ed.call("undo", scene=sid)
        await check("undone", ["Skybox", "Loose", "Fog", "Directional Light", "A", "B"])
