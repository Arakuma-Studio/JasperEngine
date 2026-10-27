#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# Regenerates the golden fixtures of jasperengine-spritenormals-tests (README.md beside this file).
#
#   python3 -I make_fixtures.py <upstream binary> [<directory of the evaluation sprites>]
#
# 1. Writes the synthetic inputs (the upstream tests' images, the tiny shapes, the two-frame APNG)
#    into inputs/, with the standard library only.
# 2. Copies the evaluation sprites (ball.png ... walk.gif) into inputs/ when their directory is
#    given (they are committed: without it the committed copies are used).
# 3. Runs the upstream spritefusion-sprite-normals binary (commit d14b067) on every input with
#    the command lines of FIXTURES, writing expected/<name>_n.png (and, for animations,
#    expected/<name>_n.color.png and expected/<name>_n.frames.json).

import math
import os
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
INPUTS = os.path.join(HERE, "inputs")
EXPECTED = os.path.join(HERE, "expected")

# name -> (input file, upstream arguments after the input and -o)
FIXTURES = [
    ("ball", "ball.png", []),
    ("highlight", "highlight.png", []),
    ("donut", "donut.png", []),
    ("character", "character.png", []),
    ("thin", "thin.png", []),
    ("flat", "flat.png", []),
    ("flat_outline", "flat_outline.png", []),
    ("flat2", "flat2.png", []),
    ("alpha", "alpha.png", []),
    ("sheet", "sheet.png", ["--frame-size", "32x32"]),
    ("tiles", "tiles.png", []),
    ("walk", "walk.gif", []),
    ("spin", "spin.gif", []),
    ("twoframes", "twoframes.png", []),
    ("character_variant", "character.png", ["--normal-strength", "2.5", "--detail-strength", "4", "--detail-scale", "1",
                                            "--shape-strength", "2", "--shape-radius", "8", "--shape-softness", "0.5"]),
    ("ball_variant", "ball.png", ["--normal-strength", "2.5", "--detail-strength", "4", "--detail-scale", "1",
                                  "--shape-strength", "2", "--shape-radius", "8", "--shape-softness", "0.5"]),
    ("character_directx", "character.png", ["--normal-format", "directx"]),
    ("ball_directx", "ball.png", ["--normal-format", "directx"]),
    ("tiny_1x1", "tiny_1x1.png", []),
    ("tiny_1x19", "tiny_1x19.png", []),
    ("tiny_23x1", "tiny_23x1.png", []),
    ("tiny_diagonal", "tiny_diagonal.png", []),
    ("disk", "disk.png", []),
    ("gradient_alpha", "gradient_alpha.png", []),
    ("ramp", "ramp.png", ["--shape-strength", "0"]),
    ("sprite9", "sprite9.png", []),
    ("islands_sheet", "islands_sheet.png", []),
]

EVALUATION_SPRITES = ["ball.png", "highlight.png", "donut.png", "character.png", "thin.png", "flat.png",
                      "flat_outline.png", "flat2.png", "alpha.png", "sheet.png", "tiles.png", "walk.gif", "spin.gif"]


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def rgba_rows(width, height, pixels):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(pixels[y * width + x])
    return bytes(raw)


def png(width, height, pixels):
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(rgba_rows(width, height, pixels), 9)) + chunk(b"IEND", b""))


def image(width, height, function):
    return png(width, height, [function(x, y) for y in range(height) for x in range(width)])


def twoframes_apng():
    # 12x10: frame 1 the whole canvas orange for 1/12 s (the default image), frame 2 a 3x2 blue patch
    # at (4, 3) for 1/20 s, blended over; no dispose (upstream tests/media.rs)
    width, height = 12, 10
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    out += chunk(b"acTL", struct.pack(">II", 2, 0))
    out += chunk(b"fcTL", struct.pack(">IIIIIHHBB", 0, width, height, 0, 0, 1, 12, 0, 1))
    out += chunk(b"IDAT", zlib.compress(rgba_rows(width, height, [(200, 100, 50, 255)] * (width * height)), 9))
    out += chunk(b"fcTL", struct.pack(">IIIIIHHBB", 1, 3, 2, 4, 3, 1, 20, 0, 1))
    out += chunk(b"fdAT", struct.pack(">I", 2) + zlib.compress(rgba_rows(3, 2, [(50, 100, 200, 255)] * 6), 9))
    return out + chunk(b"IEND", b"")


def disk_pixel(x, y):
    # upstream tests/generation.rs disk(31)
    center = 15.0
    return (145, 99, 63, 255 if math.hypot(x - center, y - center) <= center - 3.0 else 0)


def islands_sheet():
    # upstream padding_and_nearby_islands_do_not_change_a_sprite: the 9x9 sprite at (5, 5) and a
    # white 21x23 block at (15, 5) on a transparent 42x34 sheet
    def pixel(x, y):
        if 5 <= x < 14 and 5 <= y < 14:
            return (30 + (x - 5) * 20, 80, 120, 255)
        if 15 <= x < 36 and 5 <= y < 28:
            return (255, 255, 255, 255)
        return (0, 0, 0, 0)
    return image(42, 34, pixel)


SYNTHETIC = {
    "twoframes.png": twoframes_apng,
    "tiny_1x1.png": lambda: image(1, 1, lambda x, y: (80, 80, 80, 255)),
    "tiny_1x19.png": lambda: image(1, 19, lambda x, y: (80, 80, 80, 255)),
    "tiny_23x1.png": lambda: image(23, 1, lambda x, y: (80, 80, 80, 255)),
    "tiny_diagonal.png": lambda: image(20, 20, lambda x, y: (80, 80, 80, 255 if x == y else 0)),
    "disk.png": lambda: image(31, 31, disk_pixel),
    "gradient_alpha.png": lambda: image(16, 16, lambda x, y: (90, 160, 210, y * 16 + x)),
    "ramp.png": lambda: image(64, 32, lambda x, y: (40 + 3 * x, 40 + 3 * x, 40 + 3 * x, 255)),
    "sprite9.png": lambda: image(9, 9, lambda x, y: (30 + x * 20, 80, 120, 255)),
    "islands_sheet.png": islands_sheet,
}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__ or "usage: make_fixtures.py <upstream binary> [<evaluation sprites>]")
    binary = os.path.abspath(sys.argv[1])
    os.makedirs(INPUTS, exist_ok=True)
    os.makedirs(EXPECTED, exist_ok=True)
    if len(sys.argv) > 2:
        for name in EVALUATION_SPRITES:
            shutil.copyfile(os.path.join(sys.argv[2], name), os.path.join(INPUTS, name))
    for name, make in SYNTHETIC.items():
        with open(os.path.join(INPUTS, name), "wb") as f:
            f.write(make())
    for name, source, arguments in FIXTURES:
        output = os.path.join(EXPECTED, name + "_n.png")
        command = [binary, os.path.join(INPUTS, source), "-o", output, "--force"] + arguments
        subprocess.run(command, check=True, stdin=subprocess.DEVNULL)
        print(name + ": " + " ".join([os.path.basename(binary), "inputs/" + source, "-o", "expected/" + name + "_n.png",
                                      "--force"] + arguments))


if __name__ == "__main__":
    main()
