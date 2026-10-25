"""Every registered MCP tool has a test (S31 R23): its name appears quoted ("name" or 'name') in
some tools/mcp/tests/*.py file, or tools/mcp/tool_coverage_exempt.txt lists it with a reason.
The tool list is the live tools/list of every group (the selftest group included, which
test_tool_groups.py covers). Entries of the exempt list must name existing tools.
"""

from __future__ import annotations

CI = True

import re
from pathlib import Path

from harness import Editor

HERE = Path(__file__).resolve().parent
EXEMPT_FILE = HERE.parent / "tool_coverage_exempt.txt"
# Files that name every tool without testing it
NOT_TESTS = {"test_tool_coverage.py", "test_tool_conventions.py"}

EDITOR = {"create": True, "env": {"JASPER_MCP_SELFTEST_GROUP": "1"}}


def load_exempt(path: Path = EXEMPT_FILE) -> tuple[dict[str, str], list[str]]:
    """{tool: reason} and the malformed lines."""
    exempt: dict[str, str] = {}
    bad: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, reason = line.partition(" ")
        if not reason.strip():
            bad.append(f"{name}: no reason")
        exempt[name] = reason.strip()
    return exempt, bad


def read_test_files() -> dict[str, str]:
    return {p.name: p.read_text(encoding="utf-8", errors="replace")
            for p in sorted(HERE.glob("*.py")) if p.name not in NOT_TESTS}


def uncovered(names: list[str], texts: dict[str, str], exempt: dict[str, str]) -> list[str]:
    """The tools neither a test names nor the exempt list lists, plus stale exempt entries."""
    joined = "\n".join(texts.values())
    out = [n for n in names if n not in exempt and not re.search(r"""["']""" + re.escape(n) + r"""["']""", joined)]
    out += [f"{n} (exempt entry for a tool that does not exist)" for n in exempt if n not in names]
    return out


async def test_every_tool_has_a_test(ed: Editor):
    names = [t.name for t in await ed.tools(groups="all")]
    ed.check("selftest_echo" in names, "the selftest group is registered (JASPER_MCP_SELFTEST_GROUP)")
    exempt, bad = load_exempt()
    ed.check(not bad, f"malformed lines in {EXEMPT_FILE.name}: {bad}")
    missing = uncovered(names, read_test_files(), exempt)
    ed.check(not missing, f"tools without a test (add one to tools/mcp/tests, or an entry with a reason to "
                          f"{EXEMPT_FILE.name}): {missing}")
    print(f"     [coverage] {len(names)} tools, {len(exempt)} exempt")


async def test_red_control():
    """A tool no test names fails the check; exempting it with a reason passes."""
    texts = {"test_x.py": 'await ed.call("create_entity")\n'}
    assert uncovered(["create_entity", "brand_new_tool"], texts, {}) == ["brand_new_tool"]
    assert uncovered(["create_entity", "brand_new_tool"], texts, {"brand_new_tool": "why"}) == []
    assert uncovered(["create_entity"], texts, {"gone_tool": "why"}) != []
    # a name that only appears inside a longer word is not a test of it
    assert uncovered(["play"], {"test_x.py": '"play_state"'}, {}) == ["play"]
