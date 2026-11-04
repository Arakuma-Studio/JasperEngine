#!/usr/bin/env python3
# Adds the engine sources of S19 (navigation: Recast/Detour, engine/core/navigation, the
# NavigationSystem, its components and Lua bindings, util/MeshTriangles) that the Xcode workspace
# does not list yet, and Recast/Detour's include folders to the engine targets' header search
# paths. CMake lists them in engine/CMakeLists.txt (core/navigation/NavigationSources.cmake and
# libs/recastnavigation); the Xcode project lists files explicitly:
#
#   python3 tools/xcode_sync_navigation.py [--check]
#
# Each file joins the groups and build phases of a sibling file the project already has (its
# path is relative to the sibling's group), as tools/xcode_sync_gameplay.py does. Object ids are
# derived from the file path ("COMP" + sha1 of "s19/<file>"), so they never collide with other
# branches' ids. Recast/Detour compile into the engine targets (hidden in the static library like
# its other sources). --check exits 1 when something is missing (nothing is written).

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xcode_sync_gameplay as gameplay  # noqa: E402

base = gameplay.base

PREFIX = "s19/"

CORE = [
    ("navigation/GridPathfinder.cpp", "subsystem/MeshSystem.cpp"),
    ("navigation/GridPathfinder.h", "subsystem/MeshSystem.h"),
    ("navigation/NavBakeCollector.cpp", "subsystem/MeshSystem.cpp"),
    ("navigation/NavBakeCollector.h", "subsystem/MeshSystem.h"),
    ("navigation/NavBakeInput.cpp", "subsystem/MeshSystem.cpp"),
    ("navigation/NavBakeInput.h", "subsystem/MeshSystem.h"),
    ("navigation/NavBakeScheduler.cpp", "subsystem/MeshSystem.cpp"),
    ("navigation/NavBakeScheduler.h", "subsystem/MeshSystem.h"),
    ("navigation/NavCrowd.cpp", "subsystem/MeshSystem.cpp"),
    ("navigation/NavCrowd.h", "subsystem/MeshSystem.h"),
    ("navigation/NavDetourSupport.cpp", "subsystem/MeshSystem.cpp"),
    ("navigation/NavDetourSupport.h", "subsystem/MeshSystem.h"),
    ("navigation/NavFrame.h", "subsystem/MeshSystem.h"),
    ("navigation/NavGridMap.cpp", "subsystem/MeshSystem.cpp"),
    ("navigation/NavGridMap.h", "subsystem/MeshSystem.h"),
    ("navigation/NavHash.h", "subsystem/MeshSystem.h"),
    ("navigation/NavMesh.cpp", "subsystem/MeshSystem.cpp"),
    ("navigation/NavMesh.h", "subsystem/MeshSystem.h"),
    ("navigation/NavMeshBuilder.cpp", "subsystem/MeshSystem.cpp"),
    ("navigation/NavMeshBuilder.h", "subsystem/MeshSystem.h"),
    ("navigation/NavRandom.h", "subsystem/MeshSystem.h"),
    ("navigation/NavTypes.cpp", "subsystem/MeshSystem.cpp"),
    ("navigation/NavTypes.h", "subsystem/MeshSystem.h"),
    ("subsystem/NavigationSystem.cpp", "subsystem/MeshSystem.cpp"),
    ("subsystem/NavigationSystem.h", "subsystem/MeshSystem.h"),
    ("component/NavGridComponent.h", "component/Joint3DComponent.h"),
    ("component/NavMeshAgentComponent.h", "component/Joint3DComponent.h"),
    ("component/NavMeshLinkComponent.h", "component/Joint3DComponent.h"),
    ("component/NavMeshModifierComponent.h", "component/Joint3DComponent.h"),
    ("component/NavMeshObstacleComponent.h", "component/Joint3DComponent.h"),
    ("component/NavMeshSurfaceComponent.h", "component/Joint3DComponent.h"),
    ("object/navigation/NavGrid.cpp", "object/physics/Joint3D.cpp"),
    ("object/navigation/NavGrid.h", "object/physics/Joint3D.h"),
    ("object/navigation/NavMeshAgent.cpp", "object/physics/Joint3D.cpp"),
    ("object/navigation/NavMeshAgent.h", "object/physics/Joint3D.h"),
    ("object/navigation/NavMeshSurface.cpp", "object/physics/Joint3D.cpp"),
    ("object/navigation/NavMeshSurface.h", "object/physics/Joint3D.h"),
    ("script/binding/NavigationClassesLua.cpp", "script/binding/ProfilerClassesLua.cpp"),
    ("util/MeshTriangles.cpp", "subsystem/MeshSystem.cpp"),
    ("util/MeshTriangles.h", "subsystem/MeshSystem.h"),
]

RECAST_DIR = os.path.join(base.ROOT, "engine", "libs", "recastnavigation")
MODULES = ["Recast", "Detour", "DetourCrowd", "DetourTileCache"]


def recast_files():
    files = []
    for module in MODULES:
        source = os.path.join(RECAST_DIR, module, "Source")
        for name in sorted(os.listdir(source)):
            if name.endswith(".cpp"):
                # relative to engine/core, joining the sibling's group (core/subsystem)
                files.append(("../libs/recastnavigation/%s/Source/%s" % (module, name), "subsystem/MeshSystem.cpp"))
    return files


HEADER_PATHS = ['"$(SRCROOT)/../../libs/recastnavigation/%s/Include",' % module for module in MODULES]
ENGINE_TARGETS = ["jasperengine-ios", "jasperengine-macos"]


def engine_configurations(lines):
    ids = []
    for target in ENGINE_TARGETS:
        anchor = re.compile(r'^\t\t([0-9A-F]{24}) /\* Build configuration list for PBXNativeTarget "' + re.escape(target) + r'" \*/ = \{')
        start = next((i for i, line in enumerate(lines) if anchor.match(line)), None)
        if start is None:
            raise SystemExit("no build configuration list for %s" % target)
        for line in lines[start + 1:start + 8]:
            m = re.match(r"^\t\t\t\t([0-9A-F]{24}) /\* (Debug|Release) \*/,", line)
            if m:
                ids.append(m.group(1))
    return ids


def missing_header_paths(lines):
    """(index of the HEADER_SEARCH_PATHS line, the paths it lacks) per engine configuration"""
    out = []
    for config in engine_configurations(lines):
        start = next(i for i, line in enumerate(lines) if line.startswith("\t\t%s /* " % config) and "= {" in line)
        end = next(i for i in range(start, len(lines)) if lines[i] == "\t\t};")
        header = next((i for i in range(start, end) if lines[i].strip().startswith("HEADER_SEARCH_PATHS = (")), None)
        if header is None:
            raise SystemExit("engine configuration %s has no HEADER_SEARCH_PATHS list" % config)
        close = next(i for i in range(header, end) if lines[i].strip() == ");")
        present = {lines[i].strip() for i in range(header + 1, close)}
        lacking = [path for path in HEADER_PATHS if path not in present]
        if lacking:
            out.append((header, lacking))
    return out


def main():
    check = "--check" in sys.argv
    with open(base.PROJECT, encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")
    files = CORE + recast_files()
    missing = [(relative, sibling) for relative, sibling in files if base.object_id("ref", PREFIX + relative) not in text]
    headers = missing_header_paths(lines)
    if not missing and not headers:
        print("Xcode project lists every S19 file and Recast/Detour's include folders")
        return 0
    if check:
        if missing:
            print("missing from the Xcode project: " + ", ".join(relative for relative, _ in missing))
        if headers:
            print("engine targets without Recast/Detour's include folders: %d configuration(s)" % len(headers))
        return 1
    # header paths first (the file insertions below move line numbers)
    for header, lacking in sorted(headers, reverse=True):
        for path in reversed(lacking):
            lines.insert(header + 1, "\t\t\t\t\t" + path)
    for relative, sibling in missing:
        add_file(lines, relative, sibling)
    with open(base.PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d file(s) and %d header path list(s) to the Xcode project" % (len(missing), len(headers)))
    return 0


def add_file(lines, relative, sibling_relative):
    """tools/xcode_sync_gameplay.py's add_file with this script's id prefix"""
    name = os.path.basename(relative)
    sibling = os.path.basename(sibling_relative)
    group_path = os.path.relpath(relative, os.path.dirname(sibling_relative) or ".").replace(os.sep, "/")
    path = PREFIX + relative
    ref = base.object_id("ref", path)
    reference = re.compile(r"^\t\t([0-9A-F]{24}) /\* " + re.escape(sibling) + r" \*/ = \{isa = PBXFileReference;.*path = "
                           + re.escape(sibling) + ";")
    sibling_ref = next((m.group(1) for m in map(reference.match, lines) if m), None)
    if sibling_ref is None:
        raise SystemExit("the sibling %s is not in the Xcode project" % sibling_relative)
    base.insert_after_line(lines, "%s /* %s */ = {isa = PBXFileReference;" % (sibling_ref, sibling), [
        "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
        % (ref, name, gameplay.file_type(name), group_path)])
    child = "%s /* %s */," % (sibling_ref, sibling)
    for i in [i for i, line in enumerate(lines) if line.strip() == child][::-1]:
        lines.insert(i + 1, "\t\t\t\t%s /* %s */," % (ref, name))
    phase = "Headers" if name.endswith(".h") else "Sources"
    build_file = re.compile(r"^\t\t([0-9A-F]{24}) /\* " + re.escape(sibling) + " in " + phase + r" \*/ = \{isa = PBXBuildFile;")
    builds = [m.group(1) for m in map(build_file.match, lines) if m]
    for index, sibling_build in enumerate(builds):
        build = base.object_id("%s%d" % (phase, index + 1), path)
        base.insert_after_line(lines, "%s /* %s in %s */ = {isa = PBXBuildFile;" % (sibling_build, sibling, phase), [
            "\t\t%s /* %s in %s */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, name, phase, ref, name)])
        entry = "%s /* %s in %s */," % (sibling_build, sibling, phase)
        for i in [i for i, line in enumerate(lines) if line.strip() == entry][::-1]:
            lines.insert(i + 1, "\t\t\t\t%s /* %s in %s */," % (build, name, phase))


if __name__ == "__main__":
    sys.exit(main())
