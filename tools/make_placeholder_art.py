#!/usr/bin/env python3
"""Generate the placeholder JasperEngine icons and logos.

Draws a white "J" on a jasper-red rounded square and writes every icon and logo
file the editor and the sample project use. Replace the art by editing draw_mark()
(or loading a real master image there) and re-running.

Needs Pillow, e.g.:
    python3 -m venv /tmp/art && /tmp/art/bin/pip install pillow
    /tmp/art/bin/python tools/make_placeholder_art.py
"""

import io
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
ICONS = ROOT / "editor/resources/icons"
FONT_HEADER = ROOT / "editor/resources/fonts/roboto-v20-latin-regular_ttf.h"

JASPER_RED = (181, 69, 47, 255)
WHITE = (255, 255, 255, 255)
WORDMARK = "JASPERENGINE"


def font(size):
    # The editor already embeds Roboto as a C array; reuse it rather than
    # depending on fonts installed on the machine.
    data = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", FONT_HEADER.read_text()))
    return ImageFont.truetype(io.BytesIO(data), size)


def draw_mark(size):
    """The square app mark, drawn large and downsampled for clean edges."""
    big = size * 4 if size < 512 else size
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = big * 0.06
    d.rounded_rectangle([pad, pad, big - pad, big - pad], radius=big * 0.2, fill=JASPER_RED)
    f = font(int(big * 0.62))
    stroke = max(1, int(big * 0.025))
    d.text((big / 2, big / 2), "J", font=f, fill=WHITE, anchor="mm", stroke_width=stroke, stroke_fill=WHITE)
    return img.resize((size, size), Image.LANCZOS) if big != size else img


def wordmark_logo(width, height, text_color):
    """Mark above the product name, as in the original loading/sample logos."""
    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    text_h = int(height * 0.16)
    mark = draw_mark(min(width, height - text_h * 2))
    img.alpha_composite(mark, ((width - mark.width) // 2, 0))
    size = text_h
    while font(size).getlength(WORDMARK) > width * 0.96:
        size -= 1
    d = ImageDraw.Draw(img)
    d.text((width / 2, height - text_h * 0.6), WORDMARK, font=font(size), fill=text_color, anchor="mm")
    return img


def write_c_header(png_path, header_path, symbol):
    data = png_path.read_bytes()
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 12]) for i in range(0, len(data), 12)]
    header_path.write_text(
        "// (c) Eduardo Doria and contributors\n"
        "// SPDX-License-Identifier: MIT\n\n"
        "#pragma once\n\n"
        f"inline unsigned char {symbol}[] = {{\n  " + ",\n  ".join(rows) + "\n};\n"
        f"inline unsigned int {symbol}_len = {len(data)};\n"
    )


def main():
    master = draw_mark(2048)
    master.save(ICONS / "jasperengine-editor.png")

    for s in (16, 32, 48, 64, 128, 256, 512):
        draw_mark(s).save(ICONS / f"linux/jasperengine-editor-{s}.png")

    ico_sizes = [(s, s) for s in (16, 24, 32, 48, 64, 128, 256)]
    for name in ("jasperengine-editor.ico", "jasperengine-editor-cmd.ico"):
        draw_mark(256).save(ICONS / name, sizes=ico_sizes)

    # Loading/About logo: white text, shown on the editor's dark background.
    logo = ICONS / "jasperengine-logo.png"
    wordmark_logo(256, 270, WHITE).save(logo, optimize=True)
    write_c_header(logo, ICONS / "jasperengine-logo_png.h", "jasperengine_logo_png")
    logo.unlink()

    # Image shown by the default project template.
    wordmark_logo(413, 260, JASPER_RED).save(ROOT / "engine/project/assets/jasperengine.png", optimize=True)


if __name__ == "__main__":
    main()
