#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
# Fails when render code names a GPU vendor or renderer in a string literal: a driver workaround is a
# GPU quirk (engine/core/device/gpu_quirks.json, GpuQuirks::active), never a renderer-name check in
# the renderer (docs/device-tiers.md "GPU quirks", docs/specs/release-performance-and-gpu-quirks.md
# R24). The tokens are the vendor tokens of engine/core/device/gpu_tiers.json (adreno, mali, powervr,
# nvidia, geforce, radeon, intel, llvmpipe, swiftshader, ...), matched as whole words, any case.
#
#   python3 tools/check_gpu_vendor_branches.py [--root DIR] [--allowlist FILE]
#   python3 tools/check_gpu_vendor_branches.py --self-test     a planted vendor check must fail
#
# Scanned: engine/renders, engine/core/render, engine/shaderlib. Not scanned: engine/libs (vendored
# code keeps its own ledger) and engine/platform (gamepad names, the loader's workaround: not drawing
# decisions). Comments do not count.
#
# tools/gpu_vendor_branches_allowlist.txt lists the literals that stay (log-only text), one per line:
#   <path> :: <literal text> :: <reason>
# Exit 0 when every literal found is allow-listed, 1 otherwise (and for entries that match nothing).

import argparse
import json
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCANNED = ["engine/renders", "engine/core/render", "engine/shaderlib"]
EXTENSIONS = (".cpp", ".h", ".hpp", ".cc", ".mm", ".m", ".c", ".inl", ".glsl", ".vert", ".frag", ".comp")
ALLOWLIST = os.path.join("tools", "gpu_vendor_branches_allowlist.txt")

# comments, then string literals (raw strings first)
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
LITERAL = re.compile(r'R"([^(\s]*)\((.*?)\)\1"|"((?:\\.|[^"\\\n])*)"', re.S)


def tokens(root):
    with open(os.path.join(root, "engine", "core", "device", "gpu_tiers.json"), encoding="utf-8") as f:
        db = json.load(f)
    found = set()
    for vendor in db["vendors"]:
        found.update(t.lower() for t in vendor["tokens"])
    return sorted(found)


def strip_comments(text):
    # string literals first, so a "//" inside one is not a comment
    out = []
    pos = 0
    pattern = re.compile(LITERAL.pattern + r"|" + COMMENT.pattern, re.S)
    for m in pattern.finditer(text):
        out.append(text[pos:m.start()])
        piece = m.group(0)
        out.append(piece if piece.startswith(('"', 'R"')) else re.sub(r"[^\n]", " ", piece))
        pos = m.end()
    out.append(text[pos:])
    return "".join(out)


def scan(root, words):
    pattern = re.compile(r"\b(" + "|".join(re.escape(w) for w in words) + r")\b", re.I)
    found = []
    for scanned in SCANNED:
        base = os.path.join(root, scanned)
        for dirpath, _, files in os.walk(base):
            for name in sorted(files):
                if not name.endswith(EXTENSIONS):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                with open(path, encoding="utf-8", errors="replace") as f:
                    text = strip_comments(f.read())
                for m in LITERAL.finditer(text):
                    literal = m.group(2) if m.group(2) is not None else m.group(3)
                    if literal and pattern.search(literal):
                        line = text.count("\n", 0, m.start()) + 1
                        found.append((rel, line, literal))
    return found


def read_allowlist(path):
    entries = []
    if not os.path.isfile(path):
        return entries
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split(" :: ")]
            if len(parts) != 3 or not parts[2]:
                entries.append((None, line, None))
                continue
            entries.append(tuple(parts))
    return entries


def check(root, allowlist):
    words = tokens(root)
    entries = read_allowlist(allowlist)
    problems = []
    used = set()
    for entry in entries:
        if entry[0] is None:
            problems.append("%s: not '<path> :: <literal> :: <reason>': %s" % (os.path.relpath(allowlist, root), entry[1]))
    for rel, line, literal in scan(root, words):
        allowed = [i for i, e in enumerate(entries) if e[0] == rel and e[1] in literal]
        if allowed:
            used.update(allowed)
            continue
        problems.append("%s:%d: a GPU vendor or renderer in a string literal (\"%s\"): make it a GPU quirk "
                        "(engine/core/device/gpu_quirks.json, GpuQuirks::active), or allow-list log-only text in %s"
                        % (rel, line, literal[:80], ALLOWLIST))
    for i, e in enumerate(entries):
        if e[0] is not None and i not in used:
            problems.append("%s: an entry that matches nothing: %s :: %s" % (ALLOWLIST, e[0], e[1]))
    return problems


def self_test():
    """A clean copy of the scanned tree passes; a vendor check planted in a render file fails."""
    with tempfile.TemporaryDirectory() as tmp:
        for scanned in SCANNED + ["engine/core/device"]:
            shutil.copytree(os.path.join(ROOT, scanned), os.path.join(tmp, scanned))
        allowlist = os.path.join(ROOT, ALLOWLIST)
        clean = check(tmp, allowlist)
        if clean:
            print("self-test: the copied tree should pass:\n  " + "\n  ".join(clean), file=sys.stderr)
            return 1
        victim = os.path.join(tmp, "engine", "renders", "sokol", "SokolAsyncCreate.cpp")
        with open(victim, "a", encoding="utf-8") as f:
            f.write('\nstatic bool plantedMali(const char* renderer){ return strstr(renderer, "Mali") != nullptr; }\n')
        planted = check(tmp, allowlist)
        if not any("SokolAsyncCreate.cpp" in p and "Mali" in p for p in planted):
            print("self-test: the planted strstr(renderer, \"Mali\") was not found", file=sys.stderr)
            return 1
        # a comment naming a vendor is not a branch
        with open(victim, "a", encoding="utf-8") as f:
            f.write('\n// on "Adreno" drivers ...\n')
        if len(check(tmp, allowlist)) != len(planted):
            print("self-test: a comment was counted", file=sys.stderr)
            return 1
    print("self-test: ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=ROOT)
    parser.add_argument("--allowlist")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems = check(args.root, args.allowlist or os.path.join(args.root, ALLOWLIST))
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
