# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""PreToolUse hook of an eval sample: keeps the agent inside its throwaway project.

    python3 sim_hook.py --root SAMPLE_DIR --case CASE.json      (reads the hook event on stdin)

- Bash: a command matching one of the case's `shell` entries ({"match": regex, "output": text})
  is rewritten to print that canned output; a read-only command (ls, cat, grep, find, head,
  tail, wc, sed -n, pwd, git status/log/diff/show) whose paths stay inside the project runs as
  is; anything else is refused. The agent's own command is what the transcript records.
- Read, Write, Edit, Glob, Grep, NotebookEdit: only inside the project.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import sys
from pathlib import Path

READ_ONLY = {"ls", "cat", "head", "tail", "grep", "rg", "find", "wc", "pwd", "echo", "tree", "file", "stat"}
GIT_READ_ONLY = {"status", "log", "diff", "show", "ls-files"}
FILE_TOOLS = {"Read": "file_path", "Write": "file_path", "Edit": "file_path", "MultiEdit": "file_path",
              "NotebookEdit": "notebook_path", "Glob": "path", "Grep": "path"}


def inside(root: Path, raw: str) -> bool:
    if not raw:
        return True
    p = Path(os.path.expanduser(raw))
    if not p.is_absolute():
        p = root / p
    try:
        p.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def read_only(root: Path, command: str) -> bool:
    command = command.replace("2>/dev/null", "").replace("2>&1", "")
    if re.search(r"[;&`<>]|\$\(|\|\|", command):
        return False
    return all(segment_read_only(root, part) for part in command.split("|"))


def segment_read_only(root: Path, command: str) -> bool:
    try:
        words = shlex.split(command)
    except ValueError:
        return False
    if not words:
        return False
    head = words[0]
    if head == "git":
        if len(words) < 2 or words[1] not in GIT_READ_ONLY:
            return False
    elif head == "sed":
        if "-n" not in words or any(w.startswith("-i") for w in words):
            return False
    elif head not in READ_ONLY:
        return False
    if head == "find" and any(w in ("-delete", "-exec", "-execdir", "-ok") for w in words):
        return False
    return all(inside(root, w) for w in words[1:] if w.startswith(("/", "~", "..")))


def decide(root: Path, case: dict, event: dict) -> dict | None:
    tool, args = event.get("tool_name", ""), event.get("tool_input") or {}
    if tool == "Bash":
        command = str(args.get("command", ""))
        for i, entry in enumerate(case.get("shell") or []):
            if re.search(entry["match"], command):
                out = root.parent / f"shell-{i}.txt"
                out.write_text(entry.get("output", ""), encoding="utf-8")
                return allow({**args, "command": f"cat {shlex.quote(str(out))}"})
        if read_only(root, command):
            return None
        return deny("This sandbox runs only read-only commands inside the project "
                    "(ls, cat, grep, find, head, tail, wc, sed -n, git status/log/diff).")
    key = FILE_TOOLS.get(tool)
    if key and not inside(root, str(args.get(key) or "")):
        return deny(f"{args.get(key)} is outside this project ({root}).")
    return None


def allow(updated: dict) -> dict:
    return {"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "allow",
                                   "updatedInput": updated}}


def deny(reason: str) -> dict:
    return {"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny",
                                   "permissionDecisionReason": reason}}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--case", required=True)
    a = ap.parse_args()
    event = json.load(sys.stdin)
    case = json.loads(Path(a.case).read_text(encoding="utf-8"))
    out = decide(Path(a.root), case, event)
    if out:
        print(json.dumps(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
