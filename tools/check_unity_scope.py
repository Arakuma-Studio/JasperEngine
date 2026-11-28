#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
# Fails when a source hands the sources after it in a unity batch something at file scope
# (docs/testing.md, "Unity build"). JASPERENGINE_UNITY_BUILD merges 16 sources into one translation
# unit, and which 16 shifts whenever a source is added or removed, so a leak that compiles today
# breaks a later, unrelated change.
#
#   python3 tools/check_unity_scope.py [--root DIR]
#   python3 tools/check_unity_scope.py --self-test    each planted violation must fail
#
# Flagged, in the sources of editor/ and engine/ (not the trees NOT_UNITY lists, never unity-built):
#   - a `using namespace` at namespace scope, unless it names jasperengine or jasperengine::editor at
#     global scope, names a namespace that encloses it, or sits inside a namespace no other file
#     opens (put the file's code in `namespace <FileStem>_cpp { ... }` or the namespace it
#     implements, or qualify the names);
#   - in a test source (one that defines tests, in engine/tests or editor/tests): a file-local declaration (an anonymous
#     namespace, `static`, a type, an alias, a constant, a template) outside such a file-unique
#     namespace. At global scope plain functions and variables (a second definition fails every
#     build's link), macro calls (TEST, TEST_F, ...) and named namespaces may stand; inside a
#     namespace other files open too (jasperengine, jasperengine::test) no anonymous namespace or
#     `static`.
#   - a header's global `using namespace` (but jasperengine): every includer and its batch gets it;
#   - two headers with the same include guard: the source that includes both (a batch, sooner or
#     later) silently loses the second.
# Exit 0 when nothing is flagged, 1 otherwise.

import argparse
import bisect
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCANNED = ["editor", "engine"]
SKIPPED_DIRS = {"libs", "external", "third_party"}
SOURCES = (".cpp", ".cc", ".cxx")
HEADERS = (".h", ".hpp", ".hh", ".inl", ".inc")
TEST_TREES = ("engine/tests/", "editor/tests/")
# Trees CMake compiles source by source in every build (their source lists skip the unity build, or
# their target has none: engine/CMakeLists.txt, CMakeLists.txt, the *Sources.cmake lists): nothing in
# them shares a unit. A tree that joins a unity build leaves this list.
NOT_UNITY = (
    "editor/htmlui/", "editor/mcp/tools/", "editor/reflect/", "editor/vcs/", "editor/model/",
    "editor/picking/", "editor/sceneview/", "editor/procmat/", "editor/titleart/", "editor/sprite/",
    "editor/terrain/", "editor/pixelart/cleanup/", "editor/pixelart/draw/", "editor/pixelart/fx/",
    "editor/spritenormals/tests/",
    "engine/core/htmlui/", "engine/core/navigation/", "engine/core/noise/", "engine/core/picking/",
    "engine/core/spline/", "engine/core/terrain/", "engine/core/titleart/", "engine/core/voxel/",
    "engine/player/",
    # the HTML UI test target, the crash helper, and the noise suites (each its own unit for their
    # floating-point flags; NoiseGoldenCases.cpp is tools/noise/golden's library source too)
    "engine/tests/htmlui/", "engine/tests/crash/", "engine/tests/core/noise/",
    # one-source helper executables: the benchmarks, the probes and the helpers tests launch
    "engine/tests/core/bench/", "engine/tests/core/audio/AudioMeterProbe.cpp",
    "editor/tests/vcs/ProcessHelper.cpp", "editor/tests/util/LogCrashHelper.cpp",
    "editor/tests/texsynth/standalone/", "editor/tests/engine/physics/standalone/",
    # they replace the global operator new and delete, which no batch may inline
    "engine/tests/core/support/AllocationCounter.cpp", "editor/tests/support/AllocationProbe.cpp",
)
ALLOWED_AT_GLOBAL = {"jasperengine", "jasperengine::editor"}


SPECIAL = re.compile(r"""//|/\*|["']""")
RAW_PREFIX = re.compile(r"(?:^|[^\w])(?:u8|[uUL])?R$")
DIGIT_SEPARATOR = re.compile(r"[0-9][0-9a-fA-FxXbB']*$")


def blank(span):
    return re.sub(r"[^\n]", " ", span)


def strip_code(text):
    """Comments and the insides of string and character literals become spaces, lines and columns
    stay where they were, preprocessor lines (with their continuations) become blank."""
    out = []
    i = 0
    n = len(text)
    while True:
        m = SPECIAL.search(text, i)
        if not m:
            out.append(text[i:])
            break
        at = m.start()
        out.append(text[i:at])
        token = m.group(0)
        if token == "//":
            j = text.find("\n", at)
            j = n if j < 0 else j
            out.append(" " * (j - at))
            i = j
            continue
        if token == "/*":
            j = text.find("*/", at + 2)
            j = n if j < 0 else j + 2
            out.append(blank(text[at:j]))
            i = j
            continue
        if token == "'" and at > 0 and text[at - 1].isalnum() and DIGIT_SEPARATOR.search(text, max(0, at - 32), at):
            out.append("'")  # a digit separator (1'000), not a literal
            i = at + 1
            continue
        if token == '"' and RAW_PREFIX.search(text, max(0, at - 4), at):
            raw = re.match(r'"([^()\\\s]{0,16})\(', text[at:at + 20])
            if raw:
                close = ")" + raw.group(1) + '"'
                j = text.find(close, at + raw.end())
                j = n if j < 0 else j + len(close)
                out.append('"' + blank(text[at + 1:j - 1]) + '"')
                i = j
                continue
        j = at + 1
        while j < n and text[j] != token and text[j] != "\n":
            j += 2 if text[j] == "\\" else 1
        j = min(j, n)
        closed = j < n and text[j] == token
        out.append(token + " " * (j - at - 1) + (token if closed else ""))
        i = j + 1 if closed else j
    lines = "".join(out).split("\n")
    directives = []  # (line index, directive text)
    continued = False
    for index, line in enumerate(lines):
        if continued or line.lstrip().startswith("#"):
            if not continued:
                directives.append((index, line.strip()))
            continued = line.rstrip().endswith("\\")
            lines[index] = ""
    return "\n".join(lines), directives


NAMESPACE_OPEN = re.compile(r"(?:\binline\s+)?\bnamespace\s*([A-Za-z_][\w:]*)?\s*(?:\[\[[^\]]*\]\]\s*)?$")
EXTERN_OPEN = re.compile(r'\bextern\s*"\s*"\s*$')
TYPE_HEAD = re.compile(r"^(?:\[\[[^\]]*\]\]\s*)*(?:template\s*<.*>\s*)?(?:typedef\s+)?(?:struct|class|union|enum)\b")


class Item:
    """A declaration or block at namespace scope: its first and last line (0-based), its text up to
    its first brace (or all of it), the namespace path it stands in and, for a namespace, its
    name and children."""

    def __init__(self, kind, start, path):
        self.kind = kind      # "stmt", "namespace", "anon", "extern"
        self.start = start
        self.end = start
        self.path = path      # tuple of enclosing frames: ("ns", name) / ("anon",) / ("extern",)
        self.head = ""
        self.name = None
        self.body = False     # a class, enum or function body followed the head
        self.children = []


BRACES = re.compile(r"[{};]")


def first_branches(code, directives):
    """The code with every #elif/#else branch blanked: for a source whose branches each open a
    brace of their own."""
    lines = code.split("\n")
    stack = []  # per open conditional: whether its current branch is blanked
    at = dict(directives)
    for index in range(len(lines)):
        d = at.get(index)
        if d is not None:
            if re.match(r"#\s*if", d):
                stack.append(False)
            elif re.match(r"#\s*(elif|else)", d) and stack:
                stack[-1] = True
            elif re.match(r"#\s*endif", d) and stack:
                stack.pop()
            continue
        if any(stack):
            lines[index] = ""
    return "\n".join(lines)


def parse(text):
    """The namespace-scope structure of a source: a list of top-level Items (namespaces nest)."""
    code, directives = strip_code(text)
    items, balanced = parse_code(code)
    if not balanced:
        items, balanced = parse_code(first_branches(code, directives))
    return items, directives, balanced


def parse_code(code):
    line_starts = [0] + [m.end() for m in re.finditer(r"\n", code)]

    def line_of(offset):
        return bisect.bisect_right(line_starts, offset) - 1

    marks = [(m.start(), m.group(0)) for m in BRACES.finditer(code)]
    root = []
    frames = [(None, root, ())]  # (Item or None, children list, path)
    balanced = True
    boundary = 0  # where the current declaration's text starts
    k = 0
    while k < len(marks):
        at, ch = marks[k]
        owner, children, path = frames[-1]
        head = code[boundary:at].strip()
        start_line = line_of(boundary + (len(code[boundary:at]) - len(code[boundary:at].lstrip())))
        if ch == ";":
            if head:
                item = Item("stmt", start_line, path)
                item.head = head
                item.end = line_of(at)
                children.append(item)
            boundary = at + 1
            k += 1
            continue
        if ch == "}":
            if head:  # a declaration without its ';' before the closing brace
                item = Item("stmt", start_line, path)
                item.head = head
                item.end = line_of(at)
                children.append(item)
            if owner is None:
                balanced = False
            else:
                owner.end = line_of(at)
                frames.pop()
            boundary = at + 1
            k += 1
            continue
        item = Item("stmt", start_line if head else line_of(at), path)
        item.head = head
        m = NAMESPACE_OPEN.search(head)
        if m and not head[:m.start()].strip():
            name = m.group(1)
            item.kind = "namespace" if name else "anon"
            item.name = name.lstrip(":") if name else None
            children.append(item)
            frames.append((item, item.children, path + ((("ns", item.name) if name else ("anon",)),)))
            boundary = at + 1
            k += 1
            continue
        m = EXTERN_OPEN.search(head)
        if m and not head[:m.start()].strip():
            item.kind = "extern"
            children.append(item)
            frames.append((item, item.children, path + (("extern",),)))
            boundary = at + 1
            k += 1
            continue
        # a body: skip to its matching brace
        depth = 0
        j = k
        while j < len(marks):
            if marks[j][1] == "{":
                depth += 1
            elif marks[j][1] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if j >= len(marks):
            balanced = False
            j = len(marks) - 1
        close = marks[j][0]
        k = j + 1
        item.body = True
        if TYPE_HEAD.search(head):
            # a class or enum waits for its ';' (a braced initializer read as a body leaves an
            # empty declaration behind)
            while k < len(marks) and marks[k][1] != ";":
                if marks[k][1] == "{":
                    break
                k += 1
            if k < len(marks) and marks[k][1] == ";":
                close = marks[k][0]
                k += 1
        item.end = line_of(close)
        children.append(item)
        boundary = close + 1
    tail = code[boundary:].strip()
    if tail:
        item = Item("stmt", line_of(len(code) - len(code[boundary:].lstrip())), frames[-1][2])
        item.head = tail
        item.end = line_of(len(code) - 1)
        frames[-1][1].append(item)
    if len(frames) != 1:
        balanced = False
    return root, balanced


def walk(items):
    for item in items:
        yield item
        yield from walk(item.children)


def opened_namespaces(items):
    """Every qualified namespace name a file opens, each prefix too (a::b opens a and a::b)."""
    names = set()
    for item in walk(items):
        if item.kind != "namespace":
            continue
        prefix = [frame[1] for frame in item.path if frame[0] == "ns"]
        for part in item.name.split("::"):
            prefix.append(part)
            names.add("::".join(prefix))
    return names


def named_prefixes(path):
    prefix = []
    out = []
    for frame in path:
        if frame[0] == "ns":
            prefix.extend(frame[1].split("::"))
            out.append("::".join(prefix))
    return out


# a test source defines tests; the support sources beside them (fixtures, environments, TempDir)
# implement their headers' namespaces as engine sources do
TEST_MACRO = re.compile(r"^\s*(?:TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*\(", re.M)
FILE_LOCAL = re.compile(r"^(?:\[\[[^\]]*\]\]\s*)*(?:template\s*<.*?>\s*)?(?:static|inline|constexpr|consteval|constinit|thread_local)\b")
GLOBAL_LOCAL = re.compile(r"^(?:\[\[[^\]]*\]\]\s*)*(?:template\s*<\s*[^>\s]|const\b|typedef\b|using\b|namespace\s+\w+\s*=)")


def file_local(item, head, at_global):
    """A declaration whose name stays in its source (internal linkage, a type, an alias, a
    template), so another source of the batch can define it too. At global scope a plain function
    or variable has external linkage: a second definition fails every build's link, unity or not."""
    if FILE_LOCAL.match(head):
        return True
    if not at_global:
        return False
    if GLOBAL_LOCAL.match(head):
        # (a function returning a const reference has external linkage; a const variable has not)
        return not (head.startswith("const") and "(" in head.split("=", 1)[0])

    return item.body and bool(TYPE_HEAD.search(head))


def scan_file(relative, text, owners):
    """Problems of one file; owners maps a namespace name to the files that open it."""
    items, _, balanced = parse(text)
    problems = []
    is_test = relative.startswith(TEST_TREES) and relative.endswith(SOURCES) and TEST_MACRO.search(text) is not None

    def contained(path):
        return any(owners.get(name, set()) == {relative} for name in named_prefixes(path))

    for item in walk(items):
        line = item.start + 1
        head = re.sub(r"\s+", " ", item.head)
        if item.kind == "stmt":
            m = re.match(r"^using\s+namespace\s+([\w:]+)$", head)
            if m and relative.endswith(HEADERS) and not named_prefixes(item.path) and m.group(1).lstrip(":") != "jasperengine":
                problems.append("%s:%d: `using namespace %s` at a header's global scope reaches every source that includes "
                                "it and every later source of their unity batches: put it inside the header's namespace"
                                % (relative, line, m.group(1).lstrip(":")))
                continue
            if m and relative.endswith(SOURCES):
                nominated = m.group(1).lstrip(":")
                enclosing = named_prefixes(item.path)
                if contained(item.path):
                    continue
                # (a test source has no global directive at all: one that never names editor:: names would
                # see the editor's LogEntry beside the engine's after a neighbour's `using namespace
                # jasperengine::editor`)
                if not enclosing and nominated in ALLOWED_AT_GLOBAL and not is_test:
                    continue
                if enclosing and (nominated in enclosing or any(nominated == e.split("::", 1)[-1] for e in enclosing)):
                    continue
                where = enclosing[-1] if enclosing else "global scope"
                problems.append("%s:%d: `using namespace %s` at %s reaches every later source of a unity batch: "
                                "put this file's code in a namespace of its own (`namespace %s_cpp {`) or qualify the names"
                                % (relative, line, nominated, where, stem(relative)))
                continue
        if not is_test or contained(item.path):
            continue
        at_global = not named_prefixes(item.path)
        if item.kind == "anon":
            problems.append("%s:%d: an anonymous namespace %s is shared by every source of a unity batch: "
                            "put it inside `namespace %s_cpp {`" % (relative, line, "at global scope" if at_global else "in " + named_prefixes(item.path)[-1], stem(relative)))
            continue
        if item.kind != "stmt" or any(frame[0] == "anon" for frame in item.path):
            continue
        if at_global and re.match(r"^using\s+namespace\s", head):
            continue  # judged above
        if file_local(item, head, at_global):
            where = "at global scope" if at_global else "in " + named_prefixes(item.path)[-1]
            problems.append("%s:%d: a file-local declaration %s (%s) meets its namesakes in a unity batch: "
                            "put it inside `namespace %s_cpp {`" % (relative, line, where, head[:60], stem(relative)))
    if not balanced:
        problems.append("%s:1: braces do not balance once comments, strings and the preprocessor are set aside: "
                        "check_unity_scope cannot read it" % relative)
    return problems


def stem(relative):
    return re.sub(r"\W", "_", os.path.splitext(os.path.basename(relative))[0])


def files_of(root):
    for base in SCANNED:
        top = os.path.join(root, base)
        for directory, dirs, files in os.walk(top):
            dirs[:] = sorted(d for d in dirs if d not in SKIPPED_DIRS)
            for name in sorted(files):
                if name.endswith(SOURCES + HEADERS):
                    path = os.path.join(directory, name)
                    yield os.path.relpath(path, root).replace(os.sep, "/"), path


GUARD = re.compile(r"^\s*#\s*ifndef\s+(\w+)\s*\n\s*#\s*define\s+\1\b", re.M)


def check(root):
    texts = {}
    owners = {}
    guards = {}
    header_texts = {}
    for relative, path in files_of(root):
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        if relative.endswith(SOURCES):
            texts[relative] = text
        else:
            if "using namespace" in text:
                header_texts[relative] = text
            guard = GUARD.search(text[:20000])
            if guard:
                guards.setdefault(guard.group(1), []).append(relative)
        if "namespace" not in text:
            continue
        items, _, _ = parse(text)
        for name in opened_namespaces(items):
            owners.setdefault(name, set()).add(relative)
    problems = []
    for guard, headers in sorted(guards.items()):
        if len(headers) > 1:
            for relative in headers[1:]:
                problems.append("%s:1: include guard %s is %s's too: a source that includes both loses one of them; "
                                "name the guard after this file" % (relative, guard, headers[0]))
    for relative, text in list(texts.items()) + list(header_texts.items()):
        if relative.startswith(NOT_UNITY):
            continue
        if "using" in text or relative.startswith(TEST_TREES):
            problems.extend(scan_file(relative, text, owners))
    return problems


def self_test():
    """The red control: each planted violation fails, the same tree without them passes"""
    import shutil
    import subprocess
    import tempfile
    plants = {
        "editor/widget/Leaky.cpp": "using namespace jasperengine::editor::ui;\nvoid f() {}\n",
        "engine/core/Shared.cpp": "namespace jasperengine {\nusing namespace jasperengine::titleart;\n}\n",
        "engine/tests/core/AnonTest.cpp": "namespace {\nint helper() { return 1; }\n}\nTEST(A, B) { EXPECT_EQ(helper(), 1); }\n",
        "editor/tests/GlobalTest.cpp": "enum Tile { FULL, ONE_WAY };\nTEST(C, D) {}\n",
        "editor/tests/StaticTest.cpp": "namespace jasperengine::test {\nstatic int lowered() { return 0; }\n}\nTEST(S, T) {}\n",
        "engine/tests/core/TwiceTest.cpp": "namespace twice {\nusing namespace jasperengine::pose;\n}\n",
        "engine/tests/core/TwiceAgainTest.cpp": "namespace twice {\nvoid g();\n}\n",
        "editor/a/Util.h": "#ifndef UTIL_H\n#define UTIL_H\n#endif\n",
        "engine/tests/core/Driver.h": "#pragma once\nusing namespace jasperengine::editor::ui;\nnamespace driver {}\n",
        "editor/b/Util.h": "#ifndef UTIL_H\n#define UTIL_H\n#endif\n",
    }
    root = tempfile.mkdtemp(prefix="check-unity-scope-")
    try:
        for relative, text in plants.items():
            path = os.path.join(root, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as handle:
                handle.write(text)
        # what stays allowed: the file's own namespace, the two everyday directives, an enclosing
        # namespace, block scope, macros at global scope, comments and strings, the HTML UI tests
        clean = {
            "engine/tests/core/CleanTest.cpp":
                "#include <gtest/gtest.h>\nnamespace CleanTest_cpp {\nusing namespace jasperengine;\n"
                "using namespace jasperengine::titleart;\nnamespace {\nint helper() { return 1; }\n}\n"
                "static const int K = 2;\nTEST(Clean, Works) { EXPECT_EQ(helper(), 1); }\n}  // namespace CleanTest_cpp\n"
                "TEST(Clean, AtGlobal) {}\nnamespace jasperengine {\nstruct SystemTestAccess { int x; };\n}\n",
            "editor/window/Clean.cpp":
                "// using namespace jasperengine::particles;\nconst char* s = \"using namespace x;\";\n"
                "namespace jasperengine::editor {\nusing namespace jasperengine;\nvoid f() { using namespace jasperengine::particles; }\n}\n",
            "engine/tests/htmlui/HtmlTest.cpp": "namespace {\nint helper() { return 1; }\n}\n",
            "engine/tests/support/TestSupport.h": "namespace jasperengine::test {\nvoid shared();\n}\n",
        }
        for relative, text in clean.items():
            path = os.path.join(root, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as handle:
                handle.write(text)
        command = [sys.executable, os.path.abspath(__file__), "--root", root]
        failing = subprocess.run(command, capture_output=True, text=True)
        for relative in plants:
            os.remove(os.path.join(root, relative))
        passing = subprocess.run(command, capture_output=True, text=True)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    expected = [relative for relative in plants if relative not in ("engine/tests/core/TwiceAgainTest.cpp", "editor/a/Util.h")]
    missed = [relative for relative in expected if relative + ":" not in failing.stderr]
    if failing.returncode != 1 or missed or passing.returncode != 0:
        print("check_unity_scope self-test failed: planted %d (missed %s), clean %d: %s%s"
              % (failing.returncode, missed, passing.returncode, passing.stderr, failing.stderr), file=sys.stderr)
        return 1
    print("check_unity_scope self-test: %d planted violations fail (1), the clean tree passes (0)" % len(expected))
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=ROOT)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems = check(args.root)
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        print("%d file-scope leak(s) across unity batches (docs/testing.md, \"Unity build\"); "
              "python3 tools/unity_wrap.py wraps the test sources among them" % len(problems), file=sys.stderr)
        return 1
    print("check_unity_scope: nothing at file scope reaches the next source of a unity batch")
    return 0


if __name__ == "__main__":
    sys.exit(main())
