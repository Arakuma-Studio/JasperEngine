"""Input (spec docs/superpowers/specs/2026-10-31-input-design.md) end to end in editor Play: key edges
per frame, input actions fed by the gamepad, the keyboard and the MCP virtual source, touches with
phases and finger ids, Project Settings > Input authored over MCP (undoable, saved), the input
block of get_play_state, and the send_input action / axis / axis_delta events.

A probe script counts what Input reports in each onUpdate; the game runs at a fixed 60 fps with
auto_pause, so it advances only inside send_input / wait and every count is exact.
"""

import yaml

from harness import Editor

EDITOR = {"create": True, "name": "Input Actions"}
TIMEOUT = 900

PROBE = """---@class InputProbe : ScriptInstance
local InputProbe = { properties = {} }

local PHASES = {[TouchPhase.Began] = "Began", [TouchPhase.Moved] = "Moved", [TouchPhase.Stationary] = "Stationary",
                [TouchPhase.Ended] = "Ended", [TouchPhase.Canceled] = "Canceled"}

function InputProbe:init()
    _G.probe = {spaceDowns = 0, spaceUps = 0, spaceHeld = 0, jumpDowns = 0, jumpUps = 0, jumpEvents = 0,
                dashDowns = 0, frames = 0, touches = {}, mouseX = {}}
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onButtonDown")
end

function InputProbe:onButtonDown(action)
    if action == "Jump" then _G.probe.jumpEvents = _G.probe.jumpEvents + 1 end
end

function InputProbe:onUpdate()
    local p = _G.probe
    p.frames = p.frames + 1
    if Input.getKeyDown("space") then p.spaceDowns = p.spaceDowns + 1 end
    if Input.getKeyUp(Input.KEY_SPACE) then p.spaceUps = p.spaceUps + 1 end
    if Input.getKey("space") then p.spaceHeld = p.spaceHeld + 1 end
    if Input.getButtonDown("Jump") then p.jumpDowns = p.jumpDowns + 1 end
    if Input.getButtonUp("Jump") then p.jumpUps = p.jumpUps + 1 end
    if Input.hasAction("Dash") and Input.getButtonDown("Dash") then p.dashDowns = p.dashDowns + 1 end
    if Input.getAxis("Mouse X") ~= 0 then p.mouseX[#p.mouseX + 1] = string.format("%g", Input.getAxis("Mouse X")) end
    for _, touch in ipairs(Input.touches) do
        if touch.phase ~= TouchPhase.Stationary then
            p.touches[#p.touches + 1] = touch.fingerId .. ":" .. PHASES[touch.phase]
        end
    end
end

return InputProbe
"""


async def lua(ed: Editor, code: str):
    return (await ed.call("eval_lua", code=code))["values"]


async def probe(ed: Editor) -> dict:
    values = await lua(ed, "local p = _G.probe; return p.spaceDowns, p.spaceUps, p.spaceHeld, p.jumpDowns, p.jumpUps, "
                           "p.jumpEvents, p.dashDowns, table.concat(p.touches, ',')")
    keys = ["spaceDowns", "spaceUps", "spaceHeld", "jumpDowns", "jumpUps", "jumpEvents", "dashDowns", "touches"]
    return dict(zip(keys, values))


async def reset_probe(ed: Editor) -> None:
    await lua(ed, "local p = _G.probe; p.spaceDowns, p.spaceUps, p.spaceHeld, p.jumpDowns, p.jumpUps, p.jumpEvents, "
                  "p.dashDowns = 0, 0, 0, 0, 0, 0, 0; p.touches = {}")


async def send(ed: Editor, *events, frames_after=2):
    r = await ed.call_raw("send_input", {"events": list(events), "frames_after": frames_after})
    ed.check(not r.is_error, f"send_input {events}: {r.text[:400]}")
    return r


async def axis(ed: Editor, name: str, raw: bool = False) -> float:
    fn = "getAxisRaw" if raw else "getAxis"
    return (await lua(ed, f"return Input.{fn}('{name}')"))[0]


async def start(ed: Editor) -> None:
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    await ed.call("wait", frames=2)


async def stop(ed: Editor) -> None:
    await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_setup(ed: Editor):
    await ed.call("create_entity", name="InputProbe", type="empty")
    await ed.call("create_script", kind="lua", name="InputProbe", entity="InputProbe", content=PROBE)
    await ed.call("save_project", untitled_scene_dir="scenes")
    settings = await ed.call("get_project_settings", keys=["input.actions"])
    names = [a["name"] for a in settings["settings"]["input.actions"]]
    ed.check_eq(names, ["Horizontal", "Vertical", "Fire1", "Fire2", "Fire3", "Jump", "Mouse X", "Mouse Y",
                        "Mouse ScrollWheel", "Submit", "Cancel"], "Unity's defaults")
    # nothing in project.yaml until edited
    project_yaml = yaml.safe_load((ed.project / "project.yaml").read_text())
    ed.check("input" not in project_yaml, f"an input node before any edit: {project_yaml.get('input')}")
    # and the default listing leaves the long list out
    listing = await ed.call("get_project_settings")
    ed.check("input.actions" not in listing["settings"], "input.actions is asked for by name")


async def test_key_press_gives_one_down_one_up(ed: Editor):
    await start(ed)
    try:
        await reset_probe(ed)
        await send(ed, {"type": "key", "key": "space", "hold_frames": 3}, frames_after=3)
        p = await probe(ed)
        ed.check_eq(p["spaceDowns"], 1, f"one getKeyDown: {p}")
        ed.check_eq(p["spaceUps"], 1, f"one getKeyUp: {p}")
        ed.check_eq(p["spaceHeld"], 3, f"getKey for 3 frames: {p}")
        ed.check_eq(p["jumpDowns"], 1, f"space is Jump: one getButtonDown: {p}")
        ed.check_eq(p["jumpUps"], 1, f"and one getButtonUp: {p}")
        ed.check_eq(p["jumpEvents"], 1, f"one onButtonDown('Jump'): {p}")

        # the action itself, through the MCP source
        await reset_probe(ed)
        await send(ed, {"type": "action", "name": "Jump", "phase": "press", "hold_frames": 2}, frames_after=2)
        p = await probe(ed)
        ed.check_eq((p["jumpDowns"], p["jumpUps"], p["spaceDowns"]), (1, 1, 0), f"action press: {p}")
        await ed.check_error("send_input", events=[{"type": "action", "name": "Fire4"}], contains="no input action \"Fire4\"")
    finally:
        await stop(ed)


async def test_gamepad_axis_is_rescaled(ed: Editor):
    await start(ed)
    try:
        await send(ed, {"type": "gamepad", "axis": "left_x", "value": 0.6})
        value = await axis(ed, "Horizontal")
        ed.check(abs(value - 0.506) < 0.001, f"left_x 0.6 past the 0.19 dead zone reads 0.506, got {value}")
        raw = await axis(ed, "Horizontal", raw=True)
        ed.check(abs(raw - value) < 1e-6, f"no smoothing on a gamepad axis: raw {raw}")
        await send(ed, {"type": "release_all"})
        ed.check_eq(await axis(ed, "Horizontal"), 0, "released")
    finally:
        await stop(ed)


async def test_sources_sum_and_clamp(ed: Editor):
    await start(ed)
    try:
        # the MCP source alone, until released
        await send(ed, {"type": "axis", "name": "Horizontal", "value": 1})
        ed.check_eq(await axis(ed, "Horizontal"), 1, "axis value 1")
        state = await ed.call("get_play_state")
        ed.check_eq(state["input"]["actions"].get("Horizontal"), 1.0, f"the input block: {state.get('input')}")
        ed.check_eq(state["held_input"].get("axes"), {"Horizontal": 1.0}, f"held input: {state.get('held_input')}")

        # keyboard left (-1 after 20 frames at sensitivity 3) + the source's +1 = 0
        await send(ed, {"type": "key", "key": "left", "action": "down"}, frames_after=30)
        ed.check(abs(await axis(ed, "Horizontal")) < 1e-6, "keyboard -1 + axis 1 = 0")
        state = await ed.call("get_play_state")
        ed.check_in("left", state["input"]["keys_held"], "keys_held")

        await send(ed, {"type": "release_all"}, frames_after=30)
        ed.check_eq(await axis(ed, "Horizontal"), 0, "release_all: back to rest")
        state = await ed.call("get_play_state")
        ed.check("Horizontal" not in state["input"]["actions"], f"at rest: {state['input']}")

        # an axis with hold_frames goes back by itself; a delta lasts one frame and is not clamped
        await send(ed, {"type": "axis", "name": "Vertical", "value": -0.5, "hold_frames": 3}, frames_after=0)
        ed.check_eq(await axis(ed, "Vertical"), -0.5, "held")
        await ed.call("wait", frames=4)
        ed.check_eq(await axis(ed, "Vertical"), 0, "released after hold_frames")
        await lua(ed, "_G.probe.mouseX = {}")
        await send(ed, {"type": "axis_delta", "name": "Mouse X", "delta": 7}, frames_after=3)
        ed.check_eq(await lua(ed, "return table.concat(_G.probe.mouseX, ',')"), ["7"], "one frame saw the delta, unclamped")
        ed.check_eq(await axis(ed, "Mouse X"), 0, "gone after its frame")
    finally:
        await stop(ed)


async def test_touch_phases_and_finger_ids(ed: Editor):
    await start(ed)
    try:
        await reset_probe(ed)
        await send(ed,
                   {"type": "touch", "action": "start", "pointer": 0, "x": 100, "y": 100},
                   {"type": "touch", "action": "start", "pointer": 1, "x": 300, "y": 200},
                   {"type": "touch", "action": "move", "pointer": 0, "x": 140, "y": 100},
                   {"type": "touch", "action": "end", "pointer": 0, "x": 140, "y": 100},
                   {"type": "touch", "action": "end", "pointer": 1, "x": 300, "y": 200},
                   frames_after=3)
        p = await probe(ed)
        seen = p["touches"].split(",")
        for expected in ("0:Began", "1:Began", "0:Moved", "0:Ended", "1:Ended"):
            ed.check_in(expected, seen, f"touch phases {seen}")
        ed.check(seen.index("0:Began") < seen.index("0:Moved") < seen.index("0:Ended"), f"in order: {seen}")
        ed.check_eq(await lua(ed, "return Input.touchCount, Input.numTouches()"), [0, 0], "all lifted")
    finally:
        await stop(ed)


async def test_authoring_actions_over_mcp(ed: Editor):
    settings = await ed.call("get_project_settings", keys=["input.actions"])
    actions = settings["settings"]["input.actions"]
    actions.append({"name": "Dash", "type": "button", "description": "quick move",
                    "bindings": [{"kind": "buttons", "positive": ["left shift"]}]})
    await ed.call("set_project_settings", values={"input.actions": actions})
    got = (await ed.call("get_project_settings", keys=["input.actions"]))["settings"]["input.actions"]
    dash = got[-1]
    ed.check_eq(dash["name"], "Dash")
    ed.check_eq(dash["bindings"][0]["positive"], ["left_shift"], "Unity's spelling normalised")
    project_yaml = yaml.safe_load((ed.project / "project.yaml").read_text())
    ed.check_eq(project_yaml["input"]["actions"][-1]["name"], "Dash", "saved to project.yaml")

    # validated: one bad binding changes nothing
    bad = got + [{"name": "Bad", "bindings": [{"kind": "buttons", "positive": ["lft"]}]}]
    msg = await ed.call("set_project_settings", values={"input.actions": bad}, expect_error=True)
    ed.check_in('action "Bad" binding 1: unknown control "lft"', msg)

    await start(ed)
    try:
        await reset_probe(ed)
        await send(ed, {"type": "key", "key": "left_shift"}, frames_after=2)
        ed.check_eq((await probe(ed))["dashDowns"], 1, "the new action works in Play")
    finally:
        await stop(ed)

    # one undo step of the project history
    await ed.call("undo", scope="project")
    names = [a["name"] for a in (await ed.call("get_project_settings", keys=["input.actions"]))["settings"]["input.actions"]]
    ed.check("Dash" not in names, f"undone: {names}")
    project_yaml = yaml.safe_load((ed.project / "project.yaml").read_text())
    ed.check("input" not in project_yaml, "back to the defaults: no input node")
    await ed.call("redo", scope="project")
    names = [a["name"] for a in (await ed.call("get_project_settings", keys=["input.actions"]))["settings"]["input.actions"]]
    ed.check_eq(names[-1], "Dash", "redone")
    await ed.call("set_project_settings", values={"input.actions": "defaults"})
    names = [a["name"] for a in (await ed.call("get_project_settings", keys=["input.actions"]))["settings"]["input.actions"]]
    ed.check("Dash" not in names, "defaults restored")


async def test_lua_rebinding_and_simulation(ed: Editor):
    await start(ed)
    try:
        values = await lua(ed, """
            local ok, err = Input.setBindings('Jump', {{kind = 'buttons', positive = {'j'}}})
            local bad, why = Input.setBindings('Jump', {{kind = 'buttons', positive = {'nope'}}})
            local exported = Input.exportBindings()
            Input.resetBindings()
            local imported = Input.importBindings(exported)
            return ok, bad, why, Input.getBindings('Jump')[1].positive[1], Input.hasAction('Jump'), #Input.getActionNames()""")
        ed.check_eq(values[0], True, f"setBindings: {values}")
        ed.check_eq(values[1], False, "an unknown control is refused")
        ed.check_in("unknown control", values[2])
        ed.check_eq(values[3], "j", "imported back")
        ed.check_eq(values[4:], [True, 11])
        await reset_probe(ed)
        await send(ed, {"type": "key", "key": "j"}, frames_after=2)
        ed.check_eq((await probe(ed))["jumpDowns"], 1, "Jump rebound to j")

        await lua(ed, "Input.simulateAxis('Horizontal', -0.25)")
        await ed.call("wait", frames=1)
        ed.check_eq(await axis(ed, "Horizontal"), -0.25, "a script source")
        await lua(ed, "Input.clearSimulated()")
        ed.check_eq(await axis(ed, "Horizontal"), 0, "cleared")
    finally:
        await stop(ed)
    # Play stop drops the rebinding
    await start(ed)
    try:
        ed.check_eq((await lua(ed, "return Input.getBindings('Jump')[1].positive[1]"))[0], "space", "overrides reset at stop")
    finally:
        await stop(ed)
