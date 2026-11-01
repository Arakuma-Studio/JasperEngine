"""A save writes every GameObject of the scene (S03 data safety).

A prefab refresh that imports an instance again (an add_to_prefab, its undo, an apply, a Replace
Prefab, a Missing Prefab relinked) keeps the instance's GameObjects in its branch of the Transform
order, so they are saved once they are plain GameObjects (an unpack) or added ones; a move after a
GameObject with children goes after its branch; and a save that would still leave one out is
refused, naming it, the file left as it was.

Oracles, independent of the encoder: the live Hierarchy (get_scene_tree: every GameObject, its parent
and its place) against the saved file read with PyYAML (every id it writes, a prefab instance's
memberIds included), and against the scene opened again from that file (the same tree).
"""

from __future__ import annotations

import json
import shutil

from harness import Editor, ToolError
from scene_history import MODEL, MODEL_SOURCE, SCENE_INPUTS, Checked, make_prefab

TIMEOUT = 1800
CI = True


# -- a refresh, then an unpack ---------------------------------------------------------------------------------------


async def test_unpack_after_a_refresh_saves_every_gameobject():
    """Two instances I1, I2 of Pa, a cube added under I1 and add_to_prefab (the refresh imports I2
    again), then unpack_prefab of I2 completely: the save writes I2's Pa Child and Extra, and the file
    opened again has them. Also with a GameObject at the root after I2, and with a prefab of three
    levels whose added GameObject goes below the middle one"""
    async with Editor(project=str(SCENE_INPUTS), label="unpack-after-refresh") as ed:
        h = await Checked.create(ed, "Unpack After Refresh", "unpack")
        sid = h.sid
        pa = await make_prefab(h, "Pa")
        pb = await make_prefab(h, "Pb", grandchild=True)

        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="I1")
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="I2")
        await h.entity("cube", "Extra", parent="I1")
        await ed.call("wait", frames=3)
        await ed.call("add_to_prefab", scene=sid, entity="Extra", parent="I1")
        await ed.call("unpack_prefab", scene=sid, entity="I2", completely=True)
        await h.check_save("unpack of the other instance")

        # a GameObject at the root after the instance the refresh imports again
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="J1")
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="J2")
        await h.entity("cube", "After J2")
        await h.entity("cube", "More", parent="J1")
        await ed.call("wait", frames=3)
        await ed.call("add_to_prefab", scene=sid, entity="More", parent="J1")
        await ed.call("unpack_prefab", scene=sid, entity="J2", completely=True)
        await h.check_save("unpack with a GameObject after the instance")

        # three levels: the added GameObject below Pb Child, the instance between two others
        await ed.call("instantiate_prefab", scene=sid, prefab=pb, name="K1")
        await ed.call("instantiate_prefab", scene=sid, prefab=pb, name="K2")
        await ed.call("instantiate_prefab", scene=sid, prefab=pb, name="K3")
        await h.entity("cube", "After K3")
        await h.entity("cube", "Deep", parent=await h.below("K1", "Pb Child"))
        await ed.call("wait", frames=3)
        await ed.call("add_to_prefab", scene=sid, entity="Deep", parent=await h.below("K1", "Pb Child"))
        await ed.call("unpack_prefab", scene=sid, entity="K2", completely=True)
        await h.check_save("unpack of a three-level instance")
        h.done()


# -- the undo of add_to_prefab ---------------------------------------------------------------------------------------


async def test_undo_of_add_to_prefab_saves_the_added_gameobject():
    """add_to_prefab of a cube under I1 (two instances), then its undo (the refresh imports I1 again,
    the cube an added GameObject of it again): the save writes the cube; with a GameObject after the
    instances too, and the undo's redo"""
    async with Editor(project=str(SCENE_INPUTS), label="undo-add-to-prefab") as ed:
        h = await Checked.create(ed, "Undo Add To Prefab", "undo-add")
        sid = h.sid
        pa = await make_prefab(h, "Pa")
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="I1")
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="I2")
        await h.entity("cube", "Extra", parent="I1")
        await ed.call("wait", frames=3)
        await ed.call("add_to_prefab", scene=sid, entity="Extra", parent="I1")
        await ed.call("undo", scene=sid)
        data = await h.check_save("undo of add_to_prefab", reopen=False)
        if data is not None and b"Extra" not in data:
            h.problem("undo of add_to_prefab: no GameObject called Extra in the file")
        await ed.call("redo", scene=sid)
        await h.check_save("redo of add_to_prefab")

        await h.entity("cube", "After")
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="J1")
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="J2")
        await h.entity("cube", "After J2")
        await h.entity("sphere", "Added", parent="J2")
        await ed.call("wait", frames=3)
        await ed.call("add_to_prefab", scene=sid, entity="Added", parent="J2")
        await ed.call("undo", scene=sid)
        await h.check_save("undo with GameObjects after the instances")
        h.done()


# -- every refresh and creator ---------------------------------------------------------------------------------------


async def test_every_refresh_and_creator_keeps_the_branch_saved():
    """Each operation that makes GameObjects below a GameObject already in the scene, on an instance
    between two other GameObjects at the root, then (for an instance) an unpack, then a save: the
    file holds every GameObject and opens again as it was. prefab_overrides apply (every instance
    imported again), replace_prefab, a removed GameObject's revert, a Missing Prefab relinked (its
    file gone and back), a model's reload, paste and duplicate below an instance's GameObject,
    instantiate below one, a delete's undo"""
    async with Editor(project=str(SCENE_INPUTS), label="refresh-creators") as ed:
        shutil.copy(MODEL_SOURCE, ed.project / MODEL)
        h = await Checked.create(ed, "Refresh Creators", "creators")
        sid = h.sid
        pa = await make_prefab(h, "Pa", grandchild=True)
        pb = await make_prefab(h, "Pb")
        pd = await make_prefab(h, "Pd")

        async def between(prefab: str, name: str) -> None:
            await h.entity("cube", f"Before {name}")
            await ed.call("instantiate_prefab", scene=sid, prefab=prefab, name=name)
            await h.entity("cube", f"After {name}")

        # apply: the refresh imports every instance of Pa again
        await between(pa, "A1")
        await between(pa, "A2")
        await ed.call("set_transform", scene=sid, entity=await h.below("A1", "Pa Child"), position=[0, 2, 0])
        await ed.call("prefab_overrides", scene=sid, action="apply", entity="A1", all=True)
        await ed.call("unpack_prefab", scene=sid, entity="A2", completely=True)
        await h.check_save("apply, then unpack of another instance")

        # replace_prefab: the instance made again from Pb
        await between(pa, "R1")
        await ed.call("replace_prefab", scene=sid, entity="R1", prefab=pb)
        await ed.call("unpack_prefab", scene=sid, entity="R1", completely=True)
        await h.check_save("replace_prefab, then unpack")

        # a removed GameObject put back, the instance between two others
        await between(pa, "V1")
        await ed.call("delete_entities", scene=sid, entity=await h.below("V1", "Pa Child"))
        await ed.call("prefab_overrides", scene=sid, action="revert", entity="V1", kind="removed_gameobject", name="Pa Child")
        await ed.call("unpack_prefab", scene=sid, entity="V1", completely=True)
        await h.check_save("revert of a removed GameObject, then unpack")

        # a Missing Prefab relinked: the import onto its root
        await between(pd, "M1")
        await h.save()
        moved = ed.scratch.root / "Pd.prefab.away"
        shutil.move(str(ed.project / pd), str(moved))
        await ed.call("sync_files")
        shutil.move(str(moved), str(ed.project / pd))
        await ed.call("sync_files")
        info = await ed.call("get_prefab_info", scene=sid, entity="M1", usage=False)
        print(f"[creators] M1 after its prefab came back: missing {info.get('missing_prefab')}")
        await ed.call("unpack_prefab", scene=sid, entity="M1", completely=True)
        await h.check_save("a Missing Prefab relinked, then unpack")

        # a model's reload: its nodes made again below it
        await h.entity("cube", "Before Fig")
        await h.entity("model", "Fig", model=MODEL)
        await h.entity("cube", "After Fig")
        await ed.call("wait", frames=3)
        await ed.call("set_model", scene=sid, entity="Fig", path=MODEL)
        await ed.call("wait", frames=3)
        await h.check_save("a model reloaded")

        # paste, duplicate and instantiate below an instance's GameObject (added GameObjects)
        await between(pa, "P1")
        await h.entity("sphere", "Copied")
        await ed.call("copy_entities", scene=sid, entities=["Copied"])
        await ed.call("paste_entities", scene=sid, parent=await h.below("P1", "Pa Child"))
        await ed.call("duplicate_entities", scene=sid, entities=[await h.below("P1", "Pa Leaf")])
        await ed.call("instantiate_prefab", scene=sid, prefab=pb, name="Nested P", parent=await h.below("P1", "Pa Child"))
        await ed.call("unpack_prefab", scene=sid, entity="P1", completely=True)
        await h.check_save("paste, duplicate and instantiate below a GameObject, then unpack")

        # a delete's undo: the branch put back between two others
        await h.entity("cube", "Gone")
        await h.entity("sphere", "Gone Child", parent="Gone")
        await h.entity("cube", "After Gone")
        await ed.call("delete_entities", scene=sid, entity="Gone")
        await ed.call("undo", scene=sid)
        await h.check_save("a delete undone")

        h.done()


# -- a save that would lose a GameObject is refused -------------------------------------------------------------------

SPLIT_LUA = """
local kid = entity("Kid")
scene:moveChildToIndex(kid, scene:findBranchLastIndex(entity("Later")) + 1)
return scene:findBranchLastIndex(entity("Holder"))
"""

MEND_LUA = """
local kid = entity("Kid")
scene:moveChildToIndex(kid, scene:findBranchLastIndex(entity("Holder")) + 1)
return scene:findBranchLastIndex(entity("Holder"))
"""


async def test_a_save_that_would_lose_a_gameobject_is_refused():
    """A Transform order split by hand (a script's moveChildToIndex of a child past another root,
    an engine API games have): every way the editor writes the scene refuses, naming the GameObject,
    and the file on disk stays as it was: Play (its snapshot), build_project (it saves every scene
    first), save_scene, save_scene all, Ctrl+S (the background save). Mended, the save writes it."""
    async with Editor(project=str(SCENE_INPUTS), label="save-guard", xvfb="harness") as ed:
        h = await Checked.create(ed, "Save Guard", "guard")
        sid = h.sid
        await h.entity("cube", "Holder")
        await h.entity("sphere", "Kid", parent="Holder")
        await h.entity("cube", "Later")
        good = await h.save()
        await ed.call("select_scene", scene=sid)
        r = await ed.call("eval_lua", code=SPLIT_LUA, allow_edit_mode=True)
        print(f"[guard] split: {r.get('values')}")
        tree = await h.tree()
        ed.check(any(n == "Kid" for _, n, _ in tree), f"Kid is in the Hierarchy: {tree}")
        scene_file = ed.project / h.rel

        async def refused(label: str, message: str) -> None:
            print(f"[guard] {label}: {message[:400]}")
            if "left out" not in message or "Kid" not in message:
                h.problem(f"{label}: the error names the GameObject left out: {message[:600]}")
            if scene_file.read_bytes() != good:
                h.problem(f"{label}: the file on disk changed")

        # Play of the scene as saved (not modified: no save first), so its snapshot is what refuses
        await ed.console_since(None)
        r = await ed.call_raw("play", {"action": "start", "scene": sid, "timeout_seconds": 120})
        state = await ed.call("get_play_state")
        print(f"[guard] play: error {r.is_error}, state {state.get('state')}")
        if not r.is_error or state.get("state") != "stopped":
            h.problem(f"play: Play started: {r.text[:300]}")
            await ed.call("play", action="stop")
        else:
            errors = " ".join(e["message"] for e in await ed.console_since() if e.get("type") == "error")
            await refused("play", r.text + " " + errors)
        scene_file.write_bytes(good)

        # the export saves every scene first
        out = ed.scratch.root / "guard-build"
        r = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "timeout_seconds": 600})
        print(f"[guard] build_project: error {r.is_error}: {r.text[:400]}")
        if not r.is_error or "could not be saved" not in r.text:
            h.problem(f"build_project: the export went on: {r.text[:300]}")
        if scene_file.read_bytes() != good:
            h.problem("build_project: the file on disk changed")
        scene_file.write_bytes(good)

        # modified now (a script's moveChildToIndex is not an edit), for the saves of modified scenes
        await h.entity("cube", "Dirty")
        await ed.call("wait", frames=2)
        try:
            await ed.call("save_scene", scene=sid)
            h.problem("save_scene: the save went through")
            if b"Kid" not in scene_file.read_bytes():
                h.problem("save_scene: the file was written without Kid")
        except ToolError as e:
            await refused("save_scene", str(e))
        scene_file.write_bytes(good)

        r = await ed.call_raw("save_scene", {"all": True})
        if not r.is_error:
            h.problem(f"save_scene all: the save went through: {r.text[:300]}")
        else:
            await refused("save_scene all", r.text + " " + json.dumps(r.value))
        scene_file.write_bytes(good)

        # Ctrl+S: the App's save, the scenes on a thread of their own
        from ui_input import Ui
        ui = Ui(ed)
        ui.focus()
        await ed.call("focus_window", window="scene")
        await ed.console_since(None)
        await ui.key("ctrl+s")
        errors = ""
        for _ in range(40):
            await ed.call("wait", frames=5)
            errors += " ".join(e["message"] for e in await ed.console_since() if e.get("type") == "error")
            if "left out" in errors or scene_file.read_bytes() != good:
                break
        await refused("Ctrl+S", errors or "(no error in the console)")
        status = await ed.call("editor_status")
        if status.get("dialog"):
            await ed.call("respond_dialog", choice="cancel")
        scene_file.write_bytes(good)

        r = await ed.call("eval_lua", code=MEND_LUA, allow_edit_mode=True)
        print(f"[guard] mended: {r.get('values')}")
        await h.check_save("mended")
        h.done()


# -- moves, duplicates and nested added instances ---------------------------------------------------------------------


async def test_moves_duplicates_and_added_instances_survive_a_reopen():
    """Each in a scene of its own (one loss cannot hide the next): a move after a GameObject with
    children goes after its branch, not between it and its children (the save lost them); a move to
    the root behind a last root with children, and its undo; a copy of a root sits right after its
    source in the Hierarchy as in the file; an instance added below a GameObject of an instance of
    the same prefab loads (not as a Missing Prefab), also once that GameObject is duplicated"""
    async with Editor(project=str(SCENE_INPUTS), label="moves-reopen") as ed:
        maker = await Checked.create(ed, "Moves Prefabs", "moves")
        pa = await make_prefab(maker, "Pa", grandchild=True)
        await maker.save()
        await ed.call("close_scene", scene=maker.sid)
        problems: list = []

        async def case(name: str, steps) -> None:
            h = await Checked.create(ed, f"Moves {name}", f"moves {name}")
            await steps(h, h.sid)
            problems.extend(h.problems)
            await ed.call("close_scene", scene=h.sid, unsaved="discard")

        async def move_after(h, sid):
            await h.entity("cube", "Gone")
            await h.entity("sphere", "Gone Child", parent="Gone")
            await h.entity("cube", "After Gone")
            await h.entity("cube", "Mover")
            await ed.call("move_entity", scene=sid, entity="Mover", after="Gone")
            await h.check_save("a move after a GameObject with children")

        async def move_to_root(h, sid):
            await h.entity("cube", "Gone")
            await h.entity("sphere", "Gone Child", parent="Gone")
            await h.entity("cube", "Last")
            await h.entity("cube", "Last Child", parent="Last")
            await ed.call("move_entity", scene=sid, entity="Gone Child", parent=None)
            await h.check_save("a move to the root after a root with children", reopen=False)
            await ed.call("undo", scene=sid)
            await h.check_save("the move to the root undone")

        async def duplicate_root(h, sid):
            await h.entity("cube", "Gone")
            await h.entity("sphere", "Gone Child", parent="Gone")
            await h.entity("cube", "After Gone")
            await ed.call("duplicate_entities", scene=sid, entities=["Gone"])
            await h.check_save("a duplicate of a root")

        async def added_instance(h, sid):
            await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="Outer")
            await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="Inner", parent=await h.below("Outer", "Pa Child"))
            await h.check_save("an instance of the same prefab added below an instance's GameObject")

        async def added_instance_duplicated(h, sid):
            await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="Outer")
            await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="Inner", parent=await h.below("Outer", "Pa Child"))
            await ed.call("duplicate_entities", scene=sid, entities=[await h.below("Outer", "Pa Child")])
            await h.check_save("the GameObject holding an added instance duplicated")

        for name, steps in (("move after", move_after), ("move to root", move_to_root), ("duplicate root", duplicate_root),
                            ("added instance", added_instance), ("added instance duplicated", added_instance_duplicated)):
            await case(name, steps)
        ed.check(not problems, f"{len(problems)} problem(s):\n" + "\n".join(problems))


async def test_a_prefab_never_takes_an_instance_of_itself():
    """Apply All of an instance whose added GameObject holds an instance of the same prefab leaves
    that one an override (the prefab would contain itself, and the scene then recorded GameObjects
    it did not have), and add_to_prefab of it is refused, also through a prefab holding Pa"""
    async with Editor(project=str(SCENE_INPUTS), label="prefab-self") as ed:
        maker = await Checked.create(ed, "Self Prefabs", "self")
        pa = await make_prefab(maker, "Pa", grandchild=True)
        await maker.save()
        await ed.call("close_scene", scene=maker.sid)

        h = await Checked.create(ed, "Self Apply", "self apply")
        sid = h.sid
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="G1")
        await h.entity("cube", "Holder", parent="G1")
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="Self", parent="Holder")
        await ed.call("set_transform", scene=sid, entity="G1", scale=[1, 1.3, 1])
        r = await ed.call("prefab_overrides", scene=sid, action="apply", entity="G1", all=True)
        print(f"[self] Apply All with an added instance of its prefab: {r}")
        if r.get("remaining") != 1:
            h.problem(f"Apply All left {r.get('remaining')} override(s), not the added Holder: {r}")
        info = await ed.call("get_prefab_info", prefab=pa, contents=True, usage=False)
        if "Holder" in str(info.get("contents")):
            h.problem(f"Pa took Holder, which holds an instance of Pa: {info.get('contents')}")
        await h.check_save("Apply All of an instance with an added instance of its prefab")
        await ed.call("close_scene", scene=h.sid, unsaved="discard")

        h2 = await Checked.create(ed, "Self Add", "self add")
        sid = h2.sid
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="G1")
        await h2.entity("cube", "Holder", parent="G1")
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="Self", parent="Holder")
        message = await ed.call("add_to_prefab", scene=sid, entity="Holder", parent="G1", expect_error=True)
        print(f"[self] add_to_prefab of a GameObject holding an instance of Pa: {message[:300]}")
        if "contain itself" not in message:
            h2.problem(f"add_to_prefab: the refusal says why: {message[:300]}")
        await h2.entity("cube", "Wrapper Source")
        await ed.call("instantiate_prefab", scene=sid, prefab=pa, name="Wrapped", parent="Wrapper Source")
        await ed.call("create_prefab", scene=sid, entity="Wrapper Source", path="Wrapper.prefab")
        await h2.entity("cube", "Holder 2", parent="G1")
        await ed.call("instantiate_prefab", scene=sid, prefab="Wrapper.prefab", name="Wrapper", parent="Holder 2")
        message = await ed.call("add_to_prefab", scene=sid, entity="Holder 2", parent="G1", expect_error=True)
        if "contain itself" not in message:
            h2.problem(f"add_to_prefab through a prefab holding Pa: the refusal says why: {message[:300]}")
        await h2.check_save("add_to_prefab refused")
        ed.check(not (h.problems + h2.problems), "problems:\n" + "\n".join(h.problems + h2.problems))
