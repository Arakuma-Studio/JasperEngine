"""S78 Light Steps and the 2D ambient without lights in the real editor (docs/specs/S78-sprite-normals-improvements.md
R1-R7): get_scene_settings / set_scene_settings light_2d_steps (0 by default, 0-16, an integer, one undo step), saved
as light2DSteps only when on and read back; a normal-mapped sprite under one Light 2D draws at most 7 light
levels at 6 steps and its smooth light at 0; with no Light 2D a dark 2D ambient tints the sprite."""

from __future__ import annotations

import math
import sys
from collections import Counter
from pathlib import Path

from harness import Editor

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import pngdiff  # noqa: E402
from htmlui_pixels import decode_png  # noqa: E402

EDITOR = {"create": True, "name": "Light Steps"}
CI = True
TIMEOUT = 900

SIZE = 64
WHITE = "assets/sprites/white.png"
DOME = "assets/sprites/dome.png"
M = "submeshes[0].material."
BACKGROUND = (0, 0, 255)
S: dict = {}


def dome_rgba() -> bytes:
    """A hemisphere normal map, OpenGL convention: n = (nx, ny up, sqrt(1 - r2)) inside the disc, +Z outside."""
    out = bytearray()
    half = SIZE / 2
    for y in range(SIZE):
        for x in range(SIZE):
            nx = (x + 0.5 - half) / half
            ny = (half - (y + 0.5)) / half
            r2 = nx * nx + ny * ny
            n = (nx, ny, math.sqrt(1 - r2)) if r2 < 1 else (0.0, 0.0, 1.0)
            out += bytes(round((c * 0.5 + 0.5) * 255) for c in n) + b"\xff"
    return bytes(out)


async def steps(ed: Editor, sid) -> int:
    return (await ed.call("get_scene_settings", scene=sid, names=["light_2d_steps"]))["settings"]["light_2d_steps"]


async def sprite_pixels(ed: Editor, sid, label: str) -> list[tuple[int, int, int]]:
    """The game view's pixels that are not the blue background, saved as scratch/light2d-steps-<label>.png."""
    raw = await ed.call_raw("screenshot", {"view": "game", "max_width": 0, "scene": sid})
    ed.check(not raw.is_error and raw.images, f"{label}: no screenshot: {raw.text[:300]}")
    png = raw.images[0][0]
    (ed.scratch.root / f"light2d-steps-{label}.png").write_bytes(png)
    S["canvas"] = raw.value.get("canvas") if isinstance(raw.value, dict) else None
    width, height, ch, rows = decode_png(png)
    out = []
    for y in range(height):
        row = rows[y]
        for x in range(width):
            p = tuple(row[x * ch:x * ch + 3])
            if abs(p[0] - BACKGROUND[0]) + abs(p[1] - BACKGROUND[1]) + abs(p[2] - BACKGROUND[2]) > 60:
                out.append(p)
    return out


async def test_light_steps_setting(ed: Editor):
    # (the first call waits for the new project's load, its engine API copy minutes long on a busy disk)
    made = await ed.call("create_scene", name="Steps", type="2D", timeout=900)
    sid = S["scene"] = made["scene"]["id"]
    ed.check_eq(await steps(ed, sid), 0, "off by default")
    await ed.call("set_scene_settings", scene=sid, settings={"light_2d_steps": 6})
    ed.check_eq(await steps(ed, sid), 6, "set to 6")
    text = await ed.call("set_scene_settings", scene=sid, settings={"light_2d_steps": 17}, expect_error=True)
    ed.check("0 to 16" in text, f"out of range names the range: {text}")
    text = await ed.call("set_scene_settings", scene=sid, settings={"light_2d_steps": 2.5}, expect_error=True)
    ed.check("integer" in text, f"a fraction is refused: {text}")
    ed.check_eq(await steps(ed, sid), 6, "the refused values change nothing")
    await ed.call("undo", scene=sid)
    ed.check_eq(await steps(ed, sid), 0, "one undo step")
    await ed.call("redo", scene=sid)
    ed.check_eq(await steps(ed, sid), 6, "redo")

    await ed.call("save_scene", scene=sid, path="scenes/Steps.scene")
    ed.check("light2DSteps: 6" in (ed.project / "scenes/Steps.scene").read_text(), "saved as light2DSteps")
    await ed.call("close_scene", scene=sid, unsaved="discard")
    opened = await ed.call("open_scene", scene="scenes/Steps.scene")
    sid = S["scene"] = opened["scene"]["id"]
    ed.check_eq(await steps(ed, sid), 6, "read back from the file")
    await ed.call("set_scene_settings", scene=sid, settings={"light_2d_steps": 0})
    await ed.call("save_scene", scene=sid)
    ed.check("light2DSteps" not in (ed.project / "scenes/Steps.scene").read_text(), "off is not written")


async def test_six_steps_band_a_normal_mapped_sprite(ed: Editor):
    folder = ed.project / "assets" / "sprites"
    folder.mkdir(parents=True, exist_ok=True)
    (ed.project / WHITE).write_bytes(pngdiff.encode(SIZE, SIZE, b"\xff" * (SIZE * SIZE * 4)))
    (ed.project / DOME).write_bytes(pngdiff.encode(SIZE, SIZE, dome_rgba()))
    sid = S["scene"]
    await ed.call("set_scene_settings", scene=sid, settings={
        "background_color": [c / 255 for c in BACKGROUND] + [1.0], "ambient_light_2d_intensity": 0.0})
    await ed.call("select_scene", scene=sid)
    await sprite_pixels(ed, sid, "empty")
    canvas = S["canvas"]
    ed.check(canvas and canvas.get("width") and canvas.get("height"), f"the screenshot names the canvas: {canvas!r}")
    position = [canvas["width"] / 2, canvas["height"] / 2]
    sprite = await ed.call("create_entity", scene=sid, type="sprite", name="Dome", position=[position[0], position[1], 0],
                           values={"mesh": {M + "baseColorTexture": WHITE}, "sprite": {"pivotPreset": "center"},
                                   "transform": {"scale": [4, 4, 1]}})
    S["sprite"] = sprite["entity_id"]
    await ed.call("set_properties", scene=sid, entity=S["sprite"], values={"mesh": {M + "normalTexture": DOME}})
    light = await ed.call("create_entity", scene=sid, type="light_2d", name="Lamp", position=[position[0] - 60, position[1] + 40, 0],
                          values={"light2d": {"intensity": 1.0, "range": 400.0, "falloff": 1.0}})
    S["light"] = light["entity_id"]

    smooth = await sprite_pixels(ed, sid, "off")
    ed.check(len(smooth) > 1000, f"the sprite is on screen ({len(smooth)} pixels)")
    ed.check(len(set(smooth)) > 20, f"a smooth light has many levels: {len(set(smooth))}")

    await ed.call("set_scene_settings", scene=sid, settings={"light_2d_steps": 6})
    stepped = await sprite_pixels(ed, sid, "6")
    counts = Counter(stepped)
    main = counts.most_common(7)
    covered = sum(n for _, n in main) / max(len(stepped), 1)
    # (the view's scaling may blend neighbouring bands at their edges: the 7 main levels cover the sprite)
    ed.check(covered > 0.97, f"7 levels cover the stepped sprite ({covered:.3f}): {counts.most_common(12)}")
    ed.check(len([n for n in counts.values() if n > len(stepped) * 0.01]) >= 3, f"banded, not flat: {counts.most_common(12)}")


async def test_ambient_tints_without_lights(ed: Editor):
    sid = S["scene"]
    await ed.call("set_scene_settings", scene=sid, settings={"light_2d_steps": 0})
    await ed.call("delete_entities", scene=sid, entity=S["light"])
    await ed.call("set_scene_settings", scene=sid, settings={"ambient_light_2d_intensity": 1.0,
                                                             "ambient_light_2d_color": [0.3, 0.3, 0.4]})
    tinted = Counter(await sprite_pixels(ed, sid, "ambient"))
    colour, share = tinted.most_common(1)[0]
    ed.check(share > 0.9 * sum(tinted.values()), f"one colour: {tinted.most_common(4)}")
    ed.check(all(abs(a - b) <= 4 for a, b in zip(colour, (77, 77, 102))), f"the ambient tints the sprite: {colour}")
    await ed.call("set_scene_settings", scene=sid, settings={"ambient_light_2d_color": [1.0, 1.0, 1.0]})
    white = Counter(await sprite_pixels(ed, sid, "neutral"))
    ed.check(all(c >= 250 for c in white.most_common(1)[0][0]), f"a neutral ambient draws it white: {white.most_common(4)}")
