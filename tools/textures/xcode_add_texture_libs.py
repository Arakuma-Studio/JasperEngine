#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Adds the cooked-texture runtime (docs/specs/S14-texture-compression.md) to the Xcode workspace,
the way tools/htmlui/xcode_add_video_libs.py adds the video decoders (CMake builds the libraries
from their own CMakeLists.txt, the Xcode project lists everything explicitly):

    python3 tools/textures/xcode_add_texture_libs.py [--check]

- static library targets zstd-<platform> (ios, macos; products libzstd-<platform>.a), cloned from
  lunasvg-<platform>, compiling Zstandard's decoder (the sources of engine/libs/zstd's
  zstd_decompress library) with its private defines; every Frameworks phase that links lunasvg
  links it too, and its targets depend on it;
- engine/libs/zstd/lib and engine/libs/bcdec on the engine and app configurations' search paths;
- engine/core/texture's new sources (KTX2, the mip chain, the preparation, the CPU decoders, the
  fallbacks, the Basis transcode stub of native builds, the cooked index) next to TextureData.cpp
  in the engine targets; MipChain.cpp and TexturePrepare.cpp with -ffp-contract=off (the raw
  path's levels match the cook's);
- a run script phase in each app target ("Keep this platform's cooked textures") removing the
  families of assets/je-cooked its platform does not load from the bundle (iOS keeps mobile,
  macOS desktop: R9's packaging rule).

Object ids are "TXTR" + sha1 of a key, deterministic and distinct from the other scripts' ids.
Only what is missing is added: running it again changes nothing. --check exits 1 when something
is missing (nothing is written).
"""

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "htmlui"))
import xcode_add_lexbor as lx  # noqa: E402  (Project, phase_block)
import xcode_add_video_libs as xv  # noqa: E402  (the library target helpers)

PREFIX = "54585452"  # "TXTR"
LIBS = os.path.join(ROOT, "engine", "libs")
TEXTURE_SOURCES = ["BasisTranscode.cpp", "CookedTextures.cpp", "Ktx2.cpp", "MipChain.cpp", "TextureDecode.cpp",
                   "TextureFallback.cpp", "TextureFormat.cpp", "TexturePrepare.cpp"]
STRICT_FP = ("MipChain.cpp", "TexturePrepare.cpp")
APP_TARGETS = {"JasperEngine iOS": "mobile", "JasperEngine macOS": "desktop"}
FAMILIES = ("desktop", "mobile", "android_etc2", "web")


def object_id(kind, key):
    return PREFIX + hashlib.sha1((kind + ":" + key).encode()).hexdigest()[:16].upper()


# the video script's helpers make their objects with this module's ids and know zstd by name
xv.object_id = object_id
xv.SKIP_CONFIG_OWNERS = re.compile(r"^(lunasvg|rmlui|lexbor|dav1d|opus|webm|zstd)-")


def zstd_library():
    cmake = os.path.join(LIBS, "zstd", "CMakeLists.txt")
    sources = []
    for name in ("ZSTD_COMMON_SOURCES", "ZSTD_DECOMPRESS_SOURCES"):
        sources += [s.replace("${ZSTD_LIB_DIR}/", "lib/") for s in xv.cmake_sources(cmake, name)]
    return {
        "name": "zstd", "dir": "zstd", "c": "gnu11", "cxx": False, "sources": sources,
        "defines": ["ZSTD_DISABLE_ASM=1", "ZSTD_LEGACY_SUPPORT=0", "XXH_NAMESPACE=JE_ZSTD_"],
        "includes": [xv.inc("zstd/lib"), xv.inc("zstd/lib/common")],
        "public_include": xv.inc("zstd/lib"), "public_define": None,
    }


def add_texture_sources(project):
    """the new engine/core/texture sources next to TextureData.cpp (its group and Sources phases)"""
    sibling = "TextureData.cpp"
    sibling_ref = None
    for line in project.lines:
        m = re.match(r"^\t\t([0-9A-F]{24}) /\* %s \*/ = \{isa = PBXFileReference;" % re.escape(sibling), line)
        if m:
            sibling_ref = m.group(1)
    if sibling_ref is None:
        raise SystemExit("not in the Xcode project: " + sibling)
    groups = [project.owner(i)[0] for i, line in enumerate(project.lines) if line.strip() == "%s /* %s */," % (sibling_ref, sibling)]
    sibling_builds = [m.group(1) for m in (re.match(r"^\t\t([0-9A-F]{24}) /\* %s in Sources \*/ = \{isa = PBXBuildFile; fileRef = %s "
                                                    % (re.escape(sibling), sibling_ref), line) for line in project.lines) if m]
    phases = [xv.owner_of_entry(project, "%s /* %s in Sources */," % (build, sibling)) for build in sibling_builds]
    for name in TEXTURE_SOURCES:
        for header in (False, True):
            file_name = name if not header else name[:-4] + ".h"
            ref = object_id("ref", "texture/" + file_name)
            file_type = "sourcecode.c.h" if header else "sourcecode.cpp.cpp"
            project.add_object("PBXFileReference", ref, [
                "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
                % (ref, file_name, file_type, file_name)])
            for gid in groups:
                project.add_to_list(gid, "children", "\t\t\t\t%s /* %s */," % (ref, file_name), after="/* %s */," % sibling)
        ref = object_id("ref", "texture/" + name)
        settings = ' settings = {COMPILER_FLAGS = "-ffp-contract=off"; };' if name in STRICT_FP else ""
        for number, phase in enumerate(phases):
            build = object_id("src%d" % (number + 1), "texture/" + name)
            project.add_object("PBXBuildFile", build, [
                "\t\t%s /* %s in Sources */ = {isa = PBXBuildFile; fileRef = %s /* %s */;%s };" % (build, name, ref, name, settings)])
            project.add_to_list(phase, "files", "\t\t\t\t%s /* %s in Sources */," % (build, name), after="/* %s in Sources */," % sibling)
    return len(phases)


def keep_family_script(family):
    drop = [f for f in FAMILIES if f != family]
    return ('COOKED="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/assets/je-cooked"\n'
            'for family in %s; do rm -rf "$COOKED/$family"; done\n' % " ".join(drop))


def add_family_phases(project):
    for target_name, family in APP_TARGETS.items():
        target = project.target_by_name(target_name)
        phase = object_id("keep-cooked", target_name)
        project.add_object("PBXShellScriptBuildPhase", phase, [
            "\t\t%s /* Keep this platform's cooked textures */ = {" % phase,
            "\t\t\tisa = PBXShellScriptBuildPhase;",
            "\t\t\talwaysOutOfDate = 1;",
            "\t\t\tbuildActionMask = 2147483647;",
            "\t\t\tfiles = (",
            "\t\t\t);",
            "\t\t\tinputPaths = (",
            "\t\t\t);",
            "\t\t\tname = \"Keep this platform's cooked textures\";",
            "\t\t\toutputPaths = (",
            "\t\t\t);",
            "\t\t\trunOnlyForDeploymentPostprocessing = 0;",
            "\t\t\tshellPath = /bin/sh;",
            "\t\t\tshellScript = %s;" % xv.pbx_string(keep_family_script(family)),
            "\t\t};",
        ])
        project.add_to_list(target, "buildPhases", "\t\t\t\t%s /* Keep this platform's cooked textures */," % phase,
                            after="/* Resources */,")


def ensure_section(project, name, before):
    if "/* Begin %s section */" % name not in project.lines:
        index = project.lines.index("/* Begin %s section */" % before)
        project.lines[index:index] = ["/* Begin %s section */" % name, "/* End %s section */" % name, ""]
        project.added.append(name + " section")


def main():
    check = "--check" in sys.argv
    with open(lx.PROJECT, encoding="utf-8") as f:
        text = f.read()
    project = lx.Project(text.split("\n"))
    portal = next(line for line in project.lines if line.startswith("\trootObject = ")).split()[2]
    lib = zstd_library()
    xv.add_library_targets(project, lib, portal)
    xv.add_library_files(project, lib)
    xv.link_libraries(project, lib, portal)
    configs = xv.add_public_interface(project, [lib, {"name": "bcdec", "public_include": xv.inc("bcdec"), "public_define": None}])
    phases = add_texture_sources(project)
    ensure_section(project, "PBXShellScriptBuildPhase", "PBXSourcesBuildPhase")
    add_family_phases(project)

    counts = project.defined()
    duplicates = [oid for oid, count in counts.items() if count > 1]
    if duplicates:
        raise SystemExit("duplicate object ids: " + ", ".join(duplicates))

    summary = "zstd %d sources per target; %d texture sources in %d phases; %d engine configurations" % (
        len(lib["sources"]), len(TEXTURE_SOURCES), phases, len(configs))
    if not project.added:
        print("Xcode project has the cooked-texture runtime (" + summary + ")")
        return 0
    if check:
        print("the cooked-texture runtime is missing from the Xcode project (%d changes); run tools/textures/xcode_add_texture_libs.py"
              % len(project.added))
        return 1
    with open(lx.PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(project.lines))
    print("added the cooked-texture runtime to the Xcode project: %d changes (%s)" % (len(project.added), summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
