#!/usr/bin/env python3
# Fails when editor/ or engine/core/ write a file without the atomic-write primitive
# (engine/core/io/AtomicFile.h, docs/data-safety.md, S03 R9/AC9). A raw write truncates the
# target first, so a crash or a full disk mid-write destroys it.
#
#   python3 tools/check_raw_writes.py [--root DIR] [--allowlist FILE]
#   python3 tools/check_raw_writes.py --self-test    a planted raw write must fail
#
# Flagged: std::ofstream / ofstream, std::fstream (any mode: say why in the allow-list when it
# only reads), fopen/_wfopen with a "w" or "a" mode, stbi_write_* to a path (the *_to_func forms
# write to memory and are fine), and copy_file with overwrite_existing.
#
# tools/raw_writes_allowlist.txt lists the writes that stay raw, one per line:
#   <path> :: <code fragment found on the line> :: <reason>   (separated by " :: ")
# Exit 0 when every flagged line is allow-listed, 1 otherwise (and for allow-list entries that
# match nothing, so the list cannot rot).

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCANNED = ["editor", os.path.join("engine", "core")]
SKIPPED_DIRS = {"tests", "libs", "external"}
EXTENSIONS = (".cpp", ".h", ".hpp", ".cc", ".mm")

PATTERNS = [
    ("ofstream", re.compile(r"\b(?:std::)?w?ofstream\b")),
    ("fstream", re.compile(r"\bstd::w?fstream\b")),
    ("fopen write", re.compile(r"\b_?w?fopen(?:_s)?\s*\([^;]*?\"[wa][^\"]*\"")),
    ("platformFopen write", re.compile(r"\bplatformFopen\s*\([^;]*?\"[wa][^\"]*\"")),
    ("stbi_write to a path", re.compile(r"\bstbi_write_(?:png|bmp|tga|jpg|hdr)\s*\(")),
    ("copy_file overwrite", re.compile(r"\bcopy_file\s*\([^;]*overwrite_existing")),
]


def strip_comment(line):
    # good enough for this code base: no "//" inside the flagged string literals
    at = line.find("//")
    return line if at < 0 else line[:at]


def load_allowlist(path):
    entries = []
    if not os.path.exists(path):
        return entries
    with open(path, encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = [part.strip() for part in line.split(" :: ")]
            if len(parts) != 3 or not all(parts):
                print("%s:%d: expected '<path> :: <fragment> :: <reason>'" % (path, number), file=sys.stderr)
                sys.exit(2)
            entries.append({"path": parts[0], "fragment": parts[1], "reason": parts[2], "line": number, "used": False})
    return entries


def scan(root):
    hits = []
    for base in SCANNED:
        top = os.path.join(root, base)
        for directory, dirs, files in os.walk(top):
            dirs[:] = [d for d in dirs if d not in SKIPPED_DIRS]
            for name in files:
                if not name.endswith(EXTENSIONS):
                    continue
                path = os.path.join(directory, name)
                relative = os.path.relpath(path, root).replace(os.sep, "/")
                with open(path, encoding="utf-8", errors="replace") as handle:
                    for number, line in enumerate(handle, 1):
                        code = strip_comment(line)
                        for label, pattern in PATTERNS:
                            if pattern.search(code):
                                hits.append((relative, number, label, line.rstrip("\n")))
                                break
    return hits


def self_test():
    """The red control (AC9): a planted raw ofstream fails, the same tree without it passes"""
    import shutil
    import subprocess
    import tempfile
    root = tempfile.mkdtemp(prefix="check-raw-writes-")
    try:
        os.makedirs(os.path.join(root, "editor"))
        os.makedirs(os.path.join(root, "engine", "core"))
        os.makedirs(os.path.join(root, "tools"))
        open(os.path.join(root, "tools", "raw_writes_allowlist.txt"), "w").close()
        planted = os.path.join(root, "editor", "Planted.cpp")
        with open(planted, "w") as handle:
            handle.write('void save() { std::ofstream out("level.scene"); out << "x"; }\n')
        command = [sys.executable, os.path.abspath(__file__), "--root", root]
        failing = subprocess.run(command, capture_output=True, text=True)
        os.remove(planted)
        passing = subprocess.run(command, capture_output=True, text=True)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    if failing.returncode != 1 or "Planted.cpp:1" not in failing.stderr or passing.returncode != 0:
        print("check_raw_writes self-test failed: planted %d, clean %d" % (failing.returncode, passing.returncode), file=sys.stderr)
        return 1
    print("check_raw_writes self-test: a planted raw write fails (1), the clean tree passes (0)")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=ROOT)
    parser.add_argument("--allowlist", default=None)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    allowlist = load_allowlist(args.allowlist or os.path.join(args.root, "tools", "raw_writes_allowlist.txt"))

    problems = []
    for relative, number, label, line in scan(args.root):
        entry = next((e for e in allowlist if e["path"] == relative and e["fragment"] in line), None)
        if entry:
            entry["used"] = True
            continue
        problems.append("%s:%d: raw write (%s): %s" % (relative, number, label, line.strip()))
    for entry in allowlist:
        if not entry["used"]:
            problems.append("tools/raw_writes_allowlist.txt:%d: matches nothing: %s :: %s" % (entry["line"], entry["path"], entry["fragment"]))

    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        print("Write through io::writeFileAtomic / io::publishFile (engine/core/io/AtomicFile.h), or allow-list "
              "the line with a reason in tools/raw_writes_allowlist.txt.", file=sys.stderr)
        return 1
    print("check_raw_writes: no raw writes outside the allow-list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
