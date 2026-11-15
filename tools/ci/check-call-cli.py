#!/usr/bin/env python3
"""The `jasperengine-editor call` paths that need no editor, on every desktop OS (S31 AC15):
`call create_entity --help` lists the tool's flags and examples, `call --dry-run` prints the
converted arguments, and a bad value exits 2. The Linux MCP suite tests the rest
(tools/mcp/tests/test_call_cli.py).

    python3 tools/ci/check-call-cli.py <editor binary | directory to search>

A directory is searched for jasperengine-editor-cmd.exe (Windows console build), else
jasperengine-editor.exe or jasperengine-editor.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

NAMES = ("jasperengine-editor-cmd.exe", "jasperengine-editor.exe", "jasperengine-editor")


def find_editor(start: Path) -> Path:
    if start.is_file():
        return start
    for name in NAMES:
        for path in sorted(start.rglob(name)):
            if path.is_file() and (os.access(path, os.X_OK) or path.suffix == ".exe"):
                return path
    raise SystemExit(f"check-call-cli: no editor binary under {start}")


def run(editor: Path, *args: str) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as cwd:
        proc = subprocess.run([str(editor), *args], cwd=cwd, capture_output=True, text=True, timeout=120)
        if os.listdir(cwd):
            raise SystemExit(f"check-call-cli: {args} wrote {os.listdir(cwd)} in the working directory")
    return proc


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    # absolute: each call runs in its own temporary working directory
    editor = find_editor(Path(sys.argv[1]).resolve())
    problems = []
    help_out = run(editor, "call", "create_entity", "--help")
    if help_out.returncode != 0 or "--name" not in help_out.stdout or "call create_entity '{" not in help_out.stdout:
        problems.append(f"call create_entity --help: exit {help_out.returncode}\n{help_out.stdout[-800:]}{help_out.stderr[-800:]}")
    dry = run(editor, "call", "--dry-run", "create_entity", "--name", "Box", "--position", "1,2,3")
    try:
        parsed = json.loads(dry.stdout)
    except ValueError:
        parsed = None
    if dry.returncode != 0 or parsed != {"tool": "create_entity", "arguments": {"name": "Box", "position": [1.0, 2.0, 3.0]}}:
        problems.append(f"call --dry-run: exit {dry.returncode}: {dry.stdout} {dry.stderr}")
    bad = run(editor, "call", "--dry-run", "create_entity", "--type", "nope")
    if bad.returncode != 2:
        problems.append(f"a bad enum value exits 2, not {bad.returncode}: {bad.stderr}")
    for p in problems:
        print(f"check-call-cli: {p}")
    if not problems:
        print(f"check-call-cli: {editor} OK")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
