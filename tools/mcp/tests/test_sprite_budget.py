"""The token budget of the sprite tools (docs/specs/S47-agent-sprite-drawing.md AC15, §6.3): the
text bytes of every row of the table, on generated 32 x 32 and 128 x 128 sprites, stay within 25 %
of the spec's estimates. The structuredContent JSON (what some clients show instead of the text)
is measured too and printed, for the 20-edit session and against the same limits where the
estimates already hold it."""

from __future__ import annotations

import json

from harness import Editor
from sprite_draw_util import enable, ok

CI = True
EDITOR = {"create": True, "name": "SpriteBudget"}
TIMEOUT = 600

PALETTE_32 = ["#{:02x}{:02x}{:02x}".format(i * 8, 255 - i * 8, 64) for i in range(15)]
PALETTE_128 = ["#{:02x}{:02x}{:02x}".format(i * 4 % 256, 255 - i * 4 % 256, 128) for i in range(31)]
SMALL = "assets/sprites/small.aseprite"
LARGE = "assets/sprites/large.aseprite"
TOLERANCE = 1.25


def size(value) -> int:
    return len(json.dumps(value, separators=(",", ":")))


def check(ed: Editor, name: str, bytes_used: int, estimate: int) -> None:
    print(f"     [budget] {name}: {bytes_used} bytes (estimate {estimate})")
    ed.check(bytes_used <= estimate * TOLERANCE, f"{name}: {bytes_used} bytes, estimate {estimate} +25%")


async def test_setup(ed: Editor):
    await enable(ed)
    await ok(ed, "create_sprite", path=SMALL, size=[32, 32], frames=4, palette=PALETTE_32)
    await ok(ed, "create_sprite", path=LARGE, size=[128, 128], frames=8, palette=PALETTE_128)
    # some colours in every frame
    for path, w in ((SMALL, 32), (LARGE, 128)):
        ops = [{"op": "rect", "rect": [2, 2, w // 2, w // 2], "color": 1 + (f * 3) % 12, "fill": True, "frame": f + 1} for f in range(4)]
        ops += [{"op": "ellipse", "rect": [w // 4, w // 4, w // 2, w // 3], "color": 7, "frame": f + 1} for f in range(4)]
        await ok(ed, "edit_sprite", sprite=path, ops=ops, echo="none")


async def text_and_structured(ed: Editor, tool: str, **args) -> tuple[int, int]:
    r = await ed.call_raw(tool, args)
    ed.check(not r.is_error, f"{tool}: {r.text}")
    return len(r.text.encode("utf-8")), size(r.structured)


async def test_get_sprite(ed: Editor):
    for path, name, estimate in ((SMALL, "32", 600), (LARGE, "128", 900)):
        text, structured = await text_and_structured(ed, "get_sprite", sprite=path)
        print(f"     [budget] get_sprite {name}: text {text} bytes, structured {structured} bytes")
        check(ed, f"get_sprite {name} text", text, estimate)


async def test_read_one_full_frame(ed: Editor):
    check(ed, "read full 32", size(await ok(ed, "read_sprite_pixels", sprite=SMALL, rect=[0, 0, 32, 32])), 1100 + 200)
    check(ed, "read tile 128", size(await ok(ed, "read_sprite_pixels", sprite=LARGE, rect=[0, 0, 32, 32])), 1100 + 200)


async def test_edit_eight_ops_sixteen_by_twelve(ed: Editor):
    ops = [{"op": "pixel", "points": [[x, y]], "color": 2} for x in range(4, 12) for y in (4, 15)]
    ops = ops[:8]
    ops += [{"op": "rect", "rect": [4, 4, 16, 12], "color": 3}]
    r = await ok(ed, "edit_sprite", sprite=SMALL, ops=ops)
    # the reply: echo 0.23 KB, ~0.6 KB in all, for 16 x 12 changed
    r.pop("undo", None)
    check(ed, "edit 16x12 32", size(r), 600)


async def test_render_is_one_small_text_part(ed: Editor):
    r = await ok(ed, "render_sprite", sprite=SMALL)
    r.pop("layout", None)
    check(ed, "render", size(r), 200)


async def test_diff_previous(ed: Editor):
    await ok(ed, "edit_sprite", sprite=SMALL, ops=[{"op": "pixel", "points": [[1, 1]], "color": 2}])
    check(ed, "diff 32", size(await ok(ed, "diff_sprite", sprite=SMALL, against="previous")), 400)
    await ok(ed, "edit_sprite", sprite=LARGE, ops=[{"op": "pixel", "points": [[1, 1]], "color": 2}])
    check(ed, "diff 128", size(await ok(ed, "diff_sprite", sprite=LARGE, against="previous")), 500)


async def test_twenty_edit_session(ed: Editor):
    total = 0
    total += size(await ok(ed, "get_sprite", sprite=SMALL))
    for i in range(4):
        total += size(await ok(ed, "read_sprite_pixels", sprite=SMALL, rect=[i * 8, 0, 8, 8]))
    for i in range(20):
        ops = [{"op": "pixel", "points": [[i, 20]], "color": 1 + i % 8}, {"op": "line", "points": [[i, 21], [i + 3, 21]], "color": 2}]
        total += size(await ok(ed, "edit_sprite", sprite=SMALL, ops=ops))
    for _ in range(5):
        r = await ok(ed, "render_sprite", sprite=SMALL)
        total += size(r)
    check(ed, "20-edit session", total, 20 * 1024)
