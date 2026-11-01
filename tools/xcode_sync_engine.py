#!/usr/bin/env python3
# Keeps the Xcode workspace's engine targets in step with the engine's CMake source list.
#
#   python3 tools/xcode_sync_engine.py           # add what is missing
#   python3 tools/xcode_sync_engine.py --check   # exit 1 and list what is missing (writes nothing)
#
# CMake lists the engine sources explicitly (engine/CMakeLists.txt, JASPERENGINE_SRCS), the Xcode
# project (engine/workspaces/xcode/JasperEngine.xcodeproj, used by the macOS and iOS exports) lists
# its own files: a source added to one only fails the Apple builds at link time. This compares
# every core/**/*.cpp of JASPERENGINE_SRCS (and every .h of their directories) with the project's group tree
# and adds a missing file: a PBXFileReference in the group of its directory (created when
# missing), and for a .cpp one PBXBuildFile in each engine Sources phase (the phases that compile
# core/subsystem/ActionSystem.cpp: jasperengine-ios and jasperengine-macos), for a .h one in the
# Headers phase that has ActionSystem.h. Object ids are "4A534E47" + 16 hex of the sha1 of the
# kind and path: deterministic, and two branches adding different files do not collide.
#
# Only core/ is compared (and APPLE_ENGINE_SOURCES): the libraries (libs/) and the HTML UI compat
# layer have their own sync scripts (tools/htmlui/xcode_sync_*.py) and their own source lists.

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CMAKE = os.path.join(ROOT, "engine", "CMakeLists.txt")
PROJECT = os.path.join(ROOT, "engine", "workspaces", "xcode", "JasperEngine.xcodeproj", "project.pbxproj")
ENGINE = os.path.join(ROOT, "engine")

# a .cpp of the engine both engine targets compile, and its header (anchors of the phases)
ANCHOR_SOURCE = "core/subsystem/ActionSystem.cpp"
ANCHOR_HEADER = "core/subsystem/ActionSystem.h"

# header-only directories (no .cpp of theirs in the CMake list)
HEADER_DIRECTORIES = ["core/component"]

# platform sources outside JASPERENGINE_SRCS that both Apple engine targets compile (CMake picks
# them per platform): the HTTP client's NSURLSession backend
APPLE_ENGINE_SOURCES = ["platform/apple/HttpBackendApple.mm"]

OBJECT_RE = re.compile(r"^\t\t([0-9A-F]{24})(?: /\* (.*?) \*/)? = \{(.*)$")


def object_id(kind, path):
    return "4A534E47" + hashlib.sha1((kind + ":" + path).encode()).hexdigest()[:16].upper()


def cmake_sources():
    with open(CMAKE, encoding="utf-8") as f:
        # (comments dropped: a ")" in one must not end the list)
        text = re.sub(r"#[^\n]*", "", f.read())
    start = text.index("set(JASPERENGINE_SRCS")
    end = text.index(")", start)
    sources = []
    for token in text[start + len("set(JASPERENGINE_SRCS"):end].split():
        if token.startswith("core/") and token.endswith(".cpp"):
            sources.append(token)
    return sources


def unquote(value):
    value = value.strip()
    if value.startswith('"') and value.endswith('"'):
        value = value[1:-1]
    return value


def parse_project(lines):
    """objects: id -> {isa, name, path, children, files, fileRef, line, end}"""
    objects = {}
    i = 0
    while i < len(lines):
        m = OBJECT_RE.match(lines[i])
        if not m:
            i += 1
            continue
        oid, comment, rest = m.group(1), m.group(2), m.group(3)
        obj = {"comment": comment, "line": i, "children": [], "files": []}
        if rest.rstrip().endswith("};"):
            body = rest
            obj["end"] = i
        else:
            j = i + 1
            while j < len(lines) and lines[j] != "\t\t};":
                j += 1
            body = "\n".join(lines[i + 1:j])
            obj["end"] = j
        for key in ("isa", "path", "name", "sourceTree"):
            km = re.search(r"\b" + key + r" = (\"[^\"]*\"|[^;\s]+);", body)
            if km:
                obj[key] = unquote(km.group(1))
        fm = re.search(r"\bfileRef = ([0-9A-F]{24})", body)
        if fm:
            obj["fileRef"] = fm.group(1)
        for list_key, out in (("children", "children"), ("files", "files")):
            lm = re.search(list_key + r" = \((.*?)\);", body, re.S)
            if lm:
                obj[out] = re.findall(r"([0-9A-F]{24}) /\*", lm.group(1))
        objects[oid] = obj
        i = obj["end"] + 1
    return objects


def file_paths(objects, lines):
    """file reference id -> path relative to the engine directory (None when outside)"""
    main = None
    for line in lines:
        m = re.search(r"mainGroup = ([0-9A-F]{24});", line)
        if m:
            main = m.group(1)
    project_dir = os.path.dirname(os.path.dirname(PROJECT))   # the .xcodeproj's directory
    result = {}
    parent_dir = {}

    def walk(gid, directory):
        obj = objects.get(gid)
        if not obj:
            return
        path = obj.get("path")
        tree = obj.get("sourceTree", "<group>")
        if path is not None:
            if tree == "<group>":
                directory = os.path.normpath(os.path.join(directory, path))
            elif tree == "SOURCE_ROOT":
                directory = os.path.normpath(os.path.join(project_dir, path))
            else:
                directory = None
        if obj.get("isa") == "PBXFileReference":
            if directory is not None:
                rel = os.path.relpath(directory, ENGINE).replace(os.sep, "/")
                result[gid] = rel
            return
        parent_dir[gid] = directory
        for child in obj["children"]:
            if directory is not None:
                walk(child, directory)

    walk(main, project_dir)
    return result, parent_dir


def phase_containing(objects, file_ref, isa):
    phases = []
    for pid, obj in objects.items():
        if obj.get("isa") != isa:
            continue
        for build in obj["files"]:
            if objects.get(build, {}).get("fileRef") == file_ref:
                phases.append(pid)
                break
    return phases


def analyse():
    with open(PROJECT, encoding="utf-8") as f:
        lines = f.read().split("\n")
    objects = parse_project(lines)
    paths, group_dirs = file_paths(objects, lines)
    by_path = {}
    for ref, rel in paths.items():
        by_path.setdefault(rel, []).append(ref)

    anchor_ref = by_path[ANCHOR_SOURCE][0]
    source_phases = phase_containing(objects, anchor_ref, "PBXSourcesBuildPhase")
    header_phases = phase_containing(objects, by_path[ANCHOR_HEADER][0], "PBXHeadersBuildPhase")

    missing = []   # (rel path, what is missing)
    for source in cmake_sources() + APPLE_ENGINE_SOURCES:
        refs = by_path.get(source, [])
        if not refs:
            missing.append((source, "file"))
        else:
            for phase in source_phases:
                if not any(objects.get(b, {}).get("fileRef") in refs for b in objects[phase]["files"]):
                    missing.append((source, "phase " + phase))
    # the headers of every directory with a listed source (header-only ones too)
    for directory in sorted({os.path.dirname(s) for s in cmake_sources()} | set(HEADER_DIRECTORIES)):
        for name in sorted(os.listdir(os.path.join(ENGINE, directory))):
            header = directory + "/" + name
            if name.endswith(".h") and header not in by_path:
                missing.append((header, "file"))
    return lines, objects, by_path, group_dirs, source_phases, header_phases, missing


def group_for_directory(lines, objects, group_dirs, directory):
    """the id of the group whose directory is engine/<directory>, created (with parents) when missing"""
    target = os.path.normpath(os.path.join(ENGINE, directory))
    for gid, d in group_dirs.items():
        if d == target and objects[gid].get("isa") == "PBXGroup":
            return gid
    parent_rel = os.path.dirname(directory)
    parent = group_for_directory(lines, objects, group_dirs, parent_rel)
    name = os.path.basename(directory)
    gid = object_id("group", directory)
    block = [
        "\t\t%s /* %s */ = {" % (gid, name),
        "\t\t\tisa = PBXGroup;",
        "\t\t\tchildren = (",
        "\t\t\t);",
        "\t\t\tpath = %s;" % name,
        "\t\t\tsourceTree = \"<group>\";",
        "\t\t};",
    ]
    # before the parent group's definition (inside the PBXGroup section)
    at = objects[parent]["line"]
    lines[at:at] = block
    reparse(lines, objects)
    add_child(lines, objects, parent, gid, name)
    group_dirs[gid] = target
    return gid


def reparse(lines, objects):
    objects.clear()
    objects.update(parse_project(lines))


def add_child(lines, objects, gid, child, comment, key="children"):
    obj = objects[gid]
    for k in range(obj["line"], obj["end"] + 1):
        if lines[k].strip() == key + " = (":
            for e in range(k, obj["end"] + 1):
                if lines[e].strip() == ");":
                    lines.insert(e, "\t\t\t\t%s /* %s */," % (child, comment))
                    reparse(lines, objects)
                    return
    raise SystemExit("no %s list in %s" % (key, gid))


def insert_object(lines, objects, isa, line):
    """a one-line object at the end of its isa section"""
    end_marker = "/* End %s section */" % isa
    for i, text in enumerate(lines):
        if text == end_marker:
            lines.insert(i, line)
            reparse(lines, objects)
            return
    raise SystemExit("no %s section" % isa)


def main():
    check = "--check" in sys.argv
    lines, objects, by_path, group_dirs, source_phases, header_phases, missing = analyse()
    if len(source_phases) != 2:
        raise SystemExit("expected 2 engine Sources phases with %s, found %d" % (ANCHOR_SOURCE, len(source_phases)))
    if not missing:
        print("Xcode project lists every engine source of engine/CMakeLists.txt")
        return 0
    if check:
        print("missing from the Xcode project (run python3 tools/xcode_sync_engine.py):")
        for path, what in missing:
            print("  %s (%s)" % (path, what))
        return 1

    for path, what in missing:
        name = os.path.basename(path)
        is_header = path.endswith(".h")
        if what == "file":
            gid = group_for_directory(lines, objects, group_dirs, os.path.dirname(path))
            ref = object_id("ref", path)
            file_type = "sourcecode.c.h" if is_header else ("sourcecode.cpp.objcpp" if path.endswith(".mm") else "sourcecode.cpp.cpp")
            insert_object(lines, objects, "PBXFileReference",
                          "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
                          % (ref, name, file_type, name))
            add_child(lines, objects, gid, ref, name)
            by_path.setdefault(path, []).append(ref)
            phases = header_phases if is_header else source_phases
        else:
            ref = by_path[path][0]
            phases = [what.split()[1]]
        for phase in phases:
            index = source_phases.index(phase) if phase in source_phases else 0
            kind = "hdr" if is_header else "src%d" % (index + 1)
            build = object_id(kind, path)
            label = "%s in %s" % (name, "Headers" if is_header else "Sources")
            insert_object(lines, objects, "PBXBuildFile",
                          "\t\t%s /* %s */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, label, ref, name))
            add_child(lines, objects, phase, build, label, key="files")

    with open(PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added to the Xcode project: " + ", ".join(sorted({p for p, _ in missing})))
    return 0


if __name__ == "__main__":
    sys.exit(main())
