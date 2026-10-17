#!/usr/bin/env python3
# Writes engine/tests/htmlui/effects/testcard.png: the 160 x 100 image of the effects' test card
# (phase-5 spec 5.2, filter.html and backdrop.html). It is asymmetric on both axes, so a filter
# that flips or offsets its source (a wrong orientation on a backend, a half-pixel shift) shows:
# a horizontal hue ramp, a vertical brightness ramp, a white "F" in the top-left corner, a red
# dot near the top-right, a one-pixel checker strip along the bottom and a dark diagonal line.
#
#   python3 tools/htmlui/gen_testcard.py [--check]
#
# --check exits 1 when the file differs from what the script writes (nothing is written).

import math
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUTPUT = os.path.join(ROOT, "engine", "tests", "htmlui", "effects", "testcard.png")
WIDTH, HEIGHT = 160, 100


def hsv(h, s, v):
    i = int(h * 6.0) % 6
    f = h * 6.0 - math.floor(h * 6.0)
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    return [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]


def in_f(x, y):
    # an "F" in the top-left corner: a stem and two bars
    if 8 <= x < 14 and 8 <= y < 44:
        return True
    if 8 <= x < 32 and 8 <= y < 14:
        return True
    if 8 <= x < 26 and 23 <= y < 29:
        return True
    return False


def pixel(x, y):
    r, g, b = hsv(x / WIDTH, 0.65, 0.35 + 0.6 * (1.0 - y / HEIGHT))
    if in_f(x, y):
        r, g, b = 1.0, 1.0, 1.0
    if (x - 140) ** 2 + (y - 18) ** 2 <= 64:
        r, g, b = 1.0, 0.1, 0.1
    if y >= HEIGHT - 4:
        c = 1.0 if (x + y) % 2 == 0 else 0.0
        r, g, b = c, c, c
    if abs((x - 40) - (y - 20) * 1.5) < 1.0 and 30 <= y < 90:
        r, g, b = 0.05, 0.05, 0.08
    return [int(round(max(0.0, min(1.0, c)) * 255)) for c in (r, g, b)] + [255]


def png(width, height, rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    rows = []
    for y in range(HEIGHT):
        row = []
        for x in range(WIDTH):
            row.extend(pixel(x, y))
        rows.append(row)
    data = png(WIDTH, HEIGHT, rows)
    current = open(OUTPUT, "rb").read() if os.path.exists(OUTPUT) else None
    if "--check" in sys.argv[1:]:
        if current != data:
            print("%s is stale: run tools/htmlui/gen_testcard.py" % os.path.relpath(OUTPUT, ROOT))
            sys.exit(1)
        print("testcard.png is current")
        return
    if current != data:
        with open(OUTPUT, "wb") as f:
            f.write(data)
        print("wrote %s" % os.path.relpath(OUTPUT, ROOT))
    else:
        print("testcard.png is current")


if __name__ == "__main__":
    main()
