# (c) JasperDev and contributors
# SPDX-License-Identifier: MIT
"""S95 REQ-4, AC-5 (docs/specs/S95-lua-runtime-gaps.md; JasperArcade ledger S15): as Unity, a new
or copied GameObject's name is numbered ("Blue 2") only when a sibling under the same parent (or
the scene root) has it: create_entity, duplicate_entities and paste_entities. Two parents each
keep a "Blue", which `scene:findEntity(name, parent)` and `findChild` tell apart in Play.
"""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Entity Names"}


async def test_names_unique_among_siblings(ed: Editor):
    sid = (await ed.call("create_scene", name="Names", type="3d"))["scene"]["id"]

    async def create(name: str, parent: int | None = None) -> dict:
        args = {"scene": sid, "name": name, "type": "empty"}
        if parent is not None:
            args["parent"] = parent
        made = await ed.call("create_entity", **args)
        return made["entity"]

    a = (await create("A"))["id"]
    b = (await create("B"))["id"]
    blue_a = await create("Blue", a)
    blue_b = await create("Blue", b)
    ed.check_eq((blue_a["name"], blue_b["name"]), ("Blue", "Blue"), "a Blue under another parent is not a sibling")
    ed.check_eq((await create("Blue", a))["name"], "Blue 2", "A already has a Blue")
    ed.check_eq((await create("Blue"))["name"], "Blue", "no root is named Blue")

    copies = (await ed.call("duplicate_entities", scene=sid, entities=[blue_b["id"]]))["created"]
    ed.check_eq(copies[0]["name"], "Blue 2", "the copy beside B's Blue")
    ed.check_eq(copies[0]["path"], "B/Blue 2")

    await ed.call("copy_entities", scene=sid, entities=[blue_a["id"]])
    pasted = (await ed.call("paste_entities", scene=sid, parent=b))["created"]
    ed.check_eq(pasted[0]["name"], "Blue 3", "pasted below B, which has Blue and Blue 2")
    pasted_root = (await ed.call("paste_entities", scene=sid))["created"]
    ed.check_eq(pasted_root[0]["name"], "Blue 2", "pasted at the root, beside the root Blue")

    # rename keeps the name as typed (Unity): B then has two children named Blue
    await ed.call("rename_entity", scene=sid, entity=copies[0]["id"], name="Blue")
    ed.check_eq((await ed.call("find_entities", scene=sid, name="Blue"))["count"], 4, "rename does not number")

    # the same names from Lua in Play: findEntity under a parent, findChild
    await ed.call("play", action="start", scene=sid, auto_pause=True, fixed_fps=30)
    try:
        found = await ed.call("eval_lua", code=(
            "local s = Engine.getScene(); local a, b = GameObject.find('/A'), GameObject.find('/B')\n"
            "local blueA, blueB = s:findEntity('Blue', a.entity), s:findEntity('Blue', b.entity)\n"
            "return GameObject(s, blueA).parent.entity == a.entity, GameObject(s, blueB).parent.entity == b.entity,\n"
            "    b:findChild('Blue 3').parent.entity == b.entity, a:findChild('Blue 2').parent.entity == a.entity"))
        ed.check_eq(found["values"], [True, True, True, True], "lookups under a parent")
    finally:
        await ed.call("play", action="stop")
