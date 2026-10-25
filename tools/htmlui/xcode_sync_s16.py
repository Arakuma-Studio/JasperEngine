#!/usr/bin/env python3
# Adds the sources of S16 (HTML UI rendering cost and text sharpness) that the Xcode workspace does
# not list yet: the engine's new HTML UI files (the batched replay, the instanced primitives, the
# distance-field glyphs) and msdfgen's core (engine/libs/msdfgen, compiled into the engine targets
# like meshoptimizer's sources; its headers are included by relative paths, so no header search
# path changes). CMake lists or globs them, the Xcode project lists files explicitly.
#
#   python3 tools/htmlui/xcode_sync_s16.py [--check]
#
# Each .cpp goes into the two Sources phases that compile HtmlUIClock.cpp (the iOS and macOS
# engine targets), each .h of engine/core/htmlui into its Headers phase. Object ids are derived
# from the path ("S16X" + sha1, as xcode_sync_compat.py does), so running it again changes
# nothing. --check exits 1 when files are missing (nothing is written).

import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROJECT = os.path.join(ROOT, "engine", "workspaces", "xcode", "JasperEngine.xcodeproj", "project.pbxproj")
MSDFGEN = os.path.join(ROOT, "engine", "libs", "msdfgen")

HTMLUI_GROUP = "48544D4C0000000000000047"
LIBS_GROUP = "7162FC9625962A0E0075B97D"
FILE_REF_ANCHOR = "48544D4C00000000000002A8 /* HtmlUIClock.cpp */ = {isa = PBXFileReference;"
BUILD_FILE_ANCHOR = "48544D4C00000000000002AA /* HtmlUIClock.cpp in Sources */ = {isa = PBXBuildFile;"
HEADERS_ANCHOR = "48544D4C00000000000002A7 /* HtmlUIClock.h in Headers */,"
SOURCES_ANCHORS = ["48544D4C00000000000002A9 /* HtmlUIClock.cpp in Sources */,",
                   "48544D4C00000000000002AA /* HtmlUIClock.cpp in Sources */,"]

HTMLUI_FILES = ["RmlRendererBatch.cpp", "RmlRendererInternal.h", "HtmlUIPrimitive.h", "RmlFontMsdf.cpp", "RmlFontMsdf.h"]


def object_id(kind, path):
    return "53313658" + hashlib.sha1((kind + ":" + path).encode()).hexdigest()[:16].upper()


def insert_after_line(lines, anchor, new_lines):
    for i, line in enumerate(lines):
        if anchor in line:
            lines[i + 1:i + 1] = new_lines
            return
    raise SystemExit("anchor not found in project.pbxproj: " + anchor)


def group_children_end(lines, group_id):
    # index of the closing ");" of a group's children
    for i, line in enumerate(lines):
        if line.strip().startswith(group_id + " /*") and line.rstrip().endswith("= {"):
            for j in range(i, len(lines)):
                if lines[j].strip() == "children = (":
                    for k in range(j, len(lines)):
                        if lines[k].strip() == ");":
                            return k
    raise SystemExit("group not found in project.pbxproj: " + group_id)


def msdfgen_files():
    files = ["msdfgen.h", "msdfgen-config.h"]
    for name in sorted(os.listdir(os.path.join(MSDFGEN, "core"))):
        if name.endswith((".cpp", ".h", ".hpp")):
            files.append("core/" + name)
    return files


def wanted():
    # (key, group key, file name, is source)
    entries = [("htmlui/" + name, "htmlui", name, name.endswith(".cpp")) for name in HTMLUI_FILES]
    for path in msdfgen_files():
        group = "msdfgen/core" if path.startswith("core/") else "msdfgen"
        entries.append(("msdfgen/" + path, group, path.split("/")[-1], path.endswith(".cpp")))
    return entries


def main():
    check = "--check" in sys.argv
    with open(PROJECT, encoding="utf-8") as f:
        text = f.read()
    missing = [entry for entry in wanted() if object_id("ref", entry[0]) not in text]
    if not missing:
        print("Xcode project lists every S16 file")
        return 0
    if check:
        print("missing from the Xcode project: " + ", ".join(entry[0] for entry in missing))
        return 1
    lines = text.split("\n")

    # the msdfgen groups: libs/msdfgen and libs/msdfgen/core
    groups = {"htmlui": HTMLUI_GROUP}
    for key, name, parent in (("msdfgen", "msdfgen", LIBS_GROUP), ("msdfgen/core", "core", None)):
        gid = object_id("group", key)
        groups[key] = gid
        if gid in text:
            continue
        block = [
            "\t\t%s /* %s */ = {" % (gid, name),
            "\t\t\tisa = PBXGroup;",
            "\t\t\tchildren = (",
            "\t\t\t);",
            "\t\t\tpath = %s;" % name,
            "\t\t\tsourceTree = \"<group>\";",
            "\t\t};",
        ]
        index = next(i for i, line in enumerate(lines) if (HTMLUI_GROUP + " /* htmlui */ = {") in line)
        lines[index:index] = block
        parent_id = parent if parent else groups["msdfgen"]
        end = group_children_end(lines, parent_id)
        lines.insert(end, "\t\t\t\t%s /* %s */," % (gid, name))
        text = "\n".join(lines)

    for key, group, name, source in missing:
        ref = object_id("ref", key)
        is_header = not source
        file_type = "sourcecode.cpp.h" if name.endswith(".hpp") else ("sourcecode.c.h" if is_header else "sourcecode.cpp.cpp")
        insert_after_line(lines, FILE_REF_ANCHOR, [
            "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
            % (ref, name, file_type, name)])
        end = group_children_end(lines, groups[group])
        lines.insert(end, "\t\t\t\t%s /* %s */," % (ref, name))
        if source:
            for index, anchor in enumerate(SOURCES_ANCHORS):
                build = object_id("src%d" % (index + 1), key)
                insert_after_line(lines, BUILD_FILE_ANCHOR, [
                    "\t\t%s /* %s in Sources */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, name, ref, name)])
                insert_after_line(lines, anchor, ["\t\t\t\t%s /* %s in Sources */," % (build, name)])
        elif group == "htmlui":
            build = object_id("hdr", key)
            insert_after_line(lines, BUILD_FILE_ANCHOR, [
                "\t\t%s /* %s in Headers */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, name, ref, name)])
            insert_after_line(lines, HEADERS_ANCHOR, ["\t\t\t\t%s /* %s in Headers */," % (build, name)])

    with open(PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d S16 file(s) to the Xcode project" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
