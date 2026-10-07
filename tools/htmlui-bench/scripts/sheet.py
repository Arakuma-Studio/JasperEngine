#!/usr/bin/env python3
"""Contact sheet for looking at renders side by side (premultiplied PNGs composited on white).

    sheet.py --out sheet.png [--cell 96] [--names a.png,b.png | --list names.txt] LABEL=DIR ...

One row per image name, one column per LABEL=DIR. svgbench writes premultiplied RGBA into its
PNGs; references.py writes premultiplied too, so every column is composited the same way.
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageDraw


def load(path, cell):
    if not os.path.exists(path):
        img = np.zeros((cell, cell, 3), np.uint8)
        img[:, :, 0] = 255  # missing: solid red
        return Image.fromarray(img)
    a = np.asarray(Image.open(path).convert("RGBA")).astype(np.float32)
    rgb = a[:, :, :3] + (255.0 - a[:, :, 3:4])  # premultiplied over white
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
    img.thumbnail((cell, cell), Image.NEAREST if max(img.size) <= cell else Image.LANCZOS)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--cell", type=int, default=96)
    ap.add_argument("--names")
    ap.add_argument("--list")
    ap.add_argument("--zoom", type=int, default=1, help="nearest-neighbour upscale of small images")
    ap.add_argument("columns", nargs="+")
    args = ap.parse_args()
    cols = [c.split("=", 1) for c in args.columns]
    if args.names:
        names = args.names.split(",")
    elif args.list:
        names = [l.strip() for l in open(args.list) if l.strip()]
    else:
        names = sorted(os.listdir(cols[0][1]))
    cell, label_w, pad = args.cell, 220, 4
    sheet = Image.new("RGB", (label_w + len(cols) * (cell + pad), 20 + len(names) * (cell + pad)), (200, 200, 200))
    draw = ImageDraw.Draw(sheet)
    for ci, (label, _) in enumerate(cols):
        draw.text((label_w + ci * (cell + pad), 4), label, fill=(0, 0, 0))
    for ri, name in enumerate(names):
        y = 20 + ri * (cell + pad)
        draw.text((2, y + cell // 2 - 6), name[-34:], fill=(0, 0, 0))
        for ci, (_, d) in enumerate(cols):
            img = load(os.path.join(d, name), cell * 4)
            if args.zoom > 1:
                img = img.resize((img.width * args.zoom, img.height * args.zoom), Image.NEAREST)
            img.thumbnail((cell, cell), Image.LANCZOS)
            sheet.paste(img, (label_w + ci * (cell + pad), y))
    sheet.save(args.out)


if __name__ == "__main__":
    main()
