"""create_sprite (docs/specs/S47-agent-sprite-drawing.md AC14, R26): an indexed sprite with an S43
palette, a GIF's frames, delays and colours, output_exists, and one undo removing the file."""

from __future__ import annotations

import struct

from harness import Editor
from sprite_draw_util import enable, err, expand_frames, ok

CI = True
EDITOR = {"create": True, "name": "SpriteCreate"}
TIMEOUT = 600
PATH = "assets/sprites/made.aseprite"


def write_gif(path, frames, w, h, delays_cs, palette, local_palettes=None):
    """An uncompressed-LZW GIF: frames of palette indices (each frame w*h bytes); local_palettes
    gives each frame its own table of 16 colours (GIF allows 256 colours only per table)"""
    out = bytearray(b"GIF89a")
    out += struct.pack("<HHBBB", w, h, 0xF0 | 3, 0, 0)     # global palette of 16
    table = bytearray()
    for r, g, b in palette:
        table += bytes((r, g, b))
    while len(table) < 48:
        table += b"\0\0\0"
    out += table
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"
    for n, (pixels, delay) in enumerate(zip(frames, delays_cs)):
        out += b"\x21\xF9\x04" + struct.pack("<BHBB", 0x04, delay, 0, 0)
        if local_palettes:
            out += b"\x2C" + struct.pack("<HHHHB", 0, 0, w, h, 0x83)
            for r, g, b in local_palettes[n]:
                out += bytes((r, g, b))
        else:
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
    frames = expand_frames(made)
    ed.check(len(frames) == 4 and frames[0] == {"frame": 1, "duration_ms": 80}, f"{made['frames']}")
    ed.check([l["name"] for l in made["layers"]] == ["back", "front"], f"bottom first: {made['layers']}")
    # S44's reader, by way of get_asset_info: the same structure, layers named too
    info = await ed.call("get_asset_info", path=PATH)
    sprite = info["sprite"]
    ed.check(sprite["frame_count"] == 4 and sprite["width"] == 32 and sprite["color_mode"] == "indexed", f"S44 reads it: {sprite}")
    ed.check([l["name"] for l in sprite["layers"]] == ["back", "front"], f"S44 reads the layers: {sprite['layers']}")
    ed.check(made["palette"][0] == "#00000000" and len(made["palette"]) == 17, f"sweetie-16 one index up, 0 transparent: {made['palette'][:3]}")


async def test_overwrite_false_refuses_an_existing_file(ed: Editor):
    e = await err(ed, "create_sprite", path=PATH, size=[8, 8])
    ed.check(e["kind"] == "output_exists" and "overwrite" in e["fix"], f"{e}")


async def test_gif_keeps_delays_colours_and_every_frame(ed: Editor):
    w, h = 4, 4
    frames = [[(i + 1) % 4] * 16 for i in range(6)]
    delays = [10, 20, 10, 30, 10, 40]
    gif = ed.project / "assets" / "walk.gif"
    (ed.project / "assets").mkdir(exist_ok=True)
    write_gif(gif, frames, w, h, delays, [(10, 20, 30), (200, 10, 10), (10, 200, 10), (10, 10, 200)])
    path = "assets/sprites/fromgif.aseprite"
    made = await ok(ed, "create_sprite", path=path, **{"from": "assets/walk.gif"})
    ed.check(made["size"] == [4, 4] and len(expand_frames(made)) == 6, f"{made['size']} {made['frames']}")
    ed.check([f["duration_ms"] for f in expand_frames(made)] == [100, 200, 100, 300, 100, 400], f"delays: {made['frames']}")
    ed.check(made["mode"] == "indexed", f"few colours stay indexed: {made['mode']}")
    ed.check(made["palette"][0] == "#00000000", f"index 0 is the transparent index: {made['palette']}")
    # the GIF's own colour 0 (10, 20, 30) is a colour, drawn in frame 4, and is written like the others
    pal = made["palette"]
    ed.check({"#0a141e", "#c80a0a", "#0ac80a", "#0a0ac8"} <= set(pal), f"every GIF colour, its colour 0 too: {pal}")
    read = await ok(ed, "read_sprite_pixels", sprite=path, frame=[1, 2, 3, 4, 5, 6])
    # first-appearance order: the frames fill with GIF colours 1, 2, 3, 0, 1, 2
    expected = ["1", "2", "3", "4", "1", "2"]
    for f, want in zip(read["frames"], expected):
        ed.check(f["rows"] == [want * 4] * 4, f"frame {f['frame']}: {f['rows']}")
    ed.check(read["used"]["4"] == "#0a141e" and read["used"]["1"] == "#c80a0a", f"the colours: {read['used']}")


def many_colour_gif(path) -> int:
    """18 frames of 16 distinct colours each: 288 colours in all, past what an indexed sprite holds"""
    tables, frames = [], []
    for f in range(18):
        tables.append([(f * 13 % 256, (c * 16 + f) % 256, 200) for c in range(16)])
        frames.append(list(range(16)))
    write_gif(path, frames, 4, 4, [10] * 18, [(0, 0, 0)], local_palettes=tables)
    return sum(len(t) for t in tables)


async def test_mode_indexed_refuses_too_many_colours(ed: Editor):
    gif = ed.project / "assets" / "busy.gif"
    many_colour_gif(gif)
    path = "assets/sprites/busy.aseprite"
    e = await err(ed, "create_sprite", path=path, mode="indexed", **{"from": "assets/busy.gif"})
    ed.check(e["kind"] == "too_many_colors", f"an explicit indexed asks too much: {e}")
    ed.check(not (ed.project / path).exists(), "nothing written")
    made = await ok(ed, "create_sprite", path=path, **{"from": "assets/busy.gif"})
    ed.check(made["mode"] == "rgba", f"without mode it becomes RGBA: {made['mode']}")


async def test_from_outside_the_project_is_refused(ed: Editor):
    outside = ed.project.parent / "secret.gif"
    write_gif(outside, [[0] * 4], 2, 2, [10], [(1, 2, 3)])
    e = await err(ed, "create_sprite", path="assets/sprites/stolen.aseprite", **{"from": "../secret.gif"})
    ed.check(e["kind"] == "outside_assets", f"nothing outside the project is read: {e}")
    ed.check(not (ed.project / "assets/sprites/stolen.aseprite").exists(), "nothing written")


async def test_one_undo_deletes_the_file(ed: Editor):
    path = "assets/sprites/temp.aseprite"
    await ok(ed, "create_sprite", path=path, size=[8, 8])
    ed.check((ed.project / path).is_file(), "created")
    await ok(ed, "undo", scope="timeline")
    ed.check(not (ed.project / path).exists(), "one undo removes it")
    e = await err(ed, "get_sprite", sprite=path)
    ed.check(e["kind"] == "sprite_missing", f"{e}")
