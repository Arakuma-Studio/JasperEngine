#!/usr/bin/env python3
"""Drift check of the contributor docs for agents: AGENTS.md, CLAUDE.md and .claude/rules/*.md.

    python3 tools/check_agent_docs.py [--root DIR]

Plain Python, no editor and no packages. Exits 1, listing every problem, when:
- a backticked repository path does not exist (`tools/build.sh`, `editor/mcp/`, `NOTICE.md`;
  build outputs under build/, globs and <placeholders> are not paths);
- a `tools/build.sh <target>` names a target the script's usage does not accept;
- CLAUDE.md is anything but the single line @AGENTS.md;
- a rules file has no front matter with paths:.
--root checks another copy of the repository (the red control of the tests).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FILE_SUFFIXES = (".md", ".py", ".sh", ".json", ".txt", ".cmake", ".h", ".cpp", ".yml", ".Dockerfile")


def docs(root: Path) -> list[Path]:
    rules = sorted((root / ".claude" / "rules").glob("*.md"))
    return [p for p in (root / "AGENTS.md", root / "CLAUDE.md") if p.exists()] + rules


_NAMES: dict[Path, set[str]] = {}


def file_names(root: Path) -> set[str]:
    """Every file name in the repository (build outputs, external/ and .git aside)."""
    if root not in _NAMES:
        names: set[str] = set()
        for path in root.rglob("*"):
            parts = path.relative_to(root).parts
            if parts and parts[0] in ("build", "external", ".git", "node_modules"):
                continue
            names.add(path.name)
        _NAMES[root] = names
    return _NAMES[root]


def build_targets(root: Path) -> set[str]:
    """The targets `case "$TARGET" in` of tools/build.sh accepts, plus shell and exec."""
    text = (root / "tools" / "build.sh").read_text(encoding="utf-8")
    m = re.search(r'case "\$TARGET" in\s*\n\s*([a-z0-9|-]+)\) ;;', text)
    targets = set(m.group(1).split("|")) if m else set()
    return targets | {"shell", "exec"}


def path_candidates(span: str) -> list[str]:
    """The repository paths a backticked span names: its first word when it looks like a path."""
    word = span.strip().split()[0] if span.strip() else ""
    word = word.rstrip(",.;:)")
    if not word or any(c in word for c in "<>*{}$=|") or word.startswith(("-", "@", "http", "jasper://", "~")):
        return []
    if word.startswith(("build/", "./build/")):
        return []   # build outputs are not in the repository
    if "/" in word:
        return [word]
    return [word] if word.endswith(FILE_SUFFIXES) and not word.startswith(".") else []


def check(root: Path) -> list[str]:
    problems: list[str] = []
    files = docs(root)
    for required in ("AGENTS.md", "CLAUDE.md"):
        if not (root / required).exists():
            problems.append(f"{required} is missing")
    targets = build_targets(root)
    for path in files:
        rel = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        for span in re.findall(r"`([^`\n]+)`", text):
            for candidate in path_candidates(span):
                name = candidate[2:] if candidate.startswith("./") else candidate
                # a bare file name may live anywhere in the repository (`test_tool_conventions.py`)
                found = (root / name).exists() if "/" in name else name in file_names(root)
                if not found:
                    problems.append(f"{rel}: `{span}` names {candidate}, which does not exist")
        for m in re.finditer(r"tools/build\.sh ([a-z0-9|-]+)", text):
            for target in m.group(1).split("|"):
                if target not in targets:
                    problems.append(f"{rel}: tools/build.sh {target} is not a target of tools/build.sh "
                                    f"({', '.join(sorted(targets))})")
        if path.parent.name == "rules":
            front = re.match(r"---\n(.*?)\n---\n", text, re.S)
            if not front or not re.search(r"^paths:", front.group(1), re.M):
                problems.append(f"{rel}: no front matter with paths:")
    claude = root / "CLAUDE.md"
    if claude.exists() and claude.read_text(encoding="utf-8").splitlines() != ["@AGENTS.md"]:
        problems.append("CLAUDE.md is not the single line @AGENTS.md")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path, default=REPO, help="the repository to check (default: this one)")
    args = ap.parse_args()
    root = args.root.resolve()
    problems = check(root)
    for p in problems:
        print(f"check_agent_docs: {p}")
    if problems:
        return 1
    print(f"check_agent_docs: {len(docs(root))} files OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
