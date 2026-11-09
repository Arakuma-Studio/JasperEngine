"""The token budget of the sprite tools (docs/specs/S47-agent-sprite-drawing.md AC15, §6.3): every row
of the table, on generated 32 x 32 and 128 x 128 sprites, measured as the compact JSON of the reply's
structuredContent (what clients such as Claude Code show the model on a success) and held within
25 % of the spec's estimate. Nothing is removed from a reply before it is measured."""

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
RENDER_SMALL = "assets/sprites/render_small.aseprite"
RENDER_LARGE = "assets/sprites/render_large.aseprite"
TOLERANCE = 1.25


def size(value) -> int:
    return len(json.dumps(value, separators=(",", ":"), ensure_ascii=False))


def check(ed: Editor, name: str, bytes_used: int, estimate: int) -> None:
    print(f"     [budget] {name}: {bytes_used} bytes (estimate {estimate})")
    ed.check(bytes_used <= estimate * TOLERANCE, f"{name}: {bytes_used} bytes, estimate {estimate} +25%")


async def paint(ed: Editor, path: str, w: int) -> None:
    ops = [{"op": "rect", "rect": [2, 2, w // 2, w // 2], "color": 1 + (f * 3) % 12, "fill": True, "frame": f + 1} for f in range(4)]
    ops += [{"op": "ellipse", "rect": [w // 4, w // 4, w // 2, w // 3], "color": 7, "frame": f + 1} for f in range(4)]
    await ok(ed, "edit_sprite", sprite=path, ops=ops, echo="none")


async def test_setup(ed: Editor):
    await enable(ed)
    await ok(ed, "create_sprite", path=SMALL, size=[32, 32], frames=4, palette=PALETTE_32)
    await ok(ed, "create_sprite", path=LARGE, size=[128, 128], frames=8, palette=PALETTE_128)
    await paint(ed, SMALL, 32)
    await paint(ed, LARGE, 128)
    # the render rows carry no last_edit of an earlier call: a sprite no edit has touched
    await ok(ed, "create_sprite", path=RENDER_SMALL, size=[32, 32], frames=4, palette=PALETTE_32)
    await ok(ed, "create_sprite", path=RENDER_LARGE, size=[128, 128], frames=8, palette=PALETTE_128)


async def test_get_sprite_stays_in_budget(ed: Editor):
    check(ed, "get_sprite 32", size(await ok(ed, "get_sprite", sprite=SMALL)), 600)
    check(ed, "get_sprite 128", size(await ok(ed, "get_sprite", sprite=LARGE)), 900)


async def test_read_full_frame_and_tile_stay_in_budget(ed: Editor):
    check(ed, "read full 32", size(await ok(ed, "read_sprite_pixels", sprite=SMALL, rect=[0, 0, 32, 32])), 1300)
    check(ed, "read tile 128", size(await ok(ed, "read_sprite_pixels", sprite=LARGE, rect=[0, 0, 32, 32])), 1300)


async def test_edit_of_eight_ops_16x12_stays_in_budget(ed: Editor):
    ops = [{"op": "pixel", "points": [[x, 6]], "color": 2} for x in range(5, 12)]
    ops.append({"op": "rect", "rect": [4, 4, 16, 12], "color": 3})
    check(ed, "edit request", size({"sprite": SMALL, "ops": ops}), 500)
    check(ed, "edit 16x12 32", size(await ok(ed, "edit_sprite", sprite=SMALL, ops=ops)), 600)
    check(ed, "edit 16x12 128", size(await ok(ed, "edit_sprite", sprite=LARGE, ops=ops)), 600)


async def test_render_stays_in_budget(ed: Editor):
    check(ed, "render 32", size(await ok(ed, "render_sprite", sprite=RENDER_SMALL)), 200)
    # §6.3 gives the 128 row in image tokens only: its text is the layout of 8 panels (about
    # twice the 32 row's 0.2 KB), so 500 is the 8-panel bound before the estimate's 25 %
    check(ed, "render 128", size(await ok(ed, "render_sprite", sprite=RENDER_LARGE)), 500)


async def test_diff_previous_stays_in_budget(ed: Editor):
    await ok(ed, "edit_sprite", sprite=SMALL, ops=[{"op": "pixel", "points": [[1, 1]], "color": 2}])
    check(ed, "diff 32", size(await ok(ed, "diff_sprite", sprite=SMALL, against="previous")), 400)
    await ok(ed, "edit_sprite", sprite=LARGE, ops=[{"op": "pixel", "points": [[1, 1]], "color": 2}])
    check(ed, "diff 128", size(await ok(ed, "diff_sprite", sprite=LARGE, against="previous")), 500)


async def session_bytes(ed: Editor, path: str) -> int:
    total = size(await ok(ed, "get_sprite", sprite=path))
    for i in range(4):
        total += size(await ok(ed, "read_sprite_pixels", sprite=path, rect=[i * 8, 0, 8, 8]))
    for i in range(20):
        ops = [{"op": "pixel", "points": [[i, 20]], "color": 1 + i % 8}, {"op": "line", "points": [[i, 21], [i + 3, 21]], "color": 2}]
        total += size(await ok(ed, "edit_sprite", sprite=path, ops=ops))
    for _ in range(5):
        total += size(await ok(ed, "render_sprite", sprite=path))
    return total


async def test_twenty_edit_sessions_stay_in_budget(ed: Editor):
    check(ed, "20-edit session 32", await session_bytes(ed, SMALL), 20 * 1024)
    check(ed, "20-edit session 128", await session_bytes(ed, LARGE), 30 * 1024)
