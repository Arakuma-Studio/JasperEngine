#!/usr/bin/env python3
# Adds the triangle picking sources (engine/core/picking, S26) that the Xcode workspace does not
# list yet (engine/CMakeLists.txt lists them, the Xcode project lists files explicitly):
#
#   python3 tools/xcode_sync_picking.py [--check]
#
# A core/picking group, each .cpp in the two Sources phases that compile HtmlUIClock.cpp (the iOS
# and macOS engine targets) with -ffp-contract=off (as CMake builds them: every SIMD path of the
# BVH slab test does the same arithmetic), each .h in its Headers phase. Object ids are derived
# from the file path (tools/htmlui/xcode_sync_compat.py's object_id), so they never collide.
# --check exits 1 when files are missing (nothing is written).

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "htmlui"))
import xcode_sync_compat as base  # noqa: E402

PICKING = os.path.join(base.ROOT, "engine", "core", "picking")
CORE_GROUP = "7162FC2A25962A0E0075B97D"
GROUP = base.object_id("group", "@picking")


def picking_files():
    return sorted(name for name in os.listdir(PICKING) if name.endswith(".cpp") or name.endswith(".h"))


def main():
    check = "--check" in sys.argv
    with open(base.PROJECT, encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")

    missing = [name for name in picking_files() if base.object_id("ref", "picking/" + name) not in text]
    if not missing:
        print("Xcode project lists every picking file")
        return 0
    if check:
        print("missing from the Xcode project: " + ", ".join(missing))
        return 1

    if GROUP not in text:
        block = [
            "\t\t%s /* picking */ = {" % GROUP,
            "\t\t\tisa = PBXGroup;",
            "\t\t\tchildren = (",
            "\t\t\t);",
            "\t\t\tpath = picking;",
            "\t\t\tsourceTree = \"<group>\";",
            "\t\t};",
        ]
        index = next(i for i, line in enumerate(lines) if base.HTMLUI_GROUP in line)
        lines[index:index] = block
        span = base.group_block_end(lines, CORE_GROUP)
        if span is None:
            raise SystemExit("the core group was not found")
        lines.insert(span[1], "\t\t\t\t%s /* picking */," % GROUP)

    for name in missing:
        path = "picking/" + name
        ref = base.object_id("ref", path)
        is_header = name.endswith(".h")
        file_type = "sourcecode.c.h" if is_header else "sourcecode.cpp.cpp"
        base.insert_after_line(lines, base.FILE_REF_ANCHOR, [
            "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
            % (ref, name, file_type, name)])
        span = base.group_block_end(lines, GROUP)
        lines.insert(span[1], "\t\t\t\t%s /* %s */," % (ref, name))
        if is_header:
            build = base.object_id("hdr", path)
            base.insert_after_line(lines, base.BUILD_FILE_ANCHOR, [
                "\t\t%s /* %s in Headers */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, name, ref, name)])
            base.insert_after_line(lines, base.HEADERS_ANCHOR, ["\t\t\t\t%s /* %s in Headers */," % (build, name)])
        else:
            for index, anchor in enumerate(base.SOURCES_ANCHORS):
                build = base.object_id("src%d" % (index + 1), path)
                base.insert_after_line(lines, base.BUILD_FILE_ANCHOR, [
                    "\t\t%s /* %s in Sources */ = {isa = PBXBuildFile; fileRef = %s /* %s */; settings = {COMPILER_FLAGS = \"-ffp-contract=off\"; }; };"
                    % (build, name, ref, name)])
                base.insert_after_line(lines, anchor, ["\t\t\t\t%s /* %s in Sources */," % (build, name)])

    with open(base.PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d picking file(s) to the Xcode project" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
