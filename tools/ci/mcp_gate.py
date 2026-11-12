"""tools/ci/mcp_gate.py: the gate of one CI MCP shard (contract C2; CI design §9 "Skipped tests").

    python3 tools/ci/mcp_gate.py --json build/test-results/mcp.json --allowed tools/ci/mcp-allowed-skips.txt \\
        [--plan build/ci-plan/mcp-plan/plan-3.json] [--lane mcp]

Exit 1 when the results are missing or unreadable, a test failed, a worker crashed, no test ran, a
test skipped that the allow list does not name, an allowed skip's own lane is this one (it must run
here), or (--plan) a planned test was not reported exactly once. Exit 2 on a malformed allow list.

The allow list: one `<file>::<test>  # <reason>; runs in <lane>` per line (blank and # lines
ignored). Each problem prints as one `mcp_gate: FAIL: <message>` line naming its test.
An entry is the CI form of check-changed's "a selected test that skips fails": it says
why the test cannot run in this lane and where it does run.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ENTRY = re.compile(r"^(test_\w+\.py::\w+)\s+#\s*(\S.*?);\s*runs in\s+(\S.*?)\s*$")


def load_allowed(path: Path) -> tuple[dict[str, tuple[str, str]], list[str]]:
    """name -> (reason, lane), and the lines that are no entry."""
    allowed, bad = {}, []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        m = ENTRY.match(text)
        if not m:
            bad.append(f"{path}:{number}: not `<file>::<test>  # <reason>; runs in <lane>`: {text}")
            continue
        allowed[m.group(1)] = (m.group(2), m.group(3))
    return allowed, bad


def planned(plan: dict) -> list[str]:
    return [f"{c['file']}::{t}" for c in plan.get("chunks", []) for t in c.get("tests", [])]


def gate(results: dict | None, allowed: dict[str, tuple[str, str]], lane: str, plan: dict | None = None,
         source: str = "mcp.json") -> tuple[int, list[str]]:
    """The exit code and the report lines."""
    if results is None:
        return 1, [f"mcp_gate: FAIL: no results ({source}): the shard did not run"]
    rows = results.get("results", [])
    problems = []
    for r in rows:
        if r.get("status") == "fail":
            detail = (r.get("detail") or "").strip().splitlines()
            problems.append(f"{r['name']} failed" + (f": {detail[-1][:300]}" if detail else ""))
    for c in results.get("crashed", []):
        problems.append(f"worker crashed: {c}")
    skips = [r for r in rows if r.get("status") == "skip"]
    allowed_seen = []
    for r in skips:
        entry = allowed.get(r["name"])
        detail = (r.get("detail") or "")[:300]
        if entry is None:
            problems.append(f"{r['name']} skipped, and the allow list (tools/ci/mcp-allowed-skips.txt) does not name it: {detail}")
        elif entry[1] == lane:
            problems.append(f"{r['name']} skipped, but the allow list says it runs in {lane}, which is this lane: {detail}")
        else:
            allowed_seen.append(f"allowed skip {r['name']}: {entry[0]}; runs in {entry[1]}")
    if not rows:
        problems.append("no test ran")
    if plan is not None:
        want = planned(plan)
        got: dict[str, int] = {}
        for r in rows:
            got[r["name"]] = got.get(r["name"], 0) + 1
        problems += [f"{n} was planned and not reported (MISSING)" for n in want if n not in got]
        problems += [f"{n} was reported {k} times (DUPLICATE)" for n, k in got.items() if k > 1]
        problems += [f"{n} was reported and not planned (UNKNOWN)" for n in got if n not in set(want)]
    n = {s: sum(1 for r in rows if r.get("status") == s) for s in ("pass", "fail", "skip")}
    lines = [f"mcp_gate: FAIL: {p}" for p in problems] + [f"mcp_gate: {a}" for a in allowed_seen]
    lines.append(f"mcp_gate: {len(rows)} ran: {n['pass']} passed, {n['fail']} failed, {n['skip']} skipped "
                 f"({len(allowed_seen)} allowed), {len(results.get('crashed', []))} crashed: "
                 f"{'FAILED' if problems else 'passed'}")
    return (1 if problems else 0), lines


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", type=Path, required=True, help="run_tests.py --json results")
    ap.add_argument("--allowed", type=Path, required=True, help="the allow list of skips")
    ap.add_argument("--plan", type=Path, help="the shard's plan: every planned test reported exactly once")
    ap.add_argument("--lane", default="mcp", help="this lane's name (an allowed skip whose lane it is fails)")
    args = ap.parse_args(argv)
    try:
        allowed, bad = load_allowed(args.allowed)
    except OSError as e:
        print(f"mcp_gate: {args.allowed}: {e}", file=sys.stderr)
        return 2
    if bad:
        print("\n".join(bad), file=sys.stderr)
        return 2
    try:
        results = json.loads(args.json.read_text())
    except (OSError, ValueError):
        results = None
    plan = json.loads(args.plan.read_text()) if args.plan else None
    code, lines = gate(results, allowed, args.lane, plan, str(args.json))
    print("\n".join(lines))
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
