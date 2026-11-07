#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# Lists the terrain engine's sources in the Xcode workspace (CMake lists them in
# engine/core/terrain/TerrainSources.cmake; the Xcode project lists files explicitly):
#
#   python3 tools/terrain/xcode_sync_terrain.py [--check]
#
# Each .cpp goes into the two Sources phases that compile Terrain.cpp (the iOS and macOS engine
# targets), each .h into its Headers phase, under the group of its directory (made when missing).
# The folder's sources get -ffp-contract=off, as CMake gives them (docs/specs/T1-terrain-authoring.md
# R8). Object ids are derived from the file path ("TRRN" + sha1), so a merge with another branch's
# additions only has to keep both lines. --check exits 1 when something is missing.

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROJECT = os.path.join(ROOT, "engine", "workspaces", "xcode", "JasperEngine.xcodeproj", "project.pbxproj")
TERRAIN_DIR = os.path.join(ROOT, "engine", "core", "terrain")

CORE_GROUP = "7162FC2A25962A0E0075B97D"
FILE_REF_ANCHOR = "715CB115288CD40E0049344B /* Terrain.cpp */ = {isa = PBXFileReference;"
BUILD_FILE_ANCHOR = "715CB118288CD40E0049344B /* Terrain.cpp in Sources */ = {isa = PBXBuildFile;"
HEADERS_ANCHOR = "715CB119288CD40E0049344B /* Terrain.h in Headers */,"
SOURCES_ANCHORS = ["715CB117288CD40E0049344B /* Terrain.cpp in Sources */,",
                   "715CB118288CD40E0049344B /* Terrain.cpp in Sources */,"]

# engine/core-relative files outside the folder
FILES = ["object/TerrainEdit.cpp", "object/TerrainEdit.h", "subsystem/MeshSystemTerrainEdit.cpp",
         "physics/JoltTerrainRefresh.cpp", "physics/JoltTerrainRefresh.h", "script/binding/TerrainClassesLua.cpp",
         "object/TerrainOpsJson.cpp", "object/TerrainOpsJson.h"]


def terrain_files():
    names = sorted(n for n in os.listdir(TERRAIN_DIR) if n.endswith((".cpp", ".h")))
    return ["terrain/" + n for n in names]


def object_id(kind, path):
    return "5452524E" + hashlib.sha1((kind + ":" + path).encode()).hexdigest()[:16].upper()


def insert_after_line(lines, anchor, new_lines):
    for i, line in enumerate(lines):
        if anchor in line:
            lines[i + 1:i + 1] = new_lines
            return
    raise SystemExit("anchor not found in project.pbxproj: " + anchor)


def group_span(lines, group_id):
    # indexes of a group's "children = (" line and of its closing ");"
    for i, line in enumerate(lines):
        if line.strip().startswith(group_id + " /*") and line.rstrip().endswith("= {"):
            for j in range(i, len(lines)):
                if lines[j].strip() == "children = (":
                    for k in range(j, len(lines)):
                        if lines[k].strip() == ");":
                            return j, k
    return None


def child_group(lines, parent, name):
    span = group_span(lines, parent)
    if span is None:
        raise SystemExit("group not found: " + parent)
    for line in lines[span[0] + 1:span[1]]:
        m = re.match(r"\s*([0-9A-F]{24}) /\* (.+) \*/,", line)
        if m and m.group(2) == name and group_span(lines, m.group(1)) is not None:
            return m.group(1)
    return None


def ensure_group(lines, directory):
    # the group of an engine/core-relative directory, made when missing
    parent = CORE_GROUP
    walked = []
    for part in [p for p in directory.split("/") if p]:
        walked.append(part)
        gid = child_group(lines, parent, part)
        if gid is None:
            gid = object_id("group", "/".join(walked))
            block = ["\t\t%s /* %s */ = {" % (gid, part), "\t\t\tisa = PBXGroup;", "\t\t\tchildren = (", "\t\t\t);",
                     "\t\t\tpath = %s;" % part, "\t\t\tsourceTree = \"<group>\";", "\t\t};"]
            index = next(i for i, line in enumerate(lines) if line.strip().startswith(CORE_GROUP + " /* core */ = {"))
            lines[index:index] = block
            span = group_span(lines, parent)
            lines.insert(span[1], "\t\t\t\t%s /* %s */," % (gid, part))
        parent = gid
    return parent


def add_file(lines, path):
    directory, name = os.path.dirname(path), os.path.basename(path)
    gid = ensure_group(lines, directory)
    ref = object_id("ref", path)
    is_header = name.endswith(".h")
    file_type = "sourcecode.c.h" if is_header else "sourcecode.cpp.cpp"
    insert_after_line(lines, FILE_REF_ANCHOR, [
        "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
        % (ref, name, file_type, name)])
    span = group_span(lines, gid)
    lines.insert(span[1], "\t\t\t\t%s /* %s */," % (ref, name))
    if is_header:
        build = object_id("hdr", path)
        insert_after_line(lines, BUILD_FILE_ANCHOR, [
            "\t\t%s /* %s in Headers */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, name, ref, name)])
        insert_after_line(lines, HEADERS_ANCHOR, ["\t\t\t\t%s /* %s in Headers */," % (build, name)])
        return
    settings = ' settings = {COMPILER_FLAGS = "-ffp-contract=off"; };' if path.startswith("terrain/") else ""
    for index, anchor in enumerate(SOURCES_ANCHORS):
        build = object_id("src%d" % (index + 1), path)
        insert_after_line(lines, BUILD_FILE_ANCHOR, [
            "\t\t%s /* %s in Sources */ = {isa = PBXBuildFile; fileRef = %s /* %s */;%s };" % (build, name, ref, name, settings)])
        insert_after_line(lines, anchor, ["\t\t\t\t%s /* %s in Sources */," % (build, name)])


def main():
    check = "--check" in sys.argv
    with open(PROJECT, encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")
    missing = [path for path in terrain_files() + FILES if object_id("ref", path) not in text]
    if not missing:
        print("Xcode project lists every terrain file")
        return 0
    if check:
        print("missing from the Xcode project: " + ", ".join(missing))
        return 1
    for path in missing:
        add_file(lines, path)
    with open(PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d terrain file(s) to the Xcode project" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
