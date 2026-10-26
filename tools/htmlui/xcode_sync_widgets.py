#!/usr/bin/env python3
# Adds the sources of the HTML UI widgets (S17) that the Xcode workspace does not list yet, as
# tools/htmlui/xcode_sync_effects.py does for the effects (CMake globs them, the Xcode project
# lists files explicitly):
#
#   python3 tools/htmlui/xcode_sync_widgets.py [--check]
#
# - engine/core/htmlui/widgets (and its Markdown folder) under an htmlui/widgets group, each .cpp
#   in the two Sources phases that compile HtmlUIClock.cpp, each .h in its Headers phase;
# - the files S17 added next to existing ones (the Lua binding, the patch reload, the object/ui
#   headers): each goes into the groups and build phases of a sibling file.
# The compat layer's new files (TreeReconcile) are xcode_sync_compat.py's. Object ids are derived
# from the file path ("COMP" + sha1 of "widgets/<file>"), so they never collide with the others.
# --check exits 1 when files are missing (nothing is written).

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xcode_sync_compat as base  # noqa: E402

WIDGETS = os.path.join(base.ROOT, "engine", "core", "htmlui", "widgets")
GROUP = base.object_id("group", "@widgets")

# (file, the sibling whose groups and build phases it joins)
SINGLES = [
    ("script/binding/HtmlUIWidgetsLua.cpp", "HtmlUIClassesLua.cpp"),
    ("subsystem/UIDocumentSystemPatch.cpp", "UIDocumentSystemReload.cpp"),
    ("object/ui/UIItemsView.h", "UIDocument.h"),
    ("object/ui/UIDataTransfer.h", "UIDocument.h"),
    ("object/ui/UIReloadReport.h", "UIDocument.h"),
]


def widget_files():
    files = []
    for directory, _, names in os.walk(WIDGETS):
        for name in names:
            if name.endswith(".cpp") or name.endswith(".h"):
                files.append(os.path.relpath(os.path.join(directory, name), WIDGETS).replace(os.sep, "/"))
    return sorted(files)


def file_type(name):
    return "sourcecode.c.h" if name.endswith(".h") else "sourcecode.cpp.cpp"


def add_widgets(lines, missing):
    text = "\n".join(lines)
    if GROUP not in text:
        block = [
            "\t\t%s /* widgets */ = {" % GROUP,
            "\t\t\tisa = PBXGroup;",
            "\t\t\tchildren = (",
            "\t\t\t);",
            "\t\t\tpath = widgets;",
            "\t\t\tsourceTree = \"<group>\";",
            "\t\t};",
        ]
        index = next(i for i, line in enumerate(lines) if base.HTMLUI_GROUP in line)
        lines[index:index] = block
        span = base.group_block_end(lines, "48544D4C0000000000000047")
        if span is None:
            raise SystemExit("the htmlui group was not found")
        lines.insert(span[1], "\t\t\t\t%s /* widgets */," % GROUP)

    for relative in missing:
        path = "widgets/" + relative
        name = os.path.basename(relative)
        ref = base.object_id("ref", path)
        base.insert_after_line(lines, base.FILE_REF_ANCHOR, [
            "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
            % (ref, name, file_type(name), relative)])
        span = base.group_block_end(lines, GROUP)
        lines.insert(span[1], "\t\t\t\t%s /* %s */," % (ref, name))
        if name.endswith(".h"):
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


def add_single(lines, relative, sibling):
    name = os.path.basename(relative)
    path = "s17/" + relative
    ref = base.object_id("ref", path)
    reference = re.compile(r"^\t\t([0-9A-F]{24}) /\* " + re.escape(sibling) + r" \*/ = \{isa = PBXFileReference;")
    sibling_ref = next((m.group(1) for m in map(reference.match, lines) if m), None)
    if sibling_ref is None:
        raise SystemExit("the sibling %s is not in the Xcode project" % sibling)
    base.insert_after_line(lines, "%s /* %s */ = {isa = PBXFileReference;" % (sibling_ref, sibling), [
        "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
        % (ref, name, file_type(name), name)])
    # the groups listing the sibling
    child = "%s /* %s */," % (sibling_ref, sibling)
    for i in [i for i, line in enumerate(lines) if line.strip() == child][::-1]:
        lines.insert(i + 1, "\t\t\t\t%s /* %s */," % (ref, name))
    # the sibling's build files and the phases listing them
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


def main():
    check = "--check" in sys.argv
    with open(base.PROJECT, encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")

    missing = [relative for relative in widget_files() if base.object_id("ref", "widgets/" + relative) not in text]
    singles = [(relative, sibling) for relative, sibling in SINGLES if base.object_id("ref", "s17/" + relative) not in text]
    if not missing and not singles:
        print("Xcode project lists every widgets file")
        return 0
    if check:
        print("missing from the Xcode project: " + ", ".join(missing + [relative for relative, _ in singles]))
        return 1

    if missing:
        add_widgets(lines, missing)
    for relative, sibling in singles:
        add_single(lines, relative, sibling)

    with open(base.PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d widgets file(s) and %d other file(s) to the Xcode project" % (len(missing), len(singles)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
