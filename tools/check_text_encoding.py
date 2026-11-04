#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
# Fails when editor/ or engine/ convert text or paths through a code page other than UTF-8
# (docs/core-utilities.md "Text encoding", docs/superpowers/specs/2026-11-04-unicode-text-design.md).
# Every std::string is UTF-8: the Windows editor runs in the UTF-8 code page, but games still run
# on Windows before 10 1903, where the ANSI code page is the system's, and child programs write
# their own code page.
#
#   python3 tools/check_text_encoding.py [--root DIR] [--allowlist FILE]
#   python3 tools/check_text_encoding.py --self-test    each planted violation must fail
#
# Flagged:
#   - CP_ACP (convert with CP_UTF8; legacy text goes through StringUtils::legacyToUtf8)
#   - ANSI Win32 calls that take or return text or paths (GetModuleFileNameA, CreateFileA, ...):
#     use the W function and convert with CP_UTF8
#   - cStringUsingEncoding: with anything but NSUTF8StringEncoding, and NSASCIIStringEncoding
#     (NULL for a non-ASCII path: use UTF8String)
#   - findVswherePath outside editor/util/CommandRunner: vswhere runs through
#     CommandRunner::vswhere, which adds -utf8 (else it prints the console code page)
#
# tools/text_encoding_allowlist.txt lists the lines that stay, one per line:
#   <path> :: <code fragment found on the line> :: <reason>   (separated by " :: ")
# Exit 0 when every flagged line is allow-listed, 1 otherwise (and for allow-list entries that
# match nothing, so the list cannot rot).

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCANNED = ["editor", "engine"]
SKIPPED_DIRS = {"tests", "libs", "external"}
SKIPPED_PREFIXES = ("engine/platform/apple/GoogleMobileAds",)
EXTENSIONS = (".cpp", ".h", ".hpp", ".cc", ".mm", ".m", ".c", ".inl")

ANSI_CALLS = (
    "GetModuleFileNameA", "CreateFileA", "CreateDirectoryA", "CreateFileMappingA", "CreateJobObjectA",
    "LoadLibraryA", "LoadLibraryExA", "ShellExecuteA", "ShellExecuteExA", "FindFirstFileA", "FindNextFileA",
    "GetTempPathA", "GetFullPathNameA", "DeleteFileA", "MoveFileA", "MoveFileExA", "CopyFileA",
    "GetFileAttributesA", "GetFileAttributesExA", "SetCurrentDirectoryA", "GetCurrentDirectoryA",
    "CreateProcessA", "MessageBoxA", "SetWindowTextA", "GetEnvironmentVariableA", "SetEnvironmentVariableA",
    "SHGetFolderPathA", "OutputDebugStringA", "GetComputerNameA", "GetUserNameA", "RegQueryValueExA",
    "FormatMessageA",
)
VSWHERE_HOME = ("editor/util/CommandRunner.cpp", "editor/util/CommandRunner.h")

RULES = [
    ("CP_ACP: convert with CP_UTF8 (legacy text: StringUtils::legacyToUtf8)", re.compile(r"\bCP_ACP\b"), None),
    ("ANSI Win32 call: use the W function and convert with CP_UTF8",
     re.compile(r"\b(" + "|".join(ANSI_CALLS) + r")\s*\("), None),
    ("NSASCIIStringEncoding: use UTF8String", re.compile(r"\bNSASCIIStringEncoding\b"), None),
    ("cStringUsingEncoding: other than UTF-8: use UTF8String",
     re.compile(r"cStringUsingEncoding\s*:\s*(?!NSUTF8StringEncoding\b)"), None),
    ("findVswherePath outside CommandRunner: run vswhere through CommandRunner::vswhere (it adds -utf8)",
     re.compile(r"\bfindVswherePath\b"), VSWHERE_HOME),
]


STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"' + r"|'(?:\\.|[^'\\])'")


def strip_comment(line):
    # string literals first: a glob such as "src/*.cpp" is not a comment (a flagged name inside a
    # literal is text the code writes, not a call)
    line = STRING_LITERAL.sub('""', line)
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
                if relative.startswith(SKIPPED_PREFIXES):
                    continue
                in_block_comment = False
                with open(path, encoding="utf-8", errors="replace") as handle:
                    for number, line in enumerate(handle, 1):
                        code = STRING_LITERAL.sub('""', line)
                        if in_block_comment:
                            end = code.find("*/")
                            if end < 0:
                                continue
                            code = code[end + 2:]
                            in_block_comment = False
                        code = strip_comment(code)
                        start = code.find("/*")
                        if start >= 0:
                            end = code.find("*/", start + 2)
                            if end < 0:
                                in_block_comment = True
                                code = code[:start]
                            else:
                                code = code[:start] + code[end + 2:]
                        for label, pattern, home in RULES:
                            if home and relative in home:
                                continue
                            if pattern.search(code):
                                hits.append((relative, number, label, line.rstrip("\n")))
                                break
    return hits


def self_test():
    """The red control: each planted violation fails, the same tree without them passes"""
    import shutil
    import subprocess
    import tempfile
    plants = {
        "editor/Acp.cpp": "std::wstring w = widen(text, CP_ACP);\n",
        "engine/core/Ansi.cpp": "GetModuleFileNameA(nullptr, buffer, MAX_PATH);\n",
        "engine/platform/apple/Ascii.mm": "return [path cStringUsingEncoding:NSASCIIStringEncoding];\n",
        "engine/platform/apple/Latin.mm": "return [path cStringUsingEncoding:NSISOLatin1StringEncoding];\n",
        "editor/Kits.cpp": "std::string vswhere = CommandRunner::findVswherePath();\n",
    }
    root = tempfile.mkdtemp(prefix="check-text-encoding-")
    try:
        for relative, text in plants.items():
            path = os.path.join(root, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as handle:
                handle.write(text)
        # what stays allowed: UTF-8 conversions, comments, the vswhere helper's own file
        os.makedirs(os.path.join(root, "editor", "util"), exist_ok=True)
        with open(os.path.join(root, "editor", "util", "CommandRunner.cpp"), "w") as handle:
            handle.write("std::string p = findVswherePath(); // CP_ACP is not used\n/* GetModuleFileNameA( */\n"
                         "const char* s = [path cStringUsingEncoding:NSUTF8StringEncoding];\n")
        command = [sys.executable, os.path.abspath(__file__), "--root", root, "--allowlist", os.path.join(root, "none.txt")]
        failing = subprocess.run(command, capture_output=True, text=True)
        for relative in plants:
            os.remove(os.path.join(root, relative))
        passing = subprocess.run(command, capture_output=True, text=True)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    missed = [relative for relative in plants if relative + ":1:" not in failing.stderr]
    if failing.returncode != 1 or missed or passing.returncode != 0:
        print("check_text_encoding self-test failed: planted %d (missed %s), clean %d: %s"
              % (failing.returncode, missed, passing.returncode, passing.stderr), file=sys.stderr)
        return 1
    print("check_text_encoding self-test: %d planted violations fail (1), the clean tree passes (0)" % len(plants))
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=ROOT)
    parser.add_argument("--allowlist", default=None)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    allowlist_path = args.allowlist or os.path.join(args.root, "tools", "text_encoding_allowlist.txt")
    allowlist = load_allowlist(allowlist_path)

    problems = []
    for relative, number, label, line in scan(args.root):
        entry = next((e for e in allowlist if e["path"] == relative and e["fragment"] in line), None)
        if entry:
            entry["used"] = True
            continue
        problems.append("%s:%d: %s: %s" % (relative, number, label, line.strip()))
    for entry in allowlist:
        if not entry["used"]:
            problems.append("%s:%d: matches nothing: %s :: %s" % (allowlist_path, entry["line"], entry["path"], entry["fragment"]))

    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        print("Text and paths are UTF-8 (docs/core-utilities.md, \"Text encoding\"); allow-list a line with a "
              "reason in tools/text_encoding_allowlist.txt only when it must stay.", file=sys.stderr)
        return 1
    print("check_text_encoding: no code-page conversions outside the allow-list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
