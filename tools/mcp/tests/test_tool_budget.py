"""The tool budget gate (S31 R21): tools/mcp/tool_budget.py --check tools/mcp/budget.json passes on
this editor and fails, naming the tool, when a tool outgrows its limit.

- the gate itself: a run of `tool_budget.py --check` (its own editor, every group listed)
  exits 0: the default listing (core) within 100,000 characters and 150 tools, each group within
  its limits, each tool within 1,500 characters or its exceptions entry;
- red control: with JASPER_MCP_SELFTEST_GROUP=2000 the selftest group's tool has a
  2,000-character description, and the same run exits 1 naming selftest_echo;
- the rules of check() on made-up rows (each limit, stale entries, core trades);
- budget.json: every group of the group table may have an entry, no entry names a group the
  table lacks (checked by the gate run), and every exception has a reason.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import tool_budget  # noqa: E402

from harness import TestFailure  # noqa: E402

TIMEOUT = 900


async def run_check(extra_env: dict[str, str] | None = None) -> tuple[int, str]:
    env = dict(os.environ)
    env.update(extra_env or {})
    proc = await asyncio.create_subprocess_exec(
        sys.executable, str(HERE.parent / "tool_budget.py"), "--check", str(tool_budget.BUDGET_FILE),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT, env=env)
    out, _ = await asyncio.wait_for(proc.communicate(), 600)
    return proc.returncode, out.decode(errors="replace")


async def test_budget_check_passes():
    code, out = await run_check()
    tail = "\n".join(out.splitlines()[-25:])
    if code != 0:
        raise TestFailure(f"tool_budget.py --check exits {code}:\n{tail}")
    print("     [budget] " + next((line for line in out.splitlines() if " tools: total " in line), "?"))


async def test_budget_red_control():
    code, out = await run_check({"JASPER_MCP_SELFTEST_GROUP": "2000"})
    lines = [line for line in out.splitlines() if line.startswith("BUDGET:")]
    if code != 1 or not any("selftest_echo" in line for line in lines):
        raise TestFailure(f"an inflated tool must fail the gate naming it: exit {code}:\n" +
                          "\n".join(out.splitlines()[-25:]))


def row(name: str, total: int, group: str = "core") -> dict:
    return {"name": name, "group": group, "desc": 0, "schema": total - len(name), "total": total}


async def test_check_rules():
    budget = {
        "default_listing": {"chars": 1000, "tools": 3},
        "group_default": {"chars": 500, "tools": 2},
        "tool_default": 300,
        "groups": {"profiling": {"chars": 200, "tools": 1, "spec": "S15"}},
        "exceptions": {"big_tool": {"limit": 600, "reason": "grandfathered"}},
        "core_trades": [],
    }
    ok = [row("big_tool", 590), row("get_a", 300), row("get_p", 150, "profiling")]
    assert tool_budget.check(ok, budget, {"core", "profiling", "splines"}) == [], tool_budget.check(ok, budget)
    cases = {
        "core chars": ok + [row("get_b", 200)],
        "core tools": ok + [row("get_b", 10), row("get_c", 10)],
        "tool limit": [row("get_a", 301)],
        "exception limit": [row("big_tool", 601)],
        "group entry": ok + [row("get_q", 10, "profiling")],
        "group default": [row("get_s", 260, "splines"), row("get_t", 260, "splines")],
        "group default count": [row("get_s", 10, "splines"), row("get_t", 10, "splines"), row("get_u", 10, "splines")],
    }
    for label, rows in cases.items():
        assert tool_budget.check(rows, budget, {"core", "profiling", "splines"}), f"{label} passed"
    stale = dict(budget, exceptions={"gone": {"limit": 9, "reason": "x"}, "big_tool": {"limit": 600, "reason": ""}})
    problems = tool_budget.check(ok, stale)
    assert any("gone" in p for p in problems) and any("no reason" in p for p in problems), problems
    unknown = tool_budget.check(ok + [row("get_v", 5, "nope")], budget, {"core", "profiling"})
    assert any("nope" in p for p in unknown), unknown
    traded = dict(budget, core_trades=[{"tool": "get_a", "spec": "S99", "chars": 300, "trimmed": "x"}])
    assert tool_budget.check(ok, traded) == []
    assert tool_budget.check(ok, dict(budget, core_trades=[{"tool": "get_p", "spec": "S99", "trimmed": "x"}]))


async def test_budget_file_shape():
    budget = tool_budget.load_budget()
    for key in ("default_listing", "group_default", "tool_default", "groups", "exceptions", "core_trades"):
        assert key in budget, f"budget.json lacks {key}"
    assert budget["default_listing"] == {"chars": 100000, "tools": 150}, budget["default_listing"]
    assert budget["group_default"] == {"chars": 15000, "tools": 12}, budget["group_default"]
    assert budget["tool_default"] == 1500
    for name, entry in budget["exceptions"].items():
        assert int(entry["limit"]) > 0 and entry.get("reason"), f"exception {name}: {entry}"
    json.dumps(budget)
