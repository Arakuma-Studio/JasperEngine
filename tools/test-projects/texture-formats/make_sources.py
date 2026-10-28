#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Writes source/: the texture sources of the texture-formats test project (a solid colour, then
the fixture corpus's photo, normal map, sprite and HDR sky; tools/fixtures/textures/generate.py),
as a project the editor's cook-textures reads, and bench-source/: the 512² corpus photo of the
web transcode bench (main.cpp, ?transcode_bench=1), cooked to UASTC λ1. make_assets.sh cooks
them into assets/je-cooked and assets/bench."""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "fixtures", "textures"))
import generate as corpus  # noqa: E402

SOURCE = os.path.join(HERE, "source")
BENCH = os.path.join(HERE, "bench-source")


def write(rel, data, root=SOURCE):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


write("project.yaml", "name: TextureFormats\nassetsDir: assets\nscenes: []\n")
write("assets/textures/solid.png", corpus.rgba_image(64, 64, lambda x, y: (200 / 255.0, 60 / 255.0, 30 / 255.0, 1.0)))
write("assets/textures/photo.png", corpus.rgba_image(128, 128, corpus.photo))
write("assets/textures/normal.png", corpus.rgba_image(128, 128, corpus.normal))
write("assets/textures/sprite.png", corpus.rgba_image(64, 32, corpus.sprite))
write("assets/textures/sky.hdr", corpus.sky(64, 32))
# the types the scenes of a project would have given them
write("assets/textures/normal.png.meta", "metaVersion: 1\nguid: 1d6f0b0e9a4c4f6a8e3b5c7d9f1a2b3c\n"
                                         "import:\n  textureImporter:\n    version: 1\n    type: normal\n")
write("assets/textures/sprite.png.meta", "metaVersion: 1\nguid: 2e7a1c1f0b5d4a7b9f4c6d8e0a2b3c4d\n"
                                         "import:\n  textureImporter:\n    version: 1\n    type: sprite\n")

# the bench's source: R18/R20's web UASTC (λ1 + Zstandard) of a photo
write("project.yaml", "name: TextureFormatsBench\nassetsDir: assets\nscenes: []\n", BENCH)
write("assets/textures/photo.png", corpus.rgba_image(512, 512, corpus.photo), BENCH)
write("assets/textures/photo.png.meta", "metaVersion: 1\nguid: 3f8b2d2a1c6e4b8c0a5d7e9f1b3c4d5e\n"
                                        "import:\n  textureImporter:\n    version: 1\n    type: default\n"
                                        "    platforms:\n      web: {override: true, format: UASTC, rdo: 1}\n", BENCH)
