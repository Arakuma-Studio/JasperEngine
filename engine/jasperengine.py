#!/usr/bin/env python3

# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
# Based on Filip Stoklas code (http://forums.4fips.com/viewtopic.php?f=3&t=1201)

import argparse
import glob
import os
import shutil
import subprocess
import sys
import uuid

XCODE_PLATFORMS = ["macos-xcode", "ios-xcode"]

def fail(message):
    print("Error: " + message, file=sys.stderr)
    sys.exit(1)

DRY_RUN = False

def run(command, env_changes=None):
    print("> " + " ".join(command), flush=True)
    for name, value in sorted((env_changes or {}).items()):
        print("  with %s=%s" % (name, value), flush=True)
    if DRY_RUN:
        return
    env = None
    if env_changes:
        env = dict(os.environ)
        env.update(env_changes)
    result = subprocess.run(command, env=env)
    if result.returncode != 0:
        sys.exit(result.returncode)

def host_platform():
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"

def is_emscripten_dir(path):
    # Emscripten itself: its CMake toolchain file and emcc (emcc.exe on Windows from Emscripten 6,
    # emcc.bat before)
    if not os.path.isfile(os.path.join(path, "cmake", "Modules", "Platform", "Emscripten.cmake")):
        return False
    return any(os.path.exists(os.path.join(path, name)) for name in ("emcc", "emcc.exe", "emcc.bat", "emcc.py"))

def is_emsdk_root(path):
    return any(os.path.exists(os.path.join(path, name)) for name in ("emsdk.py", "emsdk", "emsdk.bat"))

def newest_tool(base, rel):
    # emsdk installs tools as node/20.18.0_64bit/bin/node ...
    if not os.path.isdir(base):
        return None
    found = sorted(os.path.join(base, entry, rel) for entry in os.listdir(base)
                   if os.path.isfile(os.path.join(base, entry, rel)))
    return found[-1] if found else None

def config_node(config):
    # NODE_JS from a .emscripten config: a quoted path, or emsdk_path + '/...' (what emsdk writes)
    try:
        with open(config, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return None
    for line in lines:
        line = line.strip()
        if not line.startswith("NODE_JS") or "=" not in line:
            continue
        value = line.split("=", 1)[1].strip()
        quote = next((c for c in value if c in "'\""), None)
        if quote is None:
            continue
        inner = value[value.index(quote) + 1:]
        if quote not in inner:
            continue
        path = inner[:inner.index(quote)]
        if value.startswith("emsdk_path"):
            return os.path.join(os.path.dirname(config), path.lstrip("/\\"))
        if value[0] in "'\"":
            return path
    return None

class Emscripten:
    def __init__(self, emscripten, emsdk):
        self.emscripten = emscripten
        self.emsdk = emsdk
        self.toolchain = os.path.join(emscripten, "cmake", "Modules", "Platform", "Emscripten.cmake")
        # the config emcc reads: EM_CONFIG, else <emscripten>/.emscripten, the emsdk's, ~/.emscripten
        self.config = None
        candidates = [os.environ.get("EM_CONFIG"), os.path.join(emscripten, ".emscripten"),
                      os.path.join(emsdk, ".emscripten") if emsdk else None, os.path.join(os.path.expanduser("~"), ".emscripten")]
        for candidate in candidates:
            if candidate and os.path.isfile(candidate):
                self.config = candidate
                break
        windows = host_platform() == "windows"
        node_rel = os.path.join("bin", "node.exe" if windows else "node")
        # node: what emsdk_env chose (EMSDK_NODE), else the config's NODE_JS, else the emsdk's newest,
        # else node on PATH
        node = os.environ.get("EMSDK_NODE")
        if not (node and os.path.isfile(node)):
            node = config_node(self.config) if self.config else None
        if node and not os.path.isfile(node):
            node = None
        if not node and emsdk:
            node = newest_tool(os.path.join(emsdk, "node"), node_rel)
        if not node:
            node = shutil.which("node")
        self.node = node
        self.python = newest_tool(os.path.join(emsdk, "python"), "python.exe" if windows else os.path.join("bin", "python3")) if emsdk else None

    def environment(self):
        # what emsdk_env sets: the SDK's folders first on PATH, EMSDK, EM_CONFIG, EMSDK_NODE, EMSDK_PYTHON
        dirs = []
        for path in (self.emsdk, self.emscripten, os.path.dirname(self.node) if self.node else None,
                     os.path.dirname(self.python) if self.python else None):
            if path and path not in dirs:
                dirs.append(path)
        env = {"PATH": os.pathsep.join(dirs + [os.environ.get("PATH", "")])}
        if self.emsdk:
            env["EMSDK"] = self.emsdk
        if self.config:
            env["EM_CONFIG"] = self.config
        if self.node:
            env["EMSDK_NODE"] = self.node
        if self.python:
            env["EMSDK_PYTHON"] = self.python
        return env

    def cmake_args(self):
        # what emcmake.py adds; CMake is run directly (emcmake.bat would run through cmd.exe, and
        # Emscripten 6 ships emcmake.exe instead), as the editor's Web build does
        args = ["-DCMAKE_TOOLCHAIN_FILE=" + cmake_path(self.toolchain)]
        if self.node:
            args.append("-DCMAKE_CROSSCOMPILING_EMULATOR=" + cmake_path(self.node))
        return args

def cmake_path(path):
    # CMake reads backslashes in cached paths as escapes; forward slashes work everywhere
    return path.replace("\\", "/")

def find_emscripten():
    # Mirrors Exporter::detectEmsdk, plus the older EMSCRIPTEN_ROOT and EMSCRIPTEN variables
    for var in ("EMSDK", "EMSCRIPTEN_ROOT", "EMSCRIPTEN"):
        root = os.environ.get(var)
        if not root:
            continue
        upstream = os.path.join(root, "upstream", "emscripten")
        if is_emscripten_dir(upstream):
            return Emscripten(upstream, root)
        if is_emscripten_dir(root):
            above = os.path.dirname(os.path.dirname(os.path.abspath(root)))
            return Emscripten(root, above if is_emsdk_root(above) else None)
    emcc = shutil.which("emcc")
    if emcc:
        for path in (os.path.dirname(os.path.realpath(emcc)), os.path.dirname(emcc)):
            if is_emscripten_dir(path):
                above = os.path.dirname(os.path.dirname(os.path.abspath(path)))
                return Emscripten(path, above if is_emsdk_root(above) else None)
    return None

def find_vs_ninja():
    # The Ninja of the Visual Studio C++ CMake tools, which Visual Studio keeps off PATH
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)") or os.environ.get("ProgramFiles") or "",
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vswhere):
        return None
    result = subprocess.run([vswhere, "-products", "*", "-sort", "-property", "installationPath"],
                            capture_output=True, text=True)
    for install in result.stdout.splitlines():
        ninja = os.path.join(install.strip(), "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "Ninja", "ninja.exe")
        if install.strip() and os.path.isfile(ninja):
            return ninja
    return None

def choose_generator(platform, build_dir, given):
    """(generator, extra CMake arguments) for a configure, or exits naming what to install."""
    if platform in XCODE_PLATFORMS:
        return "Xcode", []
    if given:
        return given, []
    # CMake reads CMAKE_GENERATOR itself (emcmake.py and the editor leave it to CMake too)
    if os.environ.get("CMAKE_GENERATOR"):
        return None, []
    # A configured build directory can't change generator: CMake keeps its own
    if os.path.isfile(os.path.join(build_dir, "CMakeCache.txt")):
        return None, []
    if platform == "web" and host_platform() == "windows":
        # Emscripten can't use Visual Studio, CMake's default here
        if shutil.which("ninja"):
            return "Ninja", []
        ninja = find_vs_ninja()
        if ninja:
            return "Ninja", ["-DCMAKE_MAKE_PROGRAM=" + cmake_path(ninja)]
        if shutil.which("mingw32-make"):
            return "MinGW Makefiles", []
        fail("no CMake generator for Emscripten: install Ninja (https://ninja-build.org) or mingw32-make and put it "
             "on PATH, or install Visual Studio's C++ CMake tools, or pass --generator")
    if platform != "windows" and shutil.which("ninja"):
        return "Ninja", []
    return None, []

def pbx_string(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'

##
# Based on Steve Robinson comment https://gitlab.kitware.com/cmake/cmake/-/issues/19588
# https://gitlab.com/ssrobins/sdl2-example/blob/cmake_issues_19588/add_xcode_folder_reference.py
##
def add_folder_reference(project, folder_path, target):

    with open(project, encoding="utf-8") as f:
        project_data = f.read()

    folder = os.path.basename(folder_path)

    id1 = str(uuid.uuid4().hex)
    id2 = str(uuid.uuid4().hex)

    pbxbuildfile_header = '/* Begin PBXBuildFile section */'
    project_data = project_data.replace(
        pbxbuildfile_header,
        '{0}\n\t\t{1} /* {2} in Resources */ = {{isa = PBXBuildFile; fileRef = {3} /* {2} */; }};'.format(pbxbuildfile_header, id1, folder, id2),
    )

    if folder.endswith(".xcassets"):
        last_known_file_type = "folder.assetcatalog"
    else:
        last_known_file_type = "folder"

    pbxfilereference_header = '/* Begin PBXFileReference section */'
    project_data = project_data.replace(
        pbxfilereference_header,
        '{0}\n\t\t{1} /* {2} */ = {{isa = PBXFileReference; lastKnownFileType = {3}; name = {4}; path = {5}; sourceTree = "<group>"; }};'.format(pbxfilereference_header, id2, folder, last_known_file_type, pbx_string(folder), pbx_string(folder_path))
    )

    # Get <RESOURCES_PBXGROUP_ID> for 'Resources' in <TARGET>:
    # <PBXGROUP_ID> /* <TARGET> */ = {
    #        isa = PBXGroup;
    #        children = (
    #            <RESOURCES_PBXGROUP_ID> /* Resources */,
    index_pbx_group = project_data.find('/* {0} */ = {{\n\t\t\tisa = PBXGroup;'.format(target))
    index_resources_child = project_data.find('/* Resources */', index_pbx_group)
    if index_pbx_group == -1 or index_resources_child == -1:
        print("Warning: no Resources group in %s, ignoring %s." % (target, folder), flush=True)
        return
    index_start_resources_pbxgroup = project_data.rfind('\t', 0, index_resources_child)
    resources_pbxgroup_id = project_data[index_start_resources_pbxgroup:index_resources_child].strip()

    pbxgroup_section = '{0} /* Resources */ = {{\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = ('.format(resources_pbxgroup_id)
    project_data = project_data.replace(
        pbxgroup_section,
        '{0}\n\t\t\t\t{1} /* {2} */,'.format(pbxgroup_section, id2, folder)
    )

    pbxbuildphase_section = 'isa = PBXResourcesBuildPhase;\n\t\t\tbuildActionMask = 2147483647;\n\t\t\tfiles = ('
    project_data = project_data.replace(
        pbxbuildphase_section,
        '{0}\n\t\t\t\t{1} /* {2} in Resources */,'.format(pbxbuildphase_section, id1, folder)
    )

    with open(project, "w", encoding="utf-8") as f:
        f.write(project_data)

def build():
    parser = argparse.ArgumentParser(description="Configure and build a JasperEngine project with CMake.")
    parser.add_argument("--platform", "-p", type=str.lower, default=host_platform(), choices=["web", "linux", "windows", "macos", "macos-xcode", "ios-xcode"], help="Platform build type (default: %(default)s)")
    parser.add_argument("--jasperengine", "-e", default=os.path.dirname(os.path.abspath(__file__)), help="JasperEngine root path")
    parser.add_argument("--project", "-s", help="Source root path of project files (default: <jasperengine>/project)")
    parser.add_argument("--appname", "-a", help="Project target name (default: from CMakeLists.txt)")
    parser.add_argument("--output", "-o", help="Output directory (default: <jasperengine>/build/<platform>)")
    parser.add_argument("--build", "-b", action=argparse.BooleanOptionalAction, default=True, help="Build after configuring")
    parser.add_argument("--debug", "-d", action=argparse.BooleanOptionalAction, default=False, help="Build type Debug or Release")
    parser.add_argument("--graphic-backend", "-g", type=str.lower, choices=["glcore", "gles3", "metal", "d3d11", "vulkan"], help="Preferred graphic API")
    parser.add_argument("--generator", "-G", help="CMake generator (default: Ninja if installed, Visual Studio for windows)")
    parser.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 1, help="Parallel build jobs (default: %(default)s)")
    parser.add_argument("--define", "-D", action="append", default=[], metavar="VAR=VALUE", help="Extra CMake definition")
    parser.add_argument("--no-cpp-init", action="store_true", help="No call C++ init on project start")
    parser.add_argument("--no-lua-init", action="store_true", help="No call Lua on project start")
    parser.add_argument("--em-shell-file", help="Emscripten shell file")
    parser.add_argument("--dry-run", action="store_true", help="Print the commands (and the environment they get) without running them")
    args = parser.parse_args()

    global DRY_RUN
    DRY_RUN = args.dry_run

    platform = args.platform
    build_type = "Debug" if args.debug else "Release"

    jasperengine_root = os.path.abspath(args.jasperengine)
    project_root = os.path.abspath(args.project or os.path.join(jasperengine_root, "project"))
    build_dir = os.path.abspath(args.output or os.path.join(jasperengine_root, "build", platform))

    required_host = "macos" if platform in XCODE_PLATFORMS else platform
    if platform != "web" and required_host != host_platform():
        fail("--platform %s must be built on %s" % (platform, required_host))
    if not shutil.which("cmake") and not DRY_RUN:
        fail("cmake not found in PATH")

####
## Executing CMake command
####
    cmake_command = [
        "cmake",
        "-S", jasperengine_root,
        "-B", build_dir,
        "-DCMAKE_BUILD_TYPE=" + build_type,
        "-DPROJECT_ROOT=" + project_root,
        ]

    generator, generator_args = choose_generator(platform, build_dir, args.generator)
    if generator:
        cmake_command.extend(["-G", generator])
    cmake_command.extend(generator_args)

    tool_env = None
    if platform == "web":
        emscripten = find_emscripten()
        if not emscripten:
            fail("Emscripten not found, set EMSDK (run emsdk_env) or add emcc to PATH")
        cmake_command.extend(emscripten.cmake_args())
        tool_env = emscripten.environment()
        if args.em_shell_file:
            cmake_command.append('-DEM_ADDITIONAL_LINK_FLAGS=--shell-file "' + os.path.abspath(args.em_shell_file) + '"')

    if platform == "ios-xcode":
        cmake_command.extend(["-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_OSX_SYSROOT=iphoneos"])

    if args.appname:
        cmake_command.append("-DAPP_NAME=" + args.appname)
    if args.graphic_backend:
        cmake_command.append("-DGRAPHIC_BACKEND=" + args.graphic_backend)
    if args.no_cpp_init:
        cmake_command.append("-DNO_CPP_INIT=1")
    if args.no_lua_init:
        cmake_command.append("-DNO_LUA_INIT=1")
    cmake_command.extend("-D" + define for define in args.define)

    run(cmake_command, tool_env)

####
## Adding folder references to Xcode app bundle
####
    # Non-Metal macOS builds are not bundles, assets and lua are copied next to the executable
    if not DRY_RUN and (platform == "ios-xcode" or (platform == "macos-xcode" and args.graphic_backend in (None, "metal"))):
        xcode_project = glob.glob(os.path.join(glob.escape(build_dir), "*.xcodeproj"))[0]
        target = os.path.splitext(os.path.basename(xcode_project))[0]

        for folder in ("assets", "lua"):
            folder_path = os.path.join(project_root, folder)
            if os.path.exists(folder_path):
                add_folder_reference(os.path.join(xcode_project, "project.pbxproj"), folder_path, target)
            else:
                print("Warning: %s does not exist, ignoring." % (folder_path), flush=True)

####
## Executing CMake build command
####
    if args.build:
        cmake_build_command = [
            "cmake",
            "--build", build_dir,
            "--config", build_type,
            "--parallel", str(args.jobs),
            ]
        if platform == "ios-xcode":
            cmake_build_command.extend(["--", "-sdk", "iphonesimulator"])

        run(cmake_build_command, tool_env)


def print_utf8():
    # Piped output (the editor's Build window, a CI log) is in the ANSI code page on Windows,
    # which cannot print every path; a terminal keeps Python's own console handling
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure") and not stream.isatty():
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")


if __name__ == '__main__':
    print_utf8()
    build()
