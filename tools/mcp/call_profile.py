"""Where MCP time goes: an editor's call log, or the MCP tests' JASPER_MCP_CALL_LOG directory.

An editor's call log (a file: JASPER_MCP_LOG=<path>, or the MCP Server window's Log Calls To File,
which writes mcp-calls.jsonl beside settings.yaml; editor/mcp/McpCallLog.h):

    python3 tools/mcp/call_profile.py mcp-calls.jsonl

Per tool, most calls first: calls, error rate, p50 and p95 ms (nearest rank) and mean result
bytes; <file>.1, the part rotated away, is read first when it exists.

The tests' directory (tools/mcp/harness.py):

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
import math
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


def load_call_log(path: Path) -> tuple[list[dict], int]:
    """The calls of an editor's call log, <path>.1 first, and the number of lines that are not calls."""
    rows, bad = [], 0
    for f in (Path(str(path) + ".1"), path):
        if not f.is_file():
            continue
        for line in f.read_text(errors="replace").splitlines():
            if not line.strip():
                continue
            try:
                row = json.loads(line)
            except ValueError:
                bad += 1
                continue
            if isinstance(row, dict) and isinstance(row.get("tool"), str) and isinstance(row.get("ms"), (int, float)):
                rows.append(row)
            else:
                bad += 1
    return rows, bad


def percentile(values: list[float], p: float) -> float:
    """Nearest rank: the smallest value with at least p % of the values at or below it."""
    ordered = sorted(values)
    return ordered[max(1, math.ceil(p / 100.0 * len(ordered))) - 1]


def summarize_calls(rows: list[dict]) -> list[dict]:
    """Per tool: calls, errors, error_rate, p50_ms, p95_ms and mean_result_bytes, most calls first."""
    by_tool: dict[str, list[dict]] = collections.defaultdict(list)
    for r in rows:
        by_tool[r["tool"]].append(r)
    out = []
    for tool, calls in by_tool.items():
        ms = [float(c["ms"]) for c in calls]
        errors = sum(1 for c in calls if not c.get("ok", True))
        out.append({"tool": tool, "calls": len(calls), "errors": errors, "error_rate": errors / len(calls),
                    "p50_ms": percentile(ms, 50), "p95_ms": percentile(ms, 95),
                    "mean_result_bytes": sum(int(c.get("result_bytes") or 0) for c in calls) / len(calls)})
    out.sort(key=lambda s: (-s["calls"], s["tool"]))
    return out


def print_call_log(path: Path) -> int:
    if not path.is_file() and not Path(str(path) + ".1").is_file():
        print(f"{path}: no such file or directory", file=sys.stderr)
        return 2
    rows, bad = load_call_log(path)
    if not rows:
        print(f"{path}: no calls" + (f" ({bad} line(s) that are not calls)" if bad else ""), file=sys.stderr)
        return 1
    summary = summarize_calls(rows)
    errors = sum(s["errors"] for s in summary)
    print(f"{len(rows)} calls of {len(summary)} tools, {100.0 * errors / len(rows):.1f}% errors"
          + (f"; {bad} line(s) skipped" if bad else ""))
    print(f"{'calls':>7} {'errors':>7} {'p50 ms':>9} {'p95 ms':>9} {'mean bytes':>11}  tool")
    for s in summary:
        print(f"{s['calls']:7d} {100.0 * s['error_rate']:6.1f}% {s['p50_ms']:9.1f} {s['p95_ms']:9.1f} "
              f"{s['mean_result_bytes']:11.0f}  {s['tool']}")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", type=Path, help="an editor's call log file, or the tests' JASPER_MCP_CALL_LOG directory")
    ap.add_argument("--top", type=int, default=15, help="tests to show")
    ap.add_argument("--calls", type=int, default=6, help="slowest calls to show per test")
    ap.add_argument("--test", help="only tests whose name contains this")
    args = ap.parse_args(argv)
    if not args.dir.is_dir():
        return print_call_log(args.dir)
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
