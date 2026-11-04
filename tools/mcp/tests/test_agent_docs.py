"""The contributor docs for agents (S31 R24-R28): tools/check_agent_docs.py passes on the repository
and fails on a scratch copy whose AGENTS.md names a path that does not exist, whose CLAUDE.md
is anything but the @AGENTS.md include, whose rules file has no paths: front matter, or that names
a tools/build.sh target the script lacks (red controls). The shape rules: AGENTS.md at most 350
lines, CLAUDE.md exactly the include, every section present.

The per-project AGENTS.md the editor generates is separate (test_generators.py).
"""

from __future__ import annotations

import asyncio
import shutil
import sys
import tempfile
from pathlib import Path

import harness
from harness import TestFailure

CI = True

CHECKER = harness.REPO / "tools" / "check_agent_docs.py"
SECTIONS = ["## Map", "## Invariants", "## What lives where", "## Generated files", "## Build", "## Run an editor",
            "## Debugging", "## Test", "## Evidence", "## Code", "## Worktrees", "## Land", "## Adding an MCP tool",
            "## MCP text", "## Reporting", "## DON'Ts"]


async def run_checker(root: Path) -> tuple[int, str]:
    proc = await asyncio.create_subprocess_exec(sys.executable, str(CHECKER), "--root", str(root),
                                                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    out, _ = await asyncio.wait_for(proc.communicate(), 300)
    return proc.returncode, out.decode(errors="replace")


def scratch_copy() -> Path:
    """A copy of the repository's agent docs; everything else links to the repository."""
    root = Path(tempfile.mkdtemp(prefix="jasper-agent-docs-"))
    for entry in harness.REPO.iterdir():
        if entry.name in ("AGENTS.md", "CLAUDE.md", ".claude", "build", ".git"):
            continue
        (root / entry.name).symlink_to(entry)
    shutil.copy(harness.REPO / "AGENTS.md", root / "AGENTS.md")
    shutil.copy(harness.REPO / "CLAUDE.md", root / "CLAUDE.md")
    shutil.copytree(harness.REPO / ".claude", root / ".claude")
    return root


async def test_check_passes():
    code, out = await run_checker(harness.REPO)
    if code != 0:
        raise TestFailure(f"tools/check_agent_docs.py exits {code}:\n{out}")


async def test_sizes_and_sections():
    agents = (harness.REPO / "AGENTS.md").read_text(encoding="utf-8")
    claude = (harness.REPO / "CLAUDE.md").read_text(encoding="utf-8").splitlines()
    if len(agents.splitlines()) > 350:
        raise TestFailure(f"AGENTS.md has {len(agents.splitlines())} lines, over 350")
    if claude != ["@AGENTS.md"]:
        raise TestFailure(f"CLAUDE.md is the single line @AGENTS.md, got {len(claude)} lines")
    scope = "For engine contributors. Inside a game project (a directory with `project.yaml`),"
    if scope not in agents.replace("\n", " "):
        raise TestFailure("AGENTS.md opens with the scope line")
    missing = [s for s in SECTIONS if s not in agents]
    if missing:
        raise TestFailure(f"AGENTS.md sections missing: {missing}")
    for name in ("mcp.md", "rendering.md", "scripting.md", "htmlui.md"):
        if not (harness.REPO / ".claude" / "rules" / name).exists():
            raise TestFailure(f".claude/rules/{name} is missing")


async def test_red_controls():
    root = scratch_copy()
    try:
        code, out = await run_checker(root)
        if code != 0:
            raise TestFailure(f"an unedited scratch copy must pass, or the red controls prove nothing:\n{out}")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    cases = {
        "a path that does not exist": ("AGENTS.md", lambda t: t + "\nSee `engine/core/NoSuchFile.cpp`.\n",
                                       "NoSuchFile.cpp"),
        "a build target the script lacks": ("AGENTS.md", lambda t: t + "\nRun `tools/build.sh nosuchtarget`.\n",
                                            "nosuchtarget"),
        "CLAUDE.md without the include": ("CLAUDE.md", lambda t: t.replace("@AGENTS.md\n", "", 1), "@AGENTS.md"),
        "CLAUDE.md with a rule after the include": ("CLAUDE.md", lambda t: t + "- Always do X.\n", "@AGENTS.md"),
        "a rules file without paths": (".claude/rules/mcp.md", lambda t: t.replace("paths:", "scope:", 1), "paths:"),
    }
    for label, (rel, edit, needle) in cases.items():
        root = scratch_copy()
        try:
            path = root / rel
            path.write_text(edit(path.read_text(encoding="utf-8")), encoding="utf-8")
            code, out = await run_checker(root)
            if code != 1 or needle not in out:
                raise TestFailure(f"red control ({label}): exit {code}, output:\n{out}")
        finally:
            shutil.rmtree(root, ignore_errors=True)
