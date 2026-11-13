#!/usr/bin/env python3
# Keeps the Xcode workspace's engine targets compiling exactly what CMake compiles for Apple.
#
#   python3 tools/xcode_sync_sources.py           # add what is missing, drop what CMake does not compile
#   python3 tools/xcode_sync_sources.py --check   # exit 1 and list every difference (writes nothing)
#
# The Xcode project (engine/workspaces/xcode/JasperEngine.xcodeproj, the template of the macOS and
# iOS exports) lists its files by hand, and CMake assembles the jasperengine target from
# engine/CMakeLists.txt, the *Sources.cmake files it includes, globs and per-platform choices: a
# source one has and the other lacks fails the Apple builds at link time (or links twice). This
# evaluates that CMake code once per Apple target, with the platform and the module set below: from
# set(JASPERENGINE_SRCS to add_library(jasperengine), then what can still set a source's flags to the
# end of the file, and the same for the libraries the project compiles in its engine targets
# (ENGINE_LIBRARIES). It models a subset of the language (set, list, if, foreach, include,
# file(GLOB), option, source properties); a condition reading a variable nobody gave or assigned, or
# a command it does not model, stops it with the name to add, so a new option is never read as OFF.
# The result is compared with the Sources phases of jasperengine-macos and jasperengine-ios: a
# missing source, one CMake does not compile, one listed twice, and one CMake builds with
# -ffp-contract=off (the bit-identical modules) without that flag, or the other way round. Other
# per-source flags (-Os, -mavx2) are not compared: the project has never carried them. Every
# configuration of both targets must also have ENGINE_HEADER_PATHS on its header search paths.
#
# The sync adds a missing source as a file reference (in the group of its directory, made when
# missing, or beside the other files of that directory) and one PBXBuildFile per target, and removes
# the build files of a source CMake does not compile or of a second listing. References stay, as
# headers' do: the module scripts (tools/xcode_sync_*.py, tools/*/xcode_sync_*.py) look theirs up by
# id. Object ids are "4A534E53" + 16 hex of the sha1 of the kind and path, so two branches adding
# different files do not collide. Headers, libraries and frameworks are tools/xcode_sync_engine.py's
# and the module scripts'.
#
# The module set is the editor engine's (every module ON) but for Title Art: the project compiles no
# titleart module and links its stub, core/subsystem/TitleArtRuntimeNone.cpp (S81 R51), and defines
# no JASPERENGINE_TITLE_ART, which the engine reads as 0. The export (editor/Exporter.cpp) excludes
# a module's files from this template with EXCLUDED_SOURCE_FILE_NAMES, so the template has them all.

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, "engine")
PROJECT = os.path.join(ENGINE, "workspaces", "xcode", "JasperEngine.xcodeproj", "project.pbxproj")

# what every Apple configuration of engine/CMakeLists.txt sees. Every variable the evaluated code
# reads and does not assign itself must be here or in MODULES (None: known to be unset).
APPLE = {"APPLE": "1", "UNIX": "1", "EMSCRIPTEN": None, "WIN32": None, "ANDROID": None, "MSVC": None, "MINGW": None,
         "CMAKE_CXX_COMPILER_ID": "AppleClang",
         # only the AVX2 flag of one file depends on it, and only -ffp-contract=off is compared
         "CMAKE_SYSTEM_PROCESSOR": "arm64",
         # the engine as a subdirectory of the build, not the standalone game project: what follows
         # add_library then is the engine target's alone
         "CMAKE_SOURCE_DIR": ROOT, "CMAKE_BINARY_DIR": os.path.join(ROOT, "build", "xcode"),
         "CMAKE_CURRENT_BINARY_DIR": os.path.join(ROOT, "build", "xcode", "engine"),
         "JASPERENGINE_LIB_TYPE": "STATIC", "JASPERENGINE_SHARED": "OFF", "JASPERENGINE_TESTS": "OFF",
         "JASPERENGINE_HTMLUI_TESTS": "OFF", "JASPERENGINE_NOISE_ALL_BACKENDS": "OFF",
         "EMSCRIPTEN_THREAD_SUPPORT": None, "JASPERENGINE_EDITOR_VERSION": None}
# the optional modules: the editor engine's set (docs/specs/S34-player-size.md R1), but Title Art,
# and V-HACD, which only the editor's desktop engine decomposes with (the project defines no
# JASPERENGINE_CONVEX_DECOMPOSITION)
MODULES = {"JASPERENGINE_PHYSICS_2D": "ON", "JASPERENGINE_PHYSICS_3D": "ON", "JASPERENGINE_RUNTIME_SHADERS": "ON",
           "JASPERENGINE_NAVIGATION": "ON", "JASPERENGINE_TERRAIN_WORLD": "ON", "JASPERENGINE_TERRAIN_EDIT": "ON",
           "JASPERENGINE_SOUND_LAYER": "ON", "JASPERENGINE_NOISE": "ON", "JASPERENGINE_VOXEL": "ON",
           "JASPERENGINE_HTMLUI": "ON", "JASPERENGINE_VIDEO": "ON", "JASPERENGINE_CONVEX_DECOMPOSITION": "OFF",
           "JASPERENGINE_TITLE_ART": "OFF", "JASPERENGINE_TITLE_ART_3D": "OFF", "JASPERENGINE_TITLE_ART_MESHER": "OFF"}
# the project's engine targets and what CMake calls their platform
TARGETS = [("jasperengine-macos", {"CMAKE_SYSTEM_NAME": "Darwin"}),
           ("jasperengine-ios", {"CMAKE_SYSTEM_NAME": "iOS", "IOS": "1"})]
# libraries CMake builds as their own targets (jasperengine links them) and the project compiles
# in its engine targets: (directory under engine/, CMake target)
ENGINE_LIBRARIES = [("libs/msdfgen", "msdfgen"), ("libs/recastnavigation", "recastnavigation"),
                    ("libs/clipper2", "clipper2")]
# include directories (under engine/) those libraries' users need on the engine targets' header search
# paths in every configuration; Recast/Detour's are tools/xcode_sync_navigation.py's
ENGINE_HEADER_PATHS = ["libs/clipper2/include"]

CONTRACT_FLAG = "-ffp-contract=off"
COMPILED = (".c", ".cc", ".cpp", ".cxx", ".m", ".mm")
ID_PREFIX = "4A534E53"


class SyncError(Exception):
    pass


def object_id(kind, path):
    return ID_PREFIX + hashlib.sha1((kind + ":" + path).encode()).hexdigest()[:16].upper()


# ---------------------------------------------------------------------------------------------
# CMake: the subset engine/CMakeLists.txt uses for the engine target's sources and their flags

class Arg:
    __slots__ = ("text", "quoted")

    def __init__(self, text, quoted):
        self.text, self.quoted = text, quoted


def cmake_commands(text, path):
    """[(name, [Arg], line)] of a CMake file (comments dropped)"""
    commands = []
    i, n, line = 0, len(text), 1

    def bracket_end(start):
        m = re.match(r"\[(=*)\[", text[start:])
        if not m:
            return None
        close = text.find("]" + m.group(1) + "]", start)
        if close < 0:
            raise SyncError("%s:%d: unterminated bracket" % (path, line))
        return close + len(m.group(1)) + 2

    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c.isspace():
            i += 1
        elif c == "#":
            end = bracket_end(i + 1)
            if end is None:
                end = text.find("\n", i)
                end = n if end < 0 else end
            line += text.count("\n", i, end)
            i = end
        else:
            m = re.match(r"[A-Za-z_][A-Za-z0-9_]*", text[i:])
            if not m:
                raise SyncError("%s:%d: expected a command, found %r" % (path, line, text[i:i + 20]))
            name, start_line = m.group(0), line
            i += len(name)
            while i < n and text[i] in " \t":
                i += 1
            if i >= n or text[i] != "(":
                raise SyncError("%s:%d: expected '(' after %s" % (path, line, name))
            i += 1
            depth, args = 1, []
            while True:
                if i >= n:
                    raise SyncError("%s:%d: unterminated %s(" % (path, start_line, name))
                c = text[i]
                if c == "\n":
                    line += 1
                    i += 1
                elif c.isspace():
                    i += 1
                elif c == "#":
                    end = bracket_end(i + 1)
                    if end is None:
                        end = text.find("\n", i)
                        end = n if end < 0 else end
                    line += text.count("\n", i, end)
                    i = end
                elif c == "(":
                    depth += 1
                    args.append(Arg("(", False))
                    i += 1
                elif c == ")":
                    depth -= 1
                    i += 1
                    if depth == 0:
                        break
                    args.append(Arg(")", False))
                elif c == '"':
                    j, out = i + 1, []
                    while True:
                        if j >= n:
                            raise SyncError("%s:%d: unterminated string" % (path, line))
                        if text[j] == "\\" and j + 1 < n:
                            nxt = text[j + 1]
                            out.append({"n": "\n", "t": "\t", "r": "\r", ";": "\\;"}.get(nxt, nxt))
                            j += 2
                            continue
                        if text[j] == '"':
                            break
                        if text[j] == "\n":
                            line += 1
                        out.append(text[j])
                        j += 1
                    args.append(Arg("".join(out), True))
                    i = j + 1
                else:
                    m = re.match(r'(?:[^\s()#"\\]|\\.)+', text[i:])
                    args.append(Arg(m.group(0), False))
                    i += len(m.group(0))
            commands.append((name.lower(), args, start_line))
    return commands


FALSE_CONSTANTS = {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"}
TRUE_CONSTANTS = {"1", "ON", "YES", "TRUE", "Y"}


def is_false(value):
    upper = value.upper()
    return upper in FALSE_CONSTANTS or upper.endswith("-NOTFOUND")


def is_constant(value):
    upper = value.upper()
    return (upper in FALSE_CONSTANTS or upper in TRUE_CONSTANTS or upper.endswith("-NOTFOUND")
            or re.fullmatch(r"-?[0-9]+(\.[0-9]+)?", value) is not None)


# commands whose effect is not a source of the target (nothing of theirs is evaluated)
IGNORED = {"add_custom_command", "add_subdirectory", "include_guard", "include_directories",
           "target_include_directories", "target_link_libraries", "target_compile_definitions",
           "target_compile_options", "target_link_options", "set_target_properties"}
# after add_library: what can still change a source's flags (and the variables those read). Any other
# command there is skipped, and so is a block with none of these inside
PROPERTY_COMMANDS = {"set", "unset", "list", "option", "set_source_files_properties", "set_property"}
ASSIGNING = {"set": 0, "unset": 0, "option": 0, "list": 1, "file": 1, "foreach": 0}
LIST_SEPARATOR = re.compile(r"(?<!\\);")
NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def glob_escape(path):
    # engine/cmake/JasperEnginePaths.cmake's jasperengine_glob_escape
    return re.sub(r"([][*?])", r"[\1]", path)


class CMake:
    """Runs CMake commands with the given variables. A variable given as None is known to be unset;
    one no evaluated code assigns and nobody gave is an error wherever it is read."""

    def __init__(self, source_dir, variables):
        self.source_dir = os.path.abspath(source_dir)
        self.vars = {k: v for k, v in variables.items() if v is not None}
        self.known = {k for k, v in variables.items() if v is None}
        self.vars["CMAKE_CURRENT_SOURCE_DIR"] = self.source_dir
        self.vars["CMAKE_CURRENT_LIST_DIR"] = self.source_dir
        self.properties = {}   # absolute source path -> {property: [values]}
        self.functions = set()
        self.where = ""

    def fail(self, message):
        raise SyncError("%s: %s" % (self.where, message))

    def unknown(self, name):
        self.fail("%s is read but not set: give it a value (or None) in APPLE or MODULES of "
                  "tools/xcode_sync_sources.py" % name)

    def learn(self, commands, base):
        """the names these commands (and the files they include) assign: read unset, they are empty"""
        for name, args, _ in commands:
            if name in ASSIGNING and len(args) > ASSIGNING[name]:
                self.known.add(args[ASSIGNING[name]].text)
            elif name == "include" and args and "${" not in args[0].text and args[0].text.endswith(".cmake"):
                path = os.path.normpath(os.path.join(base, args[0].text))
                if os.path.isfile(path):
                    with open(path, encoding="utf-8") as f:
                        self.learn(cmake_commands(f.read(), path), base)

    # values
    def expand(self, text):
        def var(m):
            name = m.group(1)
            if name in self.vars:
                return self.vars[name]
            if name not in self.known:
                self.unknown(name)
            return ""
        if "$ENV{" in text or "$CACHE{" in text:
            self.fail("environment and cache references are not modelled: " + text)
        previous = None
        while previous != text:
            previous = text
            text = re.sub(r"\$\{([A-Za-z0-9_./+-]+)\}", var, text)
        return text

    def arguments(self, args):
        """the arguments after expansion (an unquoted one splits on ';')"""
        out = []
        for arg in args:
            value = self.expand(arg.text)
            if arg.quoted:
                out.append(value)
            else:
                out.extend(v for v in LIST_SEPARATOR.split(value) if v != "")
        return out

    def list_value(self, name):
        value = self.vars.get(name, "")
        return [v for v in LIST_SEPARATOR.split(value) if v != ""]

    def absolute(self, path):
        return os.path.normpath(path if os.path.isabs(path) else os.path.join(self.source_dir, path))

    # conditions
    def condition(self, args):
        tokens = [(self.expand(a.text), a.quoted) for a in args]
        position = [0]

        def peek():
            return tokens[position[0]] if position[0] < len(tokens) else (None, False)

        def take():
            token = peek()
            position[0] += 1
            return token

        def operand(token):
            text, quoted = token
            if quoted:
                return text
            if text in self.vars:
                return self.vars[text]
            if text in self.known:
                return ""
            if NAME.fullmatch(text) and not is_constant(text):
                self.unknown(text)
            return text

        def truth(token):
            text, quoted = token
            if quoted or is_constant(text):
                return not is_false(text)
            if text in self.vars:
                return not is_false(self.vars[text])
            if text not in self.known:
                self.unknown(text)
            return False

        def atom():
            text, quoted = peek()
            if text is None:
                self.fail("incomplete if() condition")
            if text == "(" and not quoted:
                take()
                value = or_expression()
                if take()[0] != ")":
                    self.fail("unbalanced if() condition")
                return value
            if text == "DEFINED" and not quoted:
                take()
                return take()[0] in self.vars
            if text in ("EXISTS", "COMMAND", "TARGET", "POLICY", "IS_DIRECTORY") and not quoted:
                self.fail("if(%s) is not modelled" % text)
            left = take()
            op = peek()
            if not op[1] and op[0] in ("STREQUAL", "MATCHES", "IN_LIST", "EQUAL"):
                take()
                right = take()
                if op[0] == "STREQUAL":
                    return operand(left) == operand(right)
                if op[0] == "EQUAL":
                    return float(operand(left)) == float(operand(right))
                if op[0] == "MATCHES":
                    return re.search(operand(right), operand(left)) is not None
                return operand(left) in self.list_value(right[0])
            if not op[1] and op[0] is not None and re.fullmatch(r"[A-Z_]+", op[0]) and op[0] not in ("AND", "OR"):
                self.fail("if() operator %s is not modelled" % op[0])
            return truth(left)

        def not_expression():
            if peek() == ("NOT", False):
                take()
                return not not_expression()
            return atom()

        def and_expression():
            value = not_expression()
            while peek() == ("AND", False):
                take()
                right = not_expression()
                value = value and right
            return value

        def or_expression():
            value = and_expression()
            while peek() == ("OR", False):
                take()
                right = and_expression()
                value = value or right
            return value

        result = or_expression()
        if position[0] != len(tokens):
            self.fail("unexpected %r in if()" % tokens[position[0]][0])
        return result

    # blocks
    @staticmethod
    def block_end(commands, start, opener, closer, path):
        depth = 0
        for j in range(start, len(commands)):
            name = commands[j][0]
            if name == opener:
                depth += 1
            elif name == closer:
                depth -= 1
                if depth == 0:
                    return j
        raise SyncError("%s:%d: %s() without %s()" % (path, commands[start][2], opener, closer))

    def run(self, commands, start, end, path, properties_only=False):
        """runs commands[start:end]; properties_only runs only PROPERTY_COMMANDS and the blocks holding one"""
        i = start
        while i < end:
            name, args, line = commands[i]
            self.where = "%s:%d" % (os.path.relpath(path, ROOT), line)
            if name in ("function", "macro"):
                close = self.block_end(commands, i, name, "end" + name, path)
                self.functions.add(args[0].text.lower())
                i = close + 1
                continue
            if name in ("if", "foreach"):
                close = self.block_end(commands, i, name, "end" + name, path)
                if properties_only and not any(c[0] in PROPERTY_COMMANDS for c in commands[i + 1:close]):
                    i = close + 1
                    continue
                if name == "if":
                    self.run_if(commands, i, close, path, properties_only)
                else:
                    values = self.arguments(args)
                    variable, items = values[0], values[1:]
                    if items[:1] == ["IN"]:
                        if items[1:2] != ["LISTS"]:
                            self.fail("foreach(IN %s) is not modelled" % " ".join(items[1:2]))
                        items = [v for list_name in items[2:] for v in self.list_value(list_name)]
                    for item in items:
                        self.vars[variable] = item
                        self.run(commands, i + 1, close, path, properties_only)
                i = close + 1
                continue
            if not properties_only:
                self.command(name, args, path)
            elif name in PROPERTY_COMMANDS and not (name == "set_property" and args and args[0].text != "SOURCE"):
                self.command(name, args, path)
            i += 1

    def run_if(self, commands, start, close, path, properties_only):
        branches, depth, head = [], 0, start
        for j in range(start + 1, close + 1):
            inner = commands[j][0]
            if inner == "if":
                depth += 1
            elif inner == "endif" and depth > 0:
                depth -= 1
            elif depth == 0 and inner in ("elseif", "else", "endif"):
                branches.append((head, j))
                head = j
        for head, body_end in branches:
            kind, condition_args, line = commands[head]
            self.where = "%s:%d" % (os.path.relpath(path, ROOT), line)
            if kind == "else" or self.condition(condition_args):
                self.run(commands, head + 1, body_end, path, properties_only)
                return

    def run_file(self, path):
        with open(path, encoding="utf-8") as f:
            commands = cmake_commands(f.read(), path)
        saved = self.vars["CMAKE_CURRENT_LIST_DIR"]
        self.vars["CMAKE_CURRENT_LIST_DIR"] = os.path.dirname(os.path.abspath(path))
        self.run(commands, 0, len(commands), path)
        self.vars["CMAKE_CURRENT_LIST_DIR"] = saved

    def command(self, name, args, path):
        if name in IGNORED:
            return
        values = self.arguments(args)
        if name == "jasperengine_glob_escape":
            self.vars[values[0]] = glob_escape(values[1])
        elif name in self.functions:
            self.fail("%s() is a function tools/xcode_sync_sources.py does not model" % name)
        elif name == "set":
            variable, items = values[0], values[1:]
            if "PARENT_SCOPE" in items:
                self.fail("set(PARENT_SCOPE) is not modelled")
            if "CACHE" in items:
                items = items[:items.index("CACHE")]
                if variable in self.vars:
                    return
            if items:
                self.vars[variable] = ";".join(items)
            else:
                self.vars.pop(variable, None)
        elif name == "unset":
            self.vars.pop(values[0], None)
        elif name == "option":
            if values[0] not in self.vars:
                self.vars[values[0]] = values[2] if len(values) > 2 else "OFF"
        elif name == "list":
            self.list_command(values)
        elif name == "file":
            self.file_command(values)
        elif name == "include":
            target = values[0]
            if not target.endswith(".cmake"):
                self.fail("include(%s): only files are modelled" % target)
            self.run_file(self.absolute(target))
        elif name == "set_source_files_properties":
            split = values.index("PROPERTIES")
            sources, pairs = values[:split], values[split + 1:]
            if "DIRECTORY" in sources or "TARGET_DIRECTORY" in sources:
                # another directory's sources (SoLoud's): none of this target's
                return
            for source in sources:
                props = self.properties.setdefault(self.absolute(source), {})
                for k in range(0, len(pairs) - 1, 2):
                    props[pairs[k]] = LIST_SEPARATOR.split(pairs[k + 1])
        elif name == "set_property":
            if values[0] != "SOURCE":
                self.fail("set_property(%s) is not modelled" % values[0])
            split = values.index("PROPERTY")
            append = "APPEND" in values[1:split] or "APPEND_STRING" in values[1:split]
            sources = [v for v in values[1:split] if v not in ("APPEND", "APPEND_STRING")]
            prop, items = values[split + 1], values[split + 2:]
            for source in sources:
                props = self.properties.setdefault(self.absolute(source), {})
                props[prop] = (props.get(prop, []) if append else []) + items
        elif name == "message":
            if values and values[0] in ("FATAL_ERROR", "SEND_ERROR"):
                self.fail("CMake stops here: " + " ".join(values[1:]))
        else:
            self.fail("%s() is not modelled by tools/xcode_sync_sources.py" % name)

    def list_command(self, values):
        op, variable, rest = values[0], values[1], values[2:]
        items = self.list_value(variable)
        if op == "APPEND":
            items += rest
        elif op == "PREPEND":
            items = rest + items
        elif op == "REMOVE_ITEM":
            items = [v for v in items if v not in rest]
        elif op == "REMOVE_DUPLICATES":
            items = list(dict.fromkeys(items))
        elif op == "SORT" and not rest:
            items = sorted(items)
        elif op == "FILTER" and len(rest) == 3 and rest[0] in ("INCLUDE", "EXCLUDE") and rest[1] == "REGEX":
            keep = rest[0] == "INCLUDE"
            items = [v for v in items if (re.search(rest[2], v) is not None) == keep]
        else:
            self.fail("list(%s) is not modelled" % " ".join(values[:1] + rest[:2]))
        if items:
            self.vars[variable] = ";".join(items)
        else:
            self.vars.pop(variable, None)

    def file_command(self, values):
        op = values[0]
        if op not in ("GLOB", "GLOB_RECURSE"):
            self.fail("file(%s) is not modelled" % op)
        variable, patterns = values[1], [v for v in values[2:] if v != "CONFIGURE_DEPENDS"]
        if any(p in ("RELATIVE", "LIST_DIRECTORIES", "FOLLOW_SYMLINKS") for p in patterns):
            self.fail("file(%s) options are not modelled" % op)
        found = set()
        for pattern in patterns:
            pattern = self.absolute(pattern)
            # (a directory jasperengine_glob_escape escaped: its classes of one character are that character)
            directory = re.sub(r"\[(.)\]", r"\1", os.path.dirname(pattern))
            mask = os.path.basename(pattern)
            if any(ch in directory for ch in "*?["):
                self.fail("a wildcard directory in file(%s) is not modelled" % op)
            regex = re.compile(fnmatch_regex(mask))
            if op == "GLOB":
                walk = [(directory, [], os.listdir(directory) if os.path.isdir(directory) else [])]
            else:
                walk = os.walk(directory)
            for folder, _, files in walk:
                for f in files:
                    full = os.path.join(folder, f)
                    if regex.fullmatch(f) and os.path.isfile(full):
                        found.add(os.path.normpath(full))
        if found:
            self.vars[variable] = ";".join(sorted(found))
        else:
            self.vars.pop(variable, None)


def fnmatch_regex(mask):
    out = []
    for ch in mask:
        out.append(".*" if ch == "*" else "." if ch == "?" else re.escape(ch))
    return "".join(out)


def cmake_target_sources(source_dir, variables, target, cmake="CMakeLists.txt", first=None):
    """(sources relative to source_dir, the ones built with CONTRACT_FLAG) of a CMake target: cmake
    runs from set(<first> ...) (or its top) to add_library(<target> ...), then what can still set a
    source's flags runs to its end"""
    path = os.path.join(source_dir, cmake)
    with open(path, encoding="utf-8") as f:
        commands = cmake_commands(f.read(), path)
    start = 0 if first is None else next(
        (i for i, (name, args, _) in enumerate(commands) if name == "set" and args and args[0].text == first), None)
    end = next((i for i, (name, args, _) in enumerate(commands)
                if name == "add_library" and args and args[0].text == target), None)
    if start is None or end is None or end < start:
        raise SyncError("%s: no add_library(%s ...)%s" % (path, target, "" if first is None else " after set(%s ...)" % first))
    for name, args, line in commands[end + 1:]:
        if name == "target_sources" and args and args[0].text == target and \
                any(a.text.endswith(COMPILED) for a in args):
            raise SyncError("%s:%d: target_sources(%s) adds a source after add_library: not modelled" % (path, line, target))
    cm = CMake(source_dir, variables)
    cm.learn(commands[start:], source_dir)
    cm.run(commands, start, end, path)
    cm.where = "%s:%d" % (os.path.relpath(path, ROOT), commands[end][2])
    values = cm.arguments(commands[end][1])[1:]
    cm.run(commands, end + 1, len(commands), path, properties_only=True)
    sources = []
    for value in values:
        if value in ("STATIC", "SHARED", "MODULE", "OBJECT", "EXCLUDE_FROM_ALL"):
            continue
        rel = os.path.relpath(cm.absolute(value), cm.source_dir).replace(os.sep, "/")
        if rel not in sources and rel.endswith(COMPILED):
            sources.append(rel)
    contracted = set()
    for source in sources:
        props = cm.properties.get(cm.absolute(source), {})
        flags = props.get("COMPILE_OPTIONS", []) + " ".join(props.get("COMPILE_FLAGS", [])).split()
        if CONTRACT_FLAG in flags:
            contracted.add(source)
    return sources, contracted


def apple_sources(engine_dir, variables, libraries):
    """(engine-relative sources, contracted) the Xcode engine target of one platform must compile:
    the jasperengine target's and those of the libraries it compiles in"""
    sources, contracted = cmake_target_sources(engine_dir, variables, "jasperengine", first="JASPERENGINE_SRCS")
    for directory, target in libraries:
        library, library_contracted = cmake_target_sources(os.path.join(engine_dir, directory), variables, target)
        sources += [directory + "/" + s for s in library]
        contracted |= {directory + "/" + s for s in library_contracted}
    return sources, contracted


# ---------------------------------------------------------------------------------------------
# project.pbxproj: an OpenStep property list, parsed with the offsets the edits need

class PDict(dict):
    """a dictionary with, per key, (entry start, value start, value end, entry end) offsets"""

    def __init__(self):
        super().__init__()
        self.spans = {}
        self.start = self.end = 0


class PList(list):
    def __init__(self):
        super().__init__()
        self.start = self.end = 0


TOKEN = re.compile(r'\s+|/\*.*?\*/|//[^\n]*|"(?:[^"\\]|\\.)*"|[A-Za-z0-9_$+/:.\-]+|[{}()=;,]', re.S)


def pbx_tokens(text):
    tokens, i = [], 0
    while i < len(text):
        m = TOKEN.match(text, i)
        if not m:
            raise SyncError("project.pbxproj: unexpected %r at line %d" % (text[i:i + 20], text.count("\n", 0, i) + 1))
        token = m.group(0)
        if not (token[0].isspace() or token.startswith("/*") or token.startswith("//")):
            tokens.append((token, m.start(), m.end()))
        i = m.end()
    return tokens


def unquote(token):
    if token.startswith('"'):
        return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), token[1:-1])
    return token


def parse_pbxproj(text):
    tokens = pbx_tokens(text)
    position = [0]

    def line(index):
        return text.count("\n", 0, tokens[min(index, len(tokens) - 1)][1]) + 1

    def expect(what):
        if position[0] >= len(tokens) or tokens[position[0]][0] != what:
            found = tokens[position[0]][0] if position[0] < len(tokens) else "end of file"
            raise SyncError("project.pbxproj:%d: expected %r, found %r" % (line(position[0]), what, found))
        position[0] += 1
        return tokens[position[0] - 1]

    def value():
        if position[0] >= len(tokens):
            raise SyncError("project.pbxproj: unexpected end of file")
        token, start, _ = tokens[position[0]]
        if token == "{":
            position[0] += 1
            d = PDict()
            d.start = start
            while tokens[position[0]][0] != "}":
                key, key_start, _ = tokens[position[0]]
                if key in "{}()=;,":
                    raise SyncError("project.pbxproj:%d: expected a key, found %r" % (line(position[0]), key))
                position[0] += 1
                key = unquote(key)
                expect("=")
                value_start = tokens[position[0]][1]
                v = value()
                value_end = tokens[position[0] - 1][2]
                _, _, entry_end = expect(";")
                if key in d:
                    raise SyncError("project.pbxproj:%d: %s is defined twice" % (line(position[0] - 1), key))
                d[key] = v
                d.spans[key] = (key_start, value_start, value_end, entry_end)
                if position[0] >= len(tokens):
                    raise SyncError("project.pbxproj: unexpected end of file in a dictionary")
            d.end = tokens[position[0]][2]
            position[0] += 1
            return d
        if token == "(":
            position[0] += 1
            items = PList()
            items.start = start
            while tokens[position[0]][0] != ")":
                items.append(value())
                if tokens[position[0]][0] == ",":
                    position[0] += 1
                elif tokens[position[0]][0] != ")":
                    raise SyncError("project.pbxproj:%d: expected ',' or ')'" % line(position[0]))
            items.end = tokens[position[0]][2]
            position[0] += 1
            return items
        if token in "{}()=;,":
            raise SyncError("project.pbxproj:%d: unexpected %r" % (line(position[0]), token))
        position[0] += 1
        return unquote(token)

    try:
        root = value()
    except IndexError:
        raise SyncError("project.pbxproj: unexpected end of file (unbalanced braces or parentheses)")
    if position[0] != len(tokens):
        raise SyncError("project.pbxproj:%d: text after the root dictionary" % line(position[0]))
    if not isinstance(root, PDict) or not isinstance(root.get("objects"), PDict):
        raise SyncError("project.pbxproj: no objects dictionary")
    return root


OBJECT_ID = re.compile(r"[0-9A-F]{24}")


def structure_errors(root):
    """what makes the project unloadable: an object without isa, a dangling id, a build file in no
    or two phases, a file in two groups"""
    objects = root["objects"]
    errors = []
    if root.get("rootObject") not in objects:
        errors.append("rootObject %s is not an object" % root.get("rootObject"))

    def walk(value, where):
        if isinstance(value, dict):
            for key, item in value.items():
                walk(item, where + "." + key)
        elif isinstance(value, list):
            for item in value:
                walk(item, where)
        elif OBJECT_ID.fullmatch(value) and value not in objects:
            errors.append("%s names %s, which is no object" % (where, value))

    for oid, obj in objects.items():
        if not OBJECT_ID.fullmatch(oid):
            errors.append("object id %s is not 24 hex digits" % oid)
        if not isinstance(obj, dict) or "isa" not in obj:
            errors.append("object %s has no isa" % oid)
            continue
        walk(obj, oid)
    in_phase, in_group = {}, {}
    for oid, obj in objects.items():
        if not isinstance(obj, dict):
            continue
        if obj.get("isa", "").endswith("BuildPhase"):
            for build in obj.get("files", []):
                if objects.get(build, {}).get("isa") != "PBXBuildFile":
                    errors.append("phase %s lists %s, which is no PBXBuildFile" % (oid, build))
                in_phase.setdefault(build, []).append(oid)
        if obj.get("isa") in ("PBXGroup", "PBXVariantGroup"):
            for child in set(obj.get("children", [])):
                in_group.setdefault(child, []).append(oid)
    for oid, obj in objects.items():
        if isinstance(obj, dict) and obj.get("isa") == "PBXBuildFile":
            if "fileRef" not in obj and "productRef" not in obj:
                errors.append("build file %s has no fileRef" % oid)
            if len(in_phase.get(oid, [])) != 1:
                errors.append("build file %s is in %d build phases" % (oid, len(in_phase.get(oid, []))))
    for child, groups in in_group.items():
        if len(groups) > 1:
            errors.append("%s is in %d groups" % (child, len(groups)))
    return errors


def reference_paths(root, project_dir):
    """file reference id -> absolute path (references under SDKROOT and the like are left out)"""
    objects = root["objects"]
    project = objects[root["rootObject"]]
    paths = {}

    def walk(oid, directory):
        obj = objects.get(oid)
        if not isinstance(obj, dict):
            return
        tree, path = obj.get("sourceTree", "<group>"), obj.get("path")
        if tree == "<group>":
            here = os.path.normpath(os.path.join(directory, path)) if path is not None else directory
        elif tree == "SOURCE_ROOT":
            here = os.path.normpath(os.path.join(project_dir, path or ""))
        elif tree == "<absolute>":
            here = path
        else:
            here = None
        if obj.get("isa") == "PBXFileReference":
            if here is not None:
                paths[oid] = here
            return
        for child in obj.get("children", []):
            if here is not None:
                walk(child, here)

    walk(project["mainGroup"], os.path.normpath(os.path.join(project_dir, project.get("projectDirPath", ""))))
    return paths


def group_paths(root, project_dir):
    """group id -> absolute directory, for the groups of the <group>-relative tree"""
    objects = root["objects"]
    project = objects[root["rootObject"]]
    groups = {}

    def walk(oid, directory):
        obj = objects.get(oid)
        if not isinstance(obj, dict) or obj.get("isa") != "PBXGroup" or obj.get("sourceTree", "<group>") != "<group>":
            return
        here = os.path.normpath(os.path.join(directory, obj["path"])) if "path" in obj else directory
        groups[oid] = here
        for child in obj.get("children", []):
            walk(child, here)

    walk(project["mainGroup"], os.path.normpath(os.path.join(project_dir, project.get("projectDirPath", ""))))
    return groups


def target_sources(root, project_dir, source_dir, target):
    """the id of a native target's Sources phase and [(source relative to source_dir, build file id,
    compiler flags)] of its files"""
    objects = root["objects"]
    targets = [o for o in objects.values() if isinstance(o, dict) and o.get("isa") == "PBXNativeTarget"
               and o.get("name") == target]
    if len(targets) != 1:
        raise SyncError("project.pbxproj has %d targets named %s" % (len(targets), target))
    phases = [p for p in targets[0].get("buildPhases", []) if objects[p].get("isa") == "PBXSourcesBuildPhase"]
    if len(phases) != 1:
        raise SyncError("%s has %d Sources phases" % (target, len(phases)))
    paths = reference_paths(root, project_dir)
    out = []
    for build in objects[phases[0]].get("files", []):
        ref = objects[build].get("fileRef")
        if ref not in paths:
            raise SyncError("%s compiles %s, whose file reference is in no group" % (target, build))
        rel = os.path.relpath(paths[ref], source_dir).replace(os.sep, "/")
        flags = objects[build].get("settings", {}).get("COMPILER_FLAGS", "")
        out.append((rel, build, flags))
    return phases[0], out


# ---------------------------------------------------------------------------------------------
# the comparison and the edits

def compare(cmake_sources, contracted, xcode):
    """the differences of one target: {missing, extra, duplicate, flag_missing, flag_extra} (paths)"""
    listed = {}
    for rel, build, flags in xcode:
        listed.setdefault(rel, []).append((build, flags))
    wanted = set(cmake_sources)
    diff = {"missing": sorted(wanted - set(listed)),
            "extra": sorted(set(listed) - wanted),
            "duplicate": sorted(rel for rel, builds in listed.items() if len(builds) > 1),
            "flag_missing": [], "flag_extra": []}
    for rel in sorted(wanted & set(listed)):
        has = any(CONTRACT_FLAG in flags.split() for _, flags in listed[rel])
        if rel in contracted and not has:
            diff["flag_missing"].append(rel)
        elif rel not in contracted and has:
            diff["flag_extra"].append(rel)
    return diff


FILE_TYPES = {".c": "sourcecode.c.c", ".m": "sourcecode.c.objc", ".mm": "sourcecode.cpp.objcpp"}


def quote(name):
    return name if re.fullmatch(r"[A-Za-z0-9_$+/:.\-]+", name) else '"%s"' % name.replace('"', '\\"')


class Editor:
    """text edits of a project.pbxproj, planned against one parse and applied together"""

    def __init__(self, text, project_dir, source_dir):
        self.text, self.project_dir, self.source_dir = text, project_dir, source_dir
        self.root = parse_pbxproj(text)
        self.objects = self.root["objects"]
        self.edits = []                 # (start, end, replacement)
        self.new_objects = {}           # isa -> [object lines]
        self.list_items = {}            # (object id, key) -> [item lines]
        self.new_groups = {}            # absolute directory -> [id, children lines]
        self.group_dirs = group_paths(self.root, project_dir)
        # a directory's group: the one whose own path names it
        self.groups = {}
        for gid, directory in self.group_dirs.items():
            if "path" in self.objects[gid]:
                self.groups.setdefault(directory, gid)
        self.parent = {}
        for gid in self.group_dirs:
            for child in self.objects[gid].get("children", []):
                self.parent.setdefault(child, gid)
        self.ref_paths = reference_paths(self.root, project_dir)
        self.references = {}
        for ref, path in sorted(self.ref_paths.items()):
            self.references.setdefault(path, ref)
        self.taken = set(self.objects)

    def new_id(self, kind, path):
        oid = object_id(kind, path)
        if oid in self.taken:
            raise SyncError("object id %s (%s %s) is taken" % (oid, kind, path))
        self.taken.add(oid)
        return oid

    def add_object(self, isa, line):
        self.new_objects.setdefault(isa, []).append(line)

    def add_item(self, oid, key, item, comment):
        self.list_items.setdefault((oid, key), []).append("%s /* %s */," % (item, comment))

    def add_child(self, directory, child, comment):
        """a child of the group of this directory (made, with its parents, when missing)"""
        if directory in self.groups:
            self.add_item(self.groups[directory], "children", child, comment)
            return
        if directory not in self.new_groups:
            rel = os.path.relpath(directory, self.source_dir).replace(os.sep, "/")
            if rel.startswith(".."):
                raise SyncError("no group holds %s" % directory)
            self.new_groups[directory] = [self.new_id("group", rel), []]
            self.add_child(os.path.dirname(directory), self.new_groups[directory][0], os.path.basename(directory))
        self.new_groups[directory][1].append("%s /* %s */," % (child, comment))

    def reference_for(self, rel):
        absolute = os.path.normpath(os.path.join(self.source_dir, rel))
        if absolute in self.references:
            return self.references[absolute]
        name = os.path.basename(rel)
        ref = self.new_id("ref", rel)
        file_type = FILE_TYPES.get(os.path.splitext(name)[1], "sourcecode.cpp.cpp")
        directory = os.path.dirname(absolute)
        # a directory with no group of its own whose files another group lists (as ../navigation/X.cpp):
        # the new file joins them there
        siblings = sorted(r for r, path in self.ref_paths.items() if os.path.dirname(path) == directory
                          and r in self.parent and self.objects[r].get("sourceTree") == "<group>")
        if directory not in self.groups and siblings:
            group = self.parent[siblings[0]]
            path = os.path.relpath(absolute, self.group_dirs[group]).replace(os.sep, "/")
        else:
            group, path = None, name
        self.add_object("PBXFileReference", "%s /* %s */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = %s; "
                        "path = %s; sourceTree = \"<group>\"; };" % (ref, name, file_type, quote(path)))
        if group:
            self.add_item(group, "children", ref, name)
        else:
            self.add_child(directory, ref, name)
        self.references[absolute] = ref
        return ref

    def add_source(self, target, phase, rel, contracted):
        ref = self.reference_for(rel)
        name = os.path.basename(rel)
        build = self.new_id("src:" + target, rel)
        settings = ' settings = {COMPILER_FLAGS = "%s"; };' % CONTRACT_FLAG if contracted else ""
        label = name + " in Sources"
        self.add_object("PBXBuildFile", "%s /* %s */ = {isa = PBXBuildFile; fileRef = %s /* %s */;%s };"
                        % (build, label, ref, name, settings))
        self.add_item(phase, "files", build, label)

    def remove_build(self, phase, build):
        """the build file's entry in the phase and its definition"""
        if build not in self.objects[phase]["files"]:
            raise SyncError("%s is not in the files of %s" % (build, phase))
        self.remove_item(phase, "files", build)
        self.remove_definition(build)

    def remove_item(self, oid, key, item):
        items = self.objects[oid][key]
        found = list(re.compile(r"\n[ \t]*%s(?: /\*.*?\*/)?\s*," % item).finditer(self.text, items.start, items.end))
        if not found:
            raise SyncError("%s is not on a line of its own in %s of %s" % (item, key, oid))
        self.edits.extend((m.start(), m.end(), "") for m in found)

    def remove_definition(self, oid):
        start, _, _, end = self.objects.spans[oid]
        line_start = self.text.rfind("\n", 0, start)
        if self.text[line_start + 1:start].strip() == "" and self.text[end:self.text.find("\n", end)].strip() == "":
            start, end = line_start, self.text.find("\n", end)
        self.edits.append((start, end, ""))

    def set_contract_flag(self, build, on):
        settings = self.objects[build].get("settings", {})
        if set(settings) - {"COMPILER_FLAGS"}:
            raise SyncError("build file %s has settings besides COMPILER_FLAGS: edit it in Xcode" % build)
        flags = [f for f in settings.get("COMPILER_FLAGS", "").split() if f != CONTRACT_FLAG] + ([CONTRACT_FLAG] if on else [])
        _, value_start, value_end, _ = self.objects.spans[build]
        body = re.sub(r"\s*settings = \{.*?\};", "", self.text[value_start:value_end], flags=re.S)
        if flags:
            body = body[:-1].rstrip() + ' settings = {COMPILER_FLAGS = "%s"; }; }' % " ".join(flags)
        self.edits.append((value_start, value_end, body))

    def result(self):
        edits = list(self.edits)
        for directory, (gid, children) in self.new_groups.items():
            name = os.path.basename(directory)
            self.add_object("PBXGroup", "%s /* %s */ = {\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n%s\t\t\t);\n"
                            "\t\t\tpath = %s;\n\t\t\tsourceTree = \"<group>\";\n\t\t};"
                            % (gid, name, "".join("\t\t\t\t%s\n" % c for c in children), quote(name)))
        for (oid, key), lines in self.list_items.items():
            close = self.objects[oid][key].end - 1
            line_start = self.text.rfind("\n", 0, close) + 1
            if self.text[line_start:close].strip():
                raise SyncError("%s of %s is written on one line: not supported" % (key, oid))
            indent = re.match(r"[ \t]*", self.text[line_start:]).group(0)
            edits.append((line_start, line_start, "".join("%s\t%s\n" % (indent, item) for item in lines)))
        for isa, lines in self.new_objects.items():
            at = self.text.find("/* End %s section */" % isa)
            if at < 0:
                raise SyncError("project.pbxproj has no %s section" % isa)
            edits.append((at, at, "".join("\t\t%s\n" % line for line in lines)))
        edits.sort(key=lambda e: (e[0], e[1]), reverse=True)
        text, limit = self.text, len(self.text) + 1
        for start, end, replacement in edits:
            if end > limit:
                raise SyncError("overlapping edits at offset %d" % start)
            text = text[:start] + replacement + text[end:]
            limit = start
        return text


def header_path_gaps(root, targets, wanted=ENGINE_HEADER_PATHS):
    """[(target, configuration name, its HEADER_SEARCH_PATHS list, the wanted paths it lacks)]"""
    objects = root["objects"]
    paths = ["$(SRCROOT)/../../" + p for p in wanted]
    out = []
    for target, _ in targets:
        native = [o for o in objects.values() if isinstance(o, dict) and o.get("isa") == "PBXNativeTarget"
                  and o.get("name") == target]
        if len(native) != 1:
            raise SyncError("project.pbxproj has %d targets named %s" % (len(native), target))
        for config in objects[native[0]["buildConfigurationList"]]["buildConfigurations"]:
            name = objects[config].get("name", config)
            present = objects[config].get("buildSettings", {}).get("HEADER_SEARCH_PATHS")
            if not isinstance(present, PList):
                raise SyncError("%s (%s) has no HEADER_SEARCH_PATHS list" % (target, name))
            lacking = [p for p in paths if p not in present]
            if lacking:
                out.append((target, name, present, lacking))
    return out


def add_header_paths(text, gaps):
    edits = []
    for _, _, present, lacking in gaps:
        close = present.end - 1
        line_start = text.rfind("\n", 0, close) + 1
        if text[line_start:close].strip():
            raise SyncError("a HEADER_SEARCH_PATHS list is written on one line: not supported")
        indent = re.match(r"[ \t]*", text[line_start:]).group(0)
        edits.append((line_start, "".join("%s\t%s,\n" % (indent, quote(p)) for p in lacking)))
    for at, insertion in sorted(edits, reverse=True):
        text = text[:at] + insertion + text[at:]
    return text


def header_report(gaps):
    return ["  %s: not on the header search paths of %s (%s)" % (p, target, name)
            for target, name, _, lacking in gaps for p in lacking]


def analyse(text, project_dir, source_dir, targets, variables, libraries=ENGINE_LIBRARIES):
    """the parsed project and [(target, Sources phase id, cmake sources, contracted, xcode list, diff)]"""
    root = parse_pbxproj(text)
    results = []
    for target, platform in targets:
        sources, contracted = apple_sources(source_dir, dict(variables, **platform), libraries)
        phase, xcode = target_sources(root, project_dir, source_dir, target)
        results.append((target, phase, sources, contracted, xcode, compare(sources, contracted, xcode)))
    return root, results


def sync(text, project_dir, source_dir, results):
    editor = Editor(text, project_dir, source_dir)
    objects = editor.objects
    for target, phase, _, contracted, xcode, diff in results:
        builds = {}
        for rel, build, flags in xcode:
            builds.setdefault(rel, []).append((build, flags))
        for rel in diff["missing"]:
            editor.add_source(target, phase, rel, rel in contracted)
        for rel in diff["extra"]:
            for build, _ in builds[rel]:
                editor.remove_build(phase, build)
        for rel in diff["duplicate"]:
            if rel in diff["extra"]:
                continue
            # the one with CMake's flag stays, else the lowest reference id (the same in every target).
            # Both references stay: the module scripts look theirs up by id
            ranked = sorted(builds[rel], key=lambda b: ((CONTRACT_FLAG in b[1].split()) != (rel in contracted),
                                                        objects[b[0]]["fileRef"], b[0]))
            builds[rel] = ranked[:1]
            for build, _ in ranked[1:]:
                editor.remove_build(phase, build)
        for rel in diff["flag_missing"] + diff["flag_extra"]:
            editor.set_contract_flag(builds[rel][0][0], rel in contracted)
    return editor.result()


DIFF_LABELS = [("missing", "not compiled by %s (CMake compiles it)"),
               ("extra", "compiled by %s, not by CMake"),
               ("duplicate", "listed twice in %s"),
               ("flag_missing", "without " + CONTRACT_FLAG + " in %s (CMake gives it)"),
               ("flag_extra", "with " + CONTRACT_FLAG + " in %s (CMake does not give it)")]


def report(results):
    lines = []
    for target, _, _, _, _, diff in results:
        for key, label in DIFF_LABELS:
            for rel in diff[key]:
                lines.append("  %s: %s" % (rel, label % target))
    return lines


def main(argv):
    check = "--check" in argv
    project_dir = os.path.dirname(os.path.dirname(PROJECT))
    variables = dict(APPLE, **MODULES)
    with open(PROJECT, encoding="utf-8") as f:
        text = f.read()
    try:
        root, results = analyse(text, project_dir, ENGINE, TARGETS, variables)
        errors = structure_errors(root)
    except SyncError as e:
        print("xcode_sync_sources: %s" % e)
        return 1
    if errors:
        print("project.pbxproj is not consistent (fix it first):")
        print("\n".join("  " + e for e in errors))
        return 1
    try:
        gaps = header_path_gaps(root, TARGETS)
    except SyncError as e:
        print("xcode_sync_sources: %s" % e)
        return 1
    differences = report(results) + header_report(gaps)
    counts = ", ".join("%s %d" % (target, len(sources)) for target, _, sources, _, _, _ in results)
    if not differences:
        print("Xcode engine targets compile what CMake compiles for Apple (%s sources)" % counts)
        return 0
    if check:
        print("the Xcode engine targets and CMake's Apple sources differ (run python3 tools/xcode_sync_sources.py):")
        print("\n".join(differences))
        return 1
    try:
        text = sync(text, project_dir, ENGINE, results)
        text = add_header_paths(text, header_path_gaps(parse_pbxproj(text), TARGETS))
        root, results = analyse(text, project_dir, ENGINE, TARGETS, variables)
        errors = structure_errors(root) + report(results) + header_report(header_path_gaps(root, TARGETS))
    except SyncError as e:
        print("xcode_sync_sources: %s (nothing written)" % e)
        return 1
    if errors:
        print("the sync left differences (nothing written):")
        print("\n".join("  " + e for e in errors))
        return 1
    with open(PROJECT, "w", encoding="utf-8") as f:
        f.write(text)
    print("synced the Xcode engine targets with CMake's Apple sources:")
    print("\n".join(differences))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
