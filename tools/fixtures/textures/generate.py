#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Writes the texture fixture project (docs/specs/S14-texture-compression.md section 7): a saved
project whose assets are the spec's corpus at test sizes, generated (no third-party images), so
every file is a pure function of this script:

  assets/textures/photo.png       photo-like colour noise, opaque, 256x256
  assets/textures/gradient.png    smooth ramps (banding shows first), 256x256
  assets/textures/stylized.png    flat-shaded albedo with soft alpha, 256x256
  assets/textures/foliage.png     cut-out leaves (alpha 0 or 255), 256x256
  assets/textures/normal.png      tangent normal map of a height field, 256x256
  assets/textures/ao.png          grey occlusion, 128x128
  assets/textures/ui_atlas.png    UI atlas with text-like glyph strokes, 256x128
  assets/textures/sprite.png      pixel-art sprite, 64x32
  assets/textures/sky.hdr         HDR sky with a sun, 128x64
  assets/textures/height16.png    16-bit height map, 65x65 (a terrain's: CPU-readable)
  assets/textures/npot.png        non-power-of-two 200x120
  assets/textures/cube.png        a 4:3 cross cubemap of 32 px faces
  materials/Rock.material         photo + normal + ao (the material slots of R2)
  assets/models/panel.gltf        a quad whose material binds normal.png as normalTexture
  Main.scene                      a spot light masked by ao.png (CPU-readable, R3)

    python3 tools/fixtures/textures/generate.py [out dir]   (default: this directory)
"""

import base64
import json
import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def png(width, height, rows, color_type, bit_depth=8):
    """rows: one bytes object per row (already packed for the colour type)"""
    raw = b"".join(b"\x00" + row for row in rows)
    header = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def hdr(width, height, pixels):
    """Radiance RGBE, flat scanlines; pixels: (r, g, b) floats, row major"""
    out = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (height, width))
    for r, g, b in pixels:
        m = max(r, g, b)
        if m < 1e-32:
            out += b"\x00\x00\x00\x00"
            continue
        mantissa, exponent = math.frexp(m)
        scale = mantissa * 256.0 / m
        out += bytes([int(r * scale), int(g * scale), int(b * scale), exponent + 128])
    return bytes(out)


def lattice(x, y, seed):
    h = (x * 374761393 + y * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFFFF) / float(0xFFFFFF)


def noise(x, y, seed):
    xi, yi = math.floor(x), math.floor(y)
    fx, fy = x - xi, y - yi
    sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b = lattice(xi, yi, seed), lattice(xi + 1, yi, seed)
    c, d = lattice(xi, yi + 1, seed), lattice(xi + 1, yi + 1, seed)
    top = a + (b - a) * sx
    return top + (c + (d - c) * sx - top) * sy


def fbm(x, y, seed, octaves):
    total, amplitude, norm = 0.0, 0.5, 0.0
    for o in range(octaves):
        total += amplitude * noise(x, y, seed + o * 17)
        norm += amplitude
        x, y, amplitude = x * 2.03, y * 2.03, amplitude * 0.5
    return total / norm


def byte(v):
    return max(0, min(255, int(round(v * 255))))


def rgba_image(width, height, fn):
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            row += bytes(byte(c) for c in fn(x, y))
        rows.append(bytes(row))
    return png(width, height, rows, 6)


def photo(x, y):
    u, v = x / 32.0, y / 32.0
    region = fbm(u * 0.6, v * 0.6, 1, 4)
    detail = fbm(u * 4, v * 4, 2, 4)
    return (0.25 + 0.55 * region + 0.2 * detail, 0.35 + 0.35 * fbm(u * 0.8, v * 0.8, 3, 3) + 0.2 * detail,
            0.2 + 0.5 * (1 - region) * (0.6 + 0.4 * detail), 1.0)


def gradient(x, y):
    px, py = (x + 0.5) / 256, (y + 0.5) / 256
    return (px, py, 0.5 + 0.5 * math.sin((px + py) * math.pi), 1.0)


def stylized(x, y):
    u, v = x / 40.0, y / 40.0
    band = int(fbm(u, v, 12, 3) * 5) / 5.0
    alpha = min(1.0, max(0.0, (fbm(u * 0.7, v * 0.7, 13, 3) - 0.35) * 4))
    return (0.3 + 0.6 * band, 0.2 + 0.5 * band, 0.5 - 0.3 * band, alpha)


def foliage(x, y):
    u, v = x / 32.0, y / 32.0
    leaf = fbm(u * 1.5, v * 1.5, 6, 4) + 0.25 * fbm(u * 8, v * 8, 7, 2)
    shade = fbm(u * 5, v * 5, 8, 3)
    return (0.15 + 0.2 * shade, 0.35 + 0.4 * shade, 0.1 + 0.1 * shade, 1.0 if leaf > 0.62 else 0.0)


def normal(x, y):
    e = 1.0 / 64.0
    u, v = x / 32.0, y / 32.0
    h = lambda a, b: fbm(a * 2, b * 2, 9, 4) * 0.6
    hx = (h(u + e, v) - h(u - e, v)) / (2 * e) * 0.15
    hy = (h(u, v + e) - h(u, v - e)) / (2 * e) * 0.15
    length = math.sqrt(hx * hx + hy * hy + 1)
    return (-hx / length * 0.5 + 0.5, -hy / length * 0.5 + 0.5, 1 / length * 0.5 + 0.5, 1.0)


def ui_atlas(x, y):
    # rows of glyph-like strokes on panels, with a soft drop shadow
    cell_x, cell_y = x % 32, y % 32
    stroke = (cell_x in (8, 9, 22, 23) and 6 < cell_y < 26) or (cell_y in (15, 16) and 8 <= cell_x <= 23) or \
             (cell_y in (6, 7) and lattice(x // 32, y // 32, 14) > 0.5 and 8 <= cell_x <= 23)
    panel = 2 <= cell_x < 30 and 2 <= cell_y < 30
    if stroke:
        return (1.0, 1.0, 0.95, 1.0)
    if panel:
        return (0.12 + 0.3 * (x // 32) / 8, 0.2, 0.35, 0.92)
    return (0.0, 0.0, 0.0, 0.25 if 1 <= cell_x < 31 and 1 <= cell_y < 31 else 0.0)


def sprite(x, y):
    palette = [(0, 0, 0, 0), (0.95, 0.75, 0.2, 1), (0.85, 0.25, 0.2, 1), (0.2, 0.2, 0.25, 1)]
    cx, cy = x - 32, y - 16
    if cx * cx / 900 + cy * cy / 196 > 1:
        return palette[0]
    if abs(cx) < 3 and abs(cy) < 3:
        return palette[3]
    return palette[1 if (x // 4 + y // 4) % 2 else 2]


def grey_image(width, height, fn):
    return png(width, height, [bytes(byte(fn(x, y)) for x in range(width)) for y in range(height)], 0)


def height16(width, height):
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            value = int(fbm(x / 16.0, y / 16.0, 15, 4) * 65535)
            row += struct.pack(">H", max(0, min(65535, value)))
        rows.append(bytes(row))
    return png(width, height, rows, 0, 16)


def cube_cross(face):
    width, height = face * 4, face * 3
    cells = [(2, 1), (0, 1), (1, 0), (1, 2), (1, 1), (3, 1)]
    colours = [(0.9, 0.3, 0.3), (0.3, 0.9, 0.3), (0.3, 0.3, 0.9), (0.9, 0.9, 0.3), (0.3, 0.9, 0.9), (0.9, 0.3, 0.9)]

    def fn(x, y):
        for (cx, cy), (r, g, b) in zip(cells, colours):
            if cx * face <= x < (cx + 1) * face and cy * face <= y < (cy + 1) * face:
                t = ((x - cx * face) + (y - cy * face)) / (2.0 * face)
                return (r * (0.6 + 0.4 * t), g * (0.6 + 0.4 * t), b * (0.6 + 0.4 * t), 1.0)
        return (0, 0, 0, 1.0)
    return rgba_image(width, height, fn)


def sky(width, height):
    pixels = []
    for y in range(height):
        for x in range(width):
            px, py = (x + 0.5) / width, (y + 0.5) / height
            sun = math.exp(-((math.hypot(px - 0.7, py - 0.3) * 18) ** 2)) * 40
            cloud = max(0.0, fbm(px * 6, py * 3, 11, 4) - 0.5) * 2
            pixels.append((0.3 + 0.4 * py + sun + cloud * 1.5, 0.5 + 0.3 * py + sun * 0.9 + cloud * 1.5,
                           1.2 - 0.4 * py + sun * 0.7 + cloud * 1.6))
    return hdr(width, height, pixels)


def panel_gltf():
    positions = struct.pack("<12f", -1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0)
    normals = struct.pack("<12f", 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1)
    uvs = struct.pack("<8f", 0, 1, 1, 1, 1, 0, 0, 0)
    tangents = struct.pack("<16f", 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1)
    indices = struct.pack("<6H", 0, 1, 2, 0, 2, 3)
    blob = positions + normals + uvs + tangents + indices
    offsets = [0, len(positions), len(positions) + len(normals), len(positions) + len(normals) + len(uvs),
               len(positions) + len(normals) + len(uvs) + len(tangents)]
    return {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": offsets[0], "byteLength": 48, "target": 34962},
            {"buffer": 0, "byteOffset": offsets[1], "byteLength": 48, "target": 34962},
            {"buffer": 0, "byteOffset": offsets[2], "byteLength": 32, "target": 34962},
            {"buffer": 0, "byteOffset": offsets[3], "byteLength": 64, "target": 34962},
            {"buffer": 0, "byteOffset": offsets[4], "byteLength": 12, "target": 34963}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-1, -1, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC4"},
            {"bufferView": 4, "componentType": 5123, "count": 6, "type": "SCALAR"}],
        "images": [{"uri": "../textures/normal.png"}],
        "textures": [{"source": 0}],
        "materials": [{"name": "Panel", "normalTexture": {"index": 0}}],
        "meshes": [{"name": "Panel", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TANGENT": 3},
                                                      "indices": 4, "material": 0}]}],
        "nodes": [{"mesh": 0, "name": "Panel"}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }


TEXTURE = "{textureType: 2D, source: path, path: %s, minFilter: linear, magFilter: linear, wrapU: repeat, wrapV: repeat, releaseDataAfterLoad: true}"


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else HERE

    def write(rel, data):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode())

    write("assets/textures/photo.png", rgba_image(256, 256, photo))
    write("assets/textures/gradient.png", rgba_image(256, 256, gradient))
    write("assets/textures/stylized.png", rgba_image(256, 256, stylized))
    write("assets/textures/foliage.png", rgba_image(256, 256, foliage))
    write("assets/textures/normal.png", rgba_image(256, 256, normal))
    write("assets/textures/ao.png", grey_image(128, 128, lambda x, y: fbm(x / 16.0, y / 16.0, 10, 4) ** 0.7))
    write("assets/textures/ui_atlas.png", rgba_image(256, 128, ui_atlas))
    write("assets/textures/sprite.png", rgba_image(64, 32, sprite))
    write("assets/textures/sky.hdr", sky(128, 64))
    write("assets/textures/height16.png", height16(65, 65))
    write("assets/textures/npot.png", rgba_image(200, 120, photo))
    write("assets/textures/cube.png", cube_cross(32))
    write("assets/models/panel.gltf", json.dumps(panel_gltf(), indent=2) + "\n")
    write("materials/Rock.material",
          "name: materials/Rock.material\nbaseColorFactor: [1, 1, 1, 1]\nmetallicFactor: 0\nroughnessFactor: 1\n"
          "baseColorTexture: " + TEXTURE % "textures/photo.png" + "\n"
          "normalTexture: " + TEXTURE % "textures/normal.png" + "\n"
          "occlusionTexture: " + TEXTURE % "textures/ao.png" + "\n")
    write("project.yaml", "name: TextureFixture\ncanvasWidth: 1280\ncanvasHeight: 720\nassetsDir: assets\nluaDir: scripts\n"
                          "startSceneId: 1\nscenes:\n  - filepath: Main.scene\n")
    write("Main.scene",
          "id: 1\nname: Main\nscene:\n  backgroundColor: [0.16, 0.18, 0.24, 1]\nsceneType: scene_3d\nmainCamera: 0\n"
          "entities:\n"
          "  - type: Entity\n    entity: 1001\n    name: Spot\n    components:\n"
          "      transform:\n        position: [0, 0, 0]\n        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n"
          "      light:\n        type: spot\n        spotMask: " + TEXTURE % "textures/ao.png" + "\n"
          "  - type: Entity\n    entity: 1002\n    name: Panel\n    components:\n"
          "      transform:\n        position: [0, 0, 0]\n        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n"
          "      model:\n        filename: models/panel.gltf\n")
    write("scripts/.keep", "")
    write("README.md", "Fixture: the texture corpus of docs/specs/S14-texture-compression.md section 7 at test sizes, "
                       "as a saved project (no metas: opening it infers every texture's settings). Generated by "
                       "generate.py here; tests copy it before opening it.\n")


if __name__ == "__main__":
    main()
