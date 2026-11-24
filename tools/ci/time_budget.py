#!/usr/bin/env python3
"""The test-time budget: what a test, an MCP file and a whole suite may cost in CI (tools/ci/time-budget.json).

    python3 tools/ci/time_budget.py check --lane unit-core build/test-results/core.xml   # exit 1 on a finding
    python3 tools/ci/time_budget.py allow <suite> <test or file> <seconds> --reason "<why>" [--file] [--total]
    python3 tools/ci/time_budget.py ratchet --lane mcp-3 build/test-results/mcp.json     # lower what got cheaper

Suites follow the CI lane: unit-core is core, unit-editor-<k> editor, htmlui htmlui, mcp and mcp-<k> mcp;
a sanitizer or platform lane runs the same tests slower and has no budget. A test may take its suite's
`test_cap`, an MCP file `file_cap`, unless tests/files lists it with its measured seconds and a reason;
a listed one may grow by `tolerance` (a runner's noise) before it is over. A suite's `total` (and
`total_tolerance`) is checked on a complete run only (no filter: the merge queue's unit and HTML UI lanes; the editor's parts add up).
tools/ci/report.py puts what is over in each lane's report and the PR comment, marked advisory: it never
fails Check. Raising a budget is a reviewed change of the JSON with its reason (`allow`); `ratchet`
lowers an entry whose test now measures under half of it, and drops it once it fits the cap.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shlex
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

BUDGET_FILE = Path(__file__).resolve().parent / "time-budget.json"
LANE_SUITES = ((re.compile(r"^unit-core$"), "core"), (re.compile(r"^unit-editor(-\d+)?$"), "editor"),
               (re.compile(r"^htmlui$"), "htmlui"), (re.compile(r"^mcp(-[A-Za-z0-9_]+)?$"), "mcp"))
SHOWN = 15          # findings a lane's report keeps


def suite_of(lane: str) -> str | None:
    return next((suite for pattern, suite in LANE_SUITES if pattern.match(lane)), None)


def load_budget(path: Path = BUDGET_FILE) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def problems(budget: dict) -> list[str]:
    """What makes the file unusable: a suite without a positive cap, an entry without seconds or a reason"""
    out = []
    if not isinstance(budget.get("tolerance"), (int, float)) or budget["tolerance"] < 1:
        out.append("tolerance: a number of at least 1")
    for suite, caps in (budget.get("suites") or {}).items():
        if not isinstance(caps.get("test_cap"), (int, float)) or caps["test_cap"] <= 0:
            out.append(f"suites.{suite}.test_cap: a positive number of seconds")
    for section in ("tests", "files"):
        for suite, entries in (budget.get(section) or {}).items():
            for name, entry in entries.items():
                if not isinstance(entry, dict) or not isinstance(entry.get("seconds"), (int, float)) or entry["seconds"] <= 0:
                    out.append(f"{section}.{suite}.{name}: no seconds")
                elif not str(entry.get("reason") or "").strip():
                    out.append(f"{section}.{suite}.{name}: no reason")
    return out


def measure(files: list[Path]) -> dict[str, float]:
    """{test: seconds} of the tests that ran, from googletest XML and run_tests.py --json results"""
    tests: dict[str, float] = {}
    for path in files:
        path = Path(path)
        if path.suffix == ".xml":
            try:
                root = ET.parse(path).getroot()
            except (ET.ParseError, OSError):
                continue
            for suite in root.iter("testsuite"):
                for case in suite.iter("testcase"):
                    if case.get("status") == "notrun" or case.get("result") in ("suppressed", "skipped") or case.find("skipped") is not None:
                        continue
                    try:
                        tests[f"{suite.get('name', '?')}.{case.get('name', '?')}"] = float(case.get("time") or 0)
                    except ValueError:
                        continue
        elif path.suffix == ".json" and path.name.startswith("mcp") and "seconds" not in path.name:
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            for r in data.get("results") or [] if isinstance(data, dict) else []:
                if isinstance(r, dict) and r.get("status") in ("pass", "fail") and isinstance(r.get("seconds"), (int, float)):
                    tests[str(r.get("name", "?"))] = float(r["seconds"])
    return tests


def _limit(budget: dict, section: str, suite: str, name: str, cap_key: str) -> tuple[float | None, bool]:
    entry = ((budget.get(section) or {}).get(suite) or {}).get(name)
    if isinstance(entry, dict) and isinstance(entry.get("seconds"), (int, float)):
        return entry["seconds"], True
    return (budget.get("suites") or {}).get(suite, {}).get(cap_key), False


def _over(seconds: float, limit: float | None, recorded: bool, tolerance: float) -> bool:
    return limit is not None and seconds > (limit * tolerance if recorded else limit)


def check(budget: dict, suite: str, tests: dict[str, float]) -> list[dict]:
    """The tests (and, for MCP, the files) over their budget, the costliest first"""
    tolerance = float(budget.get("tolerance", 1.5))
    found = []
    for name, seconds in tests.items():
        limit, recorded = _limit(budget, "tests", suite, name, "test_cap")
        if _over(seconds, limit, recorded, tolerance):
            found.append({"kind": "test", "name": name, "seconds": round(seconds, 1), "budget": limit})
    if suite == "mcp":
        files: dict[str, float] = {}
        for name, seconds in tests.items():
            files[name.split("::")[0]] = files.get(name.split("::")[0], 0.0) + seconds
        for name, seconds in files.items():
            limit, recorded = _limit(budget, "files", suite, name, "file_cap")
            if _over(seconds, limit, recorded, tolerance):
                found.append({"kind": "file", "name": name, "seconds": round(seconds, 1), "budget": limit})
    return sorted(found, key=lambda f: -f["seconds"])


def check_totals(budget: dict, lanes: list[dict]) -> list[dict]:
    """Each suite's total over the complete runs among `lanes` ({suite, seconds, complete})"""
    tolerance = float(budget.get("total_tolerance", budget.get("tolerance", 1.5)))
    totals: dict[str, float] = {}
    incomplete = {lane["suite"] for lane in lanes if not lane.get("complete")}
    for lane in lanes:
        if lane.get("complete") and lane["suite"] not in incomplete:
            totals[lane["suite"]] = totals.get(lane["suite"], 0) + lane["seconds"]
    found = []
    for suite, seconds in sorted(totals.items()):
        limit = (budget.get("suites") or {}).get(suite, {}).get("total")
        if isinstance(limit, (int, float)) and seconds > limit * tolerance:
            found.append({"kind": "total", "name": suite, "seconds": seconds, "budget": limit})
    return found


def complete_run(command: str) -> bool:
    """A lane command that runs its whole suite: no gtest filter, no MCP selection (UNIT_SHARD parts add up)"""
    return "--gtest_filter" not in command and "mcp_shard" not in command and "mcp-tests" not in command


def lane_summary(lane: str, files: list[Path], command: str, budget: dict | None = None) -> dict | None:
    suite = suite_of(lane)
    if suite is None:
        return None
    budget = budget if budget is not None else load_budget()
    tests = measure(files)
    return {"suite": suite, "seconds": int(round(sum(tests.values()))), "complete": complete_run(command) and suite != "mcp",
            "over": check(budget, suite, tests)[:SHOWN]}


def _allow_command(finding: dict, suite: str) -> str:
    flag = {"file": " --file", "total": " --total"}.get(finding["kind"], "")
    seconds = int(math.ceil(finding["seconds"]))
    return f"python3 tools/ci/time_budget.py allow {suite} {shlex.quote(finding['name'])} {seconds} --reason '<why it must cost this>'{flag}"


def render(findings: list[dict], suite: str) -> list[str]:
    """Markdown lines: each finding, and the two ways out"""
    if not findings:
        return []
    lines = []
    for f in findings:
        what = {"test": "test", "file": "MCP file", "total": "suite total"}[f["kind"]]
        lines.append(f"- {what} `{f['name']}`: {f['seconds']:.0f} s, budget {f['budget']:.0f} s")
    lines.append(f"Make it cheaper (docs/testing.md, \"Writing load-robust tests\"), or raise its budget with a reason: "
                 f"`{_allow_command(findings[0], suite)}`")
    return lines


def _write(budget: dict, path: Path) -> None:
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(budget, indent=1, ensure_ascii=False, sort_keys=False) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def main(argv: list[str] | None = None) -> int:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--budget", type=Path, default=BUDGET_FILE)
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name in ("check", "ratchet"):
        p = sub.add_parser(name, parents=[common])
        p.add_argument("--lane", required=True)
        p.add_argument("files", nargs="+", type=Path)
    p = sub.add_parser("allow", parents=[common])
    p.add_argument("suite")
    p.add_argument("name")
    p.add_argument("seconds", type=float)
    p.add_argument("--reason", required=True)
    p.add_argument("--file", action="store_true", help="an MCP file's budget")
    p.add_argument("--total", action="store_true", help="the suite's total")
    args = parser.parse_args(argv)
    budget = load_budget(args.budget)
    if args.cmd == "allow":
        if not args.reason.strip():
            print("time_budget: a budget needs its reason", file=sys.stderr)
            return 2
        seconds = int(math.ceil(args.seconds))
        if args.total:
            budget.setdefault("suites", {}).setdefault(args.suite, {})["total"] = seconds
        else:
            section = "files" if args.file else "tests"
            budget.setdefault(section, {}).setdefault(args.suite, {})[args.name] = {"seconds": seconds, "reason": args.reason.strip()}
        _write(budget, args.budget)
        return 0
    suite = suite_of(args.lane)
    if suite is None:
        print(f"time_budget: lane {args.lane} has no budget (a sanitizer or platform lane)", file=sys.stderr)
        return 2
    tests = measure(args.files)
    if args.cmd == "check":
        findings = check(budget, suite, tests)
        print("\n".join(render(findings, suite)) or f"time_budget: {len(tests)} {suite} tests within budget")
        return 1 if findings else 0
    cap = budget["suites"][suite]["test_cap"]
    entries = (budget.get("tests") or {}).get(suite, {})
    for name in list(entries):
        if name in tests and tests[name] * 2 <= entries[name]["seconds"]:
            lowered = int(math.ceil(tests[name] * 1.25))
            if lowered <= cap:
                del entries[name]
                print(f"time_budget: {name} fits the cap of {cap} s now: entry dropped")
            else:
                print(f"time_budget: {name}: {entries[name]['seconds']} s -> {lowered} s")
                entries[name]["seconds"] = lowered
    _write(budget, args.budget)
    return 0


if __name__ == "__main__":
    sys.exit(main())
