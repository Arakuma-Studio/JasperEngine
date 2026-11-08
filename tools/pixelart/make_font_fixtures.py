#!/usr/bin/env python3
# (c) JasperDev and contributors
# SPDX-License-Identifier: MIT
"""Writes S51's bitmap font fixtures (docs/specs/S51-pixel-fonts.md AC1-AC5, AC21) into
engine/tests/core/fixtures/fonts/, from glyphs defined here (no external font):

    python3 tools/pixelart/make_font_fixtures.py [out-dir]

- grid@6x9.png: the 95 printable ASCII glyphs on a 16 x 6 grid of 6x9 cells, no gutters. Glyph i
  (codepoint 32 + i) has ink from column i % 2 to column min(4, i % 2 + (i * 7) % 5), rows
  1 + i % 3 to 6, and space is empty: its auto advance is that last column + 1 (the table of
  BitmapFontMetricsTest).
- gutter.png: the same glyphs in 6x9 cells with 1 px transparent gutters (no @WxH in the name).
- partial@6x9.png: grid@6x9.png with the top ink row of every third glyph at alpha 96.
- colored@6x9.png: the glyphs in two colours (a red top row over a white body).
- bm.fnt (text), bm_xml.fnt (XML), bm_bin.fnt (binary v3) + bm_0.png: the same glyphs cut to
  their ink as an AngelCode BMFont (size 9, lineHeight 10, base 7, two kerning pairs).
- pixel8.ttf + pixel8_bitmaps.txt: a TrueType font of 256 glyphs, unitsPerEm 800, whose outlines
  are rectangles on a 100-unit grid (8 px), drawn from the bitmaps in the .txt; U+00A4 holds a 25 %
  and a 75 % covered pixel (the threshold test) and stays off the ASCII the detection reads.

Pure standard library: PNG through zlib, TrueType tables through struct. Deterministic.
"""

import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CELL_W, CELL_H, COLS = 6, 9, 16


def png(path, width, height, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def glyph_ink(i):
    """(left, right, top, bottom) inclusive, None for space."""
    if i == 0:
        return None
    left = i % 2
    right = min(4, left + (i * 7) % 5)
    return left, right, 1 + i % 3, 6


def cell_pixels(i, variant):
    """6x9 RGBA of glyph i."""
    px = [0] * (CELL_W * CELL_H * 4)
    ink = glyph_ink(i)
    if ink is None:
        return px
    left, right, top, bottom = ink
    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            o = (y * CELL_W + x) * 4
            color = (255, 255, 255, 255)
            if variant == "partial" and y == top and i % 3 == 0:
                color = (255, 255, 255, 96)
            if variant == "colored" and y == top:
                color = (255, 40, 40, 255)
            px[o:o + 4] = color
    return px


def sheet(gap, variant):
    rows = (95 + COLS - 1) // COLS
    width = COLS * CELL_W + (COLS - 1) * gap
    height = rows * CELL_H + (rows - 1) * gap
    out = [0] * (width * height * 4)
    for i in range(95):
        cx = (i % COLS) * (CELL_W + gap)
        cy = (i // COLS) * (CELL_H + gap)
        px = cell_pixels(i, variant)
        for y in range(CELL_H):
            for x in range(CELL_W):
                o = ((cy + y) * width + cx + x) * 4
                out[o:o + 4] = px[(y * CELL_W + x) * 4:(y * CELL_W + x) * 4 + 4]
    return width, height, out


# ---- BMFont

KERNING = [(ord("A"), ord("V"), -1), (ord("T"), ord("o"), -1)]


def bmfont_chars():
    """(id, x, y, w, h, xoffset, yoffset, xadvance) on the 96 x 54 page: each glyph at its cell,
    cut to its ink."""
    chars = []
    for i in range(95):
        cx, cy = (i % COLS) * CELL_W, (i // COLS) * CELL_H
        ink = glyph_ink(i)
        if ink is None:
            chars.append((32 + i, cx, cy, 0, 0, 0, 0, 3))
            continue
        left, right, top, bottom = ink
        chars.append((32 + i, cx + left, cy + top, right - left + 1, bottom - top + 1, left, top, right + 2))
    return chars


def write_bmfont(out):
    width, height, page = sheet(0, "plain")
    png(os.path.join(out, "bm_0.png"), width, height, page)
    chars = bmfont_chars()
    common = dict(lineHeight=10, base=7, scaleW=width, scaleH=height, pages=1, packed=0)
    # text
    lines = ['info face="S51 Fixture" size=9 bold=0 italic=0 charset="" unicode=1 stretchH=100 smooth=0 aa=1 padding=0,0,0,0 spacing=1,1',
             "common lineHeight=10 base=7 scaleW=%d scaleH=%d pages=1 packed=0 alphaChnl=0 redChnl=4 greenChnl=4 blueChnl=4" % (width, height),
             'page id=0 file="bm_0.png"', "chars count=%d" % len(chars)]
    for c in chars:
        lines.append("char id=%d x=%d y=%d width=%d height=%d xoffset=%d yoffset=%d xadvance=%d page=0 chnl=15" % c)
    lines.append("kernings count=%d" % len(KERNING))
    for k in KERNING:
        lines.append("kerning first=%d second=%d amount=%d" % k)
    with open(os.path.join(out, "bm.fnt"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    # XML
    xml = ['<?xml version="1.0"?>', "<font>",
           '  <info face="S51 Fixture" size="9" bold="0" italic="0" charset="" unicode="1" stretchH="100" smooth="0" aa="1" padding="0,0,0,0" spacing="1,1"/>',
           '  <common lineHeight="10" base="7" scaleW="%d" scaleH="%d" pages="1" packed="0" alphaChnl="0" redChnl="4" greenChnl="4" blueChnl="4"/>' % (width, height),
           "  <pages>", '    <page id="0" file="bm_0.png"/>', "  </pages>", '  <chars count="%d">' % len(chars)]
    for c in chars:
        xml.append('    <char id="%d" x="%d" y="%d" width="%d" height="%d" xoffset="%d" yoffset="%d" xadvance="%d" page="0" chnl="15"/>' % c)
    xml += ["  </chars>", '  <kernings count="%d">' % len(KERNING)]
    for k in KERNING:
        xml.append('    <kerning first="%d" second="%d" amount="%d"/>' % k)
    xml += ["  </kernings>", "</font>"]
    with open(os.path.join(out, "bm_xml.fnt"), "w", newline="\n") as f:
        f.write("\n".join(xml) + "\n")
    # binary v3
    def block(kind, data):
        return struct.pack("<BI", kind, len(data)) + data
    info = struct.pack("<hBBHB", 9, 0b10000000, 0, 100, 1) + bytes([0, 0, 0, 0, 1, 1, 0]) + b"S51 Fixture\x00"
    commonb = struct.pack("<HHHHHBBBBB", 10, 7, width, height, 1, 0, 0, 4, 4, 4)
    pages = b"bm_0.png\x00"
    charsb = b"".join(struct.pack("<IHHHHhhhBB", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], 0, 15) for c in chars)
    kerns = b"".join(struct.pack("<IIh", *k) for k in KERNING)
    data = b"BMF\x03" + block(1, info) + block(2, commonb) + block(3, pages) + block(4, charsb) + block(5, kerns)
    with open(os.path.join(out, "bm_bin.fnt"), "wb") as f:
        f.write(data)


# ---- TrueType

UPEM, UNIT, ASCENT, DESCENT = 800, 100, 7, 1


def ttf_bitmap(c):
    """rows top (row 0 = 7 px above the baseline) to bottom (row 7, below it) of 5 columns."""
    if c == 32:
        return ["....."] * 8
    rows = []
    for y in range(8):
        row = ""
        for x in range(5):
            on = ((c * 31 + x * 17 + y * 13) ^ (c >> 1)) % 7 < 3
            if y == 7 and c not in (ord("g"), ord("j"), ord("p"), ord("q"), ord("y")):
                on = False
            row += "#" if on else "."
        rows.append(row)
    return rows


# 256 glyphs: printable ASCII, Latin-1 (U+00A4 aside) and U+0100-U+0141
TTF_CHARS = list(range(0x20, 0x7F)) + [c for c in range(0xA0, 0x100) if c != 0xA4] + list(range(0x100, 0x142))
HALF_CHAR = 0xA4


def rects_of(rows):
    """horizontal runs as (x0, y0, x1, y1) in font units, y up from the baseline."""
    rects = []
    for r, row in enumerate(rows):
        top = (ASCENT - r) * UNIT
        x = 0
        while x < len(row):
            if row[x] == "#":
                start = x
                while x < len(row) and row[x] == "#":
                    x += 1
                rects.append((start * UNIT, top - UNIT, x * UNIT, top))
            else:
                x += 1
    return rects


def glyf_data(rects):
    if not rects:
        return b""
    xs = [v for r in rects for v in (r[0], r[2])]
    ys = [v for r in rects for v in (r[1], r[3])]
    out = struct.pack(">hhhhh", len(rects), min(xs), min(ys), max(xs), max(ys))
    points = []
    ends = []
    for x0, y0, x1, y1 in rects:
        # clockwise in y up: left edge up, top edge right, right edge down
        points += [(x0, y0), (x0, y1), (x1, y1), (x1, y0)]
        ends.append(len(points) - 1)
    out += b"".join(struct.pack(">H", e) for e in ends) + struct.pack(">H", 0)
    out += bytes([0x01] * len(points))
    px = py = 0
    xd, yd = b"", b""
    for x, y in points:
        xd += struct.pack(">h", x - px)
        yd += struct.pack(">h", y - py)
        px, py = x, y
    out += xd + yd
    while len(out) % 4:
        out += b"\x00"
    return out


def checksum(data):
    data += b"\x00" * ((4 - len(data) % 4) % 4)
    return sum(struct.unpack(">%dI" % (len(data) // 4), data)) & 0xFFFFFFFF


def write_ttf(out):
    codes = TTF_CHARS + [HALF_CHAR]
    glyph_rects = [[(0, 0, 500, 700)]]   # .notdef: a box
    for c in TTF_CHARS:
        glyph_rects.append(rects_of(ttf_bitmap(c)))
    # U+00A4: column 0 covered 25 % (x 0..25), column 1 covered 75 % (x 100..175), row 0 above the baseline
    glyph_rects.append([(0, 0, 25, 100), (100, 0, 175, 100)])
    glyfs = [glyf_data(r) for r in glyph_rects]
    loca, offset = [], 0
    for g in glyfs:
        loca.append(offset)
        offset += len(g)
    loca.append(offset)
    n = len(glyfs)
    max_points = max([sum(4 for _ in r) for r in glyph_rects] + [0])
    max_contours = max([len(r) for r in glyph_rects] + [0])
    advance = 6 * UNIT

    tables = {}
    tables[b"head"] = struct.pack(">IIIIHHqqhhhhHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x000B, UPEM, 0, 0,
                                  0, -DESCENT * UNIT, 500, ASCENT * UNIT, 0, 8, 2, 1, 0)
    tables[b"hhea"] = struct.pack(">IhhhHhhhhhhhhhhhH", 0x00010000, ASCENT * UNIT, -DESCENT * UNIT, 0, advance, 0, 0, 500,
                                  1, 0, 0, 0, 0, 0, 0, 0, n)
    tables[b"maxp"] = struct.pack(">IHHHHHHHHHHHHHH", 0x00010000, n, max_points, max_contours, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)
    tables[b"hmtx"] = b"".join(struct.pack(">Hh", advance, 0) for _ in range(n))
    tables[b"loca"] = b"".join(struct.pack(">I", o) for o in loca)
    tables[b"glyf"] = b"".join(glyfs)
    # cmap format 4, one segment per code
    segs = sorted((c, i + 1) for i, c in enumerate(codes)) + [(0xFFFF, 0)]
    seg_count = len(segs)
    search = 2 ** (seg_count.bit_length() - 1) * 2
    sub = struct.pack(">HHHHHHH", 4, 0, 0, seg_count * 2, search, (seg_count.bit_length() - 1), seg_count * 2 - search)
    sub += b"".join(struct.pack(">H", c) for c, _ in segs) + struct.pack(">H", 0)
    sub += b"".join(struct.pack(">H", c) for c, _ in segs)
    # idDelta: glyph = code + delta (mod 65536); the closing segment maps 0xFFFF to glyph 0
    sub += b"".join(struct.pack(">H", ((g - c) & 0xFFFF) if c != 0xFFFF else 1) for c, g in segs)
    sub += b"".join(struct.pack(">H", 0) for _ in segs)
    sub = sub[:2] + struct.pack(">H", len(sub)) + sub[4:]
    tables[b"cmap"] = struct.pack(">HHHHI", 0, 1, 3, 1, 12) + sub
    # kern format 0: A V -100 (-1 px)
    index = {c: i + 1 for i, c in enumerate(codes)}
    pairs = sorted([(index[ord("A")], index[ord("V")], -UNIT)])
    kern_sub = struct.pack(">HHHHHHH", 0, 14 + 6 * len(pairs), 0x0001, len(pairs), 6, 0, 0)
    kern_sub += b"".join(struct.pack(">HHh", *p) for p in pairs)
    tables[b"kern"] = struct.pack(">HH", 0, 1) + kern_sub
    name = "S51 Pixel Test".encode("utf-16-be")
    tables[b"name"] = struct.pack(">HHH", 0, 2, 6 + 24) + struct.pack(">HHHHHH", 3, 1, 0x409, 1, len(name), 0) + \
        struct.pack(">HHHHHH", 3, 1, 0x409, 16, len(name), 0) + name
    tables[b"post"] = struct.pack(">IIhhIIIII", 0x00030000, 0, -UNIT, 50, 1, 0, 0, 0, 0)

    tags = sorted(tables)
    num = len(tags)
    entry = 2 ** (num.bit_length() - 1)
    header = struct.pack(">IHHHH", 0x00010000, num, entry * 16, num.bit_length() - 1, num * 16 - entry * 16)
    offset = 12 + 16 * num
    directory, body = b"", b""
    for tag in tags:
        data = tables[tag]
        directory += struct.pack(">4sIII", tag, checksum(data), offset + len(body), len(data))
        body += data + b"\x00" * ((4 - len(data) % 4) % 4)
    font = bytearray(header + directory + body)
    adjust = (0xB1B0AFBA - checksum(bytes(font))) & 0xFFFFFFFF
    head_offset = 12 + 16 * num + sum(len(tables[t]) + (4 - len(tables[t]) % 4) % 4 for t in tags[:tags.index(b"head")])
    font[head_offset + 8:head_offset + 12] = struct.pack(">I", adjust)
    with open(os.path.join(out, "pixel8.ttf"), "wb") as f:
        f.write(bytes(font))
    with open(os.path.join(out, "pixel8_bitmaps.txt"), "w", newline="\n") as f:
        f.write("# codepoint then 8 rows of 5 columns (row 0 = 7 px above the baseline); written by make_font_fixtures.py\n")
        for c in TTF_CHARS:
            f.write("%d %s\n" % (c, " ".join(ttf_bitmap(c))))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "engine", "tests", "core", "fixtures", "fonts")
    os.makedirs(out, exist_ok=True)
    for name, gap, variant in (("grid@6x9.png", 0, "plain"), ("gutter.png", 1, "plain"),
                               ("partial@6x9.png", 0, "partial"), ("colored@6x9.png", 0, "colored")):
        w, h, px = sheet(gap, variant)
        png(os.path.join(out, name), w, h, px)
    write_bmfont(out)
    write_ttf(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
