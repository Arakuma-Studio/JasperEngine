"""Script properties follow the script (scripting catch-up, Doriax 3d7a91e4e): a saved Lua script
that no longer declares a `properties` table has none in the Inspector, and a scene opened after
its scripts changed on disk shows their current fields without being marked modified."""

import os
from pathlib import Path

from harness import Editor

EDITOR = {"create": True, "name": "Script Properties Refresh"}
CI = True

WITH_SPEED = """local Patrol = {
    properties = {
        { name = "speed", type = "float", default = 5.0 },
    }
}
return Patrol
"""
WITHOUT_TABLE = """local Patrol = {}
function Patrol:init()
end
return Patrol
"""
WITH_JUMP = WITH_SPEED.replace('{ name = "speed", type = "float", default = 5.0 },',
                               '{ name = "speed", type = "float", default = 5.0 },\n'
                               '        { name = "jump", type = "float", default = 2.0 },')


def touch_write(path: Path, text: str) -> None:
    """An external edit, its stamp past the last MCP write's window."""
    previous = path.stat().st_mtime_ns if path.exists() else 0
    path.write_text(text)
    stamp = path.stat()
    os.utime(path, ns=(stamp.st_atime_ns, max(previous, stamp.st_mtime_ns) + 2_000_000_000))


def names(script: dict) -> list:
    return [p["name"] for p in script["properties"]]


async def test_dropped_properties_table_clears(ed: Editor):
    entity = (await ed.call("create_entity", name="Guard", type="box"))["entity_id"]
    made = await ed.call("create_script", kind="lua", name="Patrol", content=WITH_SPEED, entity=entity)
    await ed.call("set_script_properties", entity=entity, values={"speed": 9})
    ed.check_eq(names((await ed.call("list_scripts", entity=entity))["scripts"][0]), ["speed"])

    touch_write(ed.project / made["path"], WITHOUT_TABLE)
    listed = await ed.call("list_scripts", entity=entity)
    ed.check_eq(names(listed["scripts"][0]), [], "a saved script without a properties table has no fields")


async def test_scene_open_shows_current_fields(ed: Editor):
    await ed.call("create_scene", name="Opened")
    entity = (await ed.call("create_entity", name="Sentry", type="box"))["entity_id"]
    made = await ed.call("create_script", kind="lua", name="Sentry", content=WITH_SPEED, entity=entity)
    await ed.call("save_scene", path="Opened.scene")
    await ed.call("create_scene", name="Other")
    await ed.call("close_scene", scene="Opened")

    # changed while the scene was closed
    touch_write(ed.project / made["path"], WITH_JUMP)
    await ed.call("editor_status")
    await ed.call("open_scene", scene="Opened.scene")
    listed = await ed.call("list_scripts", entity="Sentry")
    ed.check_eq(names(listed["scripts"][0]), ["speed", "jump"], "the opened scene shows the script's current fields")
    scenes = (await ed.call("list_scenes"))["scenes"]
    opened = next(s for s in scenes if s["name"] == "Opened")
    ed.check_eq(opened["modified"], False, "the refresh at open is not an edit")
