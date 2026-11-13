#!/usr/bin/env python3
"""The Check job's verdict (design §9): no silent green.

    python3 tools/ci/check_gate.py --needs needs.json --lanes build/ci-plan/lanes.json \\
        --reports <dir of ci-report-* artifacts> [--advisory "unit asan htmlui mcp smoke"]

--needs is `toJSON(needs)` of the Check job. Exit 0 only when every needed job succeeded, or was
skipped while lanes.json says that lane did not need to run. It fails when:
  - a job failed or was cancelled (a cancelled run is red);
  - a job was skipped that lanes.json planned, or that lanes.json does not know, or lanes.json is
    missing or unreadable (nothing then proves a skip was meant);
  - a full run (workflow_dispatch full=true) skipped any lane, or a merge-queue run skipped one
    other than mcp (whose planned selection may be empty) while anything ran (a group only paths
    nothing reads touch runs nothing, D3);
  - a test lane (TEST_LANES) succeeded with zero tests in its report, or with no report; each MCP
    shard of lanes.json counts on its own (report mcp-<shard>).
An --advisory lane's failure or zero tests are listed, not failing (rollout, design §14.1); its
cancellation still fails. The report job only posts the comment, so its result never gates.
Prints a table of job, result and verdict, also into $GITHUB_STEP_SUMMARY when set. Exit 2 when
--needs is unreadable. Reports and lanes.json are read as untrusted data (tools/ci/report.py).
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("ci_report", HERE / "report.py")
report = importlib.util.module_from_spec(_spec)
sys.modules.setdefault(_spec.name, report)
_spec.loader.exec_module(report)

TEST_LANES = ("unit", "asan", "htmlui", "mcp")
NOT_GATING = ("report",)


def gate(needs_path: Path, lanes_path: Path | None, reports_dir: Path | None, advisory: list[str],
         test_lanes: tuple[str, ...] = TEST_LANES, out=sys.stdout) -> int:
    needs = report.load_json(needs_path)
    if not isinstance(needs, dict):
        print(f"check_gate: {needs_path} is not toJSON(needs)", file=out)
        return 2
    plan = report.load_lanes(lanes_path)
    reports = report.load_reports(reports_dir) if reports_dir else {}
    advisory_set = set(advisory)
    rows: list[tuple[str, str, str]] = []
    failing: list[str] = []
    advisory_red: list[str] = []
    notes: list[str] = []
    if not plan["ok"]:
        notes.append("no usable lanes.json: a skipped job cannot be shown to be meant")
    event = "full" if plan["full"] else plan["event"]
    ran_any = any(lane["run"] for name, lane in plan["lanes"].items() if name != "changes")

    def row(job: str, result: str, verdict: str, fails: bool, is_advisory: bool = False) -> None:
        rows.append((job, result, verdict))
        if fails:
            failing.append(job)
        if is_advisory:
            advisory_red.append(job)

    def tests_verdict(job: str, result: str, lane_name: str) -> None:
        is_advisory = lane_name in advisory_set
        if event == "push" and lane_name != "mcp":
            # push to main only builds the test trees to warm caches (design §7.2); the queue ran them
            row(job, result, "ok (build only on push)", False)
            return
        r = reports.get(job)
        if r is None:
            problem = "no report, so no proof tests ran"
        elif r["tests"]["ran"] == 0:
            problem = "0 tests ran"
        else:
            row(job, result, f"ok ({r['tests']['ran']} tests)", False)
            return
        if is_advisory:
            row(job, result, f"advisory (rollout): {problem}", False, True)
        else:
            row(job, result, f"FAIL: {problem}", True)

    for job, value in needs.items():
        result = value.get("result") if isinstance(value, dict) else None
        result = result if result in report.STATUSES else "unknown"
        lane = plan["lanes"].get(job)
        is_advisory = job in advisory_set
        if job in NOT_GATING:
            row(job, result, "ok (the comment, not a verdict)", False)
        elif result == "cancelled":
            row(job, result, "FAIL: cancelled", True)
        elif result in ("failure", "unknown"):
            if is_advisory and result == "failure":
                row(job, result, "advisory (rollout): failing", False, True)
            else:
                row(job, result, "FAIL", True)
        elif result == "skipped":
            if not plan["ok"]:
                row(job, result, "FAIL: no lanes.json to show the skip was planned", True)
            elif lane is None:
                row(job, result, "FAIL: not in lanes.json, so its skip was not planned", True)
            elif lane["run"]:
                row(job, result, "FAIL: skipped, but the plan needed it", True)
            elif event == "full":
                row(job, result, "FAIL: a full run skips no lane", True)
            elif event == "merge_group" and job != "mcp" and ran_any:
                row(job, result, "FAIL: the merge queue skips no lane", True)
            else:
                row(job, result, f"ok (not planned: {lane['why'] or 'no reason given'})", False)
        elif job in test_lanes:   # success
            if job == "mcp" and plan["shards"]:
                for shard in plan["shards"]:
                    tests_verdict(f"mcp-{shard}", result, job)
            else:
                tests_verdict(job, result, job)
        else:
            row(job, result, "ok", False)

    for name, lane in plan["lanes"].items():
        if lane["run"] and name not in needs:
            notes.append(f"warning: lanes.json plans {name}, which Check does not need")

    width = max([len(r[0]) for r in rows] + [4])
    lines = [f"{'lane':<{width}}  {'result':<9}  verdict"]
    lines += [f"{job:<{width}}  {result:<9}  {verdict}" for job, result, verdict in rows]
    lines += notes
    if advisory_red:
        lines.append(f"advisory (rollout): failing: {', '.join(advisory_red)} (listed, not counted)")
    verdict = f"check: FAIL ({', '.join(failing)})" if failing else "check: PASS"
    lines.append(verdict)
    print("\n".join(lines), file=out)

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        md = ["| lane | result | verdict |", "|---|---|---|"]
        md += [f"| {job} | {result} | {report.md(verdict_text)} |" for job, result, verdict_text in rows]
        with open(summary, "a", encoding="utf-8") as f:
            f.write("\n".join(md + [""] + [report.md(n) for n in notes] + [f"**{verdict}**", ""]) + "\n")
    return 1 if failing else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--needs", type=Path, required=True, help="toJSON(needs) of the Check job")
    parser.add_argument("--lanes", type=Path, help="lanes.json of tools/ci/plan_lanes.py")
    parser.add_argument("--reports", type=Path, help="the downloaded ci-report-* artifacts")
    parser.add_argument("--advisory", default="", help="space separated lanes whose failures do not fail (rollout)")
    args = parser.parse_args()
    return gate(args.needs, args.lanes, args.reports, args.advisory.split())


if __name__ == "__main__":
    sys.exit(main())
