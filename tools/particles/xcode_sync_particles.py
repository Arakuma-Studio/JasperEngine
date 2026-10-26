#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
#
# Lists the S20 particle sources in the Xcode workspace (CMake globs or lists them in
# engine/core/particles/ParticleSources.cmake; the Xcode project lists files explicitly):
#
#   python3 tools/particles/xcode_sync_particles.py [--check]
#
# Each .cpp goes into the two Sources phases that compile ParticleSystem.cpp (the iOS and macOS
# engine targets), each .h into its Headers phase, under the group of its directory (made when
# missing: core/particles, core/math/noise, core/math/simd). The particle kernels' sources get
# -ffp-contract=off, as CMake gives them (their SIMD and scalar paths stay bit-identical).
# ParticleSystem.{h,cpp} moved from core/action to core/object: their entries move with them.
# Object ids are derived from the file path ("PRTC" + sha1), so a merge with another branch's
# additions only has to keep both lines. --check exits 1 when something is missing.

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROJECT = os.path.join(ROOT, "engine", "workspaces", "xcode", "JasperEngine.xcodeproj", "project.pbxproj")
SOURCES_CMAKE = os.path.join(ROOT, "engine", "core", "particles", "ParticleSources.cmake")

CORE_GROUP = "7162FC2A25962A0E0075B97D"
# ParticleSystem's entries: the anchors of the phases every engine target has
PS_CPP_REF = "71F225472C587388006ED3CA"
PS_H_REF = "71F225462C587388006ED3CA"
FILE_REF_ANCHOR = PS_CPP_REF + " /* ParticleSystem.cpp */ = {isa = PBXFileReference;"
BUILD_FILE_ANCHOR = "71F2254A2C587388006ED3CA /* ParticleSystem.cpp in Sources */ = {isa = PBXBuildFile;"
HEADERS_ANCHOR = "71F225482C587388006ED3CA /* ParticleSystem.h in Headers */,"
SOURCES_ANCHORS = ["71F225492C587388006ED3CA /* ParticleSystem.cpp in Sources */,",
                   "71F2254A2C587388006ED3CA /* ParticleSystem.cpp in Sources */,"]

# engine/core-relative files S20 added
FILES = [
    "component/ForceFieldComponent.h",
    "math/Curve.cpp", "math/Curve.h",
    "math/noise/CurlNoiseBatch.cpp", "math/noise/CurlNoiseBatch.h",
    "math/noise/GradientCurl3D.h", "math/noise/GradientCurl3DSimd.h",
    "math/simd/SimdFloat4.h",
    "subsystem/ParticleSubsystem.cpp", "subsystem/ParticleSubsystem.h",
    "subsystem/ParticleSystemState.h", "subsystem/RenderSystemParticles.cpp",
]


def particle_files():
    names = sorted(n for n in os.listdir(os.path.join(ROOT, "engine", "core", "particles")) if n.endswith((".cpp", ".h")))
    return ["particles/" + n for n in names]


def contracted_sources():
    # the JASPERENGINE_PARTICLE_SRCS list: core/... paths
    with open(SOURCES_CMAKE, encoding="utf-8") as f:
        text = f.read()
    block = re.search(r"set\(JASPERENGINE_PARTICLE_SRCS(.*?)\)", text, re.S).group(1)
    return {line.strip()[len("core/"):] for line in block.split("\n") if line.strip().startswith("core/")}


def object_id(kind, path):
    return "50525443" + hashlib.sha1((kind + ":" + path).encode()).hexdigest()[:16].upper()


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
    # the group of an engine/core-relative directory ("" is core), made when missing
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


def move_particle_system(lines):
    # ParticleSystem.{h,cpp} from the action group to the object group; true when moved
    action = child_group(lines, CORE_GROUP, "action")
    target = child_group(lines, CORE_GROUP, "object")
    span = group_span(lines, action)
    moved = []
    for ref in (PS_CPP_REF, PS_H_REF):
        for i in range(span[0] + 1, span[1]):
            if lines[i].strip().startswith(ref + " /*"):
                moved.append(lines.pop(i))
                span = group_span(lines, action)
                break
    for line in moved:
        lines.insert(group_span(lines, target)[1], line)
    return bool(moved)


def add_file(lines, path, contracted):
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
    settings = ' settings = {COMPILER_FLAGS = "-ffp-contract=off"; };' if path in contracted else ""
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
    files = particle_files() + FILES
    missing = [path for path in files if object_id("ref", path) not in text]
    action = child_group(lines, CORE_GROUP, "action")
    span = group_span(lines, action)
    misplaced = any(line.strip().startswith((PS_CPP_REF + " /*", PS_H_REF + " /*")) for line in lines[span[0] + 1:span[1]])
    if not missing and not misplaced:
        print("Xcode project lists every particle file")
        return 0
    if check:
        print("missing from the Xcode project: " + ", ".join(missing) + (" (ParticleSystem still in action/)" if misplaced else ""))
        return 1
    if misplaced:
        move_particle_system(lines)
    contracted = contracted_sources()
    for path in missing:
        add_file(lines, path, contracted)
    with open(PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("added %d particle file(s) to the Xcode project%s" % (len(missing), ", moved ParticleSystem to object/" if misplaced else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
