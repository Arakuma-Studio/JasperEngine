#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# Lists the T6 Voxel Volume sources in the Xcode workspace (CMake globs engine/core/voxel in
# engine/core/voxel/VoxelSources.cmake and lists the rest in engine/CMakeLists.txt; the Xcode project
# lists files explicitly), and defines the module for the engine targets:
#
#   python3 tools/xcode_sync_voxels.py [--check]
#
# Each .cpp goes into the two Sources phases that compile ParticleSystem.cpp (the iOS and macOS engine
# targets), each .h into its Headers phase, under the group of its directory (made when missing:
# core/voxel, core/object/voxel). The voxel module's sources get -ffp-contract=off, as CMake gives them
# (a block's bytes are the same on every target, T6 R28). Object ids are derived from the file path
# ("VOXL" + sha1), so a merge with another branch's additions only has to keep both lines. Every
# target that defines JASPERENGINE_RUNTIME_SHADERS gets "JASPERENGINE_VOXEL=1": the engine's #if reads
# an undefined name as off, and the export turns it to 0 for a game that does not reach the module.
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

# engine/core-relative files T6 added outside core/voxel
FILES = [
    "component/VoxelVolumeComponent.h",
    "subsystem/VoxelSystem.cpp", "subsystem/VoxelSystem.h", "subsystem/VoxelSystemPhysics.cpp",
    "subsystem/VoxelSystemState.h", "subsystem/VoxelSystemTerrain.cpp", "subsystem/RenderSystemVoxel.cpp",
    "object/voxel/GroundQuery.cpp", "object/voxel/GroundQuery.h",
    "object/voxel/VoxelVolume.cpp", "object/voxel/VoxelVolume.h",
    "script/binding/VoxelClassesLua.cpp",
]

DEFINE_ANCHOR = "\t\t\t\t\tJASPERENGINE_RUNTIME_SHADERS,"
DEFINE_LINE = '\t\t\t\t\t"JASPERENGINE_VOXEL=1",'


def voxel_files():
    names = sorted(n for n in os.listdir(os.path.join(ROOT, "engine", "core", "voxel")) if n.endswith((".cpp", ".h")))
    return ["voxel/" + n for n in names]


def contracted_sources():
    # VoxelSources.cmake: every .cpp of core/voxel
    return {path for path in voxel_files() if path.endswith(".cpp")}


def object_id(kind, path):
    return "564F584C" + hashlib.sha1((kind + ":" + path).encode()).hexdigest()[:16].upper()


def missing_defines(lines):
    # the anchors with no define right after them
    return [i for i, line in enumerate(lines) if line == DEFINE_ANCHOR and (i + 1 >= len(lines) or lines[i + 1] != DEFINE_LINE)]


def main():
    check = "--check" in sys.argv
    with open(PROJECT, encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")
    files = voxel_files() + FILES
    missing = [path for path in files if object_id("ref", path) not in text]
    defines = missing_defines(lines)
    if not missing and not defines:
        print("Xcode project lists every voxel file and defines the module")
        return 0
    if check:
        if missing:
            print("missing from the Xcode project: " + ", ".join(missing))
        if defines:
            print("%d engine target(s) without JASPERENGINE_VOXEL" % len(defines))
        return 1
    _splines.object_id = object_id
    contracted = contracted_sources()
    for path in missing:
        _splines.add_file(lines, path, contracted)
    for index in reversed(missing_defines(lines)):
        lines.insert(index + 1, DEFINE_LINE)
    with open(PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d voxel file(s) and %d define(s) to the Xcode project" % (len(missing), len(defines)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
