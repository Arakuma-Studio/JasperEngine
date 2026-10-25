"""Public console, play-session, Lua evaluation and input contracts on an owned empty project."""

from harness import Editor, skip
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "Play Contracts"}


async def test_console_cursor_and_filters(ed: Editor):
    ed.require_tools("read_console", "clear_console", "eval_lua")
    first = await ed.call("read_console", limit=5)
    ed.check(isinstance(first.get("entries"), list), f"entries missing: {first!r}")
    cursor = first["next_cursor"]
    ed.check(isinstance(cursor, int), f"next_cursor is not an int: {cursor!r}")

    # Log.* from Lua reaches the console (edit mode eval)
    await ed.call("eval_lua", code="Log.print('mcp-probe-info'); Log.error('mcp-probe-error')", allow_edit_mode=True)
    new = await ed.call("read_console", since=cursor)
    msgs = [e["message"] for e in new["entries"]]
    ed.check(any("mcp-probe-info" in m for m in msgs), f"Log.print not in the console: {msgs}")
    ed.check(any("mcp-probe-error" in m for m in msgs), f"Log.error not in the console: {msgs}")
    for e in new["entries"]:
        ed.check({"id", "type", "message", "time"} <= set(e), f"entry fields: {e}")
        ed.check(e["id"] > cursor, "entry older than the cursor")
    text_cursor = await ed.call("read_console", since=str(cursor), limit=len(new["entries"]))
    ed.check_eq(text_cursor["entries"], new["entries"], "string and integer cursors return the same entries")
    await ed.check_error("read_console", since="not-a-cursor", contains="since")

    errors = await ed.call("read_console", since=cursor, types=["error"])
    ed.check(errors["entries"] and all(e["type"] == "error" for e in errors["entries"]), f"type filter: {errors}")
    found = await ed.call("read_console", since=cursor, contains="MCP-PROBE-INFO")
    ed.check_eq(len(found["entries"]), 1, "contains filter is case-insensitive")
    rx = await ed.call("read_console", since=cursor, regex=r"probe-(info|error)$")
    ed.check_eq(len(rx["entries"]), 2, "regex filter")
    await ed.check_error("read_console", regex="([")

    # paging: limit + more + next_cursor
    page = await ed.call("read_console", since=cursor, limit=1)
    ed.check(page["more"], "limit 1 over 2 entries should say more")
    rest = await ed.call("read_console", since=page["next_cursor"])
    ed.check(len(rest["entries"]) >= 1, "the second page is empty")

    cleared = await ed.call("clear_console")
    after = await ed.call("read_console", since=0)
    ed.check(all(e["id"] > cleared["next_cursor"] for e in after["entries"]), "clear_console left old entries")


async def test_screenshot_png_save_and_width_cap(ed: Editor):
    made = await ed.call("create_scene", name="Screenshot", type="ui")
    sid = made["scene"]["id"]
    await ed.call("create_ui", scene=sid, root={
        "type": "image", "name": "Marker", "color": [0, 1, 0, 1],
        "layout": {"anchor": "stretch"}})
    result = await ed.call_raw("screenshot", {
        "view": "game", "scene": sid, "save_path": "screenshots/probe.png", "max_width": 320})
    ed.check(not result.is_error and result.images, result.text)
    data, mime = result.images[0]
    ed.check_eq(mime, "image/png")
    width, height, channels, rows = decode_png(data)
    ed.check(0 < width <= 320 and height > 0, "the public maximum-width cap applies")
    center = rows[height // 2][(width // 2) * channels:(width // 2) * channels + 3]
    ed.check(center[1] > 200 and center[0] < 30 and center[2] < 30,
             "the saved image contains the authored UI marker")
    ed.check_eq((ed.project / "screenshots/probe.png").read_bytes(), data,
                "save_path and the returned PNG contain the same capture")
    await ed.call("close_scene", scene=sid, unsaved="discard")


async def test_not_playing_errors(ed: Editor):
    ed.check_eq((await ed.call("get_play_state"))["state"], "stopped")
    await ed.check_error("eval_lua", code="return 1", contains="allow_edit_mode")
    await ed.check_error("send_input", events=[{"type": "key", "key": "space"}], contains="play")
    await ed.call("create_entity", name="StoppedProbe", type="empty")
    await ed.check_error("get_runtime_entity", entity="StoppedProbe", contains="playing")
    await ed.check_error("wait")
    await ed.check_error("play", action="pause")
    ed.check_eq((await ed.call("play", action="stop"))["state"], "stopped")
    value = await ed.call("eval_lua", code="return 1 + 2, {a = true}", allow_edit_mode=True)
    ed.check_eq(value["values"], [3, {"a": True}])
    error = await ed.call_raw("eval_lua", {"code": "return (", "allow_edit_mode": True})
    ed.check(error.is_error and "syntax" in error.text.lower(), error.text)


async def test_eval_lua_values_errors_and_timeout(ed: Editor):
    await ed.call("create_entity", name="EvalHandle", type="sprite")
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    try:
        value = await ed.call("eval_lua", code="print('probe', 42); return Vector3(1, 2, 3), {a=true}, object('EvalHandle', 'Sprite')")
        ed.check_eq(value["values"][:2], [[1, 2, 3], {"a": True}])
        ed.check_eq(value["values"][2]["type"], "Sprite")
        ed.check_eq(value["values"][2]["name"], "EvalHandle")
        ed.check_eq(value["printed"], ["probe\t42"])
        error = await ed.call_raw("eval_lua", {"code": "error('evaluation-probe')"})
        ed.check(error.is_error and "evaluation-probe" in error.text, error.text)
        loop = await ed.call_raw("eval_lua", {"code": "while true do end", "timeout_seconds": 0.1})
        ed.check(loop.is_error and "timed out" in loop.text, loop.text)
        ed.check_eq((await ed.call("get_play_state"))["state"], "playing",
                    "an evaluation failure does not crash the running scene")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_stop_restores_scene(ed: Editor):
    await ed.call("create_entity", name="SessionProbe", type="empty", position=[1, 2, 3])
    started = await ed.call("play", action="start", wait=False, auto_pause=True, fixed_fps=60)
    if "job_id" in started:
        job = await ed.call("get_job", job_id=started["job_id"])
        while job["status"] == "running":
            job = await ed.call("get_job", job_id=started["job_id"])
        ed.check_eq(job["status"], "succeeded", job)
    try:
        ed.check_eq((await ed.call("get_play_state"))["state"], "playing")
        await ed.call("play", action="pause")
        read = await ed.call("get_runtime_entity", entity="SessionProbe", components=["transform"], properties=["position"])
        ed.check_eq(read["components"]["transform"]["position"], [1, 2, 3])
        await ed.call("set_runtime_property", entity="SessionProbe", component="transform", values={"position": [9, 8, 7]})
        actual = await ed.call("eval_lua", code="return object('SessionProbe').position")
        ed.check_eq(actual["values"], [[9, 8, 7]])
        listed = await ed.call("list_runtime_entities", name_contains="SessionProbe")
        ed.check_eq([e["name"] for e in listed["entities"]], ["SessionProbe"])
        await ed.check_error("set_runtime_property", entity="SessionProbe", component="transform", values={"nope": 1})
        ed.check_eq((await ed.call("play", action="step", frames=3))["state"], "paused")
        await ed.check_error("send_input", events=[{"type": "key", "key": "space"}], contains="paused")
        await ed.call("play", action="resume")
        await ed.call("send_input", events=[{"type": "key", "key": "up", "action": "down"}])
    finally:
        stopped = await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    ed.check_eq(stopped["last_session"]["ended"], "stopped")
    actual = await ed.call("eval_lua", code="return object('SessionProbe').position", allow_edit_mode=True)
    ed.check_eq(actual["values"], [[1, 2, 3]], "runtime edits do not change the authored scene")
    await ed.check_error("get_runtime_entity", entity="SessionProbe", contains="playing")
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    try:
        key = await ed.call("eval_lua", code="return Input.isKeyPressed(Input.KEY_UP)")
        ed.check_eq(key["values"], [False], "Stop releases held input before another session")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_held_input_and_coordinate_omission(ed: Editor):
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    try:
        await ed.call("eval_lua", code="""inputProbe = {clicks=0}
function inputProbe:onMouseDown()
    self.clicks = self.clicks + 1
    self.position = Input.getMousePosition()
end
RegisterEngineEvent(inputProbe, 'onMouseDown')""")
        await ed.call("send_input", events=[{"type": "key", "key": "up", "action": "down"}])
        held = await ed.call("eval_lua", code="return Input.isKeyPressed(Input.KEY_UP)")
        ed.check_eq(held["values"], [True])
        await ed.call("send_input", events=[{"type": "release_all"}])
        released = await ed.call("eval_lua", code="return Input.isKeyPressed(Input.KEY_UP)")
        ed.check_eq(released["values"], [False])
        await ed.call("send_input", events=[{"type": "mouse", "action": "move", "x": 100, "y": 120}])
        await ed.call("send_input", events=[{"type": "mouse", "action": "click"}])
        mouse = await ed.call("eval_lua", code="return inputProbe.clicks, inputProbe.position")
        ed.check_eq(mouse["values"][0], 1, "the click reaches the registered mouse handler")
        ed.check(all(abs(a-b) < 1e-3 for a,b in zip(mouse["values"][1], [100,120])),
                 "a click without coordinates uses the last cursor")
        await ed.check_error("send_input", events=[{"type": "key", "key": "nokey"}], contains="unknown key")
        await ed.check_error("send_input", events=[{"type": "mouse", "x": 5000, "y": 10}], contains="outside")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


async def test_lua_script_error_file(ed: Editor):
    await ed.call("create_entity", name="CrashProbe", type="empty")
    await ed.call("create_script", kind="lua", name="CrashProbe", entity="CrashProbe", content="""local CrashProbe = {}
function CrashProbe:init()
    self.armed = false
    RegisterEngineEvent(self, 'onUpdate')
end
function CrashProbe:onUpdate()
    if self.armed then error('runtime-contract-crash') end
end
return CrashProbe""")
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    try:
        runtime = await ed.call("get_runtime_entity", entity="CrashProbe")
        ed.check_eq(runtime["scripts"][0]["class"], "CrashProbe")
        ed.check_eq(runtime["scripts"][0]["state"]["armed"], False)
        await ed.call("eval_lua", code="script('CrashProbe').armed = true")
        result = await ed.call_raw("wait", {"frames": 2})
        ed.check(result.is_error and result.structured["reason"] == "play_ended", result.text)
        ed.check_in("runtime-contract-crash", result.text, "the original script failure reaches the waiting caller")
        state = await ed.call("get_play_state")
        ed.check_eq(state["state"], "stopped")
        ed.check_eq(state["last_session"]["ended"], "crashed")
        ed.check_in("runtime-contract-crash", state["last_session"].get("crash", ""))
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    await ed.call("delete_entities", entity="CrashProbe")


async def test_cpp_build_failure_reported(ed: Editor):
    """Slow (builds the project's C++ scripts with the host compiler): JASPER_SLOW_TESTS=1."""
    import os
    if not os.environ.get("JASPER_SLOW_TESTS"):
        skip("set JASPER_SLOW_TESTS=1 to build C++ scripts")
    ed.require_tools("create_script")
    user = ed.project / ".jasperengine" / "user"
    user.mkdir(parents=True, exist_ok=True)
    (user / "build.yaml").write_text("build:\n  build_jobs: 2\n")
    await ed.call("create_entity", name="CompileProbe", type="empty")
    await ed.call("create_script", kind="cpp_script", name="Broken", entity="CompileProbe")
    cpp = ed.project / "scripts" / "Broken.cpp"
    ed.check(cpp.is_file(), "the created compiler probe exists before corruption")
    cpp.write_text(cpp.read_text() + "\nthis is not C++;\n")
    r = await ed.call_raw("play", {"action": "start", "timeout_seconds": 900}, timeout=960)
    ed.check(r.is_error, f"a broken C++ script must fail play start: {r.text[:500]}")
    ed.check("error" in r.text.lower() and "Broken.cpp" in r.text, f"the compiler error is not reported: {r.text[:1500]}")
    st = await ed.call("get_play_state")
    ed.check_eq(st["state"], "stopped", "stopped after the failed build")
    ed.check_eq(st["last_session"]["ended"], "failed_to_start", f"last session: {st['last_session']}")



async def test_scene_camera_3d(ed: Editor):
    made = await ed.call("create_scene", name="Camera3D", type="3d")
    sid = made["scene"]["id"]
    try:
        await ed.call("set_scene_camera", scene=sid, position=[0, 5, 10], target=[0, 0, 0])
        camera = (await ed.call("set_scene_camera", scene=sid))["camera"]
        ed.check_eq(camera["mode"], "3d")
        ed.check(abs(camera["distance"] - 125 ** 0.5) < 0.05, camera)
        await ed.call("set_scene_camera", scene=sid, orbit={"yaw": 90}, distance=20)
        camera = (await ed.call("set_scene_camera", scene=sid))["camera"]
        ed.check(abs(camera["distance"] - 20) < 0.05, camera)
        await ed.check_error("set_scene_camera", scene=sid, zoom=2, contains="2D")
    finally:
        await ed.call("close_scene", scene=sid, unsaved="discard")
