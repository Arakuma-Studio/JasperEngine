#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
# Puts a test source's file-scope code in a namespace of its own, `namespace <FileStem>_cpp { ... }`
# (docs/testing.md, "Unity build"), the layout tools/check_unity_scope.py asks of every test source:
# its using-directives, aliases, helpers and TESTs go inside, named namespaces (definitions of what a
# header declares, *TestAccess friends), main, extern declarations and explicit specialisations stay
# outside. A source whose code already sits in a namespace only it opens keeps that one. The name is
# <dir>_<FileStem>_cpp (then two directories) when another source has the same stem.
#
#   python3 tools/unity_wrap.py [--root DIR] [file...]   wrap the files (default: every test source the
#                                                          lint flags); prints what it did
#   python3 tools/unity_wrap.py --check [file...]        only says which files it would wrap; exit 1 if any
#   python3 tools/unity_wrap.py --self-test              a flagged source wrapped passes the lint
#
# What it cannot place (a conditional straddling a declaration, braces the lint cannot read) is listed
# as MANUAL; the compiler is the judge of the result: build the test target afterwards.

import argparse
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_unity_scope as cus  # noqa: E402

PP_OPEN = re.compile(r"^#\s*(if|ifdef|ifndef)\b")
PP_MID = re.compile(r"^#\s*(elif|else|elifdef|elifndef)\b")
PP_CLOSE = re.compile(r"^#\s*endif\b")
PP_INCLUDE = re.compile(r"^#\s*(include|import)\b")


class Block:
    def __init__(self, start):
        self.start = start
        self.end = None
        self.branches = [[]]


LOCAL_TYPES = set()  # the classes the file being wrapped defines at file scope


def qualified_definition(head):
    depth = 0
    for i, ch in enumerate(head):
        if ch in "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        elif ch == "(" and depth == 0:
            before = head[:i].rstrip()
            m = re.search(r"([\w:~]+(?:operator\s*\S+)?)$", before)
            if not m:
                return False
            token = m.group(1)
            if "=" in before.replace("operator=", "").replace("==", "").replace("!=", "").replace("<=", "").replace(">=", ""):
                return False
            # a member of a class this file defines (a fixture) is defined beside it, in the namespace
            return "::" in token and token.split("::")[0] not in LOCAL_TYPES
    return False


def wrappable(item):
    if item.kind == "anon":
        return True
    if item.kind != "stmt":
        return False
    head = re.sub(r"\s+", " ", item.head)
    if re.match(r"^(?:int|auto)\s+main\s*\(", head) or re.match(r"^extern\b", head):
        return False
    if re.match(r"^template\s*<\s*>", head):
        return False  # an explicit specialisation names its template's namespace
    if qualified_definition(head):
        return False
    return True


def build_tree(text):
    items, directives, balanced = cus.parse(text)
    if not balanced:
        return None, "unbalanced"
    lines = text.split("\n")
    # an item whose first lines are a conditional's branches (attributes per compiler) starts at
    # that conditional's #if
    directive_at = dict(directives)
    for item in items:
        net = 0
        for ln in range(item.start, item.end + 1):
            d = directive_at.get(ln)
            if d is None:
                continue
            if PP_OPEN.match(d):
                net += 1
            elif PP_CLOSE.match(d):
                net -= 1
        ln = item.start - 1
        while net < 0 and ln >= 0:
            d = directive_at.get(ln)
            if d is not None and PP_OPEN.match(d):
                net += 1
                item.start = ln
            elif d is None and lines[ln].strip():
                break
            ln -= 1
        if net != 0:
            return None, "a conditional straddles a declaration"
    # directives not inside a top-level item
    inside = set()
    for item in items:
        for ln in range(item.start, item.end + 1):
            inside.add(ln)
    top = []
    stack = [top]
    blocks = []
    events = []
    for ln, d in directives:
        if ln in inside:
            # a conditional inside an item must open and close inside it: checked below
            continue
        events.append((ln, "pp", d))
    for item in items:
        events.append((item.start, "item", item))
    events.sort(key=lambda e: (e[0], 0 if e[1] == "pp" else 1))
    for ln, kind, payload in events:
        if kind == "pp":
            d = payload
            if PP_OPEN.match(d):
                b = Block(ln)
                stack[-1].append(b)
                blocks.append(b)
                stack.append(b.branches[-1])
            elif PP_MID.match(d):
                if len(stack) < 2:
                    return None, "stray #else"
                b = blocks_open(stack, blocks)
                b.branches.append([])
                stack[-1] = b.branches[-1]
            elif PP_CLOSE.match(d):
                if len(stack) < 2:
                    return None, "stray #endif"
                b = blocks_open(stack, blocks)
                b.end = ln
                stack.pop()
            elif PP_INCLUDE.match(d):
                stack[-1].append(("include", ln))
            else:
                stack[-1].append(("neutral", ln))
        else:
            stack[-1].append(payload)
    if len(stack) != 1:
        return None, "unclosed #if"
    # an item straddling a conditional's lines
    for item in items:
        for ln, d in directives:
            if item.start <= ln <= item.end and (PP_OPEN.match(d) or PP_MID.match(d) or PP_CLOSE.match(d)):
                pass  # inside the item: balanced by the parser's brace count, fine
    return top, None


def blocks_open(stack, blocks):
    # the block whose branch is stack[-1]
    for b in reversed(blocks):
        if b.end is None and stack[-1] is b.branches[-1]:
            return b
    raise RuntimeError("block")


def entry_wrappable(e):
    if isinstance(e, tuple):
        return e[0] == "neutral"
    if isinstance(e, Block):
        return all(entry_wrappable(x) for br in e.branches for x in br)
    return wrappable(e)


def is_code(e):
    if isinstance(e, tuple):
        return False
    if isinstance(e, Block):
        return any(is_code(x) for br in e.branches for x in br)
    return True


def span(e):
    if isinstance(e, tuple):
        return e[1], e[1]
    if isinstance(e, Block):
        return e.start, e.end
    return e.start, e.end


def regions(level, out):
    current = []
    for e in level + [None]:
        if e is not None and entry_wrappable(e):
            current.append(e)
            continue
        # close the run: trim neutral entries at either end
        run = [x for x in current]
        while run and not is_code(run[0]):
            run.pop(0)
        while run and not is_code(run[-1]):
            run.pop()
        if run:
            out.append((span(run[0])[0], span(run[-1])[1]))
        current = []
        if isinstance(e, Block):
            for br in e.branches:
                regions(br, out)
    return out


def comment_lines_above(lines, code_lines, start, floor):
    s = start
    while s - 1 > floor:
        raw = lines[s - 1].strip()
        if raw and not code_lines[s - 1].strip() and not raw.startswith("#"):
            s -= 1
            continue
        break
    return s


def wrap(relative, text, name):
    LOCAL_TYPES.clear()
    items, _, _ = cus.parse(text)
    for item in cus.walk(items):
        if item.kind == "stmt" and item.body and not cus.named_prefixes(item.path):
            m = re.match(r"^(?:template\s*<.*?>\s*)?(?:struct|class)\s+(?:\[\[[^\]]*\]\]\s*)?(?:alignas\([^)]*\)\s*)?(\w+)", re.sub(r"\s+", " ", item.head))
            if m:
                LOCAL_TYPES.add(m.group(1))
    top, why = build_tree(text)
    if top is None:
        return None, why
    found = regions(top, [])
    if not found:
        return None, "nothing to wrap"
    lines = text.split("\n")
    code, _ = cus.strip_code(text)
    code_lines = code.split("\n")
    inserts = defaultdict(list)  # line index -> text to insert before it
    previous_end = -1
    for start, end in found:
        s = comment_lines_above(lines, code_lines, start, previous_end)
        inserts[s].append("namespace %s {\n" % name)
        inserts[end + 1].append("}  // namespace %s\n" % name)
        previous_end = end
    out = []
    for index, line in enumerate(lines):
        for ins in inserts.get(index, []):
            if ins.startswith("namespace"):
                out.append(ins + "\n")
            else:
                out.append("\n" + ins)
        out.append(line + ("\n" if index < len(lines) - 1 else ""))
    for ins in inserts.get(len(lines), []):
        out.append(("\n" + ins) if not ins.startswith("namespace") else ins)
    result = "".join(out)
    # a closing brace inserted after the last line of a file without a final newline
    return result, None




def adopt_existing(text, name, owners, relative):
    """When the wrapped region is only a preamble before a namespace no other file opens, that
    namespace takes the preamble instead of <Stem>_cpp."""
    m = re.search(r"^namespace %s \{\n" % re.escape(name), text, re.M)
    if not m:
        return text
    close = re.compile(r"^\}  // namespace %s\n" % re.escape(name), re.M)
    c = close.search(text, m.end())
    if not c:
        return text
    rest = text[c.end():]
    n = re.match(r"((?:[ \t]*\n|[ \t]*//[^\n]*\n)*)namespace ([\w:]+) \{[ \t]*(//[^\n]*)?\n", rest)
    if not n or owners.get(n.group(2)) != {relative} or n.group(2).split("::")[0] == "jasperengine":
        return text
    theirs = n.group(2)
    merged = text[:m.start()] + "namespace %s {\n" % theirs + text[m.end():c.start()].rstrip("\n") + "\n\n" + \
        n.group(1).lstrip("\n") + rest[n.end():]
    return merged.replace("namespace %s {\n" % name, "namespace %s {\n" % theirs).replace("}  // namespace %s\n" % name, "}  // namespace %s\n" % theirs)


def namespace_owners(root):
    owners = {}
    for relative, path in cus.files_of(root):
        text = open(path, encoding="utf-8", errors="replace").read()
        if "namespace" in text:
            items, _, _ = cus.parse(text)
            for n in cus.opened_namespaces(items):
                owners.setdefault(n, set()).add(relative)
    return owners


def flagged_tests(root):
    problems = cus.check(root)
    return sorted({p.split(":")[0] for p in problems if p.startswith(cus.TEST_TREES) and p.split(":")[0].endswith(cus.SOURCES)})


def run(root, files, check):
    owners = namespace_owners(root)
    stems = defaultdict(list)
    for relative, _ in cus.files_of(root):
        if relative.endswith(cus.SOURCES):
            stems[cus.stem(relative)].append(relative)
    targets = files or flagged_tests(root)
    wrapped, manual = [], []
    for relative in targets:
        stem = cus.stem(relative)
        parts = relative.split("/")[:-1]
        candidates = [stem + "_cpp"] + ["_".join(re.sub(r"\W", "_", p) for p in parts[-k:]) + "_" + stem + "_cpp"
                                        for k in range(1, len(parts) + 1)]
        name = next((c for c in candidates[(0 if len(stems[stem]) == 1 else 1):] if c not in owners), None)
        if name is None:
            manual.append((relative, "no free namespace name"))
            continue
        path = os.path.join(root, relative)
        text = open(path, encoding="utf-8").read()
        result, why = wrap(relative, text, name)
        if result is None:
            manual.append((relative, why))
            continue
        result = adopt_existing(result, name, owners, relative)
        result = re.sub(r"\n(\}  // namespace [\w:]+\n)\n+$", r"\n\1", result)
        owners[name] = {relative}
        wrapped.append(relative)
        if not check:
            with open(path, "w", encoding="utf-8", newline="") as handle:
                handle.write(result)
    for relative in wrapped:
        print(("would wrap " if check else "wrapped ") + relative)
    for relative, why in manual:
        print("MANUAL %s: %s" % (relative, why))
    return wrapped, manual


def self_test():
    import shutil
    import tempfile
    root = tempfile.mkdtemp(prefix="unity-wrap-")
    try:
        path = os.path.join(root, "engine", "tests", "core", "PlantTest.cpp")
        os.makedirs(os.path.dirname(path))
        os.makedirs(os.path.join(root, "engine", "core"))
        with open(os.path.join(root, "engine", "core", "Shared.h"), "w") as handle:
            handle.write("namespace jasperengine {\nstruct PlantTestAccess;\n}\n")
        with open(path, "w") as handle:
            handle.write("#include <gtest/gtest.h>\n\nusing namespace jasperengine::titleart;\n\nnamespace {\n"
                         "    enum Tile { FULL, ONE_WAY };\n    int helper() { return FULL; }\n}\n\n"
                         "TEST(Plant, Works) { EXPECT_EQ(helper(), 0); }\n\n"
                         "namespace jasperengine {\nstruct PlantTestAccess { int x; };\n}\n")
        before = [p for p in cus.check(root) if p.startswith("engine/tests/core/PlantTest.cpp")]
        would, _ = run(root, [], check=True)
        run(root, [], check=False)
        after = cus.check(root)
        text = open(path).read()
    finally:
        shutil.rmtree(root, ignore_errors=True)
    ok = (before and would == ["engine/tests/core/PlantTest.cpp"] and not after and "namespace PlantTest_cpp {" in text
          and text.index("}  // namespace PlantTest_cpp") < text.index("namespace jasperengine {"))
    if not ok:
        print("unity_wrap self-test failed: lint before %s, would %s, lint after %s:\n%s" % (before, would, after, text), file=sys.stderr)
        return 1
    print("unity_wrap self-test: the flagged plant is wrapped, its friend namespace stays outside, the lint passes")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=cus.ROOT)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("files", nargs="*")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    wrapped, manual = run(args.root, args.files, args.check)
    if args.check:
        return 1 if (wrapped or manual) else 0
    return 1 if manual else 0


if __name__ == "__main__":
    sys.exit(main())
