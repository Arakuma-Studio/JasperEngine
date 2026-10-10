#!/usr/bin/env python3
# Adds every source of the HTML/CSS compatibility layer (engine/core/htmlui/compat) that the
# Xcode workspace does not list yet: CMake globs that directory, the Xcode project lists files
# explicitly. Run it after adding files there (phase-2 packages):
#
#   python3 tools/htmlui/xcode_sync_compat.py [--check]
#
# Each .cpp goes into the two Sources phases that compile HtmlUIClock.cpp (the iOS and macOS
# engine targets), each .h into its Headers phase, all under htmlui/compat/<subdirectory>
# groups. Object ids are derived from the file path ("COMP" + sha1), so two branches that add
# different files produce different ids and a merge only has to keep both lines. --check
# exits 1 when files are missing (nothing is written).

import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COMPAT = os.path.join(ROOT, "engine", "core", "htmlui", "compat")
PROJECT = os.path.join(ROOT, "engine", "workspaces", "xcode", "JasperEngine.xcodeproj", "project.pbxproj")

# anchors: the entries of HtmlUIClock (phase 1), which every engine target has
HTMLUI_GROUP = "48544D4C0000000000000047 /* htmlui */ = {"
FILE_REF_ANCHOR = "48544D4C00000000000002A8 /* HtmlUIClock.cpp */ = {isa = PBXFileReference;"
BUILD_FILE_ANCHOR = "48544D4C00000000000002AA /* HtmlUIClock.cpp in Sources */ = {isa = PBXBuildFile;"
HEADERS_ANCHOR = "48544D4C00000000000002A7 /* HtmlUIClock.h in Headers */,"
SOURCES_ANCHORS = ["48544D4C00000000000002A9 /* HtmlUIClock.cpp in Sources */,",
                   "48544D4C00000000000002AA /* HtmlUIClock.cpp in Sources */,"]


def object_id(kind, path):
    return "434F4D50" + hashlib.sha1((kind + ":" + path).encode()).hexdigest()[:16].upper()


def compat_files():
    files = []
    for directory, _, names in os.walk(COMPAT):
        for name in names:
            if name.endswith(".cpp") or name.endswith(".h"):
                files.append(os.path.relpath(os.path.join(directory, name), COMPAT).replace(os.sep, "/"))
    return sorted(files)


def insert_after_line(lines, anchor, new_lines):
    for i, line in enumerate(lines):
        if anchor in line:
            lines[i + 1:i + 1] = new_lines
            return
    raise SystemExit("anchor not found in project.pbxproj: " + anchor)


def group_block_end(lines, group_id):
    # index of the "children = (" line of a group and of its closing ");"
    for i, line in enumerate(lines):
        if line.strip().startswith(group_id + " /*") and line.rstrip().endswith("= {"):
            for j in range(i, len(lines)):
                if lines[j].strip() == "children = (":
                    for k in range(j, len(lines)):
                        if lines[k].strip() == ");":
                            return j, k
    return None


def main():
    check = "--check" in sys.argv
    with open(PROJECT, encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")

    missing = [path for path in compat_files() if object_id("ref", path) not in text]
    if not missing:
        print("Xcode project lists every compat file")
        return 0
    if check:
        print("missing from the Xcode project: " + ", ".join(missing))
        return 1

    # groups: compat, compat/<dir>, ...
    def group_id(directory):
        return object_id("group", directory)

    needed_groups = set()
    for path in missing:
        parts = path.split("/")[:-1]
        for depth in range(len(parts) + 1):
            needed_groups.add("/".join(parts[:depth]))
    for directory in sorted(needed_groups, key=lambda d: (d.count("/"), d)):
        gid = group_id(directory)
        if gid in text:
            continue
        name = directory.split("/")[-1] if directory else "compat"
        block = [
            "\t\t%s /* %s */ = {" % (gid, name),
            "\t\t\tisa = PBXGroup;",
            "\t\t\tchildren = (",
            "\t\t\t);",
            "\t\t\tpath = %s;" % name,
            "\t\t\tsourceTree = \"<group>\";",
            "\t\t};",
        ]
        index = next(i for i, line in enumerate(lines) if HTMLUI_GROUP in line)
        lines[index:index] = block
        # into its parent group
        parent = "48544D4C0000000000000047" if not directory else group_id("/".join(directory.split("/")[:-1]))
        span = group_block_end(lines, parent)
        if span is None:
            raise SystemExit("group not found: " + parent)
        lines.insert(span[1], "\t\t\t\t%s /* %s */," % (gid, name))
        text = "\n".join(lines)

    for path in missing:
        name = path.split("/")[-1]
        directory = "/".join(path.split("/")[:-1])
        ref = object_id("ref", path)
        is_header = name.endswith(".h")
        file_type = "sourcecode.c.h" if is_header else "sourcecode.cpp.cpp"
        insert_after_line(lines, FILE_REF_ANCHOR, [
            "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
            % (ref, name, file_type, name)])
        span = group_block_end(lines, group_id(directory))
        lines.insert(span[1], "\t\t\t\t%s /* %s */," % (ref, name))
        if is_header:
            build = object_id("hdr", path)
            insert_after_line(lines, BUILD_FILE_ANCHOR, [
                "\t\t%s /* %s in Headers */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, name, ref, name)])
            insert_after_line(lines, HEADERS_ANCHOR, ["\t\t\t\t%s /* %s in Headers */," % (build, name)])
        else:
            for index, anchor in enumerate(SOURCES_ANCHORS):
                build = object_id("src%d" % (index + 1), path)
                insert_after_line(lines, BUILD_FILE_ANCHOR, [
                    "\t\t%s /* %s in Sources */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, name, ref, name)])
                insert_after_line(lines, anchor, ["\t\t\t\t%s /* %s in Sources */," % (build, name)])

    with open(PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d compat file(s) to the Xcode project" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
