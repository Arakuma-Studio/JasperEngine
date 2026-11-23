#!/usr/bin/env python3
"""The Check job's verdict (design §9): no silent green.

    python3 tools/ci/check_gate.py --needs needs.json --lanes build/ci-plan/lanes.json \\
        --reports <dir of ci-report-* artifacts> [--advisory "unit asan htmlui mcp smoke"]

--needs is {job: result} of the Check job's needs (or `toJSON(needs)`). Exit 0 only when every needed job succeeded, or was
skipped while lanes.json says that lane did not need to run. It fails when:
  - a job failed or was cancelled (a cancelled run is red);
  - a job was skipped that lanes.json planned, or that lanes.json does not know, or lanes.json is
    missing or unreadable (nothing then proves a skip was meant);
  - a full run (workflow_dispatch full=true) skipped any lane or test run, or a merge-queue run
    skipped one other than mcp (whose planned selection may be empty) while anything ran (a group
    only paths nothing reads touch runs nothing, D3), unless the lane or test run is --advisory and
    lanes.json lists it under advisory_off (planned off for that: tools/ci/plan_lanes.py), or lists it
    under queue_off (ios without an Apple-only input: the nightly builds it);
  - a test lane (TEST_LANES) succeeded with zero tests in its report, with no report, or with a
    report that failed (a test step that continues on error) unless lanes.json lists it under
    build_only (a push to main building what the queue tested); each MCP
    shard of lanes.json counts on its own (report mcp-<shard>), as does each unit job (unit-<job>:
    core, editor-1, editor-2) and each asan job (asan-<job>: core, editor).
A job of a lane that is not the lane's own job (windows-compile, of windows) is planned with its lane.
A job's test run reported apart from the job (windows-editor-tests, of the windows editor leg) must
have passed with tests in its report, or be --advisory.
An --advisory lane's failure or zero tests are listed, not failing (rollout, design §14.1); its
cancellation still fails. ci.yml's Check does not need the advisory lanes, so it does not wait for them;
the report job lists them. The report job only posts the comment, so its result never gates.
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
        print(f"check_gate: {needs_path} is not a JSON object of the needed jobs", file=out)
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

    def advisory_off(name: str) -> bool:
        """The merge queue left it to the advisory run for being advisory, and Check holds it advisory."""
        return name in advisory_set and name in plan["advisory_off"]

    def row(job: str, result: str, verdict: str, fails: bool, is_advisory: bool = False) -> None:
        rows.append((job, result, verdict))
        if fails:
            failing.append(job)
        if is_advisory:
            advisory_red.append(job)

    def tests_verdict(job: str, result: str, lane_name: str) -> None:
        is_advisory = lane_name in advisory_set
        if lane_name in plan["build_only"] and event not in ("merge_group", "full"):
            # push to main builds the test trees of the lanes the queue ran to warm caches (design §7.2)
            row(job, result, "ok (build only on push)", False)
            return
        r = reports.get(job)
        if r is None:
            problem = "no report, so no proof tests ran"
        elif r["status"] == "failure":
            # (a test step that continues on error leaves its job green)
            problem = "failing"
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
        result = report.needs_result(value)
        result = result if result in report.STATUSES else "unknown"
        # (windows-compile is a job of the windows lane: planned with it)
        lane = plan["lanes"].get(job, plan["lanes"].get(report.lane_base(job)))
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
            elif event == "merge_group" and job != "mcp" and ran_any and not advisory_off(job) \
                    and job not in plan["queue_off"]:
                row(job, result, "FAIL: the merge queue skips no lane", True)
            else:
                row(job, result, f"ok (not planned: {lane['why'] or 'no reason given'})", False)
        elif job in test_lanes:   # success
            if job == "mcp" and plan["shards"]:
                for shard in plan["shards"]:
                    tests_verdict(f"mcp-{shard}", result, job)
            elif job == "unit" and plan["unit_jobs"]:
                # the unit matrix (a job per test binary, the editor's in parts): each job's report counts on its own
                for unit_job in plan["unit_jobs"]:
                    tests_verdict(f"unit-{unit_job}", result, job)
            elif job == "asan" and plan["asan_jobs"]:
                # a job per test binary: each planned one must report the tests it ran
                for asan_job in plan["asan_jobs"]:
                    tests_verdict(f"asan-{asan_job}", result, job)
            else:
                tests_verdict(job, result, job)
        else:
            row(job, result, "ok", False)

    # a job's test run reported apart from it (report.TEST_REPORTS): its job's result is its build's,
    # so the report itself must show the tests ran and passed
    for test_report, job in report.TEST_REPORTS.items():
        lane = plan["lanes"].get(job)
        if not lane or not lane["run"] or job not in needs:
            continue
        if test_report in plan["build_only"]:
            if event == "full":
                row(test_report, "skipped", "FAIL: a full run skips no test run", True)
            elif event == "merge_group" and not advisory_off(test_report):
                row(test_report, "skipped", "FAIL: the merge queue skips no test run", True)
            elif event == "merge_group":
                row(test_report, "skipped", "ok (advisory: not run in the merge queue)", False)
            continue
        is_advisory = test_report in advisory_set
        r = reports.get(test_report)
        if r is None:
            result, problem = "missing", "no report, so no proof tests ran"
        elif r["status"] != "success":
            result, problem = r["status"], "failing"
        elif r["tests"]["ran"] == 0:
            result, problem = r["status"], "0 tests ran"
        else:
            row(test_report, r["status"], f"ok ({r['tests']['ran']} tests)", False)
            continue
        if is_advisory:
            row(test_report, result, f"advisory (rollout): {problem}", False, True)
        else:
            row(test_report, result, f"FAIL: {problem}", True)

    for name, lane in plan["lanes"].items():
        # (ci.yml's Check does not need the advisory lanes: report lists them)
        if lane["run"] and name not in needs and name not in advisory_set:
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
    parser.add_argument("--needs", type=Path, required=True, help="{job: result} of the Check job's needs")
    parser.add_argument("--lanes", type=Path, help="lanes.json of tools/ci/plan_lanes.py")
    parser.add_argument("--reports", type=Path, help="the downloaded ci-report-* artifacts")
    parser.add_argument("--advisory", default="", help="space separated lanes whose failures do not fail (rollout)")
    args = parser.parse_args()
    return gate(args.needs, args.lanes, args.reports, args.advisory.split())


if __name__ == "__main__":
    sys.exit(main())
