"""GameObjects made from Lua outlive their scene's Play copy without crashing the editor.

A Lua script that creates GameObjects (`Button(self.scene)`, `GameObject(self.scene)`, ...)
gets handles that own their entities: when the garbage collector takes such a handle, its
entity is destroyed. The Play copies of the scenes are deleted at play stop while the Lua
state lives on, so those handles are collected later (a collect after stop, the next
session, or lua_close at editor shutdown) and must then leave the deleted scene alone.

Scenes: GcSolo (3D, a script making objects), GcMain (3D, the same script) with GcChild (UI,
a script making objects) as an active child scene. Checks: play/stop then a full collect,
repeated cycles, a collected handle still destroys its entity while its scene plays (no
leak), and an editor shutdown after play with handles left in Lua globals exits cleanly.
"""

import asyncio
import json

from harness import Editor, RawEditor

CI = True
EDITOR = {"create": True, "name": "Lua Gc Lifetime CI"}

TIMEOUT = 900

# Made in init: kept by the script (collected once the stop releases the instance), dropped
# at once (collected while the scene plays) and kept in a global (outlives every session,
# collected only by lua_close at shutdown)
MAKER = """---@class {cls} : ScriptInstance
local {cls} = {{
    properties = {{
        {{ name = "made", type = "int", default = 0 }},
    }}
}}

function {cls}:init()
    self.gcButton = Button(self.scene)
    self.gcImage = Image(self.scene)
    self.gcText = Text(self.scene)
    self.gcObject = GameObject(self.scene)
    self.gcShape = Primitive(self.scene)
    self.gcModel = Model(self.scene)
    self.gcSprite = Sprite(self.scene)
    self.gcShape:createBox(1, 1, 1)
    for i = 1, 6 do
        local b = Button(self.scene)
        local g = GameObject(self.scene)
        local p = Primitive(self.scene)
        local m = Model(self.scene)
    end
    gcGlobals = gcGlobals or {{}}
    table.insert(gcGlobals, Button(self.scene))
    table.insert(gcGlobals, GameObject(self.scene))
    table.insert(gcGlobals, Model(self.scene))
    self.made = self.made + 1
end

return {cls}
"""

S: dict = {}


async def setup(ed: Editor) -> dict:
    """The scenes, once per editor (a test that killed the editor gets a fresh one)."""
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="GcChild", type="UI", path="scenes/GcChild")
    S["child"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["child"], type="empty", name="ChildMaker")
    await ed.call("create_script", kind="lua", name="GcChildMaker", content=MAKER.format(cls="GcChildMaker"),
                  scene=S["child"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["child"])

    made = await ed.call("create_scene", name="GcMain", type="3D", path="scenes/GcMain")
    S["main"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["main"], type="empty", name="MainMaker")
    await ed.call("create_script", kind="lua", name="GcMainMaker", content=MAKER.format(cls="GcMainMaker"),
                  scene=S["main"], entity=ent["entity_id"])
    await ed.call("set_child_scene", scene=S["main"], child=S["child"], start_active=True)
    await ed.call("save_scene", scene=S["main"])

    made = await ed.call("create_scene", name="GcSolo", type="3D", path="scenes/GcSolo")
    S["solo"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["solo"], type="empty", name="SoloMaker")
    await ed.call("create_script", kind="lua", name="GcSoloMaker", content=MAKER.format(cls="GcSoloMaker"),
                  scene=S["solo"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["solo"])

    await ed.call("edit_build_scenes", action="add", scene="scenes/GcChild.scene")
    await ed.call("save_project")
    return S


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def play(ed: Editor, scene) -> None:
    await ed.call("play", action="start", scene=scene)
    await ed.call("wait", frames=5)


async def stop_and_collect(ed: Editor) -> None:
    """Stop, let the editor drop the Play copies, then a full collect in edit mode."""
    stopped = await ed.call("play", action="stop")
    ed.check_eq(stopped["state"], "stopped", "stop")
    await ed.call("wait", frames=3)
    await lua(ed, "collectgarbage('collect'); collectgarbage('collect'); return true", allow_edit_mode=True)
    await ed.call("wait", frames=3)
    await ed.check_alive()
    await ed.call("editor_status")
    errors = [e.get("message", "") for e in await ed.console_since(types=["error"], limit=1000)]
    ed.check(not errors, f"errors logged: {errors[:10]}")


async def test_main_scene_objects_collected_after_stop(ed: Editor):
    s = await setup(ed)
    await ed.console_since(None, limit=1)   # errors are checked from here
    await play(ed, s["solo"])
    ed.check_eq(await lua(ed, "return script('SoloMaker', 'GcSoloMaker', 'GcSolo').made"), [1], "the script ran")
    await stop_and_collect(ed)


async def test_child_scene_objects_collected_after_stop(ed: Editor):
    s = await setup(ed)
    await ed.console_since(None, limit=1)   # errors are checked from here
    await play(ed, s["main"])
    ed.check_eq(await lua(ed, "return script('ChildMaker', 'GcChildMaker', 'GcChild').made"), [1],
                "the child scene's script ran")
    ed.check_eq(await lua(ed, "return script('MainMaker', 'GcMainMaker', 'GcMain').made"), [1],
                "the main scene's script ran")
    await stop_and_collect(ed)


async def test_collected_handle_destroys_its_entity_while_playing(ed: Editor):
    """Valid cases keep their behaviour: a collected owning handle destroys its entity, a
    handle that gave up ownership (entityOwned = false) leaves it to the scene, and handles
    the script keeps are untouched."""
    s = await setup(ed)
    await ed.console_since(None, limit=1)   # errors are checked from here
    await play(ed, s["main"])
    got = await lua(ed, """
        local maker = script('ChildMaker', 'GcChildMaker', 'GcChild')
        local scene = maker.scene
        local function make(owned)
            local b = Button(scene)
            b.entityOwned = owned
            return b.entity
        end
        local owned, unowned = make(true), make(false)
        local before = scene:isEntityCreated(owned)
        collectgarbage('collect'); collectgarbage('collect')
        return before, scene:isEntityCreated(owned), scene:isEntityCreated(unowned),
               scene:isEntityCreated(maker.gcButton.entity), scene:isEntityCreated(maker.gcModel.entity)
    """)
    ed.check_eq(got, [True, False, True, True, True],
                "created / collected owned / collected unowned / kept Button / kept Model")
    await stop_and_collect(ed)


async def test_repeated_play_stop_cycles(ed: Editor):
    """Play and stop several times, collecting inside and outside play: stale handles of earlier
    sessions are collected while later Play copies (maybe at the same addresses) live."""
    s = await setup(ed)
    await ed.console_since(None, limit=1)   # errors are checked from here
    for i in range(4):
        await play(ed, s["main"] if i % 2 == 0 else s["solo"])
        await lua(ed, "collectgarbage('collect'); return true")
        await ed.call("wait", frames=2)
        await ed.call("play", action="stop")
        await ed.call("wait", frames=2)
    await stop_and_collect(ed)
    # a session after all that still works
    await play(ed, s["main"])
    ed.check_eq(await lua(ed, "return script('ChildMaker', 'GcChildMaker', 'GcChild').made"), [1], "fresh session")
    await stop_and_collect(ed)


# -- editor shutdown --------------------------------------------------------------------------


async def raw_call(raw: RawEditor, ids: list, tool: str, **args):
    ids[0] += 1
    raw.send({"jsonrpc": "2.0", "id": ids[0], "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = await raw.response(ids[0], 300)
    result = r.get("result") or {}
    if "error" in r or result.get("isError"):
        raw.fail(f"{tool} {args}: {json.dumps(r)[:800]}")
    return result.get("structuredContent")


async def test_editor_shutdown_after_play_exits_cleanly():
    """Handles left in Lua (globals, uncollected garbage) after play are collected by lua_close
    at shutdown, after the Play copies are gone: the editor still exits with 0."""
    async with RawEditor(create=True, name="Lua GC Lifetime CI") as raw:
        await raw.initialize()
        ids = [100]
        made = await raw_call(raw, ids, "create_scene", name="GcChild", type="UI", path="scenes/GcChild")
        child = made["scene"]["id"]
        ent = await raw_call(raw, ids, "create_entity", scene=child, type="empty", name="ChildMaker")
        await raw_call(raw, ids, "create_script", kind="lua", name="GcChildMaker",
                       content=MAKER.format(cls="GcChildMaker"), scene=child, entity=ent["entity_id"])
        await raw_call(raw, ids, "save_scene", scene=child)
        made = await raw_call(raw, ids, "create_scene", name="GcMain", type="3D", path="scenes/GcMain")
        main = made["scene"]["id"]
        ent = await raw_call(raw, ids, "create_entity", scene=main, type="empty", name="MainMaker")
        await raw_call(raw, ids, "create_script", kind="lua", name="GcMainMaker",
                       content=MAKER.format(cls="GcMainMaker"), scene=main, entity=ent["entity_id"])
        await raw_call(raw, ids, "set_child_scene", scene=main, child=child, start_active=True)
        await raw_call(raw, ids, "save_scene", scene=main)
        for _ in range(2):
            await raw_call(raw, ids, "play", action="start", scene=main)
            await raw_call(raw, ids, "wait", frames=5)
            await raw_call(raw, ids, "play", action="stop")
            await raw_call(raw, ids, "wait", frames=3)
        held = await raw_call(raw, ids, "eval_lua", code="return #gcGlobals", allow_edit_mode=True)
        if held["values"] != [12]:
            raw.fail(f"Lua globals should hold 3 handles per scene per session: {held}")
        code = await raw.close_stdin_and_wait(120)
        stderr = raw.scratch.stderr_log.read_text(errors="replace")
        if code != 0 or "FATAL:" in stderr:
            raw.fail(f"editor shutdown after play: exit code {code}")
