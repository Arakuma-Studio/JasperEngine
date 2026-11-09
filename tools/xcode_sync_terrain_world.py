#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# Lists the T4 Terrain World sources in the Xcode workspace (CMake lists them in
# engine/core/terrain/TerrainSources.cmake, JASPERENGINE_TERRAIN_WORLD_SRCS and its pure list; the Xcode
# project lists files explicitly):
#
#   python3 tools/xcode_sync_terrain_world.py [--check]
#
# Each .cpp goes into the two Sources phases that compile ParticleSystem.cpp (the iOS and macOS engine
# targets), each .h into its Headers phase, under the group of its directory (made when missing:
# core/terrain/world). core/terrain/world's sources get -ffp-contract=off, as CMake gives them (a tile's
# bytes are the same on every target, T1 R8). Object ids are derived from the file path ("TWLD" +
# sha1), so a merge with another branch's additions only has to keep both lines. The module's define
# is the export's to set (Exporter.cpp writes JASPERENGINE_TERRAIN_WORLD=1 or 0 by module usage).
# --check exits 1 when something is missing.

import hashlib
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.join(ROOT, "engine", "workspaces", "xcode", "JasperEngine.xcodeproj", "project.pbxproj")

# the helpers of the convention: anchors, groups and how a file is added
_spec = importlib.util.spec_from_file_location("xcode_sync_splines", os.path.join(ROOT, "tools", "xcode_sync_splines.py"))
_splines = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_splines)

# engine/core-relative files T4 added outside core/terrain/world
FILES = [
    "component/TerrainWorldComponent.h",
    "object/TerrainWorld.cpp", "object/TerrainWorld.h",
    "physics/TerrainWorldCollision.cpp", "physics/TerrainWorldCollision.h",
    "script/binding/TerrainWorldClassesLua.cpp",
    "subsystem/RenderSystemTerrainWorld.cpp", "subsystem/TerrainWorldRender.h",
    "subsystem/TerrainWorldRuntime.cpp", "subsystem/TerrainWorldRuntime.h",
    "subsystem/TerrainWorldSystem.cpp", "subsystem/TerrainWorldSystem.h",
]

def world_files():
    folder = os.path.join(ROOT, "engine", "core", "terrain", "world")
    return ["terrain/world/" + n for n in sorted(os.listdir(folder)) if n.endswith((".cpp", ".h"))]


def contracted_sources():
    return {path for path in world_files() if path.endswith(".cpp")}


def object_id(kind, path):
    return "54574C44" + hashlib.sha1((kind + ":" + path).encode()).hexdigest()[:16].upper()


def listed(text, path):
    # a file another sync already added (tools/xcode_sync_engine.py names it by its own ids)
    return object_id("ref", path) in text or ("/* " + os.path.basename(path) + " */ = {isa = PBXFileReference;") in text


def main():
    check = "--check" in sys.argv
    with open(PROJECT, encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")
    missing = [path for path in world_files() + FILES if not listed(text, path)]
    if not missing:
        print("Xcode project lists every Terrain World file")
        return 0
    if check:
        print("missing from the Xcode project: " + ", ".join(missing))
        return 1
    _splines.object_id = object_id
    contracted = contracted_sources()
    for path in missing:
        _splines.add_file(lines, path, contracted)
    with open(PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d Terrain World file(s) to the Xcode project" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
