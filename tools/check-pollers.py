#!/usr/bin/env python3
"""Fails when one of the five file pollers the file watcher replaced comes back (S05 D3, AC22).

The editor finds out about changes on disk from one watcher (engine/core/io/watch) through the
hot reload (editor/reload). Before it, five pollers stat-ed files on the main thread at their own
rates: HtmlUIWatcher, the throttled stat loop of Project::refreshLinkedMaterials, the code
editor's 1 s checks, the Project window's 1 s timer and the 2 s refreshAssetDatabase poll.

    python3 tools/check-pollers.py [--root DIR] [--self-test]

Prints `file: <rule>` for each and exits 1 when there is one.
"""

import argparse
import os
import sys
import tempfile

# (file, forbidden text, what it is)
RULES = [
    ("editor/window/CodeEditor.cpp", "lastCheckTime", "the code editor's 1 s per-buffer poll"),
    ("editor/window/CodeEditor.cpp", "lastScriptWatchTime", "the code editor's 1 s script poll"),
    ("editor/window/CodeEditor.cpp", "checkExternalScriptChanges", "the code editor's 1 s script poll"),
    ("editor/window/CodeEditor.h", "watchedScriptFiles", "the code editor's script mtime table"),
    ("editor/window/ProjectWindow.cpp", "timeSinceLastCheck", "the Project window's 1 s timer"),
    ("editor/Project.cpp", "refreshLinkedMaterials", "the linked materials' stat loop"),
    ("editor/Project.h", "materialRefreshIntervalSec", "the linked materials' throttle"),
    ("editor/project/ProjectPrefabPaths.cpp", "lastAssetRefreshTime", "the asset database's 2 s poll"),
    ("editor/App.cpp", "refreshAssetDatabase()", "the asset database polled every frame"),
    ("editor/App.cpp", "getHtmlUIWatcher", "the HTML UI poller"),
]
GONE = ["editor/util/HtmlUIWatcher.h", "editor/util/HtmlUIWatcher.cpp"]


def check(root: str) -> list[str]:
    problems = []
    for rel in GONE:
        if os.path.exists(os.path.join(root, rel)):
            problems.append(f"{rel}: the HTML UI poller is back (the hot reload replaced it)")
    for rel, text, what in RULES:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            for number, line in enumerate(f, 1):
                if text in line:
                    problems.append(f"{rel}:{number}: {what} ({text}): file changes come from the watcher (editor/reload)")
    return problems


def self_test() -> int:
    with tempfile.TemporaryDirectory() as root:
        os.makedirs(os.path.join(root, "editor/window"))
        os.makedirs(os.path.join(root, "editor/util"))
        if check(root):
            print("self-test: an empty tree has problems", file=sys.stderr)
            return 1
        with open(os.path.join(root, "editor/window/ProjectWindow.cpp"), "w") as f:
            f.write("    timeSinceLastCheck += dt;\n")
        open(os.path.join(root, "editor/util/HtmlUIWatcher.cpp"), "w").close()
        found = check(root)
        if len(found) != 2:
            print(f"self-test: expected 2 problems, got {found}", file=sys.stderr)
            return 1
    print("check-pollers self-test: ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems = check(os.path.abspath(args.root))
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
