#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Fails when an engine shader takes implicit derivatives in a loop with a per-fragment exit.

Implicit derivatives (texture(), textureOffset() and textureProj() choosing their mip level,
dFdx, dFdy, fwidth) are undefined in non-uniform control flow, and a loop whose body has
`continue` or `discard`, or a `break` or `return` under a per-fragment `if`, leaves the 2x2 quad
diverged after the first exit. Drivers answer with wrong mips, wrong receiver-plane biases or a
GPU hang (Intel GL: Doriax 1251c3139). Sample with textureLod/textureGrad there and take the
derivatives before the loop (.claude/rules/rendering.md).

A call counts when it is in such a loop's body, or in a loop's per-fragment `if` branch,
directly or through a function or a macro that makes one (any depth, by name: GLSL functions
are global once included). A condition is per-fragment when it reads a varying (`v_*`),
gl_FragCoord, gl_FrontFacing, gl_PointCoord, a sample, or a variable its function assigns from
one of those (through other variables too, anywhere in the function; parameters are not
followed). A loop whose own condition is per-fragment has a per-fragment exit. Loops and
branches without braces count too. A line, or a function's or macro's definition line,
carrying `// lod-safe: <reason>` is accepted.

    python3 tools/check_shader_lod.py [--root DIR]

Exit 0 when nothing is flagged, 1 otherwise (each finding as path:line).
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHADER_DIR = Path("engine") / "shaderlib"
EXTENSIONS = (".glsl", ".frag", ".vert")
IMPLICIT = ("texture", "textureOffset", "textureProj", "textureProjOffset", "dFdx", "dFdy", "fwidth")
SAFE_MARK = "lod-safe:"

FUNCTION_DEF = re.compile(
    r"(?m)^[ \t]*(?:(?:highp|mediump|lowp|const|inline)\s+)*[A-Za-z_]\w*\s+([A-Za-z_]\w*)\s*\([^;{)]*\)\s*\{")
MACRO_DEF = re.compile(r"(?m)^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)((?:\\\n|[^\n])*)")
LOOP = re.compile(r"\b(?:for|while)\s*\(|\bdo\b")
IF = re.compile(r"\bif\s*\(")
PER_FRAGMENT = re.compile(r"\b(?:v_\w+|gl_FragCoord|gl_FrontFacing|gl_PointCoord)\b")
KEYWORDS = {"if", "for", "while", "switch", "return", "else"}
# `name =`, `name.xy +=`, `name[i] *=` (not ==, <=, >= or !=)
ASSIGNMENT = re.compile(r"(?<![\w.])([A-Za-z_]\w*)\s*(?:\.\s*[A-Za-z_]\w*|\[[^\];]*\])*\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)")


def strip_comments(text):
    """Comments blanked (newlines kept, so offsets keep their line numbers); `lod-safe` lines are
    remembered first."""
    safe_lines = {i + 1 for i, line in enumerate(text.split("\n")) if SAFE_MARK in line}

    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))

    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    text = re.sub(r"//[^\n]*", blank, text)
    return text, safe_lines


def block_end(text, open_brace, opening="{", closing="}"):
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == opening:
            depth += 1
        elif text[i] == closing:
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


def skip_space(text, pos):
    while pos < len(text) and text[pos].isspace():
        pos += 1
    return pos


def statement_end(text, pos):
    """The end of the statement at pos: a block, a loop or an `if` (with its `else`) and the
    statement it controls, or up to its `;`."""
    pos = skip_space(text, pos)
    if pos >= len(text):
        return pos
    if text[pos] == "{":
        return block_end(text, pos)
    head = re.match(r"(?:for|while|if)\s*\(", text[pos:])
    if head:
        end = statement_end(text, block_end(text, pos + head.end() - 1, "(", ")"))
        if head.group(0).startswith("if"):
            other = skip_space(text, end)
            if re.match(r"else\b", text[other:]):
                end = statement_end(text, other + 4)
        return end
    if re.match(r"do\b", text[pos:]):
        end = statement_end(text, pos + 2)
        trailer = re.match(r"\s*while\s*\(", text[end:])
        if trailer:
            end = block_end(text, end + trailer.end() - 1, "(", ")")
        return statement_end_semicolon(text, end)
    return statement_end_semicolon(text, pos)


def statement_end_semicolon(text, pos):
    depth = 0
    for i in range(pos, len(text)):
        if text[i] in "([":
            depth += 1
        elif text[i] in ")]":
            depth -= 1
        elif text[i] == ";" and depth <= 0:
            return i + 1
    return len(text)


def loop_body(text, match):
    """(start, end) of the statement a loop controls (a do loop's before its while)."""
    if match.group(0) == "do":
        start = skip_space(text, match.end())
        return start, statement_end(text, start)
    start = skip_space(text, block_end(text, match.end() - 1, "(", ")"))
    return start, statement_end(text, start)


def loop_header(text, match, body_end):
    """What decides whether a loop goes on: a for's or while's parentheses, a do loop's while."""
    if match.group(0) == "do":
        trailer = re.match(r"\s*while\s*\(", text[body_end:])
        if not trailer:
            return ""
        open_paren = body_end + trailer.end() - 1
        return text[open_paren + 1:block_end(text, open_paren, "(", ")") - 1]
    return text[match.end():block_end(text, match.end() - 1, "(", ")") - 1]


def per_fragment(text, calls, locals_):
    return bool(PER_FRAGMENT.search(text) or calls.search(text) or (locals_ and locals_.search(text)))


def per_fragment_branches(text, start, end, calls, locals_=None):
    """(start, end) of each branch, in text[start:end], of an `if` whose condition is per-fragment."""
    branches = []
    for branch in IF.finditer(text, start, end):
        close = block_end(text, branch.end() - 1, "(", ")")
        condition = text[branch.end():close - 1]
        if not per_fragment(condition, calls, locals_):
            continue
        then_end = statement_end(text, close)
        branches.append((close, then_end))
        other = skip_space(text, then_end)
        if re.match(r"else\b", text[other:]):
            branches.append((other + 4, statement_end(text, other + 4)))
    return branches


def assigned_value(text, pos):
    """The expression assigned from pos: up to the `;`, or a `,` or `)` outside brackets."""
    depth = 0
    for i in range(pos, len(text)):
        if text[i] in "([{":
            depth += 1
        elif text[i] in ")]}":
            if depth == 0:
                return text[pos:i]
            depth -= 1
        elif text[i] == ";" or (text[i] == "," and depth == 0):
            return text[pos:i]
    return text[pos:]


def per_fragment_locals(body, calls):
    """A pattern of the variables of a function body that hold a per-fragment value: assigned (or
    declared) from a varying, gl_FragCoord, gl_FrontFacing, gl_PointCoord, a sample or another
    such variable, anywhere in the body (a loop carries a value back to its top). None for none."""
    assignments = [(m.group(1), assigned_value(body, m.end())) for m in ASSIGNMENT.finditer(body)]
    names = set()
    while True:
        known = re.compile(r"\b(?:" + "|".join(map(re.escape, sorted(names))) + r")\b") if names else None
        grown = {name for name, value in assignments if name not in names and
                 (PER_FRAGMENT.search(value) or calls.search(value) or (known and known.search(value)))}
        if not grown:
            return known
        names |= grown


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def call_pattern(names):
    return re.compile(r"(?<![\w.])(" + "|".join(sorted(map(re.escape, names), key=len, reverse=True)) + r")\s*\(")


def scan(files):
    """files: {path: source}. Returns ["path:line: name (...)"] findings."""
    sources = {}
    bodies = {}      # function name -> its bodies (overloads and copies concatenated)
    safe_defs = set()
    for path, text in files.items():
        clean, safe_lines = strip_comments(text)
        sources[path] = (clean, safe_lines)
        for match in FUNCTION_DEF.finditer(clean):
            name = match.group(1)
            if name in KEYWORDS:
                continue
            body = clean[match.end() - 1:block_end(clean, match.end() - 1)]
            bodies[name] = bodies.get(name, "") + body
            if line_of(clean, match.start(1)) in safe_lines:
                safe_defs.add(name)
        # a macro (its continuation lines too) is a function by name
        for match in MACRO_DEF.finditer(clean):
            name = match.group(1)
            bodies[name] = bodies.get(name, "") + match.group(2)
            if line_of(clean, match.start(1)) in safe_lines:
                safe_defs.add(name)

    direct = call_pattern(IMPLICIT)
    macro_names = {m.group(1) for clean, _ in sources.values() for m in MACRO_DEF.finditer(clean)}
    implicit = {name for name, body in bodies.items() if name not in safe_defs and direct.search(body)}
    changed = True
    while changed:
        changed = False
        callers = call_pattern(implicit)
        for name, body in bodies.items():
            if name in implicit or name in safe_defs:
                continue
            if any(m.group(1) != name for m in callers.finditer(body)) or \
                    any(re.search(r"\b" + re.escape(macro) + r"\b", body) for macro in implicit & macro_names if macro != name):
                implicit.add(name)
                changed = True

    findings = []
    calls = call_pattern(set(IMPLICIT) | (implicit - macro_names))
    # an object-like macro is used without parentheses
    uses = re.compile(calls.pattern + "|" + r"(?<![\w.])(" + "|".join(sorted(map(re.escape, implicit & macro_names), key=len, reverse=True) or ["(?!x)x"]) + r")\b")
    for path, (clean, safe_lines) in sorted(sources.items()):
        seen = set()
        functions = []   # (start, end, its per-fragment locals) of each function body
        for match in FUNCTION_DEF.finditer(clean):
            if match.group(1) in KEYWORDS:
                continue
            body_start = match.end() - 1
            body_end = block_end(clean, body_start)
            functions.append((body_start, body_end, per_fragment_locals(clean[body_start:body_end], uses)))

        def flag(start, end, why):
            for call in uses.finditer(clean, start, end):
                line = line_of(clean, call.start())
                if line in safe_lines or (path, line) in seen:
                    continue
                seen.add((path, line))
                findings.append(f"{path}:{line}: {call.group(1) or call.group(2)}() {why} "
                                f"(textureLod/textureGrad, derivatives before the loop, or '// {SAFE_MARK} <reason>')")

        for loop in LOOP.finditer(clean):
            start, end = loop_body(clean, loop)
            locals_ = next((found for a, b, found in functions if a <= loop.start() < b), None)
            branches = per_fragment_branches(clean, start, end, uses, locals_)
            exits = re.search(r"\b(?:continue|discard)\b", clean[start:end]) or any(
                re.search(r"\b(?:break|return)\b", clean[a:b]) for a, b in branches) or \
                per_fragment(loop_header(clean, loop, end), uses, locals_)
            if exits:
                flag(start, end, "in a loop with a per-fragment exit")
            for a, b in branches:
                flag(a, b, "in a loop's per-fragment branch")
    return findings


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", default=str(ROOT))
    args = parser.parse_args(argv)
    root = Path(args.root)
    files = {}
    for path in sorted((root / SHADER_DIR).rglob("*")):
        if path.suffix in EXTENSIONS and path.is_file():
            files[path.relative_to(root).as_posix()] = path.read_text(encoding="utf-8", errors="replace")
    findings = scan(files)
    for finding in findings:
        print(finding)
    if findings:
        print(f"{len(findings)} implicit-derivative call(s) in loops with per-fragment exits", file=sys.stderr)
        return 1
    print(f"check_shader_lod: {len(files)} shader files, nothing flagged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
