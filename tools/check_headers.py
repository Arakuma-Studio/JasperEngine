#!/usr/bin/env python3
"""Header self-sufficiency check (docs/specs/S72-build-hygiene.md): each header compiles alone, with the
flags of its target and no precompiled header (exports, web and Android build without one).

    tools/check_headers.py engine/core/Scene.h editor/Project.h   # these
    tools/check_headers.py --all                                  # every header it covers
    tools/check_headers.py --self-test

It covers engine/core/ (the flags of a unit of the jasperengine target) and editor/ (of the editor's
objects), not editor/tests/; anything else given is left out, and said to be. Needs a configured
tree (`tools/build.sh linux`) whose generated headers exist, and the build image (the tree's paths
are /src/...): tools/check-changed runs it for the headers a change edits. Standard library only.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TREE = ROOT / "build" / "linux" / "editor"
HEADER = (".h", ".hh", ".hpp")
ENGINE_DIR = "engine/core/"
EDITOR_DIR = "editor/"
# (a value of its own next to these flags)
TAKES_VALUE = {"-o", "-MF", "-MT", "-MQ"}
DROPPED = {"-c", "-MD", "-MMD", "-Winvalid-pch"}
PCH_MARK = "cmake_pch"


def covered(path: str) -> bool:
    return path.endswith(HEADER) and path.startswith((ENGINE_DIR, EDITOR_DIR)) and not path.startswith("editor/tests/")


def flags_of(command: str, source: str) -> list[str]:
    """A unit's compile command without its source, outputs, dependency files and precompiled header."""
    args = shlex.split(command)
    if args and os.path.basename(args[0]) == "ccache":
        args = args[1:]
    out: list[str] = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg in TAKES_VALUE:
            i += 2
            continue
        if arg == "-include" and i + 1 < len(args) and PCH_MARK in args[i + 1]:
            i += 2
            continue
        if arg not in DROPPED and arg != source:
            out.append(arg)
        i += 1
    return out


def target_flags(entries: list[dict]) -> dict[str, tuple[str, list[str]]]:
    """engine/core/ and editor/ -> (the unit's directory, its flags): the first own unit of each target."""
    found: dict[str, tuple[str, list[str]]] = {}
    for entry in entries:
        command = entry.get("command", "")
        source = entry.get("file", "")
        if not source.endswith((".cpp", ".cc")) or PCH_MARK in source:
            continue
        if "jasperengine-editor-objects.dir" in command and "/editor/" in source:
            found.setdefault(EDITOR_DIR, (entry["directory"], flags_of(command, source)))
        elif "/jasperengine.dir/" in command and "/engine/core/" in source:
            found.setdefault(ENGINE_DIR, (entry["directory"], flags_of(command, source)))
        if len(found) == 2:
            break
    return found


def check_one(path: str, root: Path, flags: dict[str, tuple[str, list[str]]]) -> tuple[str, bool, str]:
    directory, args = flags[ENGINE_DIR if path.startswith(ENGINE_DIR) else EDITOR_DIR]
    # (through stdin: a header as the main file warns about its #pragma once)
    command = [*args, "-fsyntax-only", "-x", "c++", "-"]
    run = subprocess.run(command, cwd=directory, input=f'#include "{root / path}"\n', capture_output=True, text=True)
    return path, run.returncode == 0, run.stderr


def all_headers(root: Path) -> list[str]:
    tracked = subprocess.run(["git", "ls-files", ENGINE_DIR, EDITOR_DIR], cwd=str(root), capture_output=True, text=True)
    return sorted(p for p in tracked.stdout.splitlines() if covered(p))


def self_test() -> int:
    command = "ccache /usr/bin/c++ -DA=1 -I/src/x -Winvalid-pch -include /t/CMakeFiles/e.dir/cmake_pch.hxx -MD -MT o.o -MF o.d -o o.o -c /src/a.cpp"
    got = flags_of(command, "/src/a.cpp")
    assert got == ["/usr/bin/c++", "-DA=1", "-I/src/x"], got
    assert covered("engine/core/Scene.h") and covered("editor/Project.h")
    assert not covered("editor/tests/X.h") and not covered("engine/libs/x.h") and not covered("editor/Project.cpp")
    entries = [
        {"directory": "/t", "file": "/src/libs/a.cpp", "command": "c++ -DV -o CMakeFiles/lib.dir/a.o -c /src/libs/a.cpp"},
        {"directory": "/t", "file": "/src/engine/core/S.cpp", "command": "c++ -DE -o engine/CMakeFiles/jasperengine.dir/core/S.o -c /src/engine/core/S.cpp"},
        {"directory": "/t", "file": "/src/editor/P.cpp", "command": "c++ -DF -o CMakeFiles/jasperengine-editor-objects.dir/editor/P.o -c /src/editor/P.cpp"},
    ]
    found = target_flags(entries)
    assert found[ENGINE_DIR][1] == ["c++", "-DE"], found
    assert found[EDITOR_DIR][1] == ["c++", "-DF"], found
    print("check_headers self-test: ok")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Header self-sufficiency check")
    parser.add_argument("headers", nargs="*", help="headers, relative to the repository")
    parser.add_argument("--all", action="store_true", help="every covered header")
    parser.add_argument("--tree", type=Path, default=DEFAULT_TREE)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()

    headers = all_headers(ROOT) if args.all else [h for h in args.headers if covered(h) and (ROOT / h).is_file()]
    left_out = [] if args.all else [h for h in args.headers if h not in headers]
    if left_out:
        print(f"check_headers: left out ({len(left_out)}, not engine/core/ or editor/ headers, or deleted): {' '.join(left_out[:10])}")
    if not headers:
        print("check_headers: no header to check")
        return 0

    tree = args.tree.resolve()
    compdb = subprocess.run(["ninja", "-C", str(tree), "-t", "compdb"], capture_output=True, text=True)
    if compdb.returncode != 0:
        print(f"check_headers: ninja -t compdb in {tree} failed (run tools/build.sh linux): {compdb.stderr.strip()}", file=sys.stderr)
        return 2
    flags = target_flags(json.loads(compdb.stdout))
    missing = [d for d in (ENGINE_DIR, EDITOR_DIR) if d not in flags]
    if missing:
        print(f"check_headers: {tree} has no unit of {missing}: is it the editor's tree?", file=sys.stderr)
        return 2

    jobs = int(os.environ["JOBS"]) if os.environ.get("JOBS", "").isdigit() else (os.cpu_count() or 4)
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for path, ok, errors in pool.map(lambda h: check_one(h, ROOT, flags), headers):
            if not ok:
                failed.append(path)
                print(f"FAIL {path}\n" + "\n".join(errors.splitlines()[:15]), file=sys.stderr)
    print(f"check_headers: {len(headers) - len(failed)} of {len(headers)} compile alone")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
