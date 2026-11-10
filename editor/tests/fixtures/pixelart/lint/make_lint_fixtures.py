#!/usr/bin/env python3
"""The hand-made fixtures of S46's lint catalogue (docs/specs/S46-pixel-art-cleanup.md AC10), written
as RGBA PNGs beside this script. One positive (<lint>_pos) and one negative (<lint>_neg) picture per
lint; a multi-frame fixture is one file per frame (<name>_f1.png, _f2.png, ...). Deterministic:
`python3 -I make_lint_fixtures.py` rewrites the same bytes. No PIL: a zlib + struct PNG writer."""

from __future__ import annotations

import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
CLEAR = (0, 0, 0, 0)


def png(path: str, pixels: list[list[tuple[int, int, int, int]]]) -> None:
    h = len(pixels)
    w = len(pixels[0])
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(os.path.join(HERE, path), "wb") as f:
        f.write(data)


def canvas(w: int, h: int, fill=CLEAR) -> list[list[tuple[int, int, int, int]]]:
    return [[fill for _ in range(w)] for _ in range(h)]


def grey(v: int, a: int = 255) -> tuple[int, int, int, int]:
    return (v, v, v, a)


def rgb(hexcode: str, a: int = 255) -> tuple[int, int, int, int]:
    return (int(hexcode[0:2], 16), int(hexcode[2:4], 16), int(hexcode[4:6], 16), a)


# Sweetie 16's colours (S43's preset), the off_palette fixtures' palette
SWEETIE = ["1a1c2c", "5d275d", "b13e53", "ef7d57", "ffcd75", "a7f070", "38b764", "257179",
           "29366f", "3b5dc9", "41a6f6", "73eff7", "f4f4f4", "94b0c2", "566c86", "333c57"]


def blob(w: int, h: int, colors: list[tuple[int, int, int, int]]) -> list[list[tuple[int, int, int, int]]]:
    """a 1-px transparent margin around bands of the given colours (each band 2 rows tall)"""
    img = canvas(w, h)
    for y in range(1, h - 1):
        for x in range(1, w - 1):
            img[y][x] = colors[((y - 1) // 2) % len(colors)]
    return img


def staircase(lengths: list[int]) -> list[list[tuple[int, int, int, int]]]:
    """a solid shape whose top edge is one chain of runs, each one row lower than the last"""
    w = sum(lengths) + 2
    h = len(lengths) + 4
    img = canvas(w, h)
    x = 1
    for k, length in enumerate(lengths):
        for _ in range(length):
            for y in range(1 + k, h - 1):
                img[y][x] = grey(90)
            x += 1
    return img


def disc(color_of) -> list[list[tuple[int, int, int, int]]]:
    """a 12x12 disc in a 1-px margin, coloured by color_of(x, y, d) (d: 4-connected distance to the outside)"""
    n = 14
    inside = [[(x - 0.5 - 6) ** 2 + (y - 0.5 - 6) ** 2 <= 36 and 1 <= x <= 12 and 1 <= y <= 12 for x in range(n)] for y in range(n)]
    dist = [[0] * n for _ in range(n)]
    frontier = []
    for y in range(n):
        for x in range(n):
            if inside[y][x] and any(not inside[y + dy][x + dx] for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                dist[y][x] = 1
                frontier.append((x, y))
    while frontier:
        nxt = []
        for x, y in frontier:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                if inside[y + dy][x + dx] and dist[y + dy][x + dx] == 0:
                    dist[y + dy][x + dx] = dist[y][x] + 1
                    nxt.append((x + dx, y + dy))
        frontier = nxt
    img = canvas(n, n)
    for y in range(n):
        for x in range(n):
            if inside[y][x]:
                img[y][x] = color_of(x, y, dist[y][x])
    return img


def main() -> int:
    # off_palette: Sweetie 16 bands, the positive with 3 pixels one step off its colours
    on = [rgb(c) for c in SWEETIE[:6]]
    img = blob(10, 10, on)
    png("off_palette_neg.png", img)
    img = blob(10, 10, on)
    img[2][2] = rgb("1a1c2d")
    img[5][5] = rgb("b13e54")
    img[7][3] = rgb("fffffe")
    png("off_palette_pos.png", img)

    # colors (rules.max_colors 4): 5 colours against 4
    png("colors_pos.png", blob(10, 12, [grey(30), grey(80), grey(130), grey(180), grey(230)]))
    png("colors_neg.png", blob(10, 12, [grey(30), grey(80), grey(130), grey(180)]))

    # partial_alpha: soft pixels along one edge
    img = blob(8, 8, [grey(100)])
    for y in range(1, 7):
        img[y][6] = grey(100, 128)
    png("partial_alpha_pos.png", img)
    png("partial_alpha_neg.png", blob(8, 8, [grey(100)]))

    # transparent_background: a sprite left on its painted background, and the same cut out
    img = canvas(10, 10, rgb("333c57"))
    for y in range(3, 7):
        for x in range(3, 7):
            img[y][x] = rgb("b13e53")
    png("transparent_background_pos.png", img)
    img = canvas(10, 10)
    for y in range(3, 7):
        for x in range(3, 7):
            img[y][x] = rgb("b13e53")
    png("transparent_background_neg.png", img)

    # size (rules.sizes [16x16]): 17x16 against 16x16
    png("size_pos.png", blob(17, 16, [grey(90)]))
    png("size_neg.png", blob(16, 16, [grey(90)]))

    # near_duplicates: two greys one step apart, against two far apart
    png("near_duplicates_pos.png", blob(8, 10, [grey(120), grey(121), grey(30)]))
    png("near_duplicates_neg.png", blob(8, 10, [grey(120), grey(200), grey(30)]))

    # orphans: one lone pixel inside a flat area, against a 2-pixel highlight
    img = blob(9, 9, [grey(60)])
    img[4][4] = grey(220)
    png("orphans_pos.png", img)
    img = blob(9, 9, [grey(60)])
    img[4][4] = grey(220)
    img[4][5] = grey(220)
    png("orphans_neg.png", img)

    # jaggies: a top edge stepping 3, 1, 3, 1, 3 (three uneven steps), against 2, 1, 2, 1, 2, 1 (a line's rhythm)
    png("jaggies_pos.png", staircase([3, 1, 3, 1, 3]))
    png("jaggies_neg.png", staircase([2, 1, 2, 1, 2, 1]))

    # doubles: three one-pixel L-corners (5-pixel shapes), against a clean diagonal and a straight line
    img = canvas(20, 7)
    for ox in (1, 7, 13):
        for k in range(3):
            img[1][ox + k] = grey(40)
            img[1 + k][ox + 2] = grey(40)
    png("doubles_pos.png", img)
    img = canvas(20, 7)
    for k in range(5):
        img[1 + k][1 + k] = grey(40)
        img[3][8 + k] = grey(40)
    png("doubles_neg.png", img)

    # holes: three pinholes in a solid block, against the solid block
    img = blob(12, 9, [grey(90)])
    for x, y in ((3, 3), (6, 5), (9, 3)):
        img[y][x] = CLEAR
    png("holes_pos.png", img)
    png("holes_neg.png", blob(12, 9, [grey(90)]))

    # flicker (4 looping frames of a 10x10 block, 100 drawn pixels): 10 pixels change in frame 2 only,
    # against the same 10 held for frames 2 and 3
    base = canvas(10, 10, grey(80))
    changed = [(x, 4) for x in range(10)]
    for name, held in (("flicker_pos", (2,)), ("flicker_neg", (2, 3))):
        for f in range(1, 5):
            img = [row[:] for row in base]
            if f in held:
                for x, y in changed:
                    img[y][x] = grey(200)
            png(f"{name}_f{f}.png", img)

    # contrast (backdrop #101018): a dark sprite, against a light one
    png("contrast_pos.png", blob(10, 10, [rgb("1a1c2c"), rgb("202030")]))
    png("contrast_neg.png", blob(10, 10, [rgb("f4f4f4"), rgb("ffcd75")]))

    # banding: three parallel 1-px ramps of 5 tones, each line one pixel right of the last
    ramp = [grey(40), grey(80), grey(120), grey(160), grey(200)]
    img = canvas(12, 7)
    for r in range(3):
        for k, c in enumerate(ramp):
            img[2 + r][2 + r + k] = c
    png("banding_pos.png", img)
    # a dithered ramp (alternating tones) on three lines: not monotone
    img = canvas(12, 5)
    for r in range(3):
        for x, v in enumerate([40, 80, 40, 80, 80, 120, 80, 120]):
            img[1 + r][1 + x] = grey(v)
    png("banding_neg.png", img)
    # a single anti-aliased corner on a solid block: no ramp on 3 lines
    img = canvas(10, 10)
    for y in range(2, 8):
        for x in range(2, 8):
            img[y][x] = grey(40)
    img[2][2] = grey(120)
    img[2][3] = grey(80)
    png("banding_corner.png", img)

    # pillow_shading: concentric rings (lighter inward), the disc lit from the top-left, a flat disc
    png("pillow_shading_pos.png", disc(lambda x, y, d: grey([0, 60, 100, 140, 180, 210, 230][min(d, 6)])))
    png("pillow_shading_neg.png", disc(lambda x, y, d: grey(max(20, 230 - (x + y) * 8))))
    png("pillow_shading_flat.png", disc(lambda x, y, d: grey(120)))
    # rings that follow the silhouette (rho high) under a strong light from the left: the rim is uneven, so not pillow
    png("pillow_shading_lit_rings.png", disc(lambda x, y, d: grey(min(255, max(0, [0, 60, 100, 140, 180, 210, 230][min(d, 6)] + (6 - x) * 9)))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
