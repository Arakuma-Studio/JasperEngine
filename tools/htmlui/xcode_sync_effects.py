#!/usr/bin/env python3
# Adds every source of the HTML UI effects (engine/core/htmlui/effects, phase 5) that the Xcode
# workspace does not list yet, as tools/htmlui/xcode_sync_compat.py does for the compatibility
# layer (CMake lists them, the Xcode project lists files explicitly):
#
#   python3 tools/htmlui/xcode_sync_effects.py [--check]
#
# Each .cpp goes into the two Sources phases that compile HtmlUIClock.cpp (the iOS and macOS
# engine targets), each .h into its Headers phase, under an htmlui/effects group. Object ids are
# derived from the file path ("COMP" + sha1 of "effects/<file>"), so they never collide with the
# compat layer's. --check exits 1 when files are missing (nothing is written).

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xcode_sync_compat as base  # noqa: E402

EFFECTS = os.path.join(base.ROOT, "engine", "core", "htmlui", "effects")
GROUP = base.object_id("group", "@effects")


def effect_files():
    return sorted(name for name in os.listdir(EFFECTS) if name.endswith(".cpp") or name.endswith(".h"))


def main():
    check = "--check" in sys.argv
    with open(base.PROJECT, encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")

    missing = [name for name in effect_files() if base.object_id("ref", "effects/" + name) not in text]
    if not missing:
        print("Xcode project lists every effects file")
        return 0
    if check:
        print("missing from the Xcode project: " + ", ".join(missing))
        return 1

    if GROUP not in text:
        block = [
            "\t\t%s /* effects */ = {" % GROUP,
            "\t\t\tisa = PBXGroup;",
            "\t\t\tchildren = (",
            "\t\t\t);",
            "\t\t\tpath = effects;",
            "\t\t\tsourceTree = \"<group>\";",
            "\t\t};",
        ]
        index = next(i for i, line in enumerate(lines) if base.HTMLUI_GROUP in line)
        lines[index:index] = block
        span = base.group_block_end(lines, "48544D4C0000000000000047")
        if span is None:
            raise SystemExit("the htmlui group was not found")
        lines.insert(span[1], "\t\t\t\t%s /* effects */," % GROUP)

    for name in missing:
        path = "effects/" + name
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
                    "\t\t%s /* %s in Sources */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, name, ref, name)])
                base.insert_after_line(lines, anchor, ["\t\t\t\t%s /* %s in Sources */," % (build, name)])

    with open(base.PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d effects file(s) to the Xcode project" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
