"""Saved data (docs/specs/savegame.md AC21): user_settings with store "saves" on the SaveGame slots
the game uses in Play: a script's slot listed and read in SaveGame JSON, a slot set by the tool read
by the game, a damaged generation recovered, Restore Backup and the newer-version refusal, a .jsave
file decoded, and the tool's group."""

from __future__ import annotations

import shutil

from harness import Editor

EDITOR = {"create": True, "name": "Saved Data"}
CI = True
TIMEOUT = 600

SAVES = ".jasperengine/user/rundata/saves"

SAVER = """---@class SaveSlotProbe : ScriptInstance
local SaveSlotProbe = { properties = {} }

function SaveSlotProbe:init()
    local ok, err = SaveGame.write("slot1", {gold = 120, pos = Vector3(4, 0, 2), items = {[1] = "a", [5] = "b"}},
        {meta = {title = "Forest"}})
    saveSlotProbe = ok and "queued" or err.message
end

return SaveSlotProbe
"""


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def play_scene(ed: Editor, name: str, script: str | None = None) -> int:
    made = await ed.call("create_scene", name=name, type="2D", path=f"scenes/{name}")
    scene = made["scene"]["id"]
    if script:
        ent = await ed.call("create_entity", scene=scene, type="empty", name="Saver")
        await ed.call("create_script", kind="lua", name="SaveSlotProbe", content=script, scene=scene, entity=ent["entity_id"])
    await ed.call("save_scene", scene=scene)
    await ed.call("play", action="start", scene=scene, auto_pause=True, fixed_fps=60)
    return scene


async def stop(ed: Editor):
    await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_saves_in_play(ed: Editor):
    ed.require_tools("user_settings", "eval_lua", "play", "create_script")
    await ed.call("user_settings", store="saves", action="clear")
    await play_scene(ed, "SaveMain", SAVER)
    try:
        ed.check_eq(await lua(ed, "return saveSlotProbe"), ["queued"])
        listed = await ed.call("user_settings", store="saves", action="list")
        ed.check_eq([s["slot"] for s in listed["slots"]], ["slot1"])
        ed.check_eq(listed["store"], "saves", "a result names the store it acted on")
        ed.check_eq(listed["slots"][0]["meta"], {"title": "Forest"})
        got = await ed.call("user_settings", store="saves", action="get", key="slot1")
        ed.check_eq(got["value"], {"gold": 120, "items": {"#1": "a", "#5": "b"}, "pos": {"$vec3": [4.0, 0.0, 2.0]}})
        ed.check_eq(got["namespaces"], ["main"])

        # the tool's write is the game's at once
        await ed.call("user_settings", store="saves", action="set", key="slot1",
                      value={"gold": 7, "pos": {"$vec3": [1.0, 2.0, 3.0]}}, namespace="main")
        ed.check_eq(await lua(ed, "local d = SaveGame.read('slot1') return d.gold, d.pos.y, math.type(d.gold)"), [7, 2.0, "integer"])
        await ed.call("user_settings", store="saves", action="set", key="slot1", value=["sword"], namespace="inventory")
        ed.check_eq(await lua(ed, "return SaveGame.read('slot1', {namespace = 'inventory'})[1]"), ["sword"])

        await ed.check_error("user_settings", {"store": "saves", "action": "set", "key": "slot1", "value": 3},
                             contains="must be an object or array in SaveGame JSON")
        await ed.check_error("user_settings", {"store": "saves", "action": "get", "key": "slot2"},
                             contains='No slot "slot2". Slots: slot1.')
        await ed.check_error("user_settings", {"store": "saves", "action": "set", "key": "Slot 1", "value": {}},
                             contains='try "slot-1"')
    finally:
        await stop(ed)
    ed.check((ed.project / SAVES).is_dir(), "Play stop flushed the slot to rundata/saves")


async def test_corrupt_and_recover(ed: Editor):
    ed.require_tools("user_settings", "eval_lua", "play")
    await ed.call("user_settings", store="saves", action="clear")
    for i in (1, 2, 3):
        await ed.call("user_settings", store="saves", action="set", key="slot1", value={"i": i})
    corrupted = await ed.call("user_settings", store="saves", action="corrupt", key="slot1", mode="flip")
    ed.check_eq(corrupted["generation"], 3)
    ed.check_in("eval_lua", corrupted["next"])
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]

    await play_scene(ed, "RecoverMain")
    try:
        ed.check_eq(await lua(ed, "local d, i = SaveGame.read('slot1') return d.i, i.recoveredFrom, #i.problems"), [2, 2, 1])
        listed = await ed.call("user_settings", store="saves", action="list")
        ed.check_eq(listed["slots"][0]["status"], "recovered")
    finally:
        await stop(ed)
    warnings = (await ed.call("read_console", since=cursor, types=["warning"]))["entries"]
    ed.check(any("recovered from generation 2" in str(w) for w in warnings), f"one recovery warning: {warnings}")
    ed.check((ed.project / SAVES / "slot1.g3.jsave.corrupt").exists(), "the damaged generation is set aside")


async def test_restore_and_newer_version(ed: Editor):
    ed.require_tools("user_settings", "undo")
    await ed.call("user_settings", store="saves", action="clear")
    await ed.call("user_settings", store="saves", action="set", key="slot2", value={"a": 1})
    await ed.call("user_settings", store="saves", action="set", key="slot2", value={"a": 2})
    restored = await ed.call("user_settings", store="saves", action="restore", key="slot2", generation=1)
    ed.check_eq(restored["generation"], 3)
    ed.check_eq((await ed.call("user_settings", store="saves", action="get", key="slot2"))["value"], {"a": 1})
    old = await ed.call("user_settings", store="saves", action="get", key="slot2", generation=2)
    ed.check_eq(old["value"], {"a": 2})

    # a newer version is never written over by an older one
    await ed.call("user_settings", store="saves", action="set", key="slot2", value={"a": 5}, version=5)
    await ed.check_error("user_settings", {"store": "saves", "action": "set", "key": "slot2", "value": {"a": 4}, "version": 4},
                         contains='slot "slot2" is version 5 and this game writes 4: pass version 5 to overwrite it, or read it with get.')

    # a data version is 1..2147483647: 0 or a negative one is refused, never cast into a huge one
    for bad in (0, -3, 4294967296):
        await ed.check_error("user_settings", {"store": "saves", "action": "set", "key": "slot2", "value": {"a": 6}, "version": bad},
                             contains=f"set: version must be an integer from 1 to 2147483647 (got {bad})")

    await ed.call("user_settings", store="saves", action="corrupt", key="slot2", mode="truncate")
    await ed.check_error("user_settings", {"store": "saves", "action": "restore", "key": "slot2", "generation": 4},
                         contains='Couldn\'t restore "slot2" generation 4: the file is corrupt')

    # delete is an undo step
    await ed.call("user_settings", store="saves", action="delete", key="slot2")
    ed.check_eq((await ed.call("user_settings", store="saves", action="list"))["count"], 0)
    await ed.call("undo", scope="project")
    ed.check_eq([s["slot"] for s in (await ed.call("user_settings", store="saves", action="list"))["slots"]], ["slot2"])


async def test_file_read(ed: Editor):
    ed.require_tools("user_settings")
    await ed.call("user_settings", store="saves", action="clear")
    await ed.call("user_settings", store="saves", action="set", key="sent", value={"level": "cave", "hp": 0.5})
    source = next((ed.project / SAVES).glob("sent.g*.jsave"))
    (ed.project / "reports").mkdir(exist_ok=True)
    shutil.copy(source, ed.project / "reports" / "from-player.jsave")
    got = await ed.call("user_settings", store="saves", action="get", file="reports/from-player.jsave")
    ed.check_eq(got["namespaces"], {"main": {"hp": 0.5, "level": "cave"}})
    (ed.project / "reports" / "broken.jsave").write_bytes(b"not a save")
    await ed.check_error("user_settings", {"store": "saves", "action": "get", "file": "reports/broken.jsave"},
                         contains="not a readable save file")


async def test_group_listing(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("user_settings" not in default, "hidden until scripting is enabled")
    await ed.call("tool_groups", enable=["scripting"])
    tools = {t.name: t for t in await ed.tools(groups=None)}
    ed.check("user_settings" in tools, "listed once scripting is enabled")
    schema = tools["user_settings"].input_schema
    for key in ("store", "namespace", "generation", "mode", "file"):
        ed.check_in(key, schema["properties"])
