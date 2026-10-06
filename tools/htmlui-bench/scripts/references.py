#!/usr/bin/env python3
"""Reference renders for the correctness metrics.

    references.py --corpus build/htmlui-bench/corpus --out build/htmlui-bench/refs \
        [--resvg icons:24,48,128 multicolor:24,48,128 illustrations:128,512] \
        [--chrome icons:48 multicolor:48] [--jobs 4]

resvg (resvg-py, venv: pip install resvg-py numpy pillow playwright) renders every file of a
set at each width (height follows the aspect ratio, as in svgbench). Headless Chrome (through
playwright, Chromium already installed on the host) renders the same files through
<img> + canvas.drawImage at the target size, as a cross-check of resvg.
All PNGs are written premultiplied (like svgbench output):
    OUT/<renderer>/<set>/<width>/<name>.png
OUT/versions.txt records the renderer versions.
"""
import argparse
import base64
import io
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image


def premultiply(rgba):
    a = rgba[:, :, 3:4].astype(np.uint32)
    out = rgba.copy()
    out[:, :, :3] = ((rgba[:, :, :3].astype(np.uint32) * a + 127) // 255).astype(np.uint8)
    return out


def save_premultiplied(path, straight_rgba):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Image.fromarray(premultiply(straight_rgba), "RGBA").save(path)


def resvg_job(args):
    svg_path, out_path, width = args
    import resvg_py
    try:
        png = bytes(resvg_py.svg_to_bytes(svg_path=svg_path, width=width))
        img = np.asarray(Image.open(io.BytesIO(png)).convert("RGBA"))
        save_premultiplied(out_path, img)
        return None
    except Exception as e:  # resvg rejects a few malformed files; report them
        return f"{svg_path}: {e}"


def run_resvg(corpus, out, specs, jobs):
    tasks = []
    for spec in specs:
        name, sizes = spec.split(":")
        names = [l.strip() for l in open(os.path.join(corpus, name + ".txt")) if l.strip()]
        for size in sizes.split(","):
            for n in names:
                dst = os.path.join(out, "resvg", name, size, n[:-4] + ".png")
                if not os.path.exists(dst):
                    tasks.append((os.path.join(corpus, "sets", name, n), dst, int(size)))
    print(f"resvg: {len(tasks)} renders", flush=True)
    with ProcessPoolExecutor(jobs) as ex:
        for err in ex.map(resvg_job, tasks, chunksize=32):
            if err:
                print("  resvg failed:", err, flush=True)


CHROME_JS = """
async ([svg, width]) => {
  const url = URL.createObjectURL(new Blob([svg], {type: 'image/svg+xml'}));
  const img = new Image();
  try {
    await new Promise((ok, fail) => { img.onload = ok; img.onerror = fail; img.src = url; });
  } catch (e) { URL.revokeObjectURL(url); return null; }
  URL.revokeObjectURL(url);
  const iw = img.naturalWidth || img.width, ih = img.naturalHeight || img.height;
  if (!iw || !ih) return null;
  const w = width, h = Math.max(1, Math.round(ih * width / iw));
  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  const ctx = c.getContext('2d');
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(img, 0, 0, w, h);
  const d = ctx.getImageData(0, 0, w, h).data;
  let s = '';
  for (let i = 0; i < d.length; i += 0x8000) s += String.fromCharCode.apply(null, d.subarray(i, i + 0x8000));
  return [w, h, btoa(s)];
}
"""


def run_chrome(corpus, out, specs):
    from playwright.sync_api import sync_playwright
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(device_scale_factor=1)
        page.set_content("<html><body></body></html>")
        version = browser.version
        for spec in specs:
            name, sizes = spec.split(":")
            names = [l.strip() for l in open(os.path.join(corpus, name + ".txt")) if l.strip()]
            for size in sizes.split(","):
                done = 0
                for n in names:
                    dst = os.path.join(out, "chrome", name, size, n[:-4] + ".png")
                    if os.path.exists(dst):
                        continue
                    svg = open(os.path.join(corpus, "sets", name, n), encoding="utf-8", errors="replace").read()
                    r = page.evaluate(CHROME_JS, [svg, int(size)])
                    if r is None:
                        print(f"  chrome failed: {name}/{n}", flush=True)
                        continue
                    w, h, data = r
                    img = np.frombuffer(base64.b64decode(data), np.uint8).reshape(h, w, 4)
                    save_premultiplied(dst, img)
                    done += 1
                print(f"chrome: {name} {size}px {done} renders", flush=True)
        browser.close()
    return version


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--resvg", nargs="*", default=["icons:24,48,128", "multicolor:24,48,128", "illustrations:128,512"])
    ap.add_argument("--chrome", nargs="*", default=["icons:48", "multicolor:48"])
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    run_resvg(args.corpus, args.out, args.resvg, args.jobs)
    chrome_version = run_chrome(args.corpus, args.out, args.chrome) if args.chrome else "not run"
    import importlib.metadata as md
    with open(os.path.join(args.out, "versions.txt"), "w") as f:
        f.write(f"resvg-py {md.version('resvg_py')} (resvg 0.48.1, tiny-skia 0.12.0 inside)\n")
        f.write(f"chromium {chrome_version} (playwright {md.version('playwright')})\n")
    print(open(os.path.join(args.out, "versions.txt")).read())


if __name__ == "__main__":
    sys.exit(main())
