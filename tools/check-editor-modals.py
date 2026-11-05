#!/usr/bin/env python3
"""Fails on a modal popup begun anywhere but in EditorModal (S69 R1, AC3).

A modal blocks the human at the window, so an agent must see it too: editor_status reports what is
drawn through editor/window/EditorModal.{h,cpp} and respond_dialog answers it. A modal begun with
ImGui's call directly is invisible there. This lint has no exceptions other than EditorModal.cpp.

    python3 tools/check-editor-modals.py [--root DIR] [--self-test]

Prints `file:line: <rule>` for each and exits 1 when there is one.
"""

import argparse
import os
import sys
import tempfile

FORBIDDEN = "BeginPopupModal"
ALLOWED = {os.path.join("editor", "window", "EditorModal.cpp")}
ROOTS = ["editor", os.path.join("engine", "project")]
EXTENSIONS = (".cpp", ".cc", ".h", ".hpp", ".inl", ".mm")
SKIP = {"libs", "thirdparty"}


def code_of(line: str) -> str:
    """the line without its // comment (a URL or a string holding // only ends the scan early)"""
    at = line.find("//")
    return line if at < 0 else line[:at]


def check(root: str) -> list[str]:
    problems = []
    for top in ROOTS:
        for folder, dirs, files in os.walk(os.path.join(root, top)):
            dirs[:] = [d for d in dirs if d not in SKIP]
            for name in files:
                if not name.endswith(EXTENSIONS):
                    continue
                path = os.path.join(folder, name)
                rel = os.path.relpath(path, root)
                if rel in ALLOWED:
                    continue
                with open(path, encoding="utf-8", errors="replace") as f:
                    for number, line in enumerate(f, 1):
                        if FORBIDDEN in code_of(line):
                            problems.append(f"{rel}:{number}: a modal popup begun outside EditorModal: agents cannot "
                                            f"see or answer it; use EditorModal::begin")
    return problems


def self_test() -> int:
    with tempfile.TemporaryDirectory() as root:
        window = os.path.join(root, "editor", "window")
        os.makedirs(window)
        with open(os.path.join(window, "EditorModal.cpp"), "w", encoding="utf-8") as f:
            f.write("bool ok = ImGui::BeginPopupModal(name);\n")
        with open(os.path.join(window, "Commented.cpp"), "w", encoding="utf-8") as f:
            f.write("// ImGui::BeginPopupModal in a comment is fine\n")
        if check(root):
            print("self-test: EditorModal.cpp and a comment must pass", file=sys.stderr)
            return 1
        with open(os.path.join(window, "Raw.cpp"), "w", encoding="utf-8") as f:
            f.write("void f() {\n    if (ImGui::BeginPopupModal(\"x\")) {}\n}\n")
        problems = check(root)
        if len(problems) != 1 or "Raw.cpp:2:" not in problems[0]:
            print(f"self-test: a raw modal must fail, got {problems}", file=sys.stderr)
            return 1
    print("check-editor-modals: self-test ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems = check(args.root)
    for problem in problems:
        print(problem)
    print(f"check-editor-modals: {len(problems)} raw modal(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
