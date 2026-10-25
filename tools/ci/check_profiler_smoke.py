#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Checks a smoke test's output against S15's acceptance (docs/specs/S15-profiling-diagnostics.md).

    tools/ci/check_profiler_smoke.py --frame-stats engine/tests/data/framestats_baseline.txt \\
        build/smoke/game.log build/smoke/game-actions.log           # AC9: the lines are unchanged
    tools/ci/check_profiler_smoke.py --no-profiler build/smoke/export-build/<game>   # AC1: release has none
    tools/ci/check_profiler_smoke.py --capture build/smoke/capture.jetrace.json build/smoke/game.log  # AC12

--frame-stats: the first "Frame stats (frame N): ..." line of each log, without its prefix, must
equal the baseline's line of the same position (byte for byte). --no-profiler: `nm -C` of the
release game has no jasperengine::profiler:: symbol. --capture: the log says "Profiler capture
written: <path>" and the file is a version 1 .jetrace.json with frames and main-thread scopes.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys


def frame_stats(baseline: str, logs: list[str]) -> list[str]:
    expected = [l.rstrip("\n") for l in open(baseline, encoding="utf-8") if l.strip() and not l.startswith("#")]
    errors = []
    for i, log in enumerate(logs):
        found = None
        for line in open(log, encoding="utf-8", errors="replace"):
            m = re.search(r"Frame stats \(frame \d+\): (.*)$", line.rstrip("\n"))
            if m:
                found = m.group(1)
                break
        if found is None:
            errors.append(f"{log}: no Frame stats line")
        elif i >= len(expected):
            errors.append(f"{log}: no baseline line {i + 1}")
        elif found != expected[i]:
            errors.append(f"{log}: the line changed\n  before: {expected[i]}\n  now:    {found}")
    return errors


def no_profiler(binary: str) -> list[str]:
    out = subprocess.run(["nm", "-C", binary], capture_output=True, text=True).stdout
    hits = [l for l in out.splitlines() if "jasperengine::profiler::" in l]
    return [f"{binary}: {len(hits)} profiler symbols, e.g. {hits[0]}"] if hits else []


def capture(path: str, log: str) -> list[str]:
    errors = []
    if f"Profiler capture written: " not in open(log, encoding="utf-8", errors="replace").read():
        errors.append(f"{log}: no 'Profiler capture written:' line")
    try:
        trace = json.load(open(path, encoding="utf-8"))
    except (OSError, ValueError) as e:
        return errors + [f"{path}: {e}"]
    jasper = trace.get("jasper", {})
    if jasper.get("version") != 1:
        errors.append(f"{path}: jasper.version {jasper.get('version')}")
    main = [t["id"] for t in jasper.get("threads", []) if t.get("main")]
    frames = {e.get("args", {}).get("frame") for e in trace.get("traceEvents", []) if e.get("ph") == "X" and main and e.get("tid") == main[0]}
    if not jasper.get("frames") or len(frames) < len(jasper.get("frames", [])):
        errors.append(f"{path}: {len(frames)} frames with main-thread scopes of {len(jasper.get('frames', []))}")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frame-stats", metavar="BASELINE")
    ap.add_argument("--no-profiler", action="store_true")
    ap.add_argument("--capture", metavar="JETRACE")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    if a.frame_stats:
        errors = frame_stats(a.frame_stats, a.files)
    elif a.no_profiler:
        errors = [e for f in a.files for e in no_profiler(f)]
    elif a.capture:
        errors = capture(a.capture, a.files[0])
    else:
        ap.error("one of --frame-stats, --no-profiler, --capture")
    for e in errors:
        print("FAIL:", e)
    if not errors:
        print("ok")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
