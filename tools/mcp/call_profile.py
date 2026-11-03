"""Where the MCP tests' time goes: sums up a JASPER_MCP_CALL_LOG directory (tools/mcp/harness.py).

    JASPER_MCP_CALL_LOG=build/test-results/calls tools/mcp/run-tests.sh tools/mcp/tests/test_shadows.py
    python3 tools/mcp/call_profile.py build/test-results/calls [--top 20] [--test SUBSTRING]

For each test, slowest first: its wall time, the time inside tool calls by tool, and the time
outside them (the test's own Python: sleeps, image diffs, subprocesses, polling gaps); then its
slowest single calls.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path


def load(root: Path) -> list[dict]:
    rows = []
    for f in sorted(root.glob("*.jsonl")):
        for line in f.read_text(errors="replace").splitlines():
            try:
                rows.append(json.loads(line))
            except ValueError:
                pass
    return rows


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", type=Path)
    ap.add_argument("--top", type=int, default=15, help="tests to show")
    ap.add_argument("--calls", type=int, default=6, help="slowest calls to show per test")
    ap.add_argument("--test", help="only tests whose name contains this")
    args = ap.parse_args(argv)
    rows = load(args.dir)
    totals: dict[str, float] = {}
    calls: dict[str, list[dict]] = collections.defaultdict(list)
    for r in rows:
        if r["tool"] == "(test)":
            totals[r["test"]] = r["seconds"]
        else:
            calls[r["test"]].append(r)
    names = [t for t in sorted(totals, key=lambda t: -totals[t]) if not args.test or args.test in t][:args.top]
    for t in names:
        by_tool: dict[str, float] = collections.Counter()
        count: dict[str, int] = collections.Counter()
        for c in calls[t]:
            by_tool[c["tool"]] += c["seconds"]
            count[c["tool"]] += 1
        inside = sum(by_tool.values())
        print(f"{totals[t]:8.1f}s {t}  (calls {inside:.1f}s, outside {totals[t] - inside:.1f}s)")
        for tool, sec in sorted(by_tool.items(), key=lambda x: -x[1])[:8]:
            print(f"            {sec:8.1f}s {count[tool]:5d}x {tool}")
        for c in sorted(calls[t], key=lambda c: -c["seconds"])[:args.calls]:
            print(f"              {c['seconds']:7.1f}s {c['tool']} {c['args'][:150]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
