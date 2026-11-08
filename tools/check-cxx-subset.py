#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Checks that engine/ and editor/ stay inside the portable C++20 subset.

The subset is what every toolchain JasperEngine supports compiles: GCC 11's libstdc++ and libc++
under the macOS 10.15 / iOS 13 deployment targets (docs/specs/S01-cxx20.md, D7). This finds the
banned constructs a pattern can find: the library headers those standard libraries lack,
std::format, std::jthread and its stop tokens, std::pmr, coroutines and modules. Vendored code
(engine/libs, editor/libs, libs) is not checked.

    tools/check-cxx-subset.py [<root>]

Prints `file:line: <rule>` for each use and exits 1 when there is one. Comments and string
literals are ignored, so code that writes C++ text (the editor's generators) does not count.
"""

import os
import re
import sys

SCANNED = ("engine", "editor")
SKIPPED = ("engine/libs", "editor/libs")
SKIPPED_PREFIXES: tuple[str, ...] = ()
EXTENSIONS = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}

BANNED_HEADERS = ("format", "semaphore", "latch", "barrier", "stop_token", "syncstream",
                  "memory_resource", "coroutine")

RULES = [
    (re.compile(r"#\s*(?:include|import)\s*<(" + "|".join(BANNED_HEADERS) + r")>"),
     lambda m: f"<{m.group(1)}> is outside the portable C++20 subset"),
    (re.compile(r"\bstd\s*::\s*(v?format(?:_to(?:_n)?)?|formatted_size|make_w?format_args|formatter)\b"),
     lambda m: f"std::{m.group(1)} is outside the portable C++20 subset (use printf-style formatting)"),
    (re.compile(r"\bstd\s*::\s*(jthread|stop_token|stop_source|stop_callback)\b"),
     lambda m: f"std::{m.group(1)} is outside the portable C++20 subset (use std::thread and a flag)"),
    (re.compile(r"\bstd\s*::\s*pmr\s*::"),
     lambda m: "std::pmr is outside the portable C++20 subset"),
    (re.compile(r"\b(co_await|co_yield|co_return)\b"),
     lambda m: f"{m.group(1)}: coroutines are outside the portable C++20 subset"),
    # module declarations and imports: `module;`, `export module a.b;`, `import <vector>;`,
    # `import "x.h";` (the string is blanked out), `import :part;` (not a call or variable named
    # import/module)
    (re.compile(r"^\s*(?:export\s+)?module\b\s*(?::?[A-Za-z_][\w.:]*)?\s*;"),
     lambda m: "module declaration: modules are outside the portable C++20 subset"),
    (re.compile(r"^\s*(?:export\s+)?import\b\s*(?:<[^>]*>|:?[A-Za-z_][\w.:]*)?\s*;"),
     lambda m: "module import: modules are outside the portable C++20 subset"),
]


def identifier_before(text, i):
    """The identifier or number token that ends right before text[i]"""
    start = i
    while start > 0 and (text[start - 1].isalnum() or text[start - 1] == "_"):
        start -= 1
    return text[start:i]


def strip_comments_and_strings(text):
    """The code of a C/C++ source with comments and string/char literals blanked out.
    Newlines are kept, so line numbers stay; the delimiters of #include <...> are untouched."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif c == "R" and i + 1 < n and text[i + 1] == '"' and identifier_before(text, i) in ("", "u8", "u", "U", "L"):
            # raw string literal R"delim( ... )delim"
            open_paren = text.find("(", i + 2)
            if open_paren < 0:
                out.append(c)
                i += 1
                continue
            delim = text[i + 2:open_paren]
            end = text.find(")" + delim + '"', open_paren)
            end = n if end < 0 else end + len(delim) + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif c in "\"'":
            # a digit separator (1'000'000) is not a character literal
            if c == "'" and identifier_before(text, i)[:1].isdigit():
                out.append(" ")
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            end = min(j + 1, n)
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        else:
            out.append(c)
            i += 1
    return "".join(out)


def scanned_files(root):
    for top in SCANNED:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, top)):
            rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
            dirnames[:] = sorted(d for d in dirnames
                                 if f"{rel_dir}/{d}" not in SKIPPED
                                 and not f"{rel_dir}/{d}".startswith(SKIPPED_PREFIXES))
            for name in sorted(filenames):
                if os.path.splitext(name)[1] in EXTENSIONS:
                    yield f"{rel_dir}/{name}"


def check_file(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as f:
        code = strip_comments_and_strings(f.read())
    problems = []
    for number, line in enumerate(code.split("\n"), 1):
        for pattern, message in RULES:
            for match in pattern.finditer(line):
                problems.append(f"{rel}:{number}: {message(match)}")
    return problems


def main(argv):
    root = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    root = os.path.abspath(root)
    problems = []
    count = 0
    for rel in scanned_files(root):
        count += 1
        problems.extend(check_file(root, rel))
    for problem in problems:
        print(problem)
    if problems:
        print(f"check-cxx-subset: {len(problems)} use(s) outside the portable C++20 subset "
              f"(docs/specs/S01-cxx20.md, D7)", file=sys.stderr)
        return 1
    print(f"check-cxx-subset: {count} files in the portable C++20 subset")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
