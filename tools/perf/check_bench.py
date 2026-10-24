#!/usr/bin/env python3
"""Check benchmark results against their budgets.

    python3 tools/perf/check_bench.py RESULT.json [RESULT.json ...] [--thresholds FILE]

Each RESULT.json is what a benchmark executable writes ({"bench": name, "results": {metric:
value}}: je-bench-jobs, je-bench-memory). The thresholds file (default
tools/perf/bench-thresholds.json) gives, per bench and metric, a "min" and/or "max". Prints
every metric with its budget and exits 1 when one is missed or a budgeted metric is missing.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("results", nargs="+", help="benchmark JSON files")
    parser.add_argument("--thresholds", default=str(HERE / "bench-thresholds.json"))
    args = parser.parse_args()

    thresholds = json.loads(Path(args.thresholds).read_text())
    failures = 0
    for path in args.results:
        data = json.loads(Path(path).read_text())
        bench = data.get("bench", "")
        budgets = thresholds.get(bench)
        if budgets is None:
            print(f"{path}: no thresholds for bench '{bench}'")
            failures += 1
            continue
        results = data.get("results", {})
        print(f"{bench} ({path}):")
        for metric, budget in budgets.items():
            if metric.startswith("_"):
                continue
            if metric not in results:
                print(f"  MISSING {metric}")
                failures += 1
                continue
            value = results[metric]
            ok = True
            bounds = []
            if "min" in budget:
                bounds.append(f">= {budget['min']}")
                ok = ok and value >= budget["min"]
            if "max" in budget:
                bounds.append(f"<= {budget['max']}")
                ok = ok and value <= budget["max"]
            status = "ok  " if ok else "FAIL"
            print(f"  {status} {metric:42} {value:>14.4g}   ({' and '.join(bounds)})  {budget.get('spec', '')}")
            failures += 0 if ok else 1
        for metric, value in results.items():
            if metric not in budgets:
                print(f"  info {metric:42} {value:>14.4g}")
    if failures:
        print(f"{failures} budget(s) missed")
        return 1
    print("all budgets hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
