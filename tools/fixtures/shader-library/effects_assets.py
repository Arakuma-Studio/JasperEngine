#!/usr/bin/env python3
"""Generates the assets of the shader-library fixture's effects group (scripts/effects.lua):
original textures (noise, ripple normals, hexagon pattern, scanlines, a numbered sprite
sheet, leaves, decals, belt and streaks) and a crossed-quads plant model whose vertex
colour's red is its wind mask.

    python3 tools/fixtures/shader-library/effects_assets.py   (needs Pillow and numpy)

Deterministic: rerunning rewrites the same files.
"""

import base64
import json
import math
import os
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
TEXTURES = os.path.join(HERE, "assets", "textures")
MODELS = os.path.join(HERE, "assets", "models")

rng = np.random.default_rng(20261010)


def save(image, name):
    image.save(os.path.join(TEXTURES, name), optimize=True)


def value_noise(size, period):
    """Tileable value noise in [0, 1]: a random period x period lattice, smoothly interpolated."""
    lattice = rng.random((period, period))
    coords = np.arange(size) * period / size
    i0 = np.floor(coords).astype(int)
    f = coords - i0
    f = f * f * (3.0 - 2.0 * f)
    i1 = (i0 + 1) % period
    # rows (y) then columns (x)
    a = lattice[i0][:, i0]
    b = lattice[i0][:, i1]
    c = lattice[i1][:, i0]
    d = lattice[i1][:, i1]
    fx = f[None, :]
    fy = f[:, None]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm(size, base_period, octaves=4):
    total = np.zeros((size, size))
    amplitude = 1.0
    norm = 0.0
    period = base_period
    for _ in range(octaves):
        total += value_noise(size, period) * amplitude
        norm += amplitude
        amplitude *= 0.5
        period *= 2
    total /= norm
    # stretched to the full range
    total = (total - total.min()) / (total.max() - total.min())
    return total


def to_u8(a):
    return np.clip(a * 255.0 + 0.5, 0, 255).astype(np.uint8)


def noise_texture():
    # three independent fbm channels: dissolve and foam read red, distortion red and green
    size = 128
    channels = [fbm(size, 4), fbm(size, 4), fbm(size, 8)]
    save(Image.fromarray(np.dstack([to_u8(c) for c in channels]), "RGB"), "fx_noise.png")


def water_normal_texture():
    size = 128
    height = fbm(size, 4, 3) * 0.6 + fbm(size, 8, 2) * 0.4
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * size / 8.0
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * size / 8.0
    n = np.dstack([-dx, -dy, np.ones_like(dx)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    save(Image.fromarray(to_u8(n * 0.5 + 0.5), "RGB"), "fx_water_normal.png")


def hex_texture():
    # a tileable hexagon grid: bright edges, dim cells, on black (for an additive shield)
    w, h = 256, 222  # sqrt(3)/2 ratio keeps the cells regular; tiles horizontally and vertically
    cell = w / 4.0
    ys, xs = np.mgrid[0:h, 0:w].astype(float)
    best = np.full((h, w), 1e9)
    second = np.full((h, w), 1e9)
    row_height = cell * math.sqrt(3) / 2.0
    rows = int(round(h / row_height))
    for r in range(-1, rows + 2):
        for c in range(-1, 6):
            cx = c * cell + (cell / 2.0 if r % 2 else 0.0)
            cy = r * row_height
            d = np.hypot(xs - cx, ys - cy)
            second = np.minimum(second, np.maximum(d, best))
            best = np.minimum(best, d)
    edge = np.clip(1.0 - (second - best) / 6.0, 0.0, 1.0) ** 1.5
    fill = 0.08 + 0.12 * (best / (cell * 0.6))
    value = np.clip(edge + fill, 0.0, 1.0)
    rgb = np.dstack([value * 0.8, value * 0.95, value])
    image = Image.fromarray(to_u8(rgb), "RGB").resize((256, 256), Image.BICUBIC)
    save(image, "fx_hex.png")


def scanline_texture():
    # one bright line with a soft trail per V period, in red (white overall)
    h = 64
    v = (np.arange(h) + 0.5) / h
    line = np.exp(-((v - 0.2) / 0.05) ** 2) + 0.5 * np.clip(1.0 - (v - 0.2) / 0.6, 0, 1) * (v > 0.2)
    line = np.clip(line, 0, 1)
    column = to_u8(line)
    image = np.repeat(column[:, None], 8, axis=1)
    save(Image.fromarray(np.dstack([image] * 3), "RGB"), "fx_scanlines.png")


def flipbook_texture():
    # 4 x 4 frames of 128 px: a numbered disc whose ring sweeps around, on transparent
    size, grid = 128, 4
    sheet = Image.new("RGBA", (size * grid, size * grid), (0, 0, 0, 0))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.load_default(size=56)
    except TypeError:
        font = ImageFont.load_default()
    for index in range(grid * grid):
        ox = (index % grid) * size
        oy = (index // grid) * size
        hue = index / (grid * grid)
        colour = tuple(int(255 * (0.55 + 0.45 * math.cos(2 * math.pi * (hue + k / 3.0)))) for k in range(3))
        draw.ellipse([ox + 10, oy + 10, ox + size - 10, oy + size - 10], fill=colour + (255,))
        draw.ellipse([ox + 24, oy + 24, ox + size - 24, oy + size - 24], fill=(30, 30, 40, 255))
        # a marker going round once over the sheet
        angle = 2 * math.pi * index / (grid * grid) - math.pi / 2
        mx = ox + size / 2 + math.cos(angle) * (size / 2 - 17)
        my = oy + size / 2 + math.sin(angle) * (size / 2 - 17)
        draw.ellipse([mx - 9, my - 9, mx + 9, my + 9], fill=(255, 255, 255, 255))
        text = str(index)
        box = draw.textbbox((0, 0), text, font=font)
        tx = ox + (size - (box[2] - box[0])) / 2 - box[0]
        ty = oy + (size - (box[3] - box[1])) / 2 - box[1]
        draw.text((tx, ty), text, font=font, fill=(255, 255, 255, 255))
    save(sheet, "fx_flipbook.png")


def leaves_texture():
    # a cluster of leaves on transparent: alpha cutout, darker veins, lighter tips
    size = 256
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    leaves = []
    # leaves along a few stems rising from the bottom centre, pointing up and out
    for stem in range(5):
        lean = (stem - 2) * 0.32
        for k in range(6):
            t = (k + 1) / 7.0
            x = 128 + math.sin(lean) * t * 210 + rng.uniform(-8, 8)
            y = 250 - math.cos(lean) * t * 215 + rng.uniform(-8, 8)
            side = -1 if k % 2 else 1
            angle = -math.pi / 2 + lean + side * rng.uniform(0.5, 1.0)
            leaves.append((x, y, angle, rng.uniform(40, 62) * (1.15 - 0.4 * t)))
    for x, y, angle, length in sorted(leaves, key=lambda leaf: -leaf[1]):
        dx, dy = math.cos(angle), math.sin(angle)
        px, py = -dy, dx
        width = length * 0.32
        points = []
        for k in range(17):
            t = k / 16.0
            s = math.sin(math.pi * t) * width * (1.0 - 0.3 * t)
            points.append((x + dx * length * t + px * s, y + dy * length * t + py * s))
        for k in range(16, -1, -1):
            t = k / 16.0
            s = math.sin(math.pi * t) * width * (1.0 - 0.3 * t)
            points.append((x + dx * length * t - px * s, y + dy * length * t - py * s))
        shade = rng.uniform(0.75, 1.0)
        draw.polygon(points, fill=(int(70 * shade), int(150 * shade), int(45 * shade), 255))
        draw.line([(x, y), (x + dx * length * 0.9, y + dy * length * 0.9)], fill=(45, 100, 30, 255), width=2)
    save(image, "fx_leaves.png")


def decal_textures():
    size = 256
    # an alpha decal: a yellow hazard roundel with a black arrow, soft outer edge
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.ellipse([12, 12, size - 12, size - 12], fill=(250, 200, 30, 255))
    draw.ellipse([34, 34, size - 34, size - 34], fill=(30, 30, 30, 255))
    draw.ellipse([46, 46, size - 46, size - 46], fill=(250, 200, 30, 255))
    draw.polygon([(128, 62), (186, 128), (150, 128), (150, 194), (106, 194), (106, 128), (70, 128)], fill=(30, 30, 30, 255))
    # worn: noise eats into the alpha
    wear = fbm(size, 8)
    alpha = np.array(image)[:, :, 3].astype(float) / 255.0
    alpha *= np.clip((wear - 0.18) * 4.0, 0.0, 1.0)
    pixels = np.array(image)
    pixels[:, :, 3] = to_u8(alpha)
    save(Image.fromarray(pixels, "RGBA"), "fx_decal.png")

    # a multiply decal: a dark, blotchy stain fading out (dirt, a fake contact shadow)
    ys, xs = np.mgrid[0:size, 0:size].astype(float)
    r = np.hypot(xs - size / 2, ys - size / 2) / (size / 2)
    blot = fbm(size, 4)
    alpha = np.clip(1.0 - r * (0.8 + 0.5 * blot), 0.0, 1.0) ** 0.8
    rgb = np.dstack([0.25 + 0.1 * blot, 0.2 + 0.08 * blot, 0.15 + 0.05 * blot])
    save(Image.fromarray(np.dstack([to_u8(rgb), to_u8(alpha)]), "RGBA"), "fx_dirt.png")


def belt_and_streaks():
    # a conveyor belt: dark rubber with bright chevrons pointing along +V
    w, h = 64, 64
    belt = Image.new("RGB", (w, h), (45, 45, 50))
    draw = ImageDraw.Draw(belt)
    draw.polygon([(8, 40), (32, 16), (56, 40), (56, 52), (32, 28), (8, 52)], fill=(230, 160, 40))
    save(belt, "fx_belt.png")

    # streaks along V (water falling, a laser's energy): noise stretched 8 times along V
    size = 128
    streak = value_noise(size, 16) * 0.6 + value_noise(size, 32) * 0.4
    streak = np.array(Image.fromarray(to_u8(streak)).resize((size, size // 8), Image.BICUBIC).resize((size, size), Image.BICUBIC)).astype(float) / 255.0
    streak = np.clip((streak - 0.25) * 1.8, 0, 1)
    alpha = 0.35 + 0.65 * streak
    rgb = np.dstack([0.75 + 0.25 * streak] * 3)
    save(Image.fromarray(np.dstack([to_u8(rgb), to_u8(alpha)]), "RGBA"), "fx_streaks.png")


def plant_model():
    # three crossed quads of 1 x 1.2, four segments high; the vertex colour's red is the
    # wind mask (0 at the root, 1 at the top), UVs map the leaves texture upright
    positions, normals, uvs, colours, indices = [], [], [], [], []
    width, height, segments = 1.0, 1.2, 4
    for q in range(3):
        angle = math.pi * q / 3.0
        ax, az = math.cos(angle), math.sin(angle)
        nx, nz = -az, ax
        base = len(positions)
        for s in range(segments + 1):
            t = s / segments
            for side in (-0.5, 0.5):
                positions.append((ax * side * width, t * height, az * side * width))
                normals.append((nx, 0.0, nz))
                uvs.append((side + 0.5, 1.0 - t))
                colours.append((t, t, t))
        for s in range(segments):
            a = base + s * 2
            indices += [a, a + 1, a + 3, a, a + 3, a + 2]

    write_gltf("fx_plant.gltf", "Plant", positions, normals, uvs, colours, indices, double_sided=True)


def water_grid_model():
    # a 1.8 x 1.8 horizontal grid of 24 x 24 cells, for the water's vertex waves
    positions, normals, uvs, colours, indices = [], [], [], [], []
    cells, size = 24, 1.8
    for j in range(cells + 1):
        for i in range(cells + 1):
            u, v = i / cells, j / cells
            positions.append(((u - 0.5) * size, 0.0, (v - 0.5) * size))
            normals.append((0.0, 1.0, 0.0))
            uvs.append((u, v))
            colours.append((1.0, 1.0, 1.0))
    for j in range(cells):
        for i in range(cells):
            a = j * (cells + 1) + i
            b = a + cells + 1
            # counter-clockwise seen from above
            indices += [a, b, b + 1, a, b + 1, a + 1]
    write_gltf("fx_water_grid.gltf", "WaterGrid", positions, normals, uvs, colours, indices)


def write_gltf(file, name, positions, normals, uvs, colours, indices, double_sided=False):
    def pack(fmt, rows):
        return b"".join(struct.pack(fmt, *row) for row in rows)

    blobs = [pack("<3f", positions), pack("<3f", normals), pack("<2f", uvs), pack("<3f", colours),
             struct.pack("<%dH" % len(indices), *indices)]
    data = b""
    views = []
    for i, blob in enumerate(blobs):
        while len(data) % 4:
            data += b"\0"
        views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob),
                      "target": 34963 if i == 4 else 34962})
        data += blob
    count = len(positions)
    xs = [p[0] for p in positions]
    ys = [p[1] for p in positions]
    zs = [p[2] for p in positions]
    gltf = {
        "asset": {"version": "2.0", "generator": "JasperEngine shader-library fixture (effects_assets.py)"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "COLOR_0": 3},
            "indices": 4, "material": 0}]}],
        "materials": [{"name": name, "doubleSided": double_sided,
                       "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0, "roughnessFactor": 0.8}}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": count, "type": "VEC3",
             "min": [min(xs), min(ys), min(zs)], "max": [max(xs), max(ys), max(zs)]},
            {"bufferView": 1, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": count, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 4, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
        "bufferViews": views,
        "buffers": [{"byteLength": len(data),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode("ascii")}],
    }
    with open(os.path.join(MODELS, file), "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    os.makedirs(TEXTURES, exist_ok=True)
    os.makedirs(MODELS, exist_ok=True)
    noise_texture()
    water_normal_texture()
    hex_texture()
    scanline_texture()
    flipbook_texture()
    leaves_texture()
    decal_textures()
    belt_and_streaks()
    plant_model()
    water_grid_model()
