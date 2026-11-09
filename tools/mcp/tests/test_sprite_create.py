"""create_sprite (docs/specs/S47-agent-sprite-drawing.md AC14, R26): an indexed sprite with an S43
palette, a GIF's frames, delays and colours, output_exists, and one undo removing the file."""

from __future__ import annotations

import struct

from harness import Editor
from sprite_draw_util import digest, enable, err, ok

CI = True
EDITOR = {"create": True, "name": "SpriteCreate"}
TIMEOUT = 600
PATH = "assets/sprites/made.aseprite"


def write_gif(path, frames, w, h, delays_cs, palette):
    """An uncompressed-LZW GIF: frames of palette indices (each frame w*h bytes)"""
    out = bytearray(b"GIF89a")
    out += struct.pack("<HHBBB", w, h, 0xF0 | 3, 0, 0)     # global palette of 16
    table = bytearray()
    for r, g, b in palette:
        table += bytes((r, g, b))
    while len(table) < 48:
        table += b"\0\0\0"
    out += table
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"
    for pixels, delay in zip(frames, delays_cs):
        out += b"\x21\xF9\x04" + struct.pack("<BHBB", 0x04, delay, 0, 0)
        out += b"\x2C" + struct.pack("<HHHHB", 0, 0, w, h, 0)
        out += bytes((4,))                                  # LZW minimum code size
        # every code 5 bits: clear (16) before each pixel keeps the table from growing
        bits = 0
        count = 0
        data = bytearray()

        def put(code):
            nonlocal bits, count
            bits |= code << count
            count += 5
            while count >= 8:
                data.append(bits & 0xFF)
                bits >>= 8
                count -= 8
        for p in pixels:
            put(16)
            put(p)
        put(17)
        if count:
            data.append(bits & 0xFF)
        for i in range(0, len(data), 255):
            block = data[i:i + 255]
            out += bytes((len(block),)) + block
        out += b"\0"
    out += b"\x3B"
    path.write_bytes(bytes(out))


async def test_create_indexed_with_palette_frames_and_layers(ed: Editor):
    await enable(ed)
    made = await ok(ed, "create_sprite", path=PATH, size=[32, 32], palette="builtin:sweetie-16", frames=4, layers=["back", "front"], duration_ms=80)
    ed.check(made["mode"] == "indexed" and made["size"] == [32, 32], f"{made}")
    ed.check(len(made["frames"]) == 4 and made["frames"][0] == {"frame": 1, "duration_ms": 80}, f"{made['frames']}")
    ed.check([l["name"] for l in made["layers"]] == ["back", "front"], f"bottom first: {made['layers']}")
    # S44's reader, by way of get_asset_info: the same structure
    info = await ed.call("get_asset_info", path=PATH)
    sprite = info["sprite"]
    ed.check(sprite["frame_count"] == 4 and sprite["width"] == 32 and sprite["color_mode"] == "indexed", f"S44 reads it: {sprite}")
    ed.check(made["palette"][0] == "#00000000" and len(made["palette"]) == 17, f"sweetie-16 one index up, 0 transparent: {made['palette'][:3]}")


async def test_overwrite_false_refuses_an_existing_file(ed: Editor):
    e = await err(ed, "create_sprite", path=PATH, size=[8, 8])
    ed.check(e["kind"] == "output_exists" and "overwrite" in e["fix"], f"{e}")


async def test_gif_keeps_delays_and_colours(ed: Editor):
    w, h = 4, 4
    frames = [[(i + 1) % 4] * 16 for i in range(6)]
    delays = [10, 20, 10, 30, 10, 40]
    gif = ed.project / "assets" / "walk.gif"
    (ed.project / "assets").mkdir(exist_ok=True)
    write_gif(gif, frames, w, h, delays, [(10, 20, 30), (200, 10, 10), (10, 200, 10), (10, 10, 200)])
    path = "assets/sprites/fromgif.aseprite"
    made = await ok(ed, "create_sprite", path=path, **{"from": "assets/walk.gif"})
    ed.check(made["size"] == [4, 4] and len(made["frames"]) == 6, f"{made['size']} {made['frames']}")
    ed.check([f["duration_ms"] for f in made["frames"]] == [100, 200, 100, 300, 100, 400], f"delays: {made['frames']}")
    ed.check(made["mode"] == "indexed", f"few colours stay indexed: {made['mode']}")
    pal = set(made["palette"])
    ed.check({"#c80a0a", "#0ac80a", "#0a0ac8"} <= pal, f"the GIF's colours: {made['palette']}")


async def test_one_undo_deletes_the_file(ed: Editor):
    path = "assets/sprites/temp.aseprite"
    await ok(ed, "create_sprite", path=path, size=[8, 8])
    ed.check((ed.project / path).is_file(), "created")
    await ok(ed, "undo", scope="timeline")
    ed.check(not (ed.project / path).exists(), "one undo removes it")
    e = await err(ed, "get_sprite", sprite=path)
    ed.check(e["kind"] == "sprite_missing", f"{e}")
