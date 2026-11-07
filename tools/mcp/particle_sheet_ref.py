"""Independent reference for the particle sprite-sheet bake (docs/specs/S91-effect-authoring.md R12, AC10):
pure Python over pngdiff.decode, written from the definitions, not from the editor's code.

- A sheet cell holds straight alpha: compositing it over a background with "over"
  (c * a + bg * (1 - a)) must give what the effect looks like drawn on that background.
- An alpha-blended particle of colour (r, g, b) and alpha A shows, where it covers alone,
  alpha A and colour (r, g, b) in the cell; where nothing is drawn, alpha 0.
"""

from __future__ import annotations


def cell(image, columns: int, rows: int, index: int):
    """(width, height, rgba) of cell `index` of a sheet, row by row from the top left."""
    width, height, pixels = image
    w, h = width // columns, height // rows
    x0, y0 = (index % columns) * w, (index // columns) * h
    out = bytearray()
    for y in range(y0, y0 + h):
        start = (y * width + x0) * 4
        out += pixels[start:start + w * 4]
    return w, h, out


def over(image, background: tuple[int, int, int]):
    """A straight-alpha image composited over a solid background, opaque."""
    width, height, pixels = image
    out = bytearray(len(pixels))
    for i in range(0, len(pixels), 4):
        a = pixels[i + 3]
        for k in range(3):
            out[i + k] = min(255, (pixels[i + k] * a + background[k] * (255 - a) + 127) // 255)
        out[i + 3] = 255
    return width, height, out


def max_difference(a, b) -> int:
    """The largest channel difference of two images of one size (alpha left out)."""
    assert a[0] == b[0] and a[1] == b[1], f"sizes {a[:2]} and {b[:2]}"
    worst = 0
    for i in range(0, len(a[2]), 4):
        for k in range(3):
            worst = max(worst, abs(a[2][i + k] - b[2][i + k]))
    return worst


def coverage(image) -> tuple[int, list[int], list[tuple[int, int, int]]]:
    """(pixels with alpha 0, the alphas of the others, their colours)."""
    _, _, pixels = image
    empty, alphas, colours = 0, [], []
    for i in range(0, len(pixels), 4):
        if pixels[i + 3] == 0:
            empty += 1
        else:
            alphas.append(pixels[i + 3])
            colours.append((pixels[i], pixels[i + 1], pixels[i + 2]))
    return empty, alphas, colours
