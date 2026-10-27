#!/usr/bin/env python3
"""Editor key bindings live in the command catalog (docs/specs/S25 R4).

Fails when an editor source binds a key by hand instead of through editor/shortcuts/:

- ``menuShortcut(...)`` (the pre-catalog helper) or a ``MenuItem(label, "Ctrl+S")`` with a
  literal shortcut text: menus take their texts from ``CommandCatalog::menuShortcutText``;
- ``IsKeyPressed(ImGuiKey_Q`` / ``IsKeyChordPressed(`` / ``ImGui::Shortcut(`` for a key that is
  not one of the local navigation keys dialogs and lists keep (Escape, Enter, Tab, the arrows,
  Page Up/Down, Backspace).

Not checked: editor/shortcuts/ (the catalog and its dispatcher), the backends (they translate
keys), the text editors' and text fields' own bindings (CustomTextEditor, SelectableTextView,
InputTextContextMenu), Play input
(PlayInput forwards keys to the game) and the tests. A line that must poll a key anyway says
why with ``// shortcut-lint: allow <reason>``.

    python3 tools/ci/check-hardcoded-shortcuts.py              # the tree
    python3 tools/ci/check-hardcoded-shortcuts.py FILE...      # given files
    python3 tools/ci/check-hardcoded-shortcuts.py --self-test  # the fixtures fail and pass as they should
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "hardcoded-shortcuts"

SKIPPED = (
    "editor/shortcuts/",
    "editor/backend/",
    "editor/tests/",
    "editor/window/widget/CustomTextEditor.cpp",
    "editor/window/widget/SelectableTextView.cpp",
    "editor/window/widget/InputTextContextMenu.cpp",
    "editor/window/PlayInput.cpp",
)

LOCAL_KEYS = {
    "Escape", "Enter", "KeypadEnter", "Tab", "UpArrow", "DownArrow", "LeftArrow", "RightArrow",
    "PageUp", "PageDown", "Backspace",
}

ALLOW = "shortcut-lint: allow"
MENU_SHORTCUT = re.compile(r"\bmenuShortcut\s*\(")
MENU_ITEM_TEXT = re.compile(r"\bMenuItem\s*\([^;]*?,\s*\"(?:Ctrl|Cmd|Alt|Option|Shift|Super|F\d{1,2}\b)[^\"]*\"")
KEY_PRESSED = re.compile(r"\bIsKeyPressed\s*\(\s*(?:ImGuiMod_\w+\s*\|\s*)*ImGuiKey_(\w+)")
CHORD_CALLS = re.compile(r"\b(?:IsKeyChordPressed|ImGui::Shortcut|SetShortcutRouting)\s*\(")


def strip_comments(line: str) -> str:
    return line.split("//", 1)[0]


def check_text(name: str, text: str) -> list[str]:
    problems = []
    for number, raw in enumerate(text.splitlines(), 1):
        if ALLOW in raw:
            continue
        line = strip_comments(raw)
        if MENU_SHORTCUT.search(line):
            problems.append(f"{name}:{number}: menuShortcut(): take the text from CommandCatalog::menuShortcutText")
        if MENU_ITEM_TEXT.search(line):
            problems.append(f"{name}:{number}: a literal shortcut text in MenuItem(): take it from the catalog")
        for match in KEY_PRESSED.finditer(line):
            if match.group(1) not in LOCAL_KEYS:
                problems.append(f"{name}:{number}: IsKeyPressed(ImGuiKey_{match.group(1)}): bind it in "
                                "editor/shortcuts/DefaultCommands.cpp and run it from the dispatcher")
        if CHORD_CALLS.search(line):
            problems.append(f"{name}:{number}: ImGui shortcut routing: bind it in the command catalog")
    return problems


def tree_files() -> list[Path]:
    files = []
    for path in sorted((ROOT / "editor").rglob("*")):
        if path.suffix not in (".cpp", ".h", ".mm") or not path.is_file():
            continue
        relative = path.relative_to(ROOT).as_posix()
        if relative.startswith(SKIPPED):
            continue
        files.append(path)
    return files


def check_files(files: list[Path]) -> list[str]:
    problems = []
    for path in files:
        try:
            name = path.resolve().relative_to(ROOT).as_posix()
        except ValueError:
            name = str(path)
        problems += check_text(name, path.read_text(encoding="utf-8", errors="replace"))
    return problems


def self_test() -> int:
    bad = check_files([FIXTURES / "bad.cpp"])
    good = check_files([FIXTURES / "good.cpp"])
    lines = (FIXTURES / "bad.cpp").read_text().splitlines()
    expected = {number for number, line in enumerate(lines, 1) if line.rstrip().endswith("// expect")}
    found = {int(problem.split(":")[1]) for problem in bad}
    ok = found == expected and not good
    for line in bad + good:
        print(line)
    print(f"self-test: bad.cpp flags lines {sorted(found)} (expected {sorted(expected)}), good.cpp {len(good)}: "
          f"{'ok' if ok else 'FAILED'}")
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    if argv[1:2] == ["--self-test"]:
        return self_test()
    files = [Path(arg) for arg in argv[1:]] if len(argv) > 1 else tree_files()
    problems = check_files(files)
    for line in problems:
        print(line)
    if problems:
        print(f"{len(problems)} hard-coded editor shortcut(s): editor keys are bound in the command catalog "
              "(editor/shortcuts/, docs/editor/keyboard-shortcuts.md)", file=sys.stderr)
        return 1
    print(f"check-hardcoded-shortcuts: {len(files)} files, no hard-coded shortcuts")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
