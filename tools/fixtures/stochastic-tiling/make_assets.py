#!/usr/bin/env python3
"""Generates the textures of the stochastic-tiling fixture: a tileable pebble field whose
repetition is easy to spot (a few bright pebbles, one orange), its normal map (OpenGL
convention: green points up the image) and a packed occlusion/roughness map, plus a
striped texture whose straight lines show what Rotation does. For the built-in Lit material:
an emission map (the orange pebble glows), a leaf cutout (RGBA, alpha 0 between the leaves) for
an alpha-masked foliage card, and a normal map whose average normal leans 9 degrees (the check
and Inspector warning about Rotation).

    uv run --no-project --with pillow --with numpy python tools/fixtures/stochastic-tiling/make_assets.py

Deterministic: rerunning rewrites the same files.
"""

import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
TEXTURES = os.path.join(HERE, "assets", "textures")
SIZE = 256

rng = np.random.default_rng(20261023)


def save(array, name):
    Image.fromarray(np.clip(array, 0, 255).astype(np.uint8)).save(os.path.join(TEXTURES, name), optimize=True)


def pebbles():
    """Height, colour and roughness of pebbles on a jittered grid, wrapping at the borders."""
    ys, xs = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64) + 0.5
    height = np.zeros((SIZE, SIZE))
    color = np.zeros((SIZE, SIZE, 3))
    rough = np.full((SIZE, SIZE), 0.9)
    # dark gritty ground
    ground = np.array([62.0, 54.0, 46.0]) + rng.normal(0.0, 6.0, (SIZE, SIZE, 1))
    color[:] = ground
    cells = 7
    step = SIZE / cells
    palette = [(150, 142, 130), (120, 112, 104), (176, 166, 150), (98, 92, 88), (140, 120, 98), (190, 184, 174)]
    for gy in range(cells):
        for gx in range(cells):
            cx = (gx + 0.5 + rng.uniform(-0.3, 0.3)) * step
            cy = (gy + 0.5 + rng.uniform(-0.3, 0.3)) * step
            rx = step * rng.uniform(0.32, 0.48)
            ry = step * rng.uniform(0.28, 0.44)
            angle = rng.uniform(0, np.pi)
            tone = np.array(palette[rng.integers(len(palette))], dtype=np.float64)
            if gx == 2 and gy == 4:
                tone = np.array([226.0, 112.0, 30.0])   # the one orange pebble
                rx, ry = step * 0.55, step * 0.45
            # wrapped offsets, so the texture tiles
            dx = (xs - cx + SIZE / 2) % SIZE - SIZE / 2
            dy = (ys - cy + SIZE / 2) % SIZE - SIZE / 2
            u = (dx * np.cos(angle) + dy * np.sin(angle)) / rx
            v = (-dx * np.sin(angle) + dy * np.cos(angle)) / ry
            r2 = u * u + v * v
            inside = r2 < 1.0
            dome = np.sqrt(np.clip(1.0 - r2, 0.0, 1.0)) * min(rx, ry) * 0.6
            take = inside & (dome > height)
            height[take] = dome[take]
            shade = 0.85 + 0.15 * np.sqrt(np.clip(1.0 - r2, 0.0, 1.0))
            color[take] = (tone * shade[..., None])[take] + rng.normal(0.0, 4.0, (SIZE, SIZE, 3))[take]
            rough[take] = 0.55
    return height, color, rough


def normal_map(height, strength=1.0):
    # central differences with wrap; rows run down the image, so green (up the image) takes +dh/drow
    dhdx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * strength
    dhdrow = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * strength
    n = np.stack([-dhdx, dhdrow, np.ones_like(height)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return (n * 0.5 + 0.5) * 255.0


def stripes():
    ys, xs = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
    band = (np.floor(xs / (SIZE / 8)) % 2)[..., None]
    a = np.array([214.0, 196.0, 120.0])
    b = np.array([70.0, 110.0, 150.0])
    image = a * band + b * (1.0 - band)
    image += rng.normal(0.0, 5.0, image.shape)
    return image


def emission(color):
    """The orange pebble glows (the only warm, saturated texels), the rest is black"""
    r, g, b = color[..., 0], color[..., 1], color[..., 2]
    glow = ((r > 170) & (r - b > 110)).astype(np.float64)[..., None]
    return glow * np.array([255.0, 150.0, 60.0])


def leaves():
    """Tileable leaves (alpha 255 on a leaf, 0 between), one large pale leaf that repeats visibly"""
    leaf_rng = np.random.default_rng(20261024)
    ys, xs = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64) + 0.5
    image = np.zeros((SIZE, SIZE, 4))
    for i in range(26):
        cx, cy = leaf_rng.uniform(0, SIZE, 2)
        length = leaf_rng.uniform(26, 44)
        width = length * leaf_rng.uniform(0.32, 0.42)
        tone = np.array([leaf_rng.uniform(40, 80), leaf_rng.uniform(110, 160), leaf_rng.uniform(30, 60)])
        if i == 0:
            cx, cy, length, width = SIZE * 0.3, SIZE * 0.6, 60.0, 26.0
            tone = np.array([196.0, 214.0, 96.0])
        angle = leaf_rng.uniform(0, np.pi)
        dx = (xs - cx + SIZE / 2) % SIZE - SIZE / 2
        dy = (ys - cy + SIZE / 2) % SIZE - SIZE / 2
        u = (dx * np.cos(angle) + dy * np.sin(angle)) / length
        v = (-dx * np.sin(angle) + dy * np.cos(angle)) / width
        # a pointed leaf: narrower towards both tips
        inside = (np.abs(u) < 1.0) & (np.abs(v) < (1.0 - u * u))
        vein = np.abs(v) < 0.08
        shade = np.where(vein, 1.25, 0.85 + 0.15 * (1.0 - np.abs(u)))
        image[inside, :3] = (tone * shade[..., None])[inside]
        image[inside, 3] = 255.0
    return image


def tilted_normal_map(height, degrees=9.0):
    """pebbles_normal.png's normals all leaning `degrees` towards +X: an average normal off +Z"""
    n = normal_map(height) / 255.0 * 2.0 - 1.0
    a = np.radians(degrees)
    x = n[..., 0] * np.cos(a) + n[..., 2] * np.sin(a)
    z = -n[..., 0] * np.sin(a) + n[..., 2] * np.cos(a)
    n = np.stack([x, n[..., 1], z], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return (n * 0.5 + 0.5) * 255.0


def main():
    os.makedirs(TEXTURES, exist_ok=True)
    height, color, rough = pebbles()
    save(color, "pebbles.png")
    save(normal_map(height), "pebbles_normal.png")
    occlusion = np.clip(0.55 + height / max(height.max(), 1e-6) * 0.45, 0, 1)
    packed = np.stack([occlusion * 255.0, rough * 255.0, np.zeros_like(rough)], axis=-1)
    save(packed, "pebbles_orm.png")
    save(stripes(), "stripes.png")
    # (after the ones above, which keep their bytes: the leaves draw from their own generator)
    save(emission(color), "pebbles_emissive.png")
    save(leaves(), "leaves.png")
    save(tilted_normal_map(height), "pebbles_normal_tilted.png")


if __name__ == "__main__":
    main()
