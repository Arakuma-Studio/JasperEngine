#!/usr/bin/env python3
"""Per-image correctness metrics of renders against a reference -> CSV rows.

    compare.py --set icons --size 48 --list corpus/icons.txt --ref resvg=refs/resvg/icons/48 \
        [--ref-straight] --out compare.csv  LABEL=RENDER_DIR ...

Every image is compared as premultiplied RGBA8 (svgbench and references.py write
premultiplied PNGs; --ref-straight converts straight-alpha references such as the resvg test
suite PNGs). Per image:
  mad      mean absolute difference over all channels and pixels (0..255)
  frac16   fraction of pixels with any channel difference > 16
  pass     frac16 < 1 %
  aa_mad   mean absolute difference over the reference's edge pixels (0 < alpha < 255)
  frac64   fraction of pixels with any channel difference > 64 (antialiasing differences stay
           below that; a render with frac64 >= 2 % is "visibly wrong", see svg-benchmark.md)
A render whose size differs from the reference is compared on the overlap; pixels outside it
count as different (like the resvg suite). A missing render counts as a failure.
The CSV is appended to (header written when the file is new).
"""
import argparse
import csv
import os

import numpy as np
from PIL import Image


def load(path, straight=False):
    a = np.asarray(Image.open(path).convert("RGBA")).astype(np.int32)
    if straight:
        al = a[:, :, 3:4]
        a = a.copy()
        a[:, :, :3] = (a[:, :, :3] * al + 127) // 255
    return a


def compare(ref, img):
    h, w = ref.shape[:2]
    total = h * w
    if img is None:
        return dict(mad=255.0, frac16=1.0, frac64=1.0, aa_mad=255.0, aa_n=0, size_ok=0)
    oh, ow = min(h, img.shape[0]), min(w, img.shape[1])
    r, i = ref[:oh, :ow], img[:oh, :ow]
    d = np.abs(r - i)
    dmax = d.max(axis=2)
    outside = total - oh * ow + (img.shape[0] * img.shape[1] - oh * ow)
    differ = int((dmax > 16).sum()) + outside
    gross = int((dmax > 64).sum()) + outside
    mad = (float(d.sum()) + outside * 4 * 255.0) / (max(total, img.shape[0] * img.shape[1]) * 4)
    edge = (r[:, :, 3] > 0) & (r[:, :, 3] < 255)
    aa_n = int(edge.sum())
    aa_mad = float(d[edge].mean()) if aa_n else 0.0
    return dict(mad=mad, frac16=differ / max(total, 1), frac64=gross / max(total, 1), aa_mad=aa_mad, aa_n=aa_n,
                size_ok=int(img.shape[:2] == ref.shape[:2]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", required=True)
    ap.add_argument("--size", required=True)
    ap.add_argument("--list", required=True)
    ap.add_argument("--ref", required=True, help="LABEL=DIR")
    ap.add_argument("--ref-straight", action="store_true")
    ap.add_argument("--out", required=True)
    ap.add_argument("renders", nargs="+")
    args = ap.parse_args()
    ref_label, ref_dir = args.ref.split("=", 1)
    names = [l.strip()[:-4] + ".png" for l in open(args.list) if l.strip() and not l.startswith("#")]
    new = not os.path.exists(args.out)
    with open(args.out, "a", newline="") as f:
        out = csv.writer(f)
        if new:
            out.writerow(["lib", "set", "size", "ref", "file", "category", "mad", "frac16", "pass", "frac64", "aa_mad", "aa_n", "size_ok"])
        for spec in args.renders:
            label, d = spec.split("=", 1)
            for n in names:
                rp = os.path.join(ref_dir, n)
                if not os.path.exists(rp):
                    continue  # no reference (the reference renderer failed): not scored
                ref = load(rp, args.ref_straight)
                ip = os.path.join(d, n)
                img = load(ip) if os.path.exists(ip) else None
                m = compare(ref, img)
                category = n.split("/")[0] if "/" in n else ""
                out.writerow([label, args.set, args.size, ref_label, n[:-4], category, f"{m['mad']:.3f}",
                              f"{m['frac16']:.5f}", int(m["frac16"] < 0.01), f"{m['frac64']:.5f}", f"{m['aa_mad']:.3f}", m["aa_n"], m["size_ok"]])


if __name__ == "__main__":
    main()
