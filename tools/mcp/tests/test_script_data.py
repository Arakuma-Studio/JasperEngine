"""Script data (spec 2026-10-31-script-data 9): the user_settings tool on the project's
UserSettings / PlayerPrefs store, a played script writing PlayerPrefs and a JSON save, eval_lua
reading them, the store flushed to .jasperengine/user/rundata/UserSettings.xml when play stops,
and Json error texts through eval_lua."""

from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path

from harness import Editor

EDITOR = {"create": True, "name": "Script Data"}
CI = True
TIMEOUT = 600

RUNDATA = ".jasperengine/user/rundata"

SAVER = """---@class SaveProbe : ScriptInstance
local SaveProbe = { properties = {} }

function SaveProbe:init()
    PlayerPrefs.setInt("Runs", PlayerPrefs.getInt("Runs") + 1)
    PlayerPrefs.setString("Hello", "world")
    local ok, err = Json.writeFile("data://saves/slot1.json", {
        coins = 7, items = {"a", "b"}, empty = Json.object(), none = Json.null,
    })
    saveProbeResult = ok and "saved" or err
end

return SaveProbe
"""


async def lua(ed: Editor, code: str, **args):
    return (await ed.call("eval_lua", code=code, **args))["values"]


def settings_file(ed: Editor) -> Path:
    return ed.project / RUNDATA / "UserSettings.xml"


async def file_contains(ed: Editor, path: Path, needles: list[str], timeout: float = 15.0) -> str:
    """The text of `path` once it has every needle (play stop finishes on a later frame)."""
    deadline = time.monotonic() + timeout
    text = ""
    while time.monotonic() < deadline:
        text = path.read_text(encoding="utf-8") if path.exists() else ""
        if all(n in text for n in needles):
            return text
        await ed.call("editor_status")
        await asyncio.sleep(0.2)
    ed.fail(f"{path} does not contain {needles} after {timeout:.0f}s:\n{text}")
    return text


async def test_user_settings_tool(ed: Editor):
    ed.require_tools("user_settings", "eval_lua")
    await ed.call("user_settings", action="clear")
    ed.check_eq((await ed.call("user_settings", action="list"))["values"], {})

    made = await ed.call("user_settings", action="set", key="Coins", value=120)
    ed.check_eq((made["type"], made["value"]), ("int", "120"))
    ed.check_eq((await ed.call("user_settings", action="set", key="Volume", value=0.5))["type"], "float")
    ed.check_eq((await ed.call("user_settings", action="set", key="Name", value="Ana"))["type"], "string")
    ed.check_eq((await ed.call("user_settings", action="set", key="Music", value=True))["value"], "true")
    ed.check_eq((await ed.call("user_settings", action="set", key="Gold", value=3, type="float"))["value"], "3")
    ed.check_eq((await ed.call("user_settings", action="set", key="best score", value="")),
                {"key": "best score", "type": "string", "value": "", "file": f"{RUNDATA}/UserSettings.xml"})

    listed = await ed.call("user_settings", action="list")
    ed.check_eq(listed["values"], {"Coins": "120", "Volume": "0.5", "Name": "Ana", "Music": "true", "Gold": "3",
                                   "best score": ""})
    ed.check_eq(listed["count"], 6)
    ed.check_eq(listed["file"], f"{RUNDATA}/UserSettings.xml")
    got = await ed.call("user_settings", action="get", key="Coins")
    ed.check_eq((got["exists"], got["value"]), (True, "120"))
    ed.check_eq((await ed.call("user_settings", action="get", key="Nope"))["exists"], False)

    # what the game reads (edit-mode Lua shares the store)
    ed.check_eq(await lua(ed, "return PlayerPrefs.getInt('Coins'), PlayerPrefs.getFloat('Volume'), "
                              "PlayerPrefs.getString('Name'), UserSettings.getBoolForKey('Music'), "
                              "PlayerPrefs.hasKey('best score'), PlayerPrefs.getString('best score', 'default')",
                          allow_edit_mode=True),
                [120, 0.5, "Ana", True, True, ""])

    # every write is on disk at once
    xml = settings_file(ed).read_text(encoding="utf-8")
    for needle in ("<Coins>120</Coins>", "<Volume>0.5</Volume>", "<Name>Ana</Name>", '<entry key="best score"/>'):
        ed.check_in(needle, xml)

    # mistakes say what is wrong
    await ed.check_error("user_settings", {"action": "set", "key": "Coins", "value": 3.5, "type": "int"}, contains="integer")
    await ed.check_error("user_settings", {"action": "set", "key": "Coins", "value": "7", "type": "int"}, contains="integer")
    await ed.check_error("user_settings", {"action": "set", "key": "Coins", "value": 7, "type": "string"}, contains="string")
    await ed.check_error("user_settings", {"action": "set", "key": "Coins"}, contains="value")
    await ed.check_error("user_settings", {"action": "get"}, contains="key")
    await ed.check_error("user_settings", {"action": "set", "key": "X", "value": 2 ** 40}, contains="32 bits")

    deleted = await ed.call("user_settings", action="delete", key="Coins")
    ed.check_eq(deleted["deleted"], True)
    ed.check_eq((await ed.call("user_settings", action="delete", key="Coins"))["deleted"], False)
    ed.check_eq(await lua(ed, "return PlayerPrefs.hasKey('Coins'), PlayerPrefs.getInt('Coins', -1)", allow_edit_mode=True),
                [False, -1])
    cleared = await ed.call("user_settings", action="clear")
    ed.check_eq(cleared["deleted"], 5)
    ed.check_eq((await ed.call("user_settings", action="list"))["count"], 0)
    ed.check("<Name>" not in settings_file(ed).read_text(encoding="utf-8"), "clear reaches the file")


async def test_played_script_saves_and_play_stop_flushes(ed: Editor):
    ed.require_tools("user_settings", "eval_lua", "play", "create_script")
    await ed.call("user_settings", action="clear")
    made = await ed.call("create_scene", name="SaveMain", type="2D", path="scenes/SaveMain")
    scene = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=scene, type="empty", name="Saver")
    await ed.call("create_script", kind="lua", name="SaveProbe", content=SAVER, scene=scene, entity=ent["entity_id"])
    await ed.call("save_scene", scene=scene)
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]

    # auto_pause: no frames run between the calls, so only Play stop writes what eval_lua sets
    await ed.call("play", action="start", scene=scene, auto_pause=True, fixed_fps=60)
    try:
        ed.check_eq(await lua(ed, "return saveProbeResult"), ["saved"])
        ed.check_eq(await lua(ed, "return PlayerPrefs.getInt('Runs'), PlayerPrefs.getString('Hello')"), [1, "world"])
        save = await lua(ed, "local s = Json.readFile('data://saves/slot1.json') return s.coins, Json.encode(s), Json.isNull(s.none)")
        ed.check_eq(save, [7, '{"coins":7,"empty":{},"items":["a","b"],"none":null}', True])
        ed.check_eq(json.loads((ed.project / RUNDATA / "saves" / "slot1.json").read_text(encoding="utf-8")),
                    {"coins": 7, "items": ["a", "b"], "empty": {}, "none": None})

        # the tool sees the running game's values and the game sees the tool's at once
        listed = await ed.call("user_settings", action="list")
        ed.check_eq((listed["values"].get("Runs"), listed["values"].get("Hello")), ("1", "world"))
        await ed.call("user_settings", action="set", key="Hello", value="changed")
        ed.check_eq(await lua(ed, "return PlayerPrefs.getString('Hello')"), ["changed"])

        # set by the game only, written by Play stop
        await lua(ed, "PlayerPrefs.setFloat('StopFlush', 0.25) UserSettings.setStringForKey('Quote', 'a <b> & c')")

        # Json errors name the place
        ed.check_eq(await lua(ed, "local ok, e = pcall(Json.decode, '{\"a\": 1,}') return e"),
                    ["Json.decode: syntax error at line 1, column 9: unexpected '}'; expected string literal"])
        ed.check_eq(await lua(ed, "return select(2, Json.tryEncode({player = {onHit = print}}))"),
                    ["cannot encode a function at $.player.onHit"])
        ed.check_eq(await lua(ed, "return select(2, Json.readFile('data://missing.json'))"),
                    ["not found: data://missing.json"])
        ed.check_eq(await lua(ed, "return select(2, File.writeText('asset://x.json', '{}'))"),
                    ["read-only path: asset://x.json"])
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)

    xml = await file_contains(ed, settings_file(ed), ["<Runs>1</Runs>", "<Hello>changed</Hello>",
                                                      "<StopFlush>0.25</StopFlush>", "<Quote>a &lt;b&gt; &amp; c</Quote>"])
    ed.check(not [p.name for p in settings_file(ed).parent.iterdir() if p.name.endswith(".tmp")], "no temporary file left")

    # the next Play reads what the last one saved
    await ed.call("play", action="start", scene=scene, auto_pause=True, fixed_fps=60)
    try:
        ed.check_eq(await lua(ed, "return PlayerPrefs.getInt('Runs'), PlayerPrefs.getFloat('StopFlush')"), [2, 0.25])
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    await file_contains(ed, settings_file(ed), ["<Runs>2</Runs>"])
    errors = (await ed.call("read_console", since=cursor, types=["error"]))["entries"]
    ed.check(not errors, f"unexpected errors: {errors}")
