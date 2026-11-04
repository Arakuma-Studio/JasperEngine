#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Fails when an engine shader takes implicit derivatives in a loop with a per-fragment exit.

Implicit derivatives (texture() choosing its mip level, dFdx, dFdy, fwidth) are undefined in
non-uniform control flow, and a loop whose body has `continue` or `discard` leaves the 2x2 quad
diverged after the first exit. Drivers answer with wrong mips, wrong receiver-plane biases or a
GPU hang (Intel GL: Doriax 1251c3139). Sample with textureLod/textureGrad there and take the
derivatives before the loop (.claude/rules/rendering.md).

A call counts when it is in such a loop's body, directly or through a function that makes one
(any depth, by name: GLSL functions are global once included). A line, or a function's
definition line, carrying `// lod-safe: <reason>` is accepted.

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
IMPLICIT = ("texture", "dFdx", "dFdy", "fwidth")
SAFE_MARK = "lod-safe:"

FUNCTION_DEF = re.compile(
    r"(?m)^[ \t]*(?:(?:highp|mediump|lowp|const|inline)\s+)*[A-Za-z_]\w*\s+([A-Za-z_]\w*)\s*\([^;{)]*\)\s*\{")
LOOP = re.compile(r"\b(?:for|while)\s*\(")
KEYWORDS = {"if", "for", "while", "switch", "return", "else"}


def strip_comments(text):
    """Comments blanked (newlines kept, so offsets keep their line numbers); `lod-safe` lines are
    remembered first."""
    safe_lines = {i + 1 for i, line in enumerate(text.split("\n")) if SAFE_MARK in line}

    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))

    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    text = re.sub(r"//[^\n]*", blank, text)
    return text, safe_lines


def block_end(text, open_brace):
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


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

    direct = call_pattern(IMPLICIT)
    implicit = {name for name, body in bodies.items() if name not in safe_defs and direct.search(body)}
    changed = True
    while changed:
        changed = False
        callers = call_pattern(implicit)
        for name, body in bodies.items():
            if name in implicit or name in safe_defs:
                continue
            if any(m.group(1) != name for m in callers.finditer(body)):
                implicit.add(name)
                changed = True

    findings = []
    calls = call_pattern(set(IMPLICIT) | implicit)
    for path, (clean, safe_lines) in sorted(sources.items()):
        seen = set()
        for loop in LOOP.finditer(clean):
            brace = clean.find("{", loop.end())
            if brace < 0:
                continue
            body = clean[brace:block_end(clean, brace)]
            if not re.search(r"\b(?:continue|discard)\b", body):
                continue
            for call in calls.finditer(body):
                line = line_of(clean, brace + call.start())
                if line in safe_lines or (path, line) in seen:
                    continue
                seen.add((path, line))
                findings.append(f"{path}:{line}: {call.group(1)}() in a loop with a per-fragment exit "
                                f"(textureLod/textureGrad, derivatives before the loop, or '// {SAFE_MARK} <reason>')")
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
