"""The lint catalogue of S46 (docs/specs/S46-pixel-art-cleanup.md R26, R27) in numpy, written from
the spec's text and never from the C++: the tests' oracle for lint_pixel_art (AC14).

Run with `uv run --no-project --with numpy python tools/mcp/pixel_lint_ref.py` and one JSON request
on stdin; the answer is one JSON line on stdout, {"<id>": {"measured": .., "count": .., "status": ..}}:

    {"frames": [<png path> | {"width": w, "height": h, "rgba": [r, g, b, a, ...]}, ...],
     "palette": ["#rrggbb", ...],              (optional; off_palette SKIPs without it)
     "backdrops": ["#rrggbb", ...],            (optional; contrast SKIPs without them)
     "loop": true, "sheet": false,
     "rules": {"max_colors": 16, "sizes": [[32, 32]], "transparent_background": true},
     "animations": [{"name": "walk", "frames": [0, 1]}],
     "frame_cells": [8.0, ...], "shared_cell": 8.0}

The interpretation choices where the spec leaves room:
- "drawn" is alpha > 0 and "opaque" (off_palette, banding, transparent_background, pillow_shading) is alpha 255;
- used colours are the distinct RGB of the drawn pixels, of every frame together;
- orphans counts an empty neighbour as a colour of its own (ritmo's TRANSPARENT index);
- flicker's share is over the pixels drawn in any frame, a blinking pixel counted once;
- jaggies counts one spot per chain and middle run, so a run judged in two chains counts twice;
- a ramp run is a maximal window of runs (a peak's run belongs to both slopes); along a diagonal the
  boundary position is the x coordinate; measured is the distinct pixels of every band group of the
  frame, count the groups;
- pillow_shading's edge pixels are d = 1 and the quadrants split at the centroid, x < cx and y < cy;
  measured is the rho of the worst body (the highest of the pillow bodies, else of all judged bodies);
  count is 3 per pillow body (its centroid and the two extreme rings);
- a lint with no qualifying input SKIPs with measured 0 and count 0."""

from __future__ import annotations

import json
import struct
import sys
import zlib

import numpy as np


# ---- input


def _read_png(path: str) -> np.ndarray:
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    at = 8
    width = height = depth = color = 0
    idat = bytearray()
    plte = trns = b""
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at : at + 8])
        body = data[at + 8 : at + 8 + length]
        at += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or interlace != 0:
                raise ValueError(f"{path}: only 8-bit, non-interlaced PNGs")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"PLTE":
            plte = body
        elif kind == b"tRNS":
            trns = body
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows = np.zeros((height, stride), dtype=np.uint8)
    previous = np.zeros(stride, dtype=np.int32)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        line = np.frombuffer(raw, dtype=np.uint8, count=stride, offset=start + 1).astype(np.int32)
        out = np.zeros(stride, dtype=np.int32)
        if kind == 0:
            out = line
        elif kind == 2:
            out = (line + previous) & 255
        else:
            for x in range(stride):
                a = out[x - channels] if x >= channels else 0
                b = previous[x]
                c = previous[x - channels] if x >= channels else 0
                if kind == 1:
                    predicted = a
                elif kind == 3:
                    predicted = (a + b) // 2
                else:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    predicted = a if pa <= pb and pa <= pc else b if pb <= pc else c
                out[x] = (line[x] + predicted) & 255
        rows[y] = out
        previous = out
    if color == 6:
        return rows.reshape(height, width, 4)
    image = np.zeros((height, width, 4), dtype=np.uint8)
    if color == 2:
        image[..., :3] = rows.reshape(height, width, 3)
        image[..., 3] = 255
    elif color == 0:
        image[..., :3] = rows[..., None]
        image[..., 3] = 255
    elif color == 4:
        pixels = rows.reshape(height, width, 2)
        image[..., :3] = pixels[..., 0, None]
        image[..., 3] = pixels[..., 1]
    else:
        table = np.frombuffer(plte, dtype=np.uint8).reshape(-1, 3)
        alpha = np.full(len(table), 255, dtype=np.uint8)
        alpha[: len(trns)] = np.frombuffer(trns, dtype=np.uint8)
        image[..., :3] = table[rows]
        image[..., 3] = alpha[rows]
    return image


def _frame(spec) -> np.ndarray:
    if isinstance(spec, str):
        return _read_png(spec)
    return np.array(spec["rgba"], dtype=np.uint8).reshape(spec["height"], spec["width"], 4)


def _hex(text: str) -> tuple[int, int, int]:
    text = text.lstrip("#")
    return int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16)


# ---- colour (OKLab of sRGB, Ottosson's matrices)


def _oklab(rgb: np.ndarray) -> np.ndarray:
    c = rgb.astype(np.float64) / 255.0
    linear = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    r, g, b = linear[..., 0], linear[..., 1], linear[..., 2]
    l = np.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    m = np.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    s = np.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    return np.stack(
        [
            0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
        ],
        axis=-1,
    )


def _packed(frame: np.ndarray) -> np.ndarray:
    return (frame[..., 0].astype(np.uint32) << 16) | (frame[..., 1].astype(np.uint32) << 8) | frame[..., 2].astype(np.uint32)


def _used(frames: list[np.ndarray]) -> np.ndarray:
    keys = [_packed(f)[f[..., 3] > 0] for f in frames]
    return np.unique(np.concatenate(keys)) if keys else np.zeros(0, dtype=np.uint32)


def _unpacked(keys: np.ndarray) -> np.ndarray:
    return np.stack([(keys >> 16) & 255, (keys >> 8) & 255, keys & 255], axis=-1).astype(np.uint8)


def _result(measured, count, status) -> dict:
    return {"measured": float(measured), "count": int(count), "status": status}


SKIP = _result(0, 0, "SKIP")


# ---- rule lints


def off_palette(frames, palette):
    if not palette:
        return SKIP
    allowed = np.array([(r << 16) | (g << 8) | b for r, g, b in palette], dtype=np.uint32)
    count = sum(int(np.count_nonzero(~np.isin(_packed(f)[f[..., 3] == 255], allowed))) for f in frames)
    return _result(count, count, "FAIL" if count else "PASS")


def colors(frames, maximum):
    if maximum is None:
        return SKIP
    used = len(_used(frames))
    return _result(used, used, "FAIL" if used > maximum else "PASS")


def partial_alpha(frames):
    count = sum(int(np.count_nonzero((f[..., 3] > 0) & (f[..., 3] < 255))) for f in frames)
    return _result(count, count, "FAIL" if count else "PASS")


def transparent_background(frames, required):
    if not required:
        return SKIP
    worst, spots, failed = -1, 0, False
    for f in frames:
        solid = f[..., 3] == 255
        ring = np.ones(solid.shape, dtype=bool)
        ring[1:-1, 1:-1] = False
        opaque = int(np.count_nonzero(solid & ring))
        corners = int(solid[0, 0]) + int(solid[0, -1]) + int(solid[-1, 0]) + int(solid[-1, -1])
        worst = max(worst, opaque)
        if corners >= 3 and opaque * 2 > int(np.count_nonzero(ring)):
            failed = True
            spots += opaque
    return _result(worst, spots if failed else 0, "FAIL" if failed else "PASS")


def size(frames, sizes):
    if not sizes:
        return SKIP
    allowed = {tuple(s) for s in sizes}
    count = sum(1 for f in frames if (f.shape[1], f.shape[0]) not in allowed)
    return _result(count, count, "FAIL" if count else "PASS")


# ---- craft lints


def near_duplicates(frames):
    used = _used(frames)
    # more than 1,024 colours is not pixel art yet: the C++ SKIPs rather than pair a photo's colours
    if len(used) < 2 or len(used) > 1024:
        return SKIP
    labs = _oklab(_unpacked(used))
    distances = np.linalg.norm(labs[:, None, :] - labs[None, :, :], axis=-1)
    upper = distances[np.triu_indices(len(used), 1)]
    pairs = int(np.count_nonzero(upper < 0.03))
    return _result(float(upper.min()), pairs, "WARN" if pairs else "PASS")


def orphans(frames, sheet):
    if sheet:
        return SKIP
    count = 0
    for f in frames:
        h, w = f.shape[:2]
        keys = _packed(f).astype(np.int64) + 1
        keys[f[..., 3] == 0] = 0
        # the 8 neighbours of every pixel; -1 off the picture
        padded = np.full((h + 2, w + 2), -1, dtype=np.int64)
        padded[1:-1, 1:-1] = keys
        for y, x in zip(*np.nonzero(keys)):
            self_key = padded[y + 1, x + 1]
            window = padded[y : y + 3, x : x + 3].copy()
            window[1, 1] = -1
            if self_key in (window[0, 1], window[2, 1], window[1, 0], window[1, 2]):
                continue
            around = window[(window != -1) & (window != self_key)]
            if around.size and np.unique(around, return_counts=True)[1].max() >= 5:
                count += 1
    return _result(count, count, "WARN" if count >= 1 else "PASS")


def _top_edge_runs(mask: np.ndarray) -> list[tuple[int, int, int]]:
    above = np.vstack([np.zeros((1, mask.shape[1]), dtype=bool), mask[:-1]])
    edge = mask & ~above
    runs = []
    for y in range(mask.shape[0]):
        padded = np.concatenate([[0], edge[y].astype(np.int8), [0]])
        steps = np.diff(padded)
        for a, b in zip(np.nonzero(steps == 1)[0], np.nonzero(steps == -1)[0]):
            runs.append((int(y), int(a), int(b) - 1))
    return runs


def _jaggy_spots(mask: np.ndarray) -> int:
    runs = _top_edge_runs(mask)
    starting = {(x0, y): (y, x0, x1) for y, x0, x1 in runs}
    ending = {(x1, y) for y, x0, x1 in runs}
    spots = 0
    for run in runs:
        for step in (1, -1):
            y, x0, _ = run
            if (x0 - 1, y - step) in ending:
                continue
            chain = [run]
            while (chain[-1][2] + 1, chain[-1][0] + step) in starting:
                chain.append(starting[(chain[-1][2] + 1, chain[-1][0] + step)])
            lengths = [c[2] - c[1] + 1 for c in chain]
            for a, b, c in zip(lengths, lengths[1:], lengths[2:]):
                if b <= min(a, c) - 2 or b >= max(a, c) + 2:
                    spots += 1
    return spots


def jaggies(frames, threshold=3):
    count = sum(_jaggy_spots(np.rot90(f[..., 3] > 0, k)) for f in frames for k in range(4))
    return _result(count, count, "WARN" if count >= threshold else "PASS")


def _components(mask: np.ndarray, diagonal: bool) -> list[list[tuple[int, int]]]:
    h, w = mask.shape
    seen = np.zeros(mask.shape, dtype=bool)
    steps = [(1, 0), (-1, 0), (0, 1), (0, -1)]
    if diagonal:
        steps += [(1, 1), (1, -1), (-1, 1), (-1, -1)]
    found = []
    for y0, x0 in zip(*np.nonzero(mask)):
        if seen[y0, x0]:
            continue
        seen[y0, x0] = True
        stack = [(int(y0), int(x0))]
        shape = []
        while stack:
            y, x = stack.pop()
            shape.append((y, x))
            for dy, dx in steps:
                ny, nx = y + dy, x + dx
                if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    stack.append((ny, nx))
        found.append(shape)
    return found


def doubles(frames, threshold=3):
    count = 0
    for f in frames:
        mask = f[..., 3] > 0
        h, w = mask.shape
        at = lambda y, x: 0 <= y < h and 0 <= x < w and bool(mask[y, x])
        for shape in _components(mask, True):
            if len(shape) < 5:
                continue
            for y, x in shape:
                neighbours = [(dy, dx) for dy in (-1, 0, 1) for dx in (-1, 0, 1) if (dy or dx) and at(y + dy, x + dx)]
                if len(neighbours) == 2 and sum(1 for dy, dx in neighbours if dy == 0) == 1 and sum(1 for dy, dx in neighbours if dx == 0) == 1:
                    count += 1
    return _result(count, count, "WARN" if count >= threshold else "PASS")


def holes(frames, threshold=3):
    count = 0
    for f in frames:
        m = f[..., 3] > 0
        if m.shape[0] >= 3 and m.shape[1] >= 3:
            count += int(np.count_nonzero(~m[1:-1, 1:-1] & m[:-2, 1:-1] & m[2:, 1:-1] & m[1:-1, :-2] & m[1:-1, 2:]))
    return _result(count, count, "WARN" if count >= threshold else "PASS")


def flicker(frames, loop):
    n = len(frames)
    if n < 3:
        return SKIP
    # a clear pixel has no colour: the RGB it keeps is neither a change nor sameness
    frames = [np.where(f[..., 3:] == 0, 0, f) for f in frames]
    any_drawn = np.any(np.stack([f[..., 3] > 0 for f in frames]), axis=0)
    blinking = np.zeros(any_drawn.shape, dtype=bool)
    for i in range(n):
        if not loop and i in (0, n - 1):
            continue
        before, now, after = frames[(i - 1) % n], frames[i], frames[(i + 1) % n]
        blinking |= np.any(now != before, axis=-1) & np.all(after == before, axis=-1)
    blinking &= any_drawn
    lit = int(np.count_nonzero(any_drawn))
    count = int(np.count_nonzero(blinking))
    share = count / lit if lit else 0.0
    return _result(share, count, "WARN" if share > 0.06 else "PASS")


def contrast(frames, backdrops):
    if not backdrops:
        return SKIP
    drawn = np.concatenate([f[f[..., 3] > 0][..., :3] for f in frames])
    labs = _oklab(drawn) if len(drawn) else np.zeros((0, 3))
    worst, worst_count = -1.0, 0
    for backdrop in backdrops:
        back = _oklab(np.array(backdrop, dtype=np.uint8))
        faint = int(np.count_nonzero(np.linalg.norm(labs - back, axis=-1) < 0.12))
        share = faint / len(labs) if len(labs) else 0.0
        if share > worst:
            worst, worst_count = share, faint
    return _result(worst, worst_count, "WARN" if worst > 0.5 else "PASS")


def _scanlines(h: int, w: int):
    """(family, line index, [(position, y, x), ...]) of every row, column and diagonal; the line
    index counts adjacent parallel lines, the position is what a boundary shift is measured in"""
    for y in range(h):
        yield 0, y, [(x, y, x) for x in range(w)]
    for x in range(w):
        yield 1, x, [(y, y, x) for y in range(h)]
    for d in range(-(h - 1), w):
        yield 2, d, [(x, x - d, x) for x in range(max(0, d), min(w, h + d))]
    for s in range(w + h - 1):
        yield 3, s, [(x, s - x, x) for x in range(max(0, s - h + 1), min(w, s + 1))]


def _ramps(colors_of: np.ndarray, light: np.ndarray, line) -> list[tuple[int, int, tuple, list]]:
    """(position, run length, colours, [(y, x), ...]) of every ramp run on a scanline"""
    runs = []   # [position, length, key, L, pixels], contiguous along the line
    for position, y, x in line:
        key = int(colors_of[y, x])
        if runs and runs[-1][2] == key:
            runs[-1][1] += 1
            runs[-1][4].append((y, x))
        else:
            runs.append([position, 1, key, float(light[y, x]), [(y, x)]])

    def valid(i: int, j: int) -> bool:
        window = runs[i : j + 1]
        if any(r[2] < 0 or r[1] != window[0][1] or r[1] > 2 for r in window):
            return False
        steps = [b[3] - a[3] for a, b in zip(window, window[1:])]
        return all(s > 0 for s in steps) or all(s < 0 for s in steps)

    n = len(runs)
    ramps = []
    for i in range(n):
        j = i
        while j + 1 < n and valid(i, j + 1):
            j += 1
        if j - i + 1 < 3:
            continue
        # keep only the maximal windows: i..j must not extend to the left
        if i > 0 and valid(i - 1, j):
            continue
        window = runs[i : j + 1]
        ramps.append((window[0][0], window[0][1], tuple(r[2] for r in window), [p for r in window for p in r[4]]))
    return ramps


def banding(frames, threshold=12):
    pixels, groups = 0, 0
    for f in frames:
        h, w = f.shape[:2]
        keys = _packed(f).astype(np.int64)
        keys[f[..., 3] != 255] = -1
        light = _oklab(f[..., :3])[..., 0]
        marked = np.zeros((h, w), dtype=bool)
        by_family: dict = {}
        for family, index, line in _scanlines(h, w):
            by_family.setdefault(family, {})[index] = _ramps(keys, light, line)
        for family, lines in by_family.items():
            for shift in (-1, 0, 1):
                table = {(index, r[0], r[1], r[2]): r for index, ramps in lines.items() for r in ramps}
                for (index, position, length, colours), ramp in table.items():
                    if (index - 1, position - shift, length, colours) in table:
                        continue
                    chain = [ramp]
                    while (index + len(chain), position + shift * len(chain), length, colours) in table:
                        chain.append(table[(index + len(chain), position + shift * len(chain), length, colours)])
                    if len(chain) < 3:
                        continue
                    groups += 1
                    for r in chain:
                        for y, x in r[3]:
                            marked[y, x] = True
        pixels += int(np.count_nonzero(marked))
    return _result(pixels, groups, "WARN" if pixels >= threshold and groups else "PASS")


def _ranks(values: np.ndarray) -> np.ndarray:
    unique, inverse, counts = np.unique(values, return_inverse=True, return_counts=True)
    ends = np.cumsum(counts)
    return ((ends - counts + 1 + ends) / 2.0)[inverse]


def _pillow_of(shape, light, grid_h, grid_w):
    ys = np.array([p[0] for p in shape], dtype=np.int64)
    xs = np.array([p[1] for p in shape], dtype=np.int64)
    if len(shape) < 64:
        return None
    lightness = light[ys, xs]
    if len(np.unique(lightness)) < 3:
        return None
    inside = np.zeros((grid_h + 2, grid_w + 2), dtype=bool)
    inside[ys + 1, xs + 1] = True
    dist = np.zeros(inside.shape, dtype=np.int64)
    level, remaining = 0, inside.copy()
    while remaining.any():
        level += 1
        shrunk = remaining.copy()
        shrunk[1:-1, 1:-1] = remaining[1:-1, 1:-1] & remaining[:-2, 1:-1] & remaining[2:, 1:-1] & remaining[1:-1, :-2] & remaining[1:-1, 2:]
        dist[remaining & ~shrunk] = level
        remaining = shrunk
    d = dist[ys + 1, xs + 1]
    a, b = _ranks(lightness), _ranks(d)
    rho = 0.0 if a.std() == 0 or b.std() == 0 else float(np.corrcoef(a, b)[0, 1])
    design = np.stack([np.ones(len(xs)), xs.astype(float), ys.astype(float)], axis=1)
    fit, *_ = np.linalg.lstsq(design, lightness, rcond=None)
    residual = float(np.sum((lightness - design @ fit) ** 2))
    total = float(np.sum((lightness - lightness.mean()) ** 2))
    r2 = 1.0 - residual / total if total > 0 else 0.0
    cx, cy = xs.mean(), ys.mean()
    edge = d == 1
    means = []
    for left in (True, False):
        for top in (True, False):
            q = edge & ((xs < cx) == left) & ((ys < cy) == top)
            if q.any():
                means.append(lightness[q].mean())
    balance = max(means) - min(means)
    return rho, r2, balance


def pillow_shading(frames, threshold=0.6):
    bodies = []
    for f in frames:
        h, w = f.shape[:2]
        light = _oklab(f[..., :3])[..., 0]
        for shape in _components(f[..., 3] == 255, False):
            measured = _pillow_of(shape, light, h, w)
            if measured is not None:
                bodies.append(measured)
    if not bodies:
        return SKIP
    pillow = [rho for rho, r2, balance in bodies if rho >= threshold and r2 <= 0.25 and balance <= 0.03]
    worst = max(pillow) if pillow else max(rho for rho, _, _ in bodies)
    return _result(worst, 3 * len(pillow), "WARN" if pillow else "PASS")


# ---- sheet lints


def empty_frame(frames, animations):
    if not animations:
        return SKIP
    wanted = sorted({i for a in animations for i in a["frames"] if 0 <= i < len(frames)})
    count = sum(1 for i in wanted if not np.any(frames[i][..., 3] > 0))
    return _result(count, count, "FAIL" if count else "PASS")


def one_frame(animations):
    if not animations:
        return SKIP
    count = sum(1 for a in animations if len(a["frames"]) == 1)
    return _result(count, count, "WARN" if count else "PASS")


def frame_scale(cells, shared):
    if not cells or not shared or shared <= 0:
        return SKIP
    count = sum(1 for c in cells if c > 0 and abs(c - shared) > 0.05 * shared)
    return _result(count, count, "WARN" if count else "PASS")


def run(request: dict) -> dict:
    frames = [_frame(spec) for spec in request.get("frames", [])]
    if not frames:
        raise ValueError("no frames")
    rules = request.get("rules") or {}
    palette = [_hex(c) for c in request.get("palette") or []]
    backdrops = [_hex(c) for c in request.get("backdrops") or []]
    animations = request.get("animations") or []
    return {
        "off_palette": off_palette(frames, palette),
        "colors": colors(frames, rules.get("max_colors")),
        "partial_alpha": partial_alpha(frames),
        "transparent_background": transparent_background(frames, bool(rules.get("transparent_background"))),
        "size": size(frames, rules.get("sizes")),
        "near_duplicates": near_duplicates(frames),
        "orphans": orphans(frames, bool(request.get("sheet"))),
        "jaggies": jaggies(frames),
        "doubles": doubles(frames),
        "holes": holes(frames),
        "flicker": flicker(frames, request.get("loop", True)),
        "contrast": contrast(frames, backdrops),
        "banding": banding(frames),
        "pillow_shading": pillow_shading(frames),
        "empty_frame": empty_frame(frames, animations),
        "one_frame": one_frame(animations),
        "frame_scale": frame_scale(request.get("frame_cells") or [], request.get("shared_cell") or 0),
    }


if __name__ == "__main__":
    print(json.dumps(run(json.load(sys.stdin))))
