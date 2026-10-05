"""Engine/editor runtime regressions found when AI agents built games through the MCP server.

Runs on a scratch copy of the TappyPlane fixture (a 2D Lua game with a HUD child UI scene).
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
import struct
import sys
import time
import zlib

from harness import Editor, find_key, skip

TIMEOUT = 900

SCENES = {}


# -- helpers -----------------------------------------------------------------


def decode_png(data: bytes):
    """(width, height, channels, rows) for the 8-bit RGB/RGBA PNGs the editor writes."""
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


def image_difference(a: bytes, b: bytes, step: int = 4) -> float:
    """Mean absolute RGB difference (0..255) of two same-sized screenshots, sampled every `step` px."""
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


async def new_scene(ed: Editor, kind: str, name: str):
    """A new scene of `kind` (2d, 3d, ui), selected; returns its id."""
    if kind in SCENES:
        return SCENES[kind]
    await ed.call("create_scene", name=name, type=kind)
    for _ in range(50):
        status = await ed.call("editor_status")
        if status.get("dialog"):
            await ed.call("respond_dialog", choice="no")
            continue
        selected = [sc for sc in status["scenes"] if sc.get("selected")]
        if selected and selected[0]["name"].startswith(name):
            SCENES[kind] = selected[0]["id"]
            return SCENES[kind]
    ed.fail(f"create_scene did not make a selected scene named {name!r}: {status!r}")


async def play_and_collect(ed: Editor, scene, marker: str):
    """Start Play on `scene`, return (start result, console lines containing `marker`), stopped again."""
    cursor = (await ed.call("read_console", limit=1)).get("next_cursor", 0)
    started = await ed.call("play", action="start", scene=scene, wait=True)
    await ed.call("wait", frames=5)
    lines = await ed.call("read_console", since=cursor, limit=200)
    await ed.call("play", action="stop")
    texts = [e.get("message", "") for e in lines.get("entries", lines.get("logs", []))]
    return started, texts


def problem_texts(started) -> list:
    return [p.get("message", "") for p in started.get("problems", [])]


# -- HUD child scene orientation (first in the file: it needs the editor's first play) ------------


async def game_shot(ed: Editor) -> bytes:
    shot = await ed.call_raw("screenshot", {"view": "game"})
    ed.check(not shot.is_error and shot.images, f"no game screenshot: {shot.text[:300]}")
    return shot.images[0][0]


async def test_child_ui_scene_not_flipped_on_first_play(ed: Editor):
    """The fixture's Main Scene stacks the HUD Scene, which is not open in the editor: its
    runtime copy comes from disk. The first and second plays must draw the same picture."""
    shots = []
    for _ in range(2):
        await ed.call("play", action="start", scene="Main Scene", wait=True)
        await ed.call("wait", seconds=1)
        shots.append(await game_shot(ed))
        await ed.call("play", action="stop")
    diff = image_difference(shots[0], shots[1])
    if diff > 1.5:
        (ed.scratch.root / "first-play.png").write_bytes(shots[0])
        (ed.scratch.root / "second-play.png").write_bytes(shots[1])
    ed.check(diff <= 1.5, f"the first play draws differently from the second (mean RGB diff {diff:.2f}): "
                          f"the HUD child scene is upside down; see {ed.scratch.root}")


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
    ball = find_key(made, "entity_id", "id")
    await ed.call("edit_colliders", scene=sid, entity=ball, body_type="dynamic", add=[{"shape": "sphere"}])
    await ed.call("create_script", kind="lua", name="BodyProbe", content=BODY3D_SCRIPT, entity=ball)

    started, lines = await play_and_collect(ed, sid, "BODY3D-INIT")
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
    puck = find_key(made, "entity_id", "id")
    await ed.call("edit_colliders", scene=sid, entity=puck, body_type="dynamic", add=[{"shape": "circle", "radius": 10}])
    await ed.call("create_script", kind="lua", name="Body2DProbe", content=BODY2D_SCRIPT, entity=puck)

    started, lines = await play_and_collect(ed, sid, "BODY2D-INIT")
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
    sid = await new_scene(ed, "3d", "BodyInit3D")
    made = await ed.call("create_entity", scene=sid, name="Rubber", type="box", position=[2, 1, 0])
    rubber = find_key(made, "entity_id", "id")
    body = await ed.call("edit_colliders", scene=sid, entity=rubber, body_type="dynamic", add=[{"shape": "box"}],
                         body={"friction": 0.9, "restitution": 0.6})
    ed.check_eq(body["dimension"], "3d")
    got = await ed.call("get_component", scene=sid, entity=rubber, component="rigidbody")
    props = got["properties"]
    ed.check("friction" in props and abs(float(props["friction"]["value"]) - 0.9) < 1e-4, f"friction in the catalog: {props}")
    ed.check("restitution" in props and abs(float(props["restitution"]["value"]) - 0.6) < 1e-4, f"restitution: {props}")

    await ed.call("set_property", scene=sid, entity=rubber, component="rigidbody", property="friction", value=0.75)
    await ed.call("create_script", kind="lua", name="FrictionProbe", content=FRICTION_SCRIPT, entity=rubber)
    started, lines = await play_and_collect(ed, sid, "FRICTION-INIT")
    hits = [line for line in lines if "FRICTION-INIT" in line]
    ed.check(hits, f"init() did not log: {problem_texts(started)} {lines[-10:]}")
    ed.check_in("friction=0.75", hits[0], "the body is made with the component's friction")
    ed.check_in("restitution=0.60", hits[0], "and its restitution")

    # the scene file keeps them
    await ed.call("save_scene", scene=sid, path="FrictionTest.scene")
    text = (ed.project / "FrictionTest.scene").read_text()
    ed.check_in("friction: 0.75", text, "friction saved")
    ed.check_in("restitution: 0.6", text, "restitution saved")
    # an editor-made mesh no longer writes MAX_BONES identity matrices
    ed.check("bonesMatrix:\n          - [[1, 0, 0, 0]" not in text, "identity bone matrices are not written")


# -- Audio without an output device -------------------------------------------------------------


async def test_no_audio_device_is_quiet(ed: Editor):
    """Without an audio device (this headless host) the sounds of the TappyPlane game used to log
    'Failed to initialize audio output' as an error on every frame they were pending."""
    cursor = (await ed.call("read_console", limit=1)).get("next_cursor", 0)
    await ed.call("play", action="start", scene="Main Scene", wait=True)
    for _ in range(4):
        await ed.call("send_input", events=[{"type": "key", "key": "space"}])
        await ed.call("wait", seconds=0.3)
    errors = await ed.call("read_console", since=cursor, types=["error"], limit=200)
    everything = await ed.call("read_console", since=cursor, contains="audio", limit=200)
    await ed.call("play", action="stop")
    audio_errors = [e["message"] for e in errors.get("entries", []) if "audio" in e["message"].lower()]
    ed.check(not audio_errors, f"audio errors logged: {audio_errors[:3]} ({len(audio_errors)})")
    ed.check(len(everything.get("entries", [])) <= 1, f"audio messages repeat: {everything.get('entries', [])[:3]}")


# -- Idle CPU -------------------------------------------------------------------------------------


def _editor_cpu_ticks(root) -> tuple:
    """(pid, utime+stime ticks) of the editor running in the scratch dir `root` (Linux /proc)."""
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            cwd = os.readlink(f"/proc/{name}/cwd")
            exe = open(f"/proc/{name}/cmdline", "rb").read().split(b"\0")[0].decode()
            stat = open(f"/proc/{name}/stat").read()
        except OSError:
            continue
        if cwd.startswith(str(root)) and "jasperengine-editor" in exe:
            fields = stat[stat.rindex(")") + 2:].split()
            return int(name), int(fields[11]) + int(fields[12])
    return None, 0


async def test_idle_editor_does_not_spin(ed: Editor):
    """A headless editor's pointer rests over the Scene view; hovering used to redraw the scene
    every frame, so the loop never idled (about two cores of software GL)."""
    if not sys.platform.startswith("linux") or ed.scratch is None:
        skip("needs /proc and a stdio editor")
    await ed.call("editor_status")
    await asyncio.sleep(3)  # past the idle delay and the settle frames
    pid, before = _editor_cpu_ticks(ed.scratch.root)
    if pid is None:
        skip("editor process not found")
    start = time.monotonic()
    await asyncio.sleep(6)
    _, after = _editor_cpu_ticks(ed.scratch.root)
    cores = (after - before) / os.sysconf("SC_CLK_TCK") / (time.monotonic() - start)
    ed.check(cores < 0.8, f"an idle editor uses {cores:.2f} cores")
