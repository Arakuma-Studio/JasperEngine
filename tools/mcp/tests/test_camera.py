"""Camera bounds over MCP (docs/specs/S94-rendering-anim-gaps.md F27, F28; AC27, AC28's Lua and MCP
part): a set_properties write of an orthographic camera's bound turns Auto Resize off in the same
undo step, so the bound survives Play and a canvas resize (R46: the canvas overwrote it); one undo
gives both back. Unity's Orthographic Size centres the bounds on the canvas's aspect and follows a
resize; a bound written by hand turns it off; Lua's Camera.orthographicSize drives it in Play."""

from harness import Editor

EDITOR = {"create": True, "name": "Camera Bounds"}
CI = True

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Bounds", type="2D", path="scenes/Bounds")
    S["scene"] = made["scene"]["id"]
    S["path"] = "scenes/Bounds.scene"
    await ed.call("create_entity", scene=S["scene"], type="camera", name="Cam", position=[0, 0, 5],
                  values={"camera": {"type": "orthographic"}})
    await ed.call("set_main_camera", scene=S["scene"], entity="Cam")
    await ed.call("create_entity", scene=S["scene"], type="cube", name="Box", position=[0, 0, 0])
    await ed.call("save_scene", scene=S["scene"], path=S["path"])
    return S


async def camera_values(ed: Editor, s: dict) -> dict:
    got = await ed.call("get_entity", scene=s["scene"], entity="Cam", components=["camera"])
    return got["components"]["camera"]


async def runtime_camera(ed: Editor) -> dict:
    got = await ed.call("get_runtime_entity", entity="Cam", components=["camera"])
    return got["components"]["camera"]


def near(a: float, b: float, tolerance: float = 1e-3) -> bool:
    return abs(a - b) <= tolerance


async def test_clip_write_clears_auto_resize(ed: Editor):
    s = await setup(ed)
    before = await camera_values(ed, s)
    ed.check(before["autoResize"] is True, f"a new camera follows the canvas: {before}")

    await ed.call("set_properties", scene=s["scene"], entity="Cam", values={"camera": {"top": 5, "bottom": -5, "left": -8, "right": 8}})
    after = await camera_values(ed, s)
    ed.check_eq((after["top"], after["bottom"], after["left"], after["right"]), (5, -5, -8, 8), f"the bounds read back: {after}")
    ed.check(after["autoResize"] is False, f"a bound written by hand turns Auto Resize off (R46): {after}")

    await ed.call("configure_game_view", resolution="320x180")
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        playing = await runtime_camera(ed)
        ed.check_eq((playing["top"], playing["right"]), (5, 8), f"the bounds survive Play: {playing}")
        await ed.call("configure_game_view", resolution="200x300")
        await ed.call("wait", frames=3)
        resized = await runtime_camera(ed)
        ed.check_eq((resized["top"], resized["right"]), (5, 8), f"and a canvas resize: {resized}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
        await ed.call("configure_game_view", resolution="free")

    await ed.call("undo", scene=s["scene"])
    undone = await camera_values(ed, s)
    ed.check(undone["autoResize"] is True, f"one undo turns Auto Resize back on: {undone}")
    ed.check(undone["top"] != 5, f"and gives the bound back: {undone}")


async def test_orthographic_size_follows_aspect(ed: Editor):
    s = await setup(ed)
    await ed.check_error("set_properties", scene=s["scene"], entity="Cam", values={"camera": {"orthographicSize": -1}}, contains="0")
    await ed.call("set_properties", scene=s["scene"], entity="Cam", values={"camera": {"orthographicSize": 5}})
    sized = await camera_values(ed, s)
    ed.check_eq(sized["orthographicSize"], 5, f"Size reads back: {sized}")

    await ed.call("configure_game_view", resolution="320x180")
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        await ed.call("wait", frames=3)
        wide = await runtime_camera(ed)
        ed.check(near(wide["top"], 5) and near(wide["bottom"], -5), f"+-Size up and down: {wide}")
        ed.check(near(wide["right"], 5 * 320 / 180) and near(wide["left"], -5 * 320 / 180), f"+-Size x aspect sideways: {wide}")
        await ed.call("configure_game_view", resolution="180x320")
        await ed.call("wait", frames=3)
        tall = await runtime_camera(ed)
        ed.check(near(tall["top"], 5) and near(tall["right"], 5 * 180 / 320), f"the bounds follow the new aspect: {tall}")

        # Lua: the property drives it, and a bound written by hand turns it off
        values = (await ed.call("eval_lua", code="local c = object('Cam', 'Camera') c.orthographicSize = 3 return c.orthographicSize"))["values"]
        ed.check_eq(values, [3], "Camera.orthographicSize reads back")
        await ed.call("wait", frames=2)
        lua = await runtime_camera(ed)
        ed.check(near(lua["top"], 3) and near(lua["right"], 3 * 180 / 320), f"the Lua Size applies: {lua}")
        values = (await ed.call("eval_lua", code="local c = object('Cam', 'Camera') c.leftClip = -2 return c.orthographicSize, c.autoResize"))["values"]
        ed.check_eq(values, [0, False], "a Lua bound turns Size and Auto Resize off")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
        await ed.call("configure_game_view", resolution="free")

    # an MCP bound turns it off in the same step
    await ed.call("set_properties", scene=s["scene"], entity="Cam", values={"camera": {"top": 4}})
    after = await camera_values(ed, s)
    ed.check_eq((after["orthographicSize"], after["top"]), (0, 4), f"a bound written by hand turns Size off: {after}")
    await ed.call("undo", scene=s["scene"])
    undone = await camera_values(ed, s)
    ed.check_eq(undone["orthographicSize"], 5, f"one undo gives Size back: {undone}")
    await ed.call("set_properties", scene=s["scene"], entity="Cam", values={"camera": {"orthographicSize": 0}})
