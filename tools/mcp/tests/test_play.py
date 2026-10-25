"""W3: play mode, runtime inspection, Lua eval, input injection, screenshots, console.

Runs against the TappyPlane fixture (a 2D Lua game: GameObject "Game" carries the GameController
script; its state is "ready" → space/click → "playing" → crash into the ground → "dead").
Tests run in order and share one editor.
"""

import struct
import zlib

from harness import Editor, skip

TIMEOUT = 900


# -- helpers -----------------------------------------------------------------


def decode_png(data: bytes):
    """(width, height, channels, byte rows) for the 8-bit RGB/RGBA PNGs the editor writes."""
    assert data.startswith(b"\x89PNG\r\n\x1a\n"), "not a PNG"
    pos = 8
    width = height = 0
    color_type = 2
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color_type = struct.unpack(">IIBB", chunk[:10])
            assert depth == 8, f"bit depth {depth}"
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + length
    channels = {2: 3, 6: 4}[color_type]
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    prev = bytearray(stride)
    i = 0
    for _ in range(height):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return width, height, channels, rows


def check_image(ed: Editor, result, label: str, min_colours: int = 8):
    ed.check(result.images, f"{label}: no image in the result: {result.text[:300]}")
    data, mime = result.images[0]
    ed.check_eq(mime, "image/png", f"{label}: mime type")
    w, h, channels, rows = decode_png(data)
    ed.check(w > 16 and h > 16, f"{label}: tiny image {w}x{h}")
    colours = len({bytes(row[x:x + 3]) for row in rows[::7] for x in range(0, w * channels, 7 * channels)})
    ed.check(colours >= min_colours, f"{label}: only {colours} colour(s): blank render? {result.text[:300]}")
    return data


async def game_state(ed: Editor):
    v = await ed.call("eval_lua", code="return script('Game').state")
    return v["values"][0]


# -- console -----------------------------------------------------------------


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

    # the noisy shader lines are skipped unless asked
    all_kept = await ed.call("read_console", since=0, limit=1000, include_shader_builds=True)
    quiet = await ed.call("read_console", since=0, limit=1000)
    noisy = [e for e in all_kept["entries"] if e["message"].startswith("Shader ") and "generated successfully" in e["message"]]
    ed.check(not any(e["message"].startswith("Shader ") and "generated successfully" in e["message"]
                     for e in quiet["entries"]), "shader build lines not filtered")
    if noisy:
        ed.check(quiet.get("skipped_shader_builds", 0) >= len(noisy), "skipped_shader_builds count")

    cleared = await ed.call("clear_console")
    after = await ed.call("read_console", since=0)
    ed.check(all(e["id"] > cleared["next_cursor"] for e in after["entries"]), "clear_console left old entries")


# -- views while stopped ------------------------------------------------------


async def test_scene_and_game_screenshots_stopped(ed: Editor):
    ed.require_tools("screenshot")
    r = await ed.call_raw("screenshot", {"view": "scene", "save_path": "screenshots/scene.png"})
    ed.check(not r.is_error, r.text)
    data = check_image(ed, r, "scene view")
    saved = ed.project / "screenshots" / "scene.png"
    ed.check(saved.exists() and saved.read_bytes() == data, "save_path did not write the same PNG")

    r = await ed.call_raw("screenshot", {"view": "game", "max_width": 400})
    ed.check(not r.is_error, r.text)
    check_image(ed, r, "game view (stopped)")
    info = r.structured
    ed.check(info["width"] <= 400, f"max_width ignored: {info}")
    ed.check("canvas_rect_in_image" in info, f"no canvas rect: {info}")

    r = await ed.call_raw("screenshot", {"view": "editor", "max_width": 800})
    ed.check(not r.is_error, r.text)
    check_image(ed, r, "editor window", min_colours=12)

    await ed.check_error("screenshot", {"view": "nope"})
    await ed.check_error("screenshot", {"view": "scene", "scene": "No Such Scene"})


async def test_scene_camera_and_frame(ed: Editor):
    ed.require_tools("set_scene_camera", "frame_entity")
    cam = await ed.call("set_scene_camera")
    ed.check_eq(cam["camera"]["mode"], "2d", "TappyPlane is a 2D scene")
    moved = await ed.call("set_scene_camera", center=[640, 360], zoom=2.0, display={"show_grid_2d": True})
    ed.check(abs(moved["camera"]["zoom"] - 2.0) < 1e-3, f"zoom not applied: {moved}")
    ed.check(moved["display"]["show_grid_2d"] is True, "display flag not applied")
    await ed.check_error("set_scene_camera", {"orbit": {"yaw": 10}}, contains="3D")
    await ed.check_error("set_scene_camera", {"display": {"bogus": True}})

    framed = await ed.call_raw("frame_entity", {"entity": "Plane", "screenshot": True})
    ed.check(not framed.is_error, framed.text)
    check_image(ed, framed, "framed plane")
    ed.check(framed.structured["camera"]["zoom"] < 2.0, f"framing a plane should zoom in: {framed.structured}")
    await ed.check_error("frame_entity", {"entity": "Nope/Nothing"})


async def test_focus_and_game_view_settings(ed: Editor):
    ed.require_tools("focus_window", "configure_game_view")
    r = await ed.call("focus_window", window="console")
    ed.check(r["shown"], f"console not shown: {r}")
    r = await ed.call("focus_window", window="scene")
    ed.check(r["shown"], f"scene tab not shown: {r}")
    await ed.check_error("focus_window", {"window": "bogus"})

    before = await ed.call("configure_game_view")
    ed.check("aspect" in before and "render_size" in before, f"settings: {before}")
    after = await ed.call("configure_game_view", aspect="16:9", show_stats=False)
    ed.check_eq(after["aspect"], "16:9", "aspect")
    await ed.call("configure_game_view", aspect=before["aspect"])
    await ed.check_error("configure_game_view", {"aspect": "21:9"})


# -- edge cases before play ------------------------------------------------------


async def test_not_playing_errors(ed: Editor):
    ed.require_tools("play", "eval_lua", "send_input", "wait", "get_play_state")
    st = await ed.call("get_play_state")
    ed.check_eq(st["state"], "stopped", "state before play")
    await ed.check_error("eval_lua", {"code": "return 1"}, contains="allow_edit_mode")
    await ed.check_error("send_input", {"events": [{"type": "key", "key": "space"}]}, contains="play")
    await ed.check_error("get_runtime_entity", {"entity": "Plane"}, contains="playing")
    await ed.check_error("wait", {})
    await ed.check_error("play", {"action": "pause"})
    stop = await ed.call("play", action="stop")
    ed.check_eq(stop["state"], "stopped", "stop when stopped is a no-op")
    # edit-mode eval works (and says so)
    v = await ed.call("eval_lua", code="return 1 + 2, 'x', {1, 2, {a = true}}", allow_edit_mode=True)
    ed.check_eq(v["values"], [3, "x", [1, 2, {"a": True}]], "edit-mode values")
    ed.check_eq(v["mode"], "edit", "mode")
    syntax = await ed.call_raw("eval_lua", {"code": "return (", "allow_edit_mode": True})
    ed.check(syntax.is_error and "syntax" in syntax.text.lower(), syntax.text)


# -- play ----------------------------------------------------------------------


async def test_play_start_and_screenshot(ed: Editor):
    ed.require_tools("play", "wait", "screenshot")
    # remember the edit-time plane position to check Stop restores it
    edit = await ed.call("eval_lua", code="return object('Plane').position", allow_edit_mode=True)
    ed.edit_plane = edit["values"][0]

    started = await ed.call("play", action="start")
    ed.check_eq(started["state"], "playing", f"play start: {started}")
    ed.check("problems" in started and "next_cursor" in started, f"start result fields: {started}")
    again = await ed.call("play", action="start")
    ed.check_eq(again["state"], "playing", "start while playing is a no-op")

    waited = await ed.call_raw("wait", {"seconds": 2, "screenshot": True})
    ed.check(not waited.is_error, waited.text)
    ed.check(waited.structured["frames"] > 5, f"the game did not advance: {waited.structured}")
    ed.check_eq(waited.structured["reason"], "seconds", "wait reason")
    check_image(ed, waited, "game view (playing)")

    shot = await ed.call_raw("screenshot", {"view": "game"})
    ed.check(shot.structured["playing"], "screenshot says not playing")
    check_image(ed, shot, "game view screenshot (playing)")

    scene = await ed.call_raw("screenshot", {"view": "scene"})
    ed.check(not scene.is_error, scene.text)
    check_image(ed, scene, "scene view (playing)")


async def test_eval_lua_reads_game_state(ed: Editor):
    v = await ed.call("eval_lua", code="local gc = script('Game'); return {state = gc.state, score = gc.score, y = gc.planeY}")
    state = v["values"][0]
    ed.check_eq(state["state"], "ready", "game starts in the ready state")
    ed.check_eq(state["score"], 0, "score")
    ed.check(isinstance(state["y"], (int, float)), f"planeY: {state}")

    v = await ed.call("eval_lua", code="return object('Plane', 'Sprite'), object('Plane').position, Engine.canvasWidth")
    handle, position, canvas_w = v["values"]
    ed.check_eq(handle.get("type"), "Sprite", f"handle: {handle}")
    ed.check_eq(handle.get("name"), "Plane", f"handle: {handle}")
    ed.check(isinstance(position, list) and len(position) == 3, f"Vector3 as array: {position}")
    ed.check_eq(canvas_w, 1280, "canvas width")

    # the HUD runs as a Play copy of another scene: reachable by its name
    hud = await ed.call("eval_lua", code="return object('Score', 'Text', 'HUD Scene').text")
    ed.check_eq(hud["values"], ["0"], "HUD score text")
    listed = await ed.call("list_runtime_entities", scene="HUD Scene")
    ed.check("Score" in [e["name"] for e in listed["entities"]], f"HUD entities: {listed['entities']}")
    ed.check(len(listed["running_scenes"]) >= 2, f"running scenes: {listed['running_scenes']}")

    v = await ed.call("eval_lua", code="print('hello', 42); return nil")
    ed.check_eq(v["printed"], ["hello\t42"], "print captured")

    err = await ed.call_raw("eval_lua", {"code": "error('boom')"})
    ed.check(err.is_error and "boom" in err.text, err.text)
    loop = await ed.call_raw("eval_lua", {"code": "while true do end", "timeout_seconds": 0.5})
    ed.check(loop.is_error and "timed out" in loop.text, loop.text)
    missing = await ed.call_raw("eval_lua", {"code": "return script('No/Such')"})
    ed.check(missing.is_error and "No GameObject" in missing.text, missing.text)
    # the game survived all that
    ed.check_eq((await ed.call("get_play_state"))["state"], "playing", "eval errors must not stop the game")


async def test_runtime_entities(ed: Editor):
    ed.require_tools("list_runtime_entities", "get_runtime_entity", "set_runtime_property")
    listed = await ed.call("list_runtime_entities", name_contains="rock")
    names = [e["name"] for e in listed["entities"]]
    ed.check(len([n for n in names if n.startswith("Rock ")]) == 8 and "Rocks" in names, f"rocks: {names}")
    ed.check(all(e["path"].startswith("Rocks") for e in listed["entities"]), f"paths: {listed['entities']}")
    got = await ed.call("get_runtime_entity", entity="Game")
    ed.check("transform" in got["components"], f"components: {list(got['components'])}")
    scripts = got.get("scripts") or []
    ed.check(scripts and scripts[0]["class"] == "GameController", f"scripts: {scripts}")
    ed.check_eq(scripts[0]["state"].get("state"), "ready", "script state readable")

    await ed.call("play", action="pause")
    plane = await ed.call("get_runtime_entity", entity="Plane", components=["transform"], properties=["position"])
    pos = plane["components"]["transform"]["position"]
    new_pos = [pos[0] + 100, pos[1], pos[2]]
    setr = await ed.call("set_runtime_property", entity="Plane", component="transform", values={"position": new_pos})
    ed.check(abs(setr["values"]["position"][0] - new_pos[0]) < 1e-3, f"set_runtime_property: {setr}")
    v = await ed.call("eval_lua", code="return object('Plane').position")
    ed.check(abs(v["values"][0][0] - new_pos[0]) < 1e-3, f"the game does not see the new position: {v}")
    await ed.check_error("set_runtime_property", {"entity": "Plane", "component": "transform", "values": {"nope": 1}})
    await ed.check_error("set_runtime_property", {"entity": "Plane", "component": "rigidbody3d", "values": {"mass": 1}})

    stepped = await ed.call("play", action="step", frames=3)
    ed.check_eq(stepped["stepped_frames"], 3, "step frames")
    ed.check_eq(stepped["state"], "paused", "still paused after step")
    await ed.check_error("send_input", {"events": [{"type": "key", "key": "space"}]}, contains="paused")
    resumed = await ed.call("play", action="resume")
    ed.check_eq(resumed["state"], "playing", "resume")


async def test_send_input_and_wait_until(ed: Editor):
    ed.require_tools("send_input")
    ed.check_eq(await game_state(ed), "ready", "ready before input")
    r = await ed.call("send_input", events=[{"type": "key", "key": "space"}])
    ed.check(r["frames"] >= 2, f"input frames: {r}")
    ed.check_eq(await game_state(ed), "playing", "space starts the run")

    dead = await ed.call_raw("wait", {"until": "script('Game').state == 'dead'", "timeout_seconds": 20, "screenshot": True})
    ed.check(not dead.is_error, dead.text)
    ed.check_eq(dead.structured["reason"], "condition", "until reason")
    check_image(ed, dead, "game over")

    # after the restart delay a click restarts (ready), another starts a run
    await ed.call("wait", seconds=0.6)
    # one frame down: a longer hold would also start a run (resetGame clears flapHeld)
    canvas_center = {"type": "mouse", "action": "click", "x": 640, "y": 360, "hold_frames": 1}
    await ed.call("send_input", events=[canvas_center])
    ed.check_eq(await game_state(ed), "ready", "click after game over restarts")
    await ed.call("send_input", events=[{"type": "mouse", "action": "click", "x": 0.5, "y": 0.5}], space="normalized")
    ed.check_eq(await game_state(ed), "playing", "normalized click starts a run")

    # holding a key across calls, then releasing everything
    held = await ed.call("send_input", events=[{"type": "key", "key": "up", "action": "down"}])
    ed.check("up" in held["held_input"]["keys"], f"held keys: {held['held_input']}")
    v = await ed.call("eval_lua", code="return Input.isKeyPressed(Input.KEY_UP)")
    ed.check_eq(v["values"], [True], "the game sees the held key")
    released = await ed.call("send_input", events=[{"type": "release_all"}])
    ed.check(not released["held_input"]["keys"], "release_all")
    v = await ed.call("eval_lua", code="return Input.isKeyPressed(Input.KEY_UP)")
    ed.check_eq(v["values"], [False], "released")

    # a drag ends where it was asked to, in canvas coordinates
    await ed.call("send_input", events=[
        {"type": "mouse", "action": "move", "x": 100, "y": 100},
        {"type": "mouse", "action": "drag", "x": 100, "y": 100, "to": [300, 200], "steps": 4},
    ])
    v = await ed.call("eval_lua", code="return Input.getMousePosition()")
    ed.check(abs(v["values"][0][0] - 300) < 1.5 and abs(v["values"][0][1] - 200) < 1.5, f"mouse position after drag: {v}")

    # other event kinds are accepted and reach the engine
    await ed.call("send_input", events=[
        {"type": "mouse", "action": "scroll", "dy": -1},
        {"type": "touch", "action": "tap", "x": 640, "y": 360},
        {"type": "text", "text": "héllo"},
        {"type": "gamepad", "button": "a"},
        {"type": "gamepad", "axis": "left_x", "value": 0.5},
        {"type": "wait", "frames": 2},
    ])
    v = await ed.call("eval_lua", code="return Input.isGamepadConnected(0)")
    ed.check_eq(v["values"], [True], "virtual gamepad connected")

    await ed.check_error("send_input", {"events": [{"type": "key", "key": "nokey"}]}, contains="unknown key")
    await ed.check_error("send_input", {"events": [{"type": "mouse", "x": 5000, "y": 10}]}, contains="outside")
    await ed.check_error("send_input", {"events": [{"type": "warp"}]})


async def test_stop_restores_scene(ed: Editor):
    stopped = await ed.call("play", action="stop")
    ed.check_eq(stopped["state"], "stopped", "stop")
    ed.check_eq(stopped["last_session"]["ended"], "stopped", f"session end: {stopped['last_session']}")
    ed.check(not stopped.get("held_input"), "input still held after stop")
    v = await ed.call("eval_lua", code="return object('Plane').position", allow_edit_mode=True)
    edit = getattr(ed, "edit_plane", None)
    if edit:
        ed.check(all(abs(a - b) < 1e-3 for a, b in zip(v["values"][0], edit)),
                 f"Stop did not restore the plane: {v['values'][0]} vs {edit}")
    await ed.check_error("get_runtime_entity", {"entity": "Plane"}, contains="playing")


async def test_restart_and_job_mode(ed: Editor):
    started = await ed.call("play", action="start", wait=False)
    job = started.get("job_id")
    ed.check(job or started.get("state") == "playing", f"wait false: {started}")
    if job:
        st = await ed.call("get_job", job_id=job)
        ed.check(st["status"] in ("running", "succeeded"), f"job: {st}")
    restarted = await ed.call("play", action="restart")
    ed.check_eq(restarted["state"], "playing", f"restart: {restarted}")
    await ed.call("play", action="stop")


# -- crashes -------------------------------------------------------------------


async def test_crash_is_reported(ed: Editor):
    # Hold between calls so the handler cannot crash before wait begins observing play.
    await ed.call("play", action="start", auto_pause=True)
    try:
        # an engine event whose Lua handler errors: the editor's crash guard stops play
        await ed.call("eval_lua", code=(
            "local t = {__name = 'McpCrashProbe'}\n"
            "function t:onUpdate() error('mcp crash probe') end\n"
            "RegisterEngineEvent(t, 'onUpdate')"))
        r = await ed.call_raw("wait", {"seconds": 5})
        ed.check(r.is_error, f"wait should fail when play crashes: {r.text}")
        ed.check_eq(r.structured["reason"], "play_ended", "reason")
        ed.check("mcp crash probe" in r.text, f"crash error not reported: {r.text}")
        st = await ed.call("get_play_state")
        ed.check_eq(st["state"], "stopped", "crash stops play")
        ed.check_eq(st["last_session"]["ended"], "crashed", f"last session: {st['last_session']}")
        ed.check("mcp crash probe" in st["last_session"].get("crash", ""), "crash message kept")
    finally:
        await ed.call("play", action="set", auto_pause=False)


async def test_lua_script_error_file(ed: Editor):
    """A real script file whose onUpdate errors: play starts, then the crash stops it."""
    ed.require_tools("attach_script")
    (ed.project / "scripts" / "Crasher.lua").write_text(
        "local Crasher = { properties = {} }\n"
        "function Crasher:init() RegisterEngineEvent(self, 'onUpdate') self.frames = 0 end\n"
        "function Crasher:onUpdate()\n"
        "  self.frames = self.frames + 1\n"
        "  if self.frames > 30 then local x = nil; return x.field end\n"
        "end\n"
        "return Crasher\n")
    await ed.call("attach_script", entity="Plane", path="scripts/Crasher.lua")
    started = await ed.call("play", action="start")
    ed.check_eq(started["state"], "playing", "starts")
    r = await ed.call_raw("wait", {"seconds": 10})
    ed.check(r.is_error and r.structured["reason"] == "play_ended", f"crash during wait: {r.text[:500]}")
    ed.check("Crasher" in r.text or "field" in r.text, f"the script error is in the report: {r.text[:800]}")
    await ed.call("undo")


async def test_cpp_build_failure_reported(ed: Editor):
    """Slow (builds the project's C++ scripts with the host compiler): JASPER_SLOW_TESTS=1."""
    import os
    if not os.environ.get("JASPER_SLOW_TESTS"):
        skip("set JASPER_SLOW_TESTS=1 to build C++ scripts")
    ed.require_tools("create_script")
    user = ed.project / ".jasperengine" / "user"
    user.mkdir(parents=True, exist_ok=True)
    (user / "build.yaml").write_text("build:\n  build_jobs: 2\n")
    await ed.call("create_script", kind="cpp_script", name="Broken", entity="Game", scene="Main Scene")
    for cpp in (ed.project / "scripts").glob("Broken*.cpp"):
        cpp.write_text(cpp.read_text() + "\nthis is not C++;\n")
    r = await ed.call_raw("play", {"action": "start", "scene": "Main Scene", "timeout_seconds": 900}, timeout=960)
    ed.check(r.is_error, f"a broken C++ script must fail play start: {r.text[:500]}")
    ed.check("error:" in r.text and "Broken.cpp" in r.text, f"the compiler error is not reported: {r.text[:1500]}")
    st = await ed.call("get_play_state")
    ed.check_eq(st["state"], "stopped", "stopped after the failed build")
    ed.check_eq(st["last_session"]["ended"], "failed_to_start", f"last session: {st['last_session']}")


async def test_scene_camera_3d(ed: Editor):
    """A 3D scene: position/target, orbit and distance of the Scene view camera (last: it changes the open scenes)."""
    ed.require_tools("create_scene")
    await ed.call("create_scene", name="Cam3D", type="3D")
    cam = await ed.call("set_scene_camera", scene="Cam3D", position=[0, 5, 10], target=[0, 0, 0])
    ed.check_eq(cam["camera"]["mode"], "3d", "mode")
    ed.check(abs(cam["camera"]["distance"] - (125 ** 0.5)) < 0.05, f"distance: {cam['camera']}")
    orbit = await ed.call("set_scene_camera", scene="Cam3D", orbit={"yaw": 90}, distance=20)
    pos = orbit["camera"]["position"]
    ed.check(abs(orbit["camera"]["distance"] - 20) < 0.05, f"distance after: {orbit['camera']}")
    ed.check(abs(pos[0]) > 5, f"yaw 90 should swing the camera sideways: {pos}")
    await ed.check_error("set_scene_camera", {"scene": "Cam3D", "zoom": 2}, contains="2D")
    shot = await ed.call_raw("set_scene_camera", {"scene": "Cam3D", "screenshot": True})
    ed.check(not shot.is_error and shot.images, shot.text)
    # play's default is the start scene, not the selected Cam3D
    st = await ed.call("get_play_state")
    ed.check_eq(st.get("play_would_start", {}).get("name"), "Main Scene", f"default play scene: {st!r}")


# -- Play Mode Changes (S24 R26-R30, R39; AC13) -----------------------------------------------

SPAWNER = """---@class PlaySpawner : ScriptInstance
local PlaySpawner = { properties = {} }

function PlaySpawner:init()
    local go = GameObject(self.scene)
    go.name = "Spawned By Script"
end

return PlaySpawner
"""


async def test_play_change_review():
    """Edits made while playing are offered after Stop: the authored light's intensity and cube's
    position, a GameObject made by create_entity, and an edit of a script-spawned GameObject
    (that one can't be kept). Keeping the light's only gives it its play value, the cube keeps
    its edit value; one undo gives the light back its edit value. keep_changes all keeps a
    GameObject made while playing."""
    async with Editor(create=True, name="Play Changes CI") as ed:
        made = await ed.call("create_scene", name="PlayChanges", type="3D", path="scenes/PlayChanges")
        scene = made["scene"]["id"]
        await ed.call("select_scene", scene=scene)
        light = (await ed.call("create_entity", scene=scene, type="point_light", name="Lamp"))["entity_id"]
        cube = (await ed.call("create_entity", scene=scene, type="box", name="Crate"))["entity_id"]
        host = (await ed.call("create_entity", scene=scene, type="empty", name="Host"))["entity_id"]
        await ed.call("create_script", kind="lua", name="PlaySpawner", content=SPAWNER, scene=scene, entity=host)
        edit_intensity = (await ed.call("get_entity", scene=scene, entity=light, components=["light"],
                                        properties=["intensity"]))["components"]["light"]["intensity"]
        await ed.call("save_scene", scene=scene)

        started = await ed.call("play", action="start", scene=scene)
        ed.check_eq(started["state"], "playing", f"play start: {started}")
        await ed.call("wait", frames=3)
        await ed.call("set_properties", scene=scene, entity=light, values={"light": {"intensity": edit_intensity + 4}})
        await ed.call("set_transform", scene=scene, entity=cube, position=[4, 0, 0])
        await ed.call("create_entity", scene=scene, type="empty", name="Made In Play")
        spawned = await ed.call("find_entities", scene=scene, name="Spawned By Script", runtime=True)
        ed.check(spawned["entities"], f"the script's GameObject: {spawned}")
        await ed.call("set_properties", scene=scene, entity=spawned["entities"][0]["id"],
                      values={"transform": {"position": [9, 9, 9]}})

        stopped = await ed.call("play", action="stop")
        ed.check_eq(stopped["state"], "stopped", "stop")
        ed.check_eq(stopped["pending_play_changes"], 4, f"pending rows: {stopped}")
        state = await ed.call("get_play_state")
        ed.check_eq(state["pending_play_changes"], 4, "get_play_state")

        rows = (await ed.call("get_play_changes", scene=scene))["rows"]
        ed.check_eq(len(rows), 4, f"rows: {rows}")
        by_name = {(r["game_object"] or {}).get("name"): r for r in rows}
        ed.check(by_name["Spawned By Script"]["keepable"] is False, f"script-spawned: {by_name['Spawned By Script']}")
        ed.check_eq(by_name["Spawned By Script"].get("reason"), "Created by a script during Play", "reason")
        ed.check(by_name["Lamp"]["keepable"] and by_name["Crate"]["keepable"] and by_name["Made In Play"]["keepable"],
                 f"keepable rows: {rows}")
        ed.check_eq(by_name["Made In Play"]["kind"], "created", "created row")

        kept = await ed.call("apply_play_changes", scene=scene, ids=[by_name["Lamp"]["id"]])
        ed.check(kept.get("undo", {}).get("name", "").startswith("Apply Play Mode Changes"), f"one step: {kept}")
        intensity = (await ed.call("get_entity", scene=scene, entity=light, components=["light"],
                                   properties=["intensity"]))["components"]["light"]["intensity"]
        ed.check(abs(intensity - (edit_intensity + 4)) < 1e-3, f"the light has its play value: {intensity}")
        pos = (await ed.call("get_entity", scene=scene, entity=cube, components=["transform"]))["components"]["transform"]["position"]
        ed.check(abs(pos[0]) < 1e-3, f"the cube keeps its edit value: {pos}")
        ed.check_eq((await ed.call("get_play_state"))["pending_play_changes"], 0, "the rest was discarded")

        await ed.call("undo", scope="scene", scene=scene)
        intensity = (await ed.call("get_entity", scene=scene, entity=light, components=["light"],
                                   properties=["intensity"]))["components"]["light"]["intensity"]
        ed.check(abs(intensity - edit_intensity) < 1e-3, f"undo: the edit value: {intensity}")

        # keep_changes all: a GameObject made while playing exists after Stop
        await ed.call("play", action="start", scene=scene)
        await ed.call("create_entity", scene=scene, type="box", name="Kept From Play")
        stopped = await ed.call("play", action="stop", keep_changes="all")
        ed.check(stopped.get("kept_changes", {}).get("kept", 0) >= 1, f"kept: {stopped}")
        found = await ed.call("find_entities", scene=scene, name="Kept From Play")
        ed.check(found["entities"], f"the GameObject made while playing exists after Stop: {found}")
