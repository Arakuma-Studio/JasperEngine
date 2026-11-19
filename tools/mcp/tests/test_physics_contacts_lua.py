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


# ---- spatial queries (docs/specs/spatial-queries.md AC18-AC21): query_physics, its hints, the
# query debug draw and the monitors

async def query_scene(ed: Editor, name: str) -> str:
    """Three bodies and a trigger along x: Left (x -3), Middle (0, layer 8), Right (3), Pickup (a trigger, 0, 0, 3)."""
    scene = f"scenes/{name}.scene"
    await ed.call("create_scene", path=scene, name=name, type="3D")
    for entity, x, z in (("Left", -3, 0), ("Middle", 0, 0), ("Right", 3, 0), ("Pickup", 0, 3)):
        await ed.call("create_entity", scene=scene, type="cube", name=entity, position=[x, 0, z])
        await ed.call("edit_colliders", scene=scene, entity=entity, body_type="static",
                      add=[{"shape": "box", "is_trigger": entity == "Pickup"}])
    await ed.call("set_entity_state", scene=scene, entities=["Middle"], layer=8)
    await ed.call("save_scene", scene=scene)
    return scene


def paths(result: dict) -> list[str]:
    return [hit["entity"]["path"] for hit in result["hits"]]


async def test_query_physics_overlap(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("query_physics" not in default, "query_physics is hidden until the physics group is enabled")
    await ed.call("tool_groups", enable=["physics"])
    ed.check("query_physics" in {t.name for t in await ed.tools(groups=None)}, "listed once physics is enabled")

    scene = await query_scene(ed, "Queries")
    # edit mode: physics worlds refuse, the drawn world answers
    await ed.check_error("query_physics", {"op": "overlap", "shape": {"type": "sphere", "radius": 2}, "origin": [0, 0, 0]},
                         contains="play")
    drawn = await ed.call("query_physics", world="drawn", shape={"type": "sphere", "radius": 1}, origin=[-3, 0, 0])
    ed.check_eq(paths(drawn), ["Left"], f"the drawn world in edit mode: {drawn}")
    await ed.check_error("query_physics", {"world": "drawn", "op": "cast", "origin": [0, 0, 0], "direction": [1, 0, 0],
                                           "max_distance": 5}, contains="raycast_meshes")

    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        everything = await ed.call("query_physics", op="overlap", shape={"type": "sphere", "radius": 5}, origin=[0, 0, 0],
                                   layer_mask=-1, verify=True)
        ed.check_eq(sorted(paths(everything)), ["Left", "Middle", "Pickup", "Right"], f"{everything}")
        ed.check(everything["verify"]["ok"] is True and everything["verify"]["reference_count"] == 4, f"verify: {everything['verify']}")
        trigger = next(h for h in everything["hits"] if h["entity"]["path"] == "Pickup")
        ed.check(trigger["trigger"] is True, f"the pickup is a trigger: {trigger}")
        ignored = await ed.call("query_physics", shape={"type": "sphere", "radius": 5}, origin=[0, 0, 0], layer_mask=-1,
                                triggers="ignore", ignore=["Left"])
        ed.check_eq(sorted(paths(ignored)), ["Middle", "Right"], f"triggers ignore and the ignore list: {ignored}")
        check = await ed.call("query_physics", op="check", shape={"type": "box", "half_extents": [0.4, 0.4, 0.4]}, origin=[3, 0, 0])
        ed.check(check["hit"] is True and paths(check) == ["Right"], f"check: {check}")
        cast = await ed.call("query_physics", op="cast", shape={"type": "sphere", "radius": 0.25}, origin=[-8, 0, 0],
                             direction=[1, 0, 0], max_distance=20, layer_mask=-1)
        ed.check_eq(paths(cast), ["Left", "Middle", "Right"], f"cast, nearest first: {cast}")
        ed.check(abs(cast["hits"][0]["distance"] - 4.25) < 0.01 and cast["hits"][0]["normal"][0] < -0.99, f"{cast['hits'][0]}")
        ray = await ed.call("query_physics", op="raycast", origin=[0, 5, 3], direction=[0, -1, 0], max_distance=10)
        ed.check_eq(paths(ray), ["Pickup"], f"raycast hits the trigger by default: {ray}")
        capped = await ed.call("query_physics", shape={"type": "sphere", "radius": 5}, origin=[0, 0, 0], layer_mask=-1, max_hits=2)
        ed.check(capped["count"] == 4 and capped["truncated"] is True and len(capped["hits"]) == 2, f"{capped}")
        problems = await ed.call("read_console", types=["error"], limit=20)
        ed.check(not problems.get("entries"), f"no error: {problems.get('entries')}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)

    # the CLI shares the registry (R30)
    import harness
    import subprocess
    help_text = subprocess.run(harness.editor_command() + ["call", "query_physics", "--help"], capture_output=True, text=True,
                               timeout=300, env={k: v for k, v in __import__("os").environ.items() if k != "DISPLAY"})
    ed.check(help_text.returncode == 0 and "--max-distance" in help_text.stdout and "--verify" in help_text.stdout,
             f"call query_physics --help lists its parameters: {help_text.returncode} {help_text.stdout[:400]} {help_text.stderr[-400:]}")


async def test_query_physics_hints(ed: Editor):
    await ed.call("tool_groups", enable=["physics"])
    scene = await query_scene(ed, "QueryHints")
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        masked = await ed.call("query_physics", shape={"type": "sphere", "radius": 0.5}, origin=[0, 0, 0], layer_mask=1)
        ed.check(masked["count"] == 0, f"{masked}")
        ed.check("layer_mask excludes 1 bodies" in masked.get("next", "") and "8" in masked.get("next", ""),
                 f"the next hint names the layer: {masked}")
        only_trigger = await ed.call("query_physics", shape={"type": "sphere", "radius": 0.5}, origin=[0, 0, 3], triggers="ignore")
        ed.check(only_trigger["count"] == 0 and "triggers: collide" in only_trigger.get("next", ""), f"{only_trigger}")
        found = await ed.call("query_physics", shape={"type": "sphere", "radius": 0.5}, origin=[0, 0, 3], triggers="collide")
        ed.check_eq(paths(found), ["Pickup"], f"{found}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)


async def test_query_debug_draw(ed: Editor):
    from htmlui_pixels import decode_png
    await ed.call("tool_groups", enable=["physics"])
    scene = "scenes/QueryDraw.scene"
    await ed.call("create_scene", path=scene, name="QueryDraw", type="3D")
    await ed.call("create_entity", scene=scene, type="cube", name="Box", position=[0, 0, -40])
    await ed.call("edit_colliders", scene=scene, entity="Box", body_type="static", add=[{"shape": "box"}])
    await ed.call("save_scene", scene=scene)
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)

    def changed(a: bytes, b: bytes) -> tuple[int, int, int, int]:
        """Pixels that differ (left half, right half) and the image size."""
        wa, ha, ca, ra = decode_png(a)
        _, _, cb, rb = decode_png(b)
        left = right = 0
        for y in range(ha):
            for x in range(wa):
                if any(abs(ra[y][x * ca + k] - rb[y][x * cb + k]) > 24 for k in range(3)):
                    if x < wa // 2:
                        left += 1
                    else:
                        right += 1
        return left, right, wa, ha

    try:
        await ed.call("wait", frames=2)
        before = await ed.screenshot("game", max_width=320)
        # the camera looks down -z from the origin: a sphere query left of the centre, the right half untouched
        await ed.call("configure_game_view", physics_debug_queries=True)
        view = await ed.call("configure_game_view")
        ed.check(view["physics_debug_queries"] is True, f"{view}")
        await ed.call("eval_lua", code="""
            _G.queryDrawOn = true
            local function query()
                if not _G.queryDrawOn then return end
                Physics.overlapSphere(scene, Vector3(-4, 0, -20), 2)
                Timer.nextFrame(query)
            end
            Timer.nextFrame(query)
            return true""")
        await ed.call("wait", frames=4)
        on = await ed.screenshot("game", max_width=320)
        left, right, _, _ = changed(before, on)
        ed.check(left > 0 and right == 0, f"the query drawn in its region only: {left} pixels left, {right} right")
        await ed.call("configure_game_view", physics_debug_queries=False)
        await ed.call("eval_lua", code="_G.queryDrawOn = false return true")
        await ed.call("wait", frames=90)
        off = await ed.screenshot("game", max_width=320)
        left, right, _, _ = changed(before, off)
        ed.check(left == 0 and right == 0, f"off: nothing drawn after the entries expired ({left}, {right})")
        # query_physics {draw} shows a query with the toggle off
        await ed.call("query_physics", shape={"type": "sphere", "radius": 2}, origin=[4, 0, -20], draw=True)
        await ed.call("wait", frames=2)
        drawn = await ed.screenshot("game", max_width=320)
        left, right, _, _ = changed(before, drawn)
        ed.check(right > 0 and left == 0, f"query_physics draw: {left} pixels left, {right} right")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
        await ed.call("configure_game_view", physics_debug_queries=False)


async def test_query_monitors(ed: Editor):
    scene = await query_scene(ed, "QueryMonitors")
    await ed.call("play", action="start", scene=scene, fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=2)
        await ed.call("eval_lua", code="""
            local function ten()
                for i = 1, 10 do Physics.overlapSphere(scene, Vector3(0, 0, 0), 5, -1) end
                Timer.nextFrame(ten)
            end
            Timer.nextFrame(ten)
            return true""")
        await ed.call("wait", frames=10)
        r = await ed.call("get_frame_times", window=5, monitors=["Physics queries", "Physics query hits"])
        queries = r["monitors"]["Physics queries"]
        hits = r["monitors"]["Physics query hits"]
        ed.check(queries["latest"] == 10, f"10 queries a frame: {queries}")
        ed.check(hits["latest"] == 40, f"four bodies each: {hits}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
