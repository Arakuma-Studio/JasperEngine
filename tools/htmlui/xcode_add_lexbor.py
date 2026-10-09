#!/usr/bin/env python3
# Adds the vendored HTML5 parser lexbor (engine/libs/lexbor) to the Xcode workspace the way
# LunaSVG is there: CMake builds it from engine/libs/lexbor/CMakeLists.txt, the Xcode project
# lists everything explicitly.
#
#   python3 tools/htmlui/xcode_add_lexbor.py [--check]
#
# - static library targets lexbor-ios and lexbor-macos (products liblexbor-ios.a and
#   liblexbor-macos.a) compiling every .c of lexbor/{core,dom,html,ns,tag} (recursively, the
#   directories CMakeLists.txt globs) and of the posix port (lexbor/ports/posix/lexbor/core);
#   their build settings are cloned from lunasvg-ios / lunasvg-macos (deployment targets,
#   hidden visibility, warnings off) with C99 (gnu99, CMake's C_STANDARD 99 with extensions),
#   LEXBOR_STATIC and the include root "$(SRCROOT)/../../libs/lexbor";
# - a libs/lexbor group mirroring the directories (.c and .h files);
# - every Frameworks phase that links liblunasvg-<platform>.a also links
#   liblexbor-<platform>.a, and its target depends on lexbor-<platform>;
# - every build configuration that has "$(SRCROOT)/../../libs/lunasvg/include" in its
#   HEADER_SEARCH_PATHS (the engine and app targets; not lunasvg's or rmlui's own) gets
#   "$(SRCROOT)/../../libs/lexbor" after it and LEXBOR_STATIC after LUNASVG_BUILD_STATIC.
#
# Object ids are "LXBR" + sha1 of a key (as in xcode_sync_compat.py), so they are
# deterministic and cannot collide with the existing ids. Only what is missing is added:
# running it again changes nothing, and new lexbor files are picked up. --check exits 1 when
# something is missing (nothing is written).

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LEXBOR = os.path.join(ROOT, "engine", "libs", "lexbor")
PROJECT = os.path.join(ROOT, "engine", "workspaces", "xcode", "JasperEngine.xcodeproj", "project.pbxproj")

PREFIX = "4C584252"  # "LXBR"
PLATFORMS = ("ios", "macos")
SOURCE_DIRS = ("lexbor/core", "lexbor/dom", "lexbor/html", "lexbor/ns", "lexbor/tag")
PORT_DIR = "lexbor/ports/posix/lexbor/core"
LIBS_GROUP = "7162FC9625962A0E0075B97D"
LEXBOR_INCLUDE = '"$(SRCROOT)/../../libs/lexbor"'
LUNASVG_INCLUDE = '"$(SRCROOT)/../../libs/lunasvg/include"'
# configurations of these targets keep their search paths (they never include lexbor)
SKIP_CONFIG_OWNERS = re.compile(r"^(lunasvg|rmlui|lexbor)-")
# C++-only settings of the lunasvg configurations not cloned into lexbor's
CXX_ONLY = ("CLANG_CXX_LANGUAGE_STANDARD", "GCC_ENABLE_CPP_EXCEPTIONS", "GCC_ENABLE_CPP_RTTI")

OBJECT_LINE = re.compile(r"^\t\t([0-9A-F]{24}) /\* (.*?) \*/ = \{")


def object_id(kind, key):
    return PREFIX + hashlib.sha1((kind + ":" + key).encode()).hexdigest()[:16].upper()


def lexbor_files():
    # paths relative to engine/libs/lexbor
    files = []
    for top in SOURCE_DIRS:
        for directory, _, names in os.walk(os.path.join(LEXBOR, top)):
            for name in names:
                if name.endswith(".c") or name.endswith(".h"):
                    files.append(os.path.relpath(os.path.join(directory, name), LEXBOR).replace(os.sep, "/"))
    port = os.path.join(LEXBOR, PORT_DIR)
    for name in os.listdir(port):
        if name.endswith(".c") or name.endswith(".h"):
            files.append(PORT_DIR + "/" + name)
    return sorted(files)


class Project:
    def __init__(self, lines):
        self.lines = lines
        self.added = []

    # --- lookup -------------------------------------------------------------------------

    def defined(self):
        ids = {}
        for line in self.lines:
            m = OBJECT_LINE.match(line)
            if m:
                ids[m.group(1)] = ids.get(m.group(1), 0) + 1
        return ids

    def has(self, oid):
        prefix = "\t\t" + oid + " "
        return any(line.startswith(prefix) for line in self.lines)

    def block(self, oid):
        # (first, last) line index of a multi-line object
        prefix = "\t\t" + oid + " /*"
        for i, line in enumerate(self.lines):
            if line.startswith(prefix) and line.rstrip().endswith("= {"):
                for j in range(i + 1, len(self.lines)):
                    if self.lines[j] == "\t\t};":
                        return i, j
        raise SystemExit("object not found in project.pbxproj: " + oid)

    def list_span(self, oid, key, indent="\t\t\t"):
        # (open, close) line index of the list "key = (" of an object
        first, last = self.block(oid)
        for i in range(first, last):
            if self.lines[i] == indent + key + " = (":
                for j in range(i + 1, last):
                    if self.lines[j] == indent + ");":
                        return i, j
        raise SystemExit("list %s not found in object %s" % (key, oid))

    def owner(self, index):
        # id of the object whose block contains line index
        for i in range(index, -1, -1):
            m = OBJECT_LINE.match(self.lines[i])
            if m:
                return m.group(1), m.group(2)
        raise SystemExit("no object around line %d" % index)

    def target_by_name(self, name):
        for line in self.lines:
            m = OBJECT_LINE.match(line)
            if m and m.group(2) == name and line.rstrip().endswith("= {"):
                first, _ = self.block(m.group(1))
                if self.lines[first + 1].strip() == "isa = PBXNativeTarget;":
                    return m.group(1)
        raise SystemExit("target not found: " + name)

    def configurations(self, list_comment):
        # {name: id} of the configurations of an XCConfigurationList
        for line in self.lines:
            m = OBJECT_LINE.match(line)
            if m and m.group(2) == list_comment:
                open_, close = self.list_span(m.group(1), "buildConfigurations")
                result = {}
                for entry in self.lines[open_ + 1:close]:
                    em = re.match(r"^\t+([0-9A-F]{24}) /\* (.*) \*/,$", entry)
                    result[em.group(2)] = em.group(1)
                return result
        raise SystemExit("configuration list not found: " + list_comment)

    # --- insertion ----------------------------------------------------------------------

    def add_object(self, section, oid, new_lines):
        if self.has(oid):
            return False
        marker = "/* End %s section */" % section
        index = self.lines.index(marker)
        self.lines[index:index] = new_lines
        self.added.append(oid)
        return True

    def add_to_list(self, oid, key, entry, after=None, sort_name=None):
        # appends entry (or puts it after the line containing `after`, or in name order among
        # the entries when sort_name is given) unless the list has it already
        open_, close = self.list_span(oid, key)
        entry_id = entry.strip().split(" ")[0]
        if any(line.strip().startswith(entry_id + " ") for line in self.lines[open_ + 1:close]):
            return
        position = close
        if after is not None:
            for i in range(open_ + 1, close):
                if after in self.lines[i]:
                    position = i + 1
                    break
        elif sort_name is not None:
            for i in range(open_ + 1, close):
                m = re.match(r"^\t+[0-9A-F]{24} /\* (.*) \*/,$", self.lines[i])
                if m and m.group(1).lower() > sort_name.lower():
                    position = i
                    break
        self.lines.insert(position, entry)
        self.added.append(entry_id + " in " + oid)


def group_block(gid, name):
    return [
        "\t\t%s /* %s */ = {" % (gid, name),
        "\t\t\tisa = PBXGroup;",
        "\t\t\tchildren = (",
        "\t\t\t);",
        "\t\t\tpath = %s;" % name,
        "\t\t\tsourceTree = \"<group>\";",
        "\t\t};",
    ]


def phase_block(pid, name, isa):
    return [
        "\t\t%s /* %s */ = {" % (pid, name),
        "\t\t\tisa = %s;" % isa,
        "\t\t\tbuildActionMask = 2147483647;",
        "\t\t\tfiles = (",
        "\t\t\t);",
        "\t\t\trunOnlyForDeploymentPostprocessing = 0;",
        "\t\t};",
    ]


def clone_configuration(project, template_id, new_id, name):
    # lunasvg's configuration with lexbor's defines, search path and C standard
    first, last = project.block(template_id)
    out = ["\t\t%s /* %s */ = {" % (new_id, name)]
    i = first + 1
    while i < last:
        line = project.lines[i]
        key = line.strip().split(" ")[0]
        if key in CXX_ONLY:
            i += 1
            continue
        if key == "GCC_C_LANGUAGE_STANDARD":
            out.append("\t\t\t\tGCC_C_LANGUAGE_STANDARD = gnu99;")
            i += 1
            continue
        if key in ("GCC_PREPROCESSOR_DEFINITIONS", "HEADER_SEARCH_PATHS") and line.rstrip().endswith("= ("):
            values = ['"$(inherited)"', "LEXBOR_STATIC"] if key == "GCC_PREPROCESSOR_DEFINITIONS" else [LEXBOR_INCLUDE]
            out.append(line)
            out.extend("\t\t\t\t\t%s," % value for value in values)
            while project.lines[i].strip() != ");":
                i += 1
            out.append(project.lines[i])
            i += 1
            continue
        out.append(line)
        i += 1
    out.append("\t\t};")
    return out


def sync(project):
    files = lexbor_files()
    sources = [path for path in files if path.endswith(".c")]
    portal = next(line for line in project.lines if line.startswith("\trootObject = ")).split()[2]

    # --- the two static library targets -----------------------------------------------
    products_group = None
    for platform in PLATFORMS:
        name = "lexbor-" + platform
        lib = "lib%s.a" % name
        template_target = project.target_by_name("lunasvg-" + platform)
        product = object_id("product", platform)
        target = object_id("target", platform)
        headers = object_id("headers", platform)
        sources_phase = object_id("sources", platform)
        frameworks = object_id("frameworks", platform)
        config_list = object_id("configlist", platform)
        list_comment = 'Build configuration list for PBXNativeTarget "%s"' % name

        project.add_object("PBXFileReference", product, [
            "\t\t%s /* %s */ = {isa = PBXFileReference; explicitFileType = archive.ar; includeInIndex = 0; path = \"%s\"; sourceTree = BUILT_PRODUCTS_DIR; };"
            % (product, lib, lib)])
        project.add_object("PBXHeadersBuildPhase", headers, phase_block(headers, "Headers", "PBXHeadersBuildPhase"))
        project.add_object("PBXSourcesBuildPhase", sources_phase, phase_block(sources_phase, "Sources", "PBXSourcesBuildPhase"))
        project.add_object("PBXFrameworksBuildPhase", frameworks, phase_block(frameworks, "Frameworks", "PBXFrameworksBuildPhase"))

        template_configs = project.configurations('Build configuration list for PBXNativeTarget "lunasvg-%s"' % platform)
        config_ids = []
        for config_name in sorted(template_configs):
            cid = object_id("config", platform + ":" + config_name)
            config_ids.append((cid, config_name))
            project.add_object("XCBuildConfiguration", cid,
                               clone_configuration(project, template_configs[config_name], cid, config_name))
        project.add_object("XCConfigurationList", config_list, [
            "\t\t%s /* %s */ = {" % (config_list, list_comment),
            "\t\t\tisa = XCConfigurationList;",
            "\t\t\tbuildConfigurations = (",
        ] + ["\t\t\t\t%s /* %s */," % (cid, config_name) for cid, config_name in config_ids] + [
            "\t\t\t);",
            "\t\t\tdefaultConfigurationIsVisible = 0;",
            "\t\t\tdefaultConfigurationName = Release;",
            "\t\t};",
        ])
        project.add_object("PBXNativeTarget", target, [
            "\t\t%s /* %s */ = {" % (target, name),
            "\t\t\tisa = PBXNativeTarget;",
            "\t\t\tbuildConfigurationList = %s /* %s */;" % (config_list, list_comment),
            "\t\t\tbuildPhases = (",
            "\t\t\t\t%s /* Headers */," % headers,
            "\t\t\t\t%s /* Sources */," % sources_phase,
            "\t\t\t\t%s /* Frameworks */," % frameworks,
            "\t\t\t);",
            "\t\t\tbuildRules = (",
            "\t\t\t);",
            "\t\t\tdependencies = (",
            "\t\t\t);",
            "\t\t\tname = \"%s\";" % name,
            "\t\t\tproductName = \"%s\";" % name,
            "\t\t\tproductReference = %s /* %s */;" % (product, lib),
            "\t\t\tproductType = \"com.apple.product-type.library.static\";",
            "\t\t};",
        ])

        # Products group and the project's target list, next to lunasvg's
        lunasvg_product = None
        first, last = project.block(template_target)
        for line in project.lines[first:last]:
            m = re.match(r"^\t\t\tproductReference = ([0-9A-F]{24}) ", line)
            if m:
                lunasvg_product = m.group(1)
        for i, line in enumerate(project.lines):
            if line.startswith("\t\t\t\t" + lunasvg_product + " /*") and line.endswith(","):
                products_group = project.owner(i)[0]
        project.add_to_list(products_group, "children", "\t\t\t\t%s /* %s */," % (product, lib),
                            after="liblunasvg-ios.a */,")
        project.add_to_list(portal, "targets", "\t\t\t\t%s /* %s */," % (target, name),
                            after="/* lunasvg-ios */,")

    # --- groups and files -----------------------------------------------------------------
    def group_id(directory):
        return object_id("group", directory)

    directories = set()
    for path in files:
        parts = path.split("/")[:-1]
        for depth in range(len(parts) + 1):
            directories.add("/".join(parts[:depth]))
    for directory in sorted(directories, key=lambda d: (d.count("/") if d else -1, d)):
        gid = group_id(directory)
        name = directory.split("/")[-1] if directory else "lexbor"
        project.add_object("PBXGroup", gid, group_block(gid, name))
        if directory:
            parent = group_id("/".join(directory.split("/")[:-1]))
            project.add_to_list(parent, "children", "\t\t\t\t%s /* %s */," % (gid, name))
        else:
            project.add_to_list(LIBS_GROUP, "children", "\t\t\t\t%s /* %s */," % (gid, name), sort_name=name)

    for path in files:
        name = path.split("/")[-1]
        directory = "/".join(path.split("/")[:-1])
        ref = object_id("ref", path)
        file_type = "sourcecode.c.h" if name.endswith(".h") else "sourcecode.c.c"
        project.add_object("PBXFileReference", ref, [
            "\t\t%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; path = %s; sourceTree = \"<group>\"; };"
            % (ref, name, file_type, name)])
        project.add_to_list(group_id(directory), "children", "\t\t\t\t%s /* %s */," % (ref, name))

    for platform in PLATFORMS:
        sources_phase = object_id("sources", platform)
        for path in sources:
            name = path.split("/")[-1]
            ref = object_id("ref", path)
            build = object_id("src-" + platform, path)
            project.add_object("PBXBuildFile", build, [
                "\t\t%s /* %s in Sources */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, name, ref, name)])
            project.add_to_list(sources_phase, "files", "\t\t\t\t%s /* %s in Sources */," % (build, name))

    # --- the targets that link lunasvg link lexbor --------------------------------------
    for platform in PLATFORMS:
        name = "lexbor-" + platform
        lib = "lib%s.a" % name
        target = object_id("target", platform)
        product = object_id("product", platform)
        lunasvg_target = project.target_by_name("lunasvg-" + platform)

        link_phases = []
        for i, line in enumerate(project.lines):
            if re.match(r"^\t\t\t\t[0-9A-F]{24} /\* liblunasvg-%s\.a in Frameworks \*/,$" % platform, line):
                link_phases.append(project.owner(i)[0])
        for phase in link_phases:
            build = object_id("link-" + platform, phase)
            project.add_object("PBXBuildFile", build, [
                "\t\t%s /* %s in Frameworks */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };" % (build, lib, product, lib)])
            project.add_to_list(phase, "files", "\t\t\t\t%s /* %s in Frameworks */," % (build, lib),
                                after="liblunasvg-%s.a in Frameworks */," % platform)

        lunasvg_dependencies = set()
        for i, line in enumerate(project.lines):
            if line.startswith("\t\t\ttarget = " + lunasvg_target + " "):
                lunasvg_dependencies.add(project.owner(i)[0])
        dependents = []
        for i, line in enumerate(project.lines):
            m = re.match(r"^\t\t\t\t([0-9A-F]{24}) /\* PBXTargetDependency \*/,$", line)
            if m and m.group(1) in lunasvg_dependencies:
                dependents.append((project.owner(i)[0], m.group(1)))
        for dependent, lunasvg_dependency in dependents:
            proxy = object_id("proxy-" + platform, dependent)
            dependency = object_id("dependency-" + platform, dependent)
            project.add_object("PBXContainerItemProxy", proxy, [
                "\t\t%s /* PBXContainerItemProxy */ = {" % proxy,
                "\t\t\tisa = PBXContainerItemProxy;",
                "\t\t\tcontainerPortal = %s /* Project object */;" % portal,
                "\t\t\tproxyType = 1;",
                "\t\t\tremoteGlobalIDString = %s;" % target,
                "\t\t\tremoteInfo = \"%s\";" % name,
                "\t\t};",
            ])
            project.add_object("PBXTargetDependency", dependency, [
                "\t\t%s /* PBXTargetDependency */ = {" % dependency,
                "\t\t\tisa = PBXTargetDependency;",
                "\t\t\ttarget = %s /* %s */;" % (target, name),
                "\t\t\ttargetProxy = %s /* PBXContainerItemProxy */;" % proxy,
                "\t\t};",
            ])
            project.add_to_list(dependent, "dependencies", "\t\t\t\t%s /* PBXTargetDependency */," % dependency,
                                after=lunasvg_dependency + " /* PBXTargetDependency */,")

    # --- search path and define of the configurations that see lunasvg's headers -------
    owners = {}
    for line in project.lines:
        m = OBJECT_LINE.match(line)
        if m and m.group(2).startswith("Build configuration list for "):
            open_, close = project.list_span(m.group(1), "buildConfigurations")
            for entry in project.lines[open_ + 1:close]:
                owners[entry.strip().split(" ")[0]] = re.search(r'"(.*)"', m.group(2)).group(1)
    config_ids = []
    for i, line in enumerate(project.lines):
        if line.strip() == LUNASVG_INCLUDE + ",":
            config = project.owner(i)[0]
            if not SKIP_CONFIG_OWNERS.match(owners.get(config, "")):
                config_ids.append(config)
    for config in config_ids:
        open_, close = project.list_span(config, "HEADER_SEARCH_PATHS", "\t\t\t\t")
        if not any(line.strip() == LEXBOR_INCLUDE + "," for line in project.lines[open_ + 1:close]):
            position = next(i for i in range(open_ + 1, close) if project.lines[i].strip() == LUNASVG_INCLUDE + ",")
            project.lines.insert(position + 1, "\t\t\t\t\t%s," % LEXBOR_INCLUDE)
            project.added.append("search path in " + config)
        open_, close = project.list_span(config, "GCC_PREPROCESSOR_DEFINITIONS", "\t\t\t\t")
        values = [line.strip() for line in project.lines[open_ + 1:close]]
        if "LEXBOR_STATIC," not in values:
            if "LUNASVG_BUILD_STATIC," in values:
                position = open_ + 1 + values.index("LUNASVG_BUILD_STATIC,") + 1
            else:
                position = close
            project.lines.insert(position, "\t\t\t\t\tLEXBOR_STATIC,")
            project.added.append("LEXBOR_STATIC in " + config)
    return len(sources), config_ids


def main():
    check = "--check" in sys.argv
    with open(PROJECT, encoding="utf-8") as f:
        text = f.read()
    project = Project(text.split("\n"))
    source_count, configs = sync(project)

    counts = project.defined()
    duplicates = [oid for oid, count in counts.items() if count > 1]
    if duplicates:
        raise SystemExit("duplicate object ids: " + ", ".join(duplicates))

    if not project.added:
        print("Xcode project has lexbor (%d sources per target, %d configurations)" % (source_count, len(configs)))
        return 0
    if check:
        print("lexbor is missing from the Xcode project (%d changes needed); run tools/htmlui/xcode_add_lexbor.py"
              % len(project.added))
        return 1
    with open(PROJECT, "w", encoding="utf-8") as f:
        f.write("\n".join(project.lines))
    print("added lexbor to the Xcode project: %d changes, %d sources per target, %d configurations"
          % (len(project.added), source_count, len(configs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
