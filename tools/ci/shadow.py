"""tools/ci/shadow.py: shadow mode of CI's test selection (docs/testing.md "CI", "Shadow mode").

A run that runs more than a pull request would (the merge queue's unfiltered unit tests, the
nightly's whole MCP suite) also says what a PR's selection would have picked for its diff
(lanes.json `shadow`, tools/ci/plan_lanes.py). Every test that failed and that selection leaves out
is a miss: the shortcut would have let it through.

    python3 tools/ci/shadow.py check --lanes plan/lanes.json --reports reports --out shadow.json
                                     [--history shadow.jsonl] [--run-id ID] [--run-url URL] [--summary FILE]
    python3 tools/ci/shadow.py merge --history shadow.jsonl --out new.jsonl RECORD.json...
    python3 tools/ci/shadow.py report --history shadow.jsonl
    python3 tools/ci/shadow.py since --history shadow.jsonl [--days 7]

check judges the run's lane reports (tools/ci/report.py collect: ci-report-*.json) and writes its
record; a failure the history saw within KNOWN_WINDOW_DAYS is pre-existing or flaky, not a miss; a
run whose shadow diff is empty attributes nothing. merge keeps the records the history does not
have yet; report adds the history up (the tracked report: shadow.jsonl on the ci-plan-inputs
branch, tools/ci/plan_inputs.py). since prints where to look for new CI runs. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

KNOWN_WINDOW_DAYS = 7
MAX_JSON_BYTES = 4_000_000
LISTED = 30
SCOPES = ("core", "editor", "htmlui", "mcp")
# lane report name -> what the PR selection of it is (the Linux lanes a selection narrows)
LANE_SCOPES = ((re.compile(r"^(unit|asan)-core$"), "core"), (re.compile(r"^(unit|asan)-editor(-\d+)?$"), "editor"),
               (re.compile(r"^htmlui$"), "htmlui"), (re.compile(r"^mcp-\d+$"), "mcp"))


def scope(lane: str) -> str | None:
    return next((name for pattern, name in LANE_SCOPES if pattern.match(lane)), None)


def unit_selected(selection, title: str) -> bool:
    """Whether a gtest selection (\"all\" or suites) runs the test a failure names: Suite.Test,
    Prefix/Suite.Test/0, Suite/0.Test, with a trailing word (\"crashed\") or not."""
    if selection == "all":
        return True
    name = title.split()[0] if title.split() else title
    suite = name.rsplit(".", 1)[0] if "." in name else name
    candidates = {suite, *suite.split("/")}
    return bool(candidates.intersection(selection or []))


def mcp_selected(selection, title: str) -> bool:
    if selection == "all":
        return True
    return title.split("::", 1)[0] in (selection or [])


def selected(shadow: dict, where: str, title: str, lane: str = "") -> bool:
    if where == "mcp":
        return mcp_selected(shadow.get("mcp"), title)
    # an ASan job's failure: the PR's ASan selection, which skips what the sanitizers cannot see (a
    # shadow from before it had one: the unit lane's)
    if lane.startswith("asan-") and isinstance(shadow.get("asan"), dict):
        return unit_selected(shadow["asan"].get(where), title)
    return unit_selected(shadow.get("unit", {}).get(where), title)


def whole_scopes(lanes: dict) -> list[str]:
    """The scopes the run ran whole (lanes.json): only there can it show what a selection leaves out
    (the merge queue's MCP lane runs the planned files, the selection itself). A lanes.json without
    the fields (an older one) counts as whole."""
    found = []
    unit_run, unit_filter = lanes.get("unit_run", {}), lanes.get("unit_filter", {})
    unit_lane = lanes.get("lanes", {}).get("unit", {"run": True}).get("run")
    for name in ("core", "editor"):
        if unit_lane and unit_run.get(name, True) and not unit_filter.get(name, ""):
            found.append(name)
    htmlui = lanes.get("lanes", {}).get("htmlui", {"run": True})
    if htmlui.get("run") and not lanes.get("htmlui_filter", ""):
        found.append("htmlui")
    if lanes.get("mcp", {}).get("mode", "ci") in ("ci", "nightly"):
        found.append("mcp")
    return found


def size(value) -> int | str:
    return "all" if value == "all" else len(value or [])


def judge(lanes: dict, reports: list[dict], history: list[dict], run_id: str = "", run_url: str = "",
          now: float | None = None) -> dict:
    """The run's record: each failed test of a lane the selection narrows, caught or missed."""
    now = time.time() if now is None else now
    shadow = lanes.get("shadow")
    record = {"run": str(run_id), "url": run_url, "event": lanes.get("event", ""), "head": lanes.get("head", ""),
              "time": now, "judged": bool(shadow), "why": lanes.get("shadow_why", ""), "base": (shadow or {}).get("base", ""),
              "changed": len((shadow or {}).get("files") or []), "fallbacks": (shadow or {}).get("fallbacks", []),
              "scopes": whole_scopes(lanes), "selected": {}, "ran": {}, "caught": 0, "failures": [], "misses": [],
              "known": [], "unattributed": []}
    if shadow:
        record["selected"] = {name: size(shadow.get("unit", {}).get(name)) for name in ("core", "editor", "htmlui")}
        record["selected"]["mcp"] = size(shadow.get("mcp"))
    cutoff = now - KNOWN_WINDOW_DAYS * 86400
    known = {(f.get("scope"), f.get("test")) for h in history if float(h.get("time", 0)) >= cutoff
             for f in h.get("failures", [])}
    order = {name: i for i, name in enumerate(SCOPES)}
    for rep in sorted(reports, key=lambda r: (order.get(scope(str(r.get("lane", ""))) or "", 9), str(r.get("lane", "")))):
        lane = str(rep.get("lane", ""))
        where = scope(lane)
        if where is None or where not in record["scopes"]:
            continue
        record["ran"][where] = record["ran"].get(where, 0) + int((rep.get("tests") or {}).get("ran", 0) or 0)
        for failure in rep.get("failures", []):
            if failure.get("kind") != "test" or not shadow:
                continue
            title = str(failure.get("title", ""))[:300]
            entry = {"scope": where, "lane": lane, "test": title}
            record["failures"].append({"scope": where, "test": title})
            if selected(shadow, where, title, lane):
                record["caught"] += 1
            elif not shadow.get("files"):
                record["unattributed"].append(entry)
            elif (where, title) in known:
                record["known"].append(entry)
            else:
                record["misses"].append(entry)
    if shadow and not any(record["ran"].values()):
        record["judged"] = False
        record["why"] = "the lanes a selection narrows ran no test whole (a build-only run, or only planned files)"
    return record


def markdown(rec: dict) -> str:
    lines = ["### Shadow selection", ""]
    if not rec["judged"]:
        lines.append(f"Not judged: {rec.get('why') or 'no PR-style selection in lanes.json'}.")
        return "\n".join(lines) + "\n"
    sel = ", ".join(f"{k} {v}" for k, v in rec["selected"].items())
    lines += [f"What a pull request's selection would have run for this diff ({rec['changed']} files against "
              f"{str(rec['base'])[:12]}): {sel}. Judged where this run ran every test: {', '.join(rec['scopes'])}.", "",
              f"**{len(rec['misses'])} misses** (a failed test it leaves out), {rec['caught']} caught, "
              f"{len(rec['known'])} pre-existing or flaky (failed within {KNOWN_WINDOW_DAYS} days), "
              f"{len(rec['unattributed'])} with no change to blame."]
    for title, entries in (("Misses", rec["misses"]), ("Pre-existing or flaky", rec["known"])):
        if entries:
            lines += ["", f"{title}:"] + [f"- `{e['test']}` ({e['lane']})" for e in entries[:LISTED]]
    if rec["fallbacks"]:
        lines += ["", "The selection widened: " + "; ".join(rec["fallbacks"])]
    return "\n".join(lines) + "\n"


def read_json(path: Path):
    try:
        if path.stat().st_size > MAX_JSON_BYTES:
            return None
        return json.loads(path.read_text(errors="replace"))
    except (OSError, ValueError):
        return None


def read_reports(directory: Path) -> list[dict]:
    found = []
    for path in sorted(directory.rglob("*.json")):
        data = read_json(path)
        if isinstance(data, dict) and isinstance(data.get("lane"), str) and isinstance(data.get("failures"), list):
            found.append(data)
    return found


def read_history(path: Path | None) -> list[dict]:
    if not path or not path.is_file():
        return []
    records = []
    for line in path.read_text(errors="replace").splitlines():
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict):
            records.append(value)
    return records


def report(history: list[dict]) -> str:
    judged = [r for r in history if r.get("judged")]
    misses = [(r, m) for r in judged for m in r.get("misses", [])]
    lines = ["### Shadow mode: the PR selection against the full runs", "",
             "| Runs | Caught | Misses | Pre-existing or flaky | No change to blame |", "|---|---|---|---|---|",
             f"| {len(judged)} | {sum(int(r.get('caught', 0)) for r in judged)} | {len(misses)} | "
             f"{sum(len(r.get('known', [])) for r in judged)} | {sum(len(r.get('unattributed', [])) for r in judged)} |"]
    by_event: dict[str, int] = {}
    for r in judged:
        by_event[r.get("event", "?")] = by_event.get(r.get("event", "?"), 0) + 1
    if by_event:
        lines += ["", "Runs: " + ", ".join(f"{n} {e}" for e, n in sorted(by_event.items()))]
    if misses:
        lines += ["", "Misses, newest first:"]
        for r, m in sorted(misses, key=lambda rm: -float(rm[0].get("time", 0)))[:LISTED]:
            when = time.strftime("%Y-%m-%d", time.gmtime(float(r.get("time", 0))))
            lines.append(f"- {when} `{m['test']}` ({m['lane']}, {r.get('event')} run {r.get('run')}, "
                         f"{r.get('changed', '?')} files against {str(r.get('base', ''))[:12]})")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    c = sub.add_parser("check")
    c.add_argument("--lanes", required=True, type=Path)
    c.add_argument("--reports", required=True, type=Path)
    c.add_argument("--out", required=True, type=Path)
    c.add_argument("--history", type=Path)
    c.add_argument("--run-id", default="")
    c.add_argument("--run-url", default="")
    c.add_argument("--summary", type=Path, help="append the Markdown here ($GITHUB_STEP_SUMMARY)")
    m = sub.add_parser("merge")
    m.add_argument("--history", required=True, type=Path)
    m.add_argument("--out", required=True, type=Path)
    m.add_argument("records", nargs="*", type=Path)
    r = sub.add_parser("report")
    r.add_argument("--history", required=True, type=Path)
    s = sub.add_parser("since")
    s.add_argument("--history", required=True, type=Path)
    s.add_argument("--days", type=float, default=KNOWN_WINDOW_DAYS, help="with no history: this many days back")
    args = ap.parse_args(argv)
    if args.command == "check":
        lanes = read_json(args.lanes)
        if not isinstance(lanes, dict):
            lanes = {"shadow": None, "shadow_why": f"no readable {args.lanes}"}
        rec = judge(lanes, read_reports(args.reports), read_history(args.history), args.run_id, args.run_url)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(rec, separators=(",", ":")) + "\n")
        text = markdown(rec)
        print(text)
        if args.summary:
            with args.summary.open("a") as f:
                f.write(text)
        return 0
    if args.command == "merge":
        seen = {str(h.get("run")) for h in read_history(args.history)}
        lines = []
        for path in args.records:
            rec = read_json(path)
            if isinstance(rec, dict) and rec.get("run") and str(rec["run"]) not in seen:
                seen.add(str(rec["run"]))
                lines.append(json.dumps(rec, separators=(",", ":")))
        args.out.write_text("".join(f"{line}\n" for line in lines))
        print(f"shadow: {len(lines)} new record(s)")
        return 0
    if args.command == "report":
        print(report(read_history(args.history)))
        return 0
    history = read_history(args.history)
    newest = max((float(h.get("time", 0)) for h in history), default=0.0)
    since = newest - 86400 if newest else time.time() - args.days * 86400
    print(time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(since)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
