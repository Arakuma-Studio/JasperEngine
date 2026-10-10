#!/usr/bin/env python3
# Keeps the rmlui-ios / rmlui-macos targets of the Xcode workspace in step with the vendored
# RmlUi fork (engine/libs/rmlui): CMake globs the sources, the Xcode project lists them. Run it
# after a fork patch adds .cpp files (phase 3 and later):
#
#   python3 tools/htmlui/xcode_sync_rmlui.py [--check]
#
# Each missing .cpp of Source/Core, Source/Core/Elements, Source/Core/Layout, Source/Debugger
# and Source/SVG gets a file reference in its group and a build file in each of the two
# Sources phases of the rmlui targets. Object ids are derived from the file path ("RMLU" +
# sha1), so two branches that add different files produce different ids and a merge only has
# to keep both lines. It also makes every configuration that defines RMLUI_STATIC_LIB define
# RMLUI_MATH_EXPRESSIONS (calc(), docs/html-ui/FORK.md: it changes the layout of RmlUi's
# ComputedValues, so the library and everything including its headers must agree). --check
# exits 1 when something is missing (nothing is written).

import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RMLUI = os.path.join(ROOT, "engine", "libs", "rmlui")
PROJECT = os.path.join(ROOT, "engine", "workspaces", "xcode", "JasperEngine.xcodeproj", "project.pbxproj")

# directory (relative to engine/libs/rmlui) -> its PBXGroup
GROUPS = {
    "Source/Core": "48544D4C00000000000001EF",
    "Source/Core/Elements": "48544D4C000000000000023E",
    "Source/Core/Layout": "48544D4C0000000000000272",
    "Source/Debugger": "48544D4C000000000000028E",
    "Source/SVG": "505645430000000000000162",
}
# a build file of each of the two Sources phases of the rmlui targets (Variant.cpp's)
SOURCES_ANCHORS = ["48544D4C00000000000001D5 /* Variant.cpp in Sources */,",
                   "48544D4C00000000000001D6 /* Variant.cpp in Sources */,"]
FILE_REF_ANCHOR = "48544D4C00000000000001D4 /* Variant.cpp */ = {isa = PBXFileReference;"
BUILD_FILE_ANCHOR = "48544D4C00000000000001D6 /* Variant.cpp in Sources */ = {isa = PBXBuildFile;"
DEFINES = ["RMLUI_MATH_EXPRESSIONS"]


def object_id(kind, path):
    return "524D4C55" + hashlib.sha1((kind + ":" + path).encode()).hexdigest()[:16].upper()


def group_children(lines, group_id):
    # indexes of the "children = (" line of a group and of its closing ");"
    for i, line in enumerate(lines):
        if line.strip().startswith(group_id + " /*") and line.rstrip().endswith("= {"):
            for j in range(i, len(lines)):
                if lines[j].strip() == "children = (":
                    for k in range(j, len(lines)):
                        if lines[k].strip() == ");":
                            return j, k
    raise SystemExit("group not found in project.pbxproj: " + group_id)


def insert_after_line(lines, anchor, new_lines):
    for i, line in enumerate(lines):
        if anchor in line:
            lines[i + 1:i + 1] = new_lines
            return
    raise SystemExit("anchor not found in project.pbxproj: " + anchor)


def main():
    check = "--check" in sys.argv
    with open(PROJECT, encoding="utf-8") as f:
        lines = f.read().split("\n")

    missing = []
    for directory, group in GROUPS.items():
        begin, end = group_children(lines, group)
        listed = "\n".join(lines[begin:end])
        for name in sorted(os.listdir(os.path.join(RMLUI, directory))):
            if name.endswith(".cpp") and ("/* " + name + " */") not in listed:
                missing.append((directory, group, name))

    missing_defines = []
    for i, line in enumerate(lines):
        if line.strip() == "RMLUI_STATIC_LIB,":
            following = [l.strip() for l in lines[i + 1:i + 1 + len(DEFINES)]]
            for define in DEFINES:
                if define + "," not in following:
                    missing_defines.append((i, define))

    if not missing and not missing_defines:
        print("Xcode project lists every rmlui source and define")
        return 0
    if check:
        for directory, _, name in missing:
            print("missing from the Xcode project: " + directory + "/" + name)
        for i, define in missing_defines:
            print("missing define " + define + " after line " + str(i + 1))
        return 1

    # defines first (line numbers of the other edits are found by anchors)
    for i, define in reversed(missing_defines):
        indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
        lines.insert(i + 1, indent + define + ",")

    for directory, group, name in missing:
        path = directory + "/" + name
        ref = object_id("ref", path)
        builds = [object_id("build-ios", path), object_id("build-macos", path)]
        insert_after_line(lines, FILE_REF_ANCHOR, [
            "\t\t" + ref + " /* " + name + " */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = sourcecode.cpp.cpp; path = " +
            name + "; sourceTree = \"<group>\"; };"])
        insert_after_line(lines, BUILD_FILE_ANCHOR, [
            "\t\t" + build + " /* " + name + " in Sources */ = {isa = PBXBuildFile; fileRef = " + ref + " /* " + name + " */; };"
            for build in builds])
        for anchor, build in zip(SOURCES_ANCHORS, builds):
            insert_after_line(lines, anchor, ["\t\t\t\t" + build + " /* " + name + " in Sources */,"])
        begin, end = group_children(lines, group)
        lines.insert(end, "\t\t\t\t" + ref + " /* " + name + " */,")
        print("added " + path)

    with open(PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
