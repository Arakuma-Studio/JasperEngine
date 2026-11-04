#!/usr/bin/env python3
"""Header fan-out gate (docs/specs/S72-build-hygiene.md, docs/testing.md "Build speed"): how many
translation units an edit of a header rebuilds, from the dependency database of a built tree.

    tools/check_fanout.py                   # build/linux/editor: fails past the limits below
    tools/check_fanout.py --table           # the Markdown tables too
    tools/check_fanout.py --json FILE       # every number
    tools/check_fanout.py --show a.h,b.h    # these headers as well, by path suffix

A component header is the file of engine/core/component/. Its limits count the units of the editor's
targets: the worst one rebuilds at most WORST_LIMIT, the median one at most TYPICAL_LIMIT. The
tables also list the most included editor headers and the engine's units, which have no limit.

Needs a tree that is built (`tools/build.sh linux`; run in the build image, which has Ninja). A tree
whose database lists no component header, or too few units, is an error: nothing measured is not a pass.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TREE = ROOT / "build" / "linux" / "editor"
COMPONENT_DIR = "engine/core/component/"
WORST_LIMIT = 150
TYPICAL_LIMIT = 80
TOP_EDITOR_HEADERS = 20
# fewer units than this: a tree that is not built (the editor alone has over a thousand)
MIN_UNITS = 1000
SOURCES = (".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".S", ".asm", ".nasm")


def repo_path(path: str, tree: Path, root: Path) -> str:
    """A database path as a path of the repository; the image mounts the worktree at /src."""
    if path.startswith("/src/"):
        return path[len("/src/"):]
    full = os.path.normpath(path if os.path.isabs(path) else os.path.join(str(tree), path))
    try:
        return str(Path(full).relative_to(root))
    except ValueError:
        return full


def parse_deps(text: str, tree: Path, root: Path) -> dict[str, set[str]]:
    """`ninja -t deps`: object -> the headers it read, as repository paths."""
    units: dict[str, set[str]] = {}
    current = None
    for line in text.splitlines():
        if not line.strip():
            current = None
        elif not line[0].isspace():
            head = line.split(":", 1)[0]
            current = units.setdefault(head, set()) if head.endswith(".o") else None
        elif current is not None:
            path = line.strip()
            if not path.endswith(SOURCES):
                current.add(repo_path(path, tree, root))
    return units


def is_editor_unit(obj: str) -> bool:
    # (the editor's targets: jasperengine-editor-objects, jasperengine-editor)
    return "/jasperengine-editor" in "/" + obj


def fan_out(units: dict[str, set[str]]) -> dict[str, dict[str, int]]:
    """header -> {"editor": units of the editor's targets, "other": the rest of the tree}."""
    result: dict[str, dict[str, int]] = {}
    for obj, headers in units.items():
        key = "editor" if is_editor_unit(obj) else "other"
        for header in headers:
            result.setdefault(header, {"editor": 0, "other": 0})[key] += 1
    return result


def component_rows(fan: dict[str, dict[str, int]], root: Path) -> list[tuple[str, int, int]]:
    """Every component header, those no unit reads included: they rebuild nothing."""
    names = {COMPONENT_DIR + p.name for p in (root / COMPONENT_DIR).glob("*.h")} | {h for h in fan if h.startswith(COMPONENT_DIR)}
    rows = [(h, fan.get(h, {}).get("editor", 0), fan.get(h, {}).get("other", 0)) for h in names]
    return sorted(rows, key=lambda r: (-r[1], r[0]))


def editor_rows(fan: dict[str, dict[str, int]], top: int) -> list[tuple[str, int, int]]:
    rows = [(h, n["editor"], n["other"]) for h, n in fan.items() if h.startswith("editor/")]
    return sorted(rows, key=lambda r: (-r[1], r[0]))[:top]


def check_limits(rows: list[tuple[str, int, int]]) -> list[str]:
    """The limits' failures, as lines for a person."""
    failures = []
    for header, editor, _ in rows:
        if editor > WORST_LIMIT:
            failures.append(f"{header} rebuilds {editor} editor units (worst-case limit {WORST_LIMIT})")
    if rows:
        typical = statistics.median(r[1] for r in rows)
        if typical > TYPICAL_LIMIT:
            failures.append(f"a typical component header rebuilds {typical:g} editor units (limit {TYPICAL_LIMIT})")
    return failures


def table(title: str, rows: list[tuple[str, int, int]]) -> str:
    lines = [f"### {title}", "", "| Header | Editor units | Other units |", "|---|---:|---:|"]
    lines += [f"| `{h}` | {e} | {o} |" for h, e, o in rows]
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Header fan-out gate")
    parser.add_argument("--tree", type=Path, default=DEFAULT_TREE, help="a built Ninja tree")
    parser.add_argument("--table", action="store_true", help="print the Markdown tables")
    parser.add_argument("--json", type=Path, help="write every header's numbers here")
    parser.add_argument("--show", default="", help="comma-separated path suffixes to print as well")
    args = parser.parse_args(argv)

    tree = args.tree.resolve()
    if not (tree / "build.ninja").is_file():
        print(f"check_fanout: {tree} is not a configured tree: run tools/build.sh linux", file=sys.stderr)
        return 2
    run = subprocess.run(["ninja", "-C", str(tree), "-t", "deps"], capture_output=True, text=True)
    if run.returncode != 0:
        print(f"check_fanout: ninja -t deps failed: {run.stderr.strip()}", file=sys.stderr)
        return 2
    units = parse_deps(run.stdout, tree, ROOT)
    fan = fan_out(units)
    rows = component_rows(fan, ROOT)
    editor_units = sum(1 for obj in units if is_editor_unit(obj))
    if len(units) < MIN_UNITS or not any(h.startswith(COMPONENT_DIR) for h in fan):
        print(f"check_fanout: {tree} lists {len(units)} units and no component header: is it built?", file=sys.stderr)
        return 2

    worst = rows[0]
    typical = statistics.median(r[1] for r in rows)
    print(f"check_fanout: {len(units)} units ({editor_units} of the editor's targets), {len(rows)} component headers: "
          f"worst {worst[1]} ({worst[0]}), median {typical:g}; limits {WORST_LIMIT} and {TYPICAL_LIMIT}")
    for suffix in filter(None, args.show.split(",")):
        for header in sorted(h for h in fan if h.endswith(suffix)):
            print(f"  {header}: {fan[header]['editor']} editor, {fan[header]['other']} other")
    if args.table:
        print()
        print(table("Component headers", rows))
        print(table(f"The {TOP_EDITOR_HEADERS} most included editor headers", editor_rows(fan, TOP_EDITOR_HEADERS)))
    if args.json:
        args.json.write_text(json.dumps(fan, indent=0, sort_keys=True))

    failures = check_limits(rows)
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        print("A header that only names a component forward-declares it (engine/core/ecs/ComponentFwd.h); "
              "only the sources that read it include it (docs/testing.md, \"Build speed\").", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
