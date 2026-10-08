"""Pixels of HTML UI screenshots for the MCP tests: a PNG decoder (8-bit RGB/RGBA, no
dependencies), a screenshot addressed in canvas pixels, color checks. Shared by the phase-1
HTML UI tests (test_html_ui_editor.py, test_html_ui_reload.py, test_html_ui_export.py).
"""

import struct
import zlib

from harness import Editor


def decode_png(data: bytes):
    """(width, height, channels, rows) of an 8-bit RGB/RGBA PNG."""
    assert data.startswith(b"\x89PNG\r\n\x1a\n"), "not a PNG"
    pos, width, height, color_type, idat = 8, 0, 0, 2, b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind, chunk = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color_type = struct.unpack(">IIBB", chunk[:10])
            assert depth == 8, f"bit depth {depth}"
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + length
    ch = {2: 3, 6: 4}[color_type]
    raw, stride, rows, prev, i = zlib.decompress(idat), width * ch, [], bytearray(width * ch), 0
    for _ in range(height):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
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
    return width, height, ch, rows


class Shot:
    """A Game view screenshot addressed in canvas pixels."""

    def __init__(self, png: bytes, info: dict):
        self.png = png
        self.w, self.h, self.ch, self.rows = decode_png(png)
        rect, canvas = info["canvas_rect_in_image"], info["canvas"]
        self.rx, self.ry = rect["x"], rect["y"]
        self.sx, self.sy = rect["width"] / canvas["width"], rect["height"] / canvas["height"]

    def px(self, cx: float, cy: float) -> tuple:
        x = min(self.w - 1, max(0, int(self.rx + (cx + 0.5) * self.sx)))
        y = min(self.h - 1, max(0, int(self.ry + (cy + 0.5) * self.sy)))
        i = x * self.ch
        return tuple(self.rows[y][i:i + 3])

    def region(self, x0, y0, x1, y1):
        """Image pixels of a canvas rectangle."""
        ix0, iy0 = int(self.rx + x0 * self.sx), int(self.ry + y0 * self.sy)
        ix1, iy1 = int(self.rx + x1 * self.sx), int(self.ry + y1 * self.sy)
        for y in range(max(0, iy0), min(self.h, iy1)):
            row = self.rows[y]
            for x in range(max(0, ix0), min(self.w, ix1)):
                yield tuple(row[x * self.ch:x * self.ch + 3])


def near(a, b, tol=12) -> bool:
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def check_color(ed: Editor, shot: Shot, point, want, what: str, tol=12):
    got = shot.px(*point)
    ed.check(near(got, want, tol), f"{what}: pixel at canvas {point} is {got}, expected {want} +-{tol}")




def count_near(png: bytes, color, tol: int = 30, step: int = 4) -> int:
    """Samples (every `step` pixels) of a whole image near `color`."""
    w, h, ch, rows = decode_png(png)
    return sum(1 for y in range(0, h, step) for x in range(0, w, step)
               if near(tuple(rows[y][x * ch:x * ch + 3]), color, tol))
