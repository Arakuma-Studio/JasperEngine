"""Engine/editor runtime regressions found when AI agents built games through the MCP server.

Each retained regression authors its minimal scene in a new empty project.
Tests run in order and share one editor.

- Rigidbody / Rigidbody2D are usable from a Lua init() on the first play (the body used to be
  made only when the scene entered the Engine, after the scripts' init()).
- The HUD child scene is not drawn upside down on the first play.
- Rigidbody (3D) friction and restitution are catalog properties that survive the scene file.
- No audio device: one warning, no error per sound.
- An idle headless editor idles (the resting pointer over the Scene view kept it drawing).
"""

import asyncio
import os
import sys
import time
import wave

from harness import Editor, skip
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "Runtime Regressions"}
CI = True
TIMEOUT = 900



# -- helpers -----------------------------------------------------------------


async def new_scene(ed: Editor, kind: str, name: str):
    return (await ed.call("create_scene", name=name, type=kind))["scene"]["id"]


async def play_and_collect(ed: Editor, scene):
    """Start Play on `scene`, collect console lines, and stop again."""
    cursor = (await ed.call("read_console", limit=1)).get("next_cursor", 0)
    started = await ed.call("play", action="start", scene=scene, wait=True)
    try:
        await ed.call("wait", frames=1)
        lines = await ed.call("read_console", since=cursor, limit=200)
    finally:
        await ed.call("play", action="stop")
    texts = [e["message"] for e in lines["entries"]]
    return started, texts


def image_difference(a: bytes, b: bytes, step: int = 4) -> float:
    """Mean absolute RGB difference (0..255) of two same-sized screenshots, sampled every `step` px
    (the atmosphere tests import it from here)."""
    wa, ha, ca, ra = decode_png(a)
    wb, hb, cb, rb = decode_png(b)
    assert (wa, ha) == (wb, hb), f"sizes differ: {wa}x{ha} vs {wb}x{hb}"
    total = 0
    count = 0
    for y in range(0, ha, step):
        for x in range(0, wa, step):
            for k in range(3):
                total += abs(ra[y][x * ca + k] - rb[y][x * cb + k])
            count += 3
    return total / max(count, 1)


def problem_texts(started) -> list:
    return [p.get("message", "") for p in started.get("problems", [])]


# -- HUD child scene orientation (first in the file: it needs the editor's first play) ------------


async def test_child_ui_scene_not_flipped_on_first_play(ed: Editor):
    await ed.call("set_project_settings", values={"graphics.canvas_width": 1280, "graphics.canvas_height": 720})
    child = (await ed.call("create_scene", name="OrientationChild", type="ui"))["scene"]["id"]
    for name, color, y in [("Top", [1, 0, 0, 1], 0), ("Bottom", [0, 0, 1, 1], 360)]:
        await ed.call("create_ui", scene=child, root={
            "type": "image", "name": name, "color": color,
            "layout": {"anchor": "top_left", "position": [0, y], "size": [1280, 360]}})
    await ed.call("save_scene", scene=child, path="OrientationChild.scene")
    parent = await new_scene(ed, "2d", "OrientationParent")
    await ed.call("set_child_scene", scene=parent, child=child, start_active=True)
    await ed.call("save_scene", scene=parent, path="OrientationParent.scene")
    await ed.call("close_scene", scene=child, unsaved="discard")
    await ed.call("play", action="start", scene=parent, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=2)
        shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 640})
        ed.check(not shot.is_error and shot.images, shot.text)
        data = shot.images[0][0]
        (ed.scratch.root / "child-ui-first-play.png").write_bytes(data)
        _, _, channels, rows = decode_png(data)
        rect = shot.value["canvas_rect_in_image"]
        x = round(rect["x"] + rect["width"] / 2)
        top = rows[round(rect["y"] + rect["height"] / 4)][x*channels:x*channels+3]
        bottom = rows[round(rect["y"] + rect["height"] * 3 / 4)][x*channels:x*channels+3]
        ed.check(top[0] > 200 and top[2] < 30, "the authored top panel is red on the first disk-loaded child play")
        ed.check(bottom[2] > 200 and bottom[0] < 30, "the authored bottom panel is blue")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


# -- 1. Rigidbody ready in init() --------------------------------------------------------------


BODY3D_SCRIPT = """local BodyProbe = {}

function BodyProbe:init()
    local body = Rigidbody(self.scene, self.entity)
    body.friction = 0.7
    local p = body.position
    Log.print(string.format("BODY3D-INIT friction=%.2f y=%.2f", body.friction, p.y))
end

return BodyProbe
"""

BODY2D_SCRIPT = """local Body2DProbe = {}

function Body2DProbe:init()
    local body = Rigidbody2D(self.scene, self.entity)
    local v = body.linearVelocity
    local p = body.position
    Log.print(string.format("BODY2D-INIT vx=%.2f x=%.2f", v.x, p.x))
end

return Body2DProbe
"""


async def test_rigidbody3d_ready_in_init(ed: Editor):
    sid = await new_scene(ed, "3d", "BodyInit3D")
    made = await ed.call("create_entity", scene=sid, name="Ball", type="sphere", position=[0, 3, 0])
    ball = made["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=ball, body_type="dynamic", add=[{"shape": "sphere"}])
    await ed.call("create_script", kind="lua", name="BodyProbe", content=BODY3D_SCRIPT, entity=ball)

    started, lines = await play_and_collect(ed, sid)
    problems = problem_texts(started)
    ed.check(not any("not loaded" in p or "init() failed" in p for p in problems),
             f"Rigidbody not ready in init() on the first play: {problems}")
    hits = [line for line in lines if "BODY3D-INIT" in line]
    ed.check(hits, f"init() did not log: problems={problems} console={lines[-10:]}")
    ed.check_in("friction=0.70", hits[0], "friction set in init() reads back")
    ed.check_in("y=3.00", hits[0], "the body starts at the entity's position")


async def test_rigidbody2d_ready_in_init(ed: Editor):
    sid = await new_scene(ed, "2d", "BodyInit2D")
    made = await ed.call("create_entity", scene=sid, name="Puck", type="sprite", position=[50, 80, 0])
    puck = made["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=puck, body_type="dynamic", add=[{"shape": "circle", "radius": 10}])
    await ed.call("create_script", kind="lua", name="Body2DProbe", content=BODY2D_SCRIPT, entity=puck)

    started, lines = await play_and_collect(ed, sid)
    problems = problem_texts(started)
    ed.check(not any("not loaded" in p or "init() failed" in p for p in problems),
             f"Rigidbody2D not ready in init() on the first play: {problems}")
    hits = [line for line in lines if "BODY2D-INIT" in line]
    ed.check(hits, f"init() did not log: problems={problems} console={lines[-10:]}")
    ed.check_in("x=50.00", hits[0], "the body starts at the entity's position")


# -- 3D friction / restitution in the catalog ---------------------------------------------------


FRICTION_SCRIPT = """local FrictionProbe = {}

function FrictionProbe:init()
    local body = Rigidbody(self.scene, self.entity)
    Log.print(string.format("FRICTION-INIT friction=%.2f restitution=%.2f", body.friction, body.restitution))
end

return FrictionProbe
"""


async def test_rigidbody3d_friction_restitution_properties(ed: Editor):
    sid = await new_scene(ed, "3d", "MaterialPersistence3D")
    made = await ed.call("create_entity", scene=sid, name="Rubber", type="box", position=[2, 1, 0])
    rubber = made["entity_id"]
    body = await ed.call("edit_colliders", scene=sid, entity=rubber, body_type="dynamic", add=[{"shape": "box"}],
                         body={"friction": 0.9, "restitution": 0.6})
    ed.check_eq(body["dimension"], "3d")
    got = await ed.call("get_entity", scene=sid, entity=rubber, components=["rigidbody"], properties=["friction", "restitution"])
    props = got["components"]["rigidbody"]
    ed.check("friction" in props and abs(float(props["friction"]) - 0.9) < 1e-4, f"friction in the catalog: {props}")
    ed.check("restitution" in props and abs(float(props["restitution"]) - 0.6) < 1e-4, f"restitution: {props}")

    await ed.call("set_properties", scene=sid, entity=rubber, values={"rigidbody": {"friction": 0.75}})
    await ed.call("create_script", kind="lua", name="FrictionProbe", content=FRICTION_SCRIPT, entity=rubber)
    started, lines = await play_and_collect(ed, sid)
    hits = [line for line in lines if "FRICTION-INIT" in line]
    ed.check(hits, f"init() did not log: {problem_texts(started)} {lines[-10:]}")
    ed.check_in("friction=0.75", hits[0], "the body is made with the component's friction")
    ed.check_in("restitution=0.60", hits[0], "and its restitution")

    await ed.call("save_scene", scene=sid, path="FrictionTest.scene")
    await ed.call("close_scene", scene=sid, unsaved="discard")
    reopened = await ed.call("open_scene", scene="FrictionTest.scene")
    props = (await ed.call("get_entity", scene=reopened["scene"]["id"], entity="Rubber",
                          components=["rigidbody"], properties=["friction", "restitution"]))["components"]["rigidbody"]
    ed.check(abs(float(props["friction"]) - 0.75) < 1e-4, props)
    ed.check(abs(float(props["restitution"]) - 0.6) < 1e-4, props)


# -- Audio without an output device -------------------------------------------------------------


async def test_no_audio_device_is_quiet(ed: Editor):
    """A real valid sound is requested repeatedly; unavailable output stays quiet."""
    path = ed.project / "assets" / "probe.wav"
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as sound:
        sound.setparams((1, 2, 44100, 441, "NONE", "not compressed"))
        sound.writeframes(bytes(441 * 2))
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    try:
        loaded = await ed.call("eval_lua", code="audioProbe = AudioSource(scene); audioProbe:loadSound('assets/probe.wav'); audioProbe:play()")
        await ed.call("wait", until="audioProbe:getLength() > 0", timeout_seconds=10)
        loaded = await ed.call("eval_lua", code="return audioProbe:getLength()")
        ed.check(abs(loaded["values"][0] - 0.01) < 1e-4, "the owned valid PCM sample was decoded before playback")
        for _ in range(4):
            await ed.call("eval_lua", code="audioProbe:play()")
            await ed.call("wait", frames=1)
        errors = await ed.call("read_console", since=cursor, types=["error"], contains="audio")
        device_warnings = await ed.call("read_console", since=cursor, contains="No audio output available")
        ed.check_eq(errors["entries"], [], "lack of an output device is not a playback error")
        ed.check(len(device_warnings["entries"]) <= 1, "the unavailable output device is not retried for every sound")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


# -- Idle CPU -------------------------------------------------------------------------------------


def _editor_cpu_ticks(root) -> tuple:
    """(pid, utime+stime ticks) of the editor running in the scratch dir `root` (Linux /proc)."""
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            cwd = os.readlink(f"/proc/{name}/cwd")
            with open(f"/proc/{name}/cmdline", "rb") as process:
                command = process.read().split(b"\0")
            with open(f"/proc/{name}/stat") as process:
                stat = process.read()
        except OSError:
            continue
        if cwd == str(root) and b"mcp" in command:
            fields = stat[stat.rindex(")") + 2:].split()
            return int(name), int(fields[11]) + int(fields[12])
    return None, 0


async def wait_for_quiet_jobs(ed: Editor, timeout: float = 600.0) -> None:
    """Until the job system has nothing queued or running on any lane and no asset job, three
    reads in a row (a cook batch queues its next texture as one ends)."""
    deadline = time.monotonic() + timeout
    quiet = 0
    stats = {}
    while quiet < 3:
        stats = await ed.call("get_job_system_stats", window_frames=1)
        busy = stats["asset_jobs"] or any(lane["queued"] or lane["running"] for lane in stats["lanes"].values())
        quiet = 0 if busy else quiet + 1
        ed.check(time.monotonic() < deadline, f"the editor's jobs never went quiet: {stats['lanes']}, {stats['asset_jobs']} asset job(s)")
        await asyncio.sleep(0.5)


async def test_idle_editor_does_not_spin(ed: Editor):
    """A headless editor's pointer rests over the Scene view; hovering used to redraw the scene
    every frame, so the loop never idled (about two cores of software GL). Measured once the
    editor has no work left: a project's first open queues background jobs (S14 cooks every
    texture of the project for the view's platform), which are work, not spinning."""
    if not sys.platform.startswith("linux") or ed.scratch is None:
        skip("needs /proc and a stdio editor")
    await ed.call("editor_status")
    await wait_for_quiet_jobs(ed)
    await asyncio.sleep(3)  # past the idle delay and the settle frames
    pid, before = _editor_cpu_ticks(ed.scratch.root)
    ed.check(pid is not None, "the owned local editor process is measurable")
    start = time.monotonic()
    await asyncio.sleep(6)
    _, after = _editor_cpu_ticks(ed.scratch.root)
    cores = (after - before) / os.sysconf("SC_CLK_TCK") / (time.monotonic() - start)
    ed.check(cores < 0.8, f"an idle editor uses {cores:.2f} cores")
