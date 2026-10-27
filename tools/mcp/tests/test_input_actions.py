"""S22 Input Actions as an agent drives them (docs/specs/S22-gameplay.md AC15, R38, R42, R34):
edit_input_actions builds the spec's asset (§6) op by op as undo steps, undo/redo restore the
file byte for byte, get_input_actions reads it back with display names, Play with that asset
(Project Settings > Input) maps W to Gameplay/Move = (0, 1), enabling the exclusive Menu map
makes it (0, 0), and send_input's input_action event fires an action's onPerformed once.
"""

import json

from harness import Editor, tool_meta

CI = True
EDITOR = {"create": True, "name": "Input Actions CI"}

TIMEOUT = 900

ASSET = "assets/input/player.inputactions"

PLAYER = """---@class ActionReader : ScriptInstance
local ActionReader = { properties = {} }

function ActionReader:init()
    performed = 0
    canceled = 0
    InputActions.get("Gameplay/Jump").onPerformed = function(e) performed = performed + 1 end
    InputActions.get("Gameplay/Jump").onCanceled = function(e) canceled = canceled + 1 end
end

return ActionReader
"""

# the spec's asset, as ops (ids are made by the editor)
OPS = [
    {"op": "add_map", "name": "Gameplay"},
    {"op": "add_action", "map": "Gameplay", "name": "Move", "type": "VECTOR2"},
    {"op": "add_composite", "action": "Gameplay/Move", "composite": "dpad2d",
     "parts": {"up": "keyboard/w", "down": "keyboard/s", "left": "keyboard/a", "right": "keyboard/d"},
     "groups": ["Keyboard&Mouse"]},
    {"op": "add_binding", "action": "Gameplay/Move", "path": "gamepad/left_stick", "groups": ["Gamepad"]},
    {"op": "add_action", "map": "Gameplay", "name": "Jump", "type": "BUTTON"},
    {"op": "add_binding", "action": "Gameplay/Jump", "path": "keyboard/space"},
    {"op": "add_binding", "action": "Gameplay/Jump", "path": "gamepad/a"},
    {"op": "add_map", "name": "Menu", "priority": 100, "enabled": False, "exclusive": True},
]

S: dict = {}


def read_bytes(ed: Editor) -> bytes:
    return (ed.project / ASSET).read_bytes()


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="ActionsCI", type="3D", path="scenes/ActionsCI")
    S["scene"] = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=S["scene"], type="empty", name="Reader")
    await ed.call("create_script", kind="lua", name="ActionReader", content=PLAYER, scene=S["scene"], entity=ent["entity_id"])
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("save_project")
    return S


async def test_tools_are_in_the_gameplay_group(ed: Editor):
    ed.require_tools("tool_groups", "get_input_actions", "edit_input_actions")
    for name in ("get_input_actions", "edit_input_actions"):
        ed.check_eq(tool_meta(ed.tool(name)).get("jasper/group"), "gameplay", f"{name}'s group")
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_input_actions" not in default, "hidden by default")


async def test_build_the_asset_and_undo_byte_for_byte(ed: Editor):
    await setup(ed)
    states = []
    for op in OPS:
        result = await ed.call("edit_input_actions", path=ASSET, ops=[op])
        ed.check_eq(result["applied"], 1, f"{op}: {result}")
        states.append(read_bytes(ed))
    text = json.loads(states[-1])
    ed.check_eq(text["version"], 1, "version")
    ed.check_eq([m["name"] for m in text["maps"]], ["Gameplay", "Menu"], "maps")
    menu = text["maps"][1]
    ed.check_eq((menu["priority"], menu["enabled"], menu["exclusive"]), (100, False, True), f"Menu: {menu}")
    move = text["maps"][0]["actions"][0]
    ed.check_eq(move["bindings"][0]["composite"], "dpad2d", f"Move: {move}")
    ed.check_eq(move["bindings"][0]["parts"]["up"]["path"], "keyboard/w", f"Move: {move}")
    ids = [b["id"] for a in text["maps"][0]["actions"] for b in a["bindings"]]
    ed.check(all(len(i) == 8 and all(c in "0123456789abcdef" for c in i) for i in ids), f"8-hex ids: {ids}")

    # each undo restores the file of the step before, byte for byte; redo the step after
    for i in range(len(OPS) - 1, 0, -1):
        undone = await ed.call("undo", scope="project")
        ed.check_eq(len(undone["undone"]), 1, f"one step: {undone}")
        ed.check_eq(read_bytes(ed), states[i - 1], f"undo of step {i}")
    for i in range(1, len(OPS)):
        await ed.call("redo", scope="project")
        ed.check_eq(read_bytes(ed), states[i], f"redo of step {i}")

    read = await ed.call("get_input_actions", path=ASSET)
    names = read["display_names"]
    ed.check_eq(names[ids[0]], "W/A/S/D", f"display names: {names}")
    ed.check_eq(names[ids[1]], "Left Stick", f"display names: {names}")

    # a failing op changes nothing
    bad = await ed.call_raw("edit_input_actions", {"path": ASSET, "ops": [{"op": "add_binding", "action": "Gameplay/Jump", "path": "keyboard/nope"}]})
    ed.check(bad.is_error, f"a bad path fails: {bad.text}")
    ed.check_eq(read_bytes(ed), states[-1], "nothing was written")


async def test_play_reads_actions_and_menu_blocks(ed: Editor):
    await setup(ed)
    await ed.call("set_project_settings", values={"input.actions": ASSET})
    started = await ed.call("play", action="start", scene=S["scene"], auto_pause=True, fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"start: {started}")
    try:
        await ed.call("send_input", events=[{"type": "key", "key": "w", "action": "down"}], frames_after=0)
        move = (await ed.call("eval_lua", code="local v = InputActions.get('Gameplay/Move'):readVector2() return {v.x, v.y}"))["values"][0]
        ed.check(abs(move[0]) < 1e-4 and abs(move[1] - 1) < 1e-4, f"W gives (0, 1): {move}")
        state = await ed.call("get_input_state", fields=["actions"])
        gameplay = [m for m in state["actions"]["maps"] if m["name"] == "Gameplay"][0]
        moved = [a for a in gameplay["actions"] if a["name"] == "Move"][0]
        ed.check(moved["pressed"], f"get_input_state shows the action: {moved}")

        await ed.call("eval_lua", code="InputActions.enableMap('Menu', true)")
        await ed.call("wait", frames=1)
        move = (await ed.call("eval_lua", code="local v = InputActions.get('Gameplay/Move'):readVector2() return {v.x, v.y}"))["values"][0]
        ed.check(abs(move[0]) < 1e-4 and abs(move[1]) < 1e-4, f"the exclusive Menu blocks Gameplay: {move}")
        state = await ed.call("get_input_state", fields=["actions"])
        gameplay = [m for m in state["actions"]["maps"] if m["name"] == "Gameplay"][0]
        ed.check(gameplay.get("blocked"), f"Gameplay is blocked: {gameplay}")
        await ed.call("eval_lua", code="InputActions.enableMap('Menu', false)")
        await ed.call("send_input", events=[{"type": "key", "key": "w", "action": "up"}], frames_after=0)

        # an injected press: onPerformed once, then onCanceled once
        await ed.call("eval_lua", code="performed = 0 canceled = 0")
        await ed.call("send_input", events=[{"type": "input_action", "name": "Gameplay/Jump", "action": "press", "hold_frames": 0}])
        counts = (await ed.call("eval_lua", code="return {performed, canceled}"))["values"][0]
        ed.check_eq(counts, [1, 1], "a tap of the action performs and cancels once")
        await ed.call("send_input", events=[{"type": "input_action", "name": "Gameplay/Jump", "action": "press", "hold_frames": 3}])
        counts = (await ed.call("eval_lua", code="return {performed, canceled}"))["values"][0]
        ed.check_eq(counts, [2, 2], "a held press performs once")
        bad = await ed.call_raw("send_input", {"events": [{"type": "input_action", "name": "Gameplay/Nope"}]})
        ed.check(bad.is_error, f"an unknown action fails: {bad.text}")

        # a live edit applies during Play
        await ed.call("edit_input_actions", path=ASSET, ops=[{"op": "add_binding", "action": "Gameplay/Jump", "path": "keyboard/j"}])
        await ed.call("eval_lua", code="performed = 0")
        await ed.call("send_input", events=[{"type": "key", "key": "j", "hold_frames": 0}])
        counts = (await ed.call("eval_lua", code="return performed"))["values"]
        ed.check_eq(counts, [1], "the new binding works at once")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
        await ed.call("set_project_settings", values={"input.actions": ""})
