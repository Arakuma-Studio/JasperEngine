"""Lua handlers of the 3D physics events run on the Lua thread.

Jolt reports contacts and activations from its job threads. A Lua handler of onContactAdded3D,
onContactPersisted3D, onContactRemoved3D or onBodyActivated3D used to run there, Lua off its own
thread: with the harness's JASPER_LUA_THREAD_CHECK=abort the editor aborted (an upgraded
origin/main game did, tools/ci/upgrade_from_main), without it the state was used from other
threads. The events are delivered on the main thread after the step now; a shouldCollide3D
handler, which answers inside the step, makes the step run on the main thread.
"""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Physics Contacts Lua"}

SCRIPT = """---@class ContactWatch : ScriptInstance
local ContactWatch = { properties = {} }

function ContactWatch:init()
    self.added, self.persisted, self.removed, self.activated, self.filtered = 0, 0, 0, 0, 0
    self.friction = -1
    local physics = self.scene:getPhysicsSystem()
    RegisterEvent(self, physics.onContactAdded3D, "onAdded")
    RegisterEvent(self, physics.onContactPersisted3D, "onPersisted")
    RegisterEvent(self, physics.onContactRemoved3D, "onRemoved")
    RegisterEvent(self, physics.onBodyActivated3D, "onActivated")
    RegisterEvent(self, physics.shouldCollide3D, "filter")
end

function ContactWatch:onAdded(a, b, contact)
    self.added = self.added + 1
    self.friction = contact.combinedFriction
    self.points = contact:getContactPointCount()
end
function ContactWatch:onPersisted(a, b, contact) self.persisted = self.persisted + 1 end
function ContactWatch:onRemoved(a, b, shapeA, shapeB) self.removed = self.removed + 1 end
function ContactWatch:onActivated(body) self.activated = self.activated + 1 end
function ContactWatch:filter(a, b, offset, result)
    self.filtered = self.filtered + 1
    return true
end

return ContactWatch
"""


async def test_lua_contact_handlers_run_on_the_lua_thread(ed: Editor):
    scene = "scenes/Contacts.scene"
    await ed.call("create_scene", path=scene, name="Contacts", type="3D")
    await ed.call("create_entity", scene=scene, type="cube", name="Floor", position=[0, -0.5, 0], scale=[30, 1, 30])
    await ed.call("edit_colliders", scene=scene, entity="Floor", body_type="static", add=[{"shape": "box"}])
    for i in range(12):
        name = f"Box {i}"
        await ed.call("create_entity", scene=scene, type="cube", name=name, position=[(i % 4) * 1.1 - 2, 0.6 + (i // 4) * 1.1, 0])
        await ed.call("edit_colliders", scene=scene, entity=name, body_type="dynamic", add=[{"shape": "box"}])
    await ed.call("create_entity", scene=scene, type="empty", name="Watch")
    await ed.call("create_script", kind="lua", name="ContactWatch", content=SCRIPT, entity="Watch", scene=scene)
    await ed.call("save_scene", scene=scene)

    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        # the editor aborted here when a handler ran on one of Jolt's threads
        await ed.call("wait", frames=120)
        got = (await ed.call("eval_lua", code="local w = script('Watch'); return {w.added, w.persisted, w.activated, w.filtered, w.friction, w.points}"))["values"][0]
        added, persisted, activated, filtered, friction, points = got
        ed.check(added > 0 and persisted > 0, f"contacts reached the Lua handlers: {got}")
        ed.check(activated > 0, f"activations reached the Lua handler: {got}")
        ed.check(filtered > 0, f"the shouldCollide3D handler answered: {got}")
        ed.check(friction >= 0 and points >= 1, f"the delivered contact carries its values: {got}")
        state = await ed.call("get_play_state")
        ed.check(state.get("state") in ("playing", "paused") or state.get("playing"), f"still playing: {state}")
        problems = await ed.call("read_console", types=["error"], limit=20)
        ed.check(not problems.get("entries"), f"no error: {problems.get('entries')}")
    finally:
        await ed.call("play", action="stop")
