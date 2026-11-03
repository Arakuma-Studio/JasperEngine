#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The profiler's CPU overhead (spec S15 AC4, R27/R28), measured with `jasperengine-editor benchmark`.

    tools/perf/profiler_overhead.py --editor build/linux/editor-install/bin/jasperengine-editor \\
        --baseline <editor without the profiler> --project tools/fixtures/post-processing \\
        [--scene <scene>] [--runs 3] [--frames 300] [--warmup 120] [--json out.json]

Four configurations (`--configs` picks some), `--runs` rounds of one editor process each, in a
shuffled order per round. Each round gives every configuration's overhead against the baseline
of the same round (paired: a drift of the machine cancels), from the mean frame time `avgMs`
(the measured frames' wall time over their count); the report is the mean of those overheads
with its 95 % confidence interval (Student's t) and the medians:

    compiled in, off        the editor as shipped (the profiler idle)       <= 0.5 % or 5 us
    recording               --profile                                       <= 3 % or 30 us
    recording + GPU timers  --profile --gpu-passes                          <= 3.5 % or 40 us

each "or" taken as whichever is larger (AC4's absolute floor a frame, approved 2026-11-01: on a GPU's
~1 ms frame 0.5 % is 5 us, under the measurement's resolution).

The baseline is an editor built without S15 (the commit before it: the editor itself always has
the profiler, so "compiled out" is that build). Run it on an otherwise idle machine (the benchmark
server; 30+ rounds for the idle configuration's 0.5 %). Exit 1 when a mean overhead is over its
threshold; --no-check only prints.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import statistics
import subprocess
import sys
import tempfile

THRESHOLDS = {"off": 0.5, "recording": 3.0, "recording_gpu": 3.5}
# the absolute floor a frame of each configuration (us): the bound is whichever is larger
FLOORS_US = {"off": 5.0, "recording": 30.0, "recording_gpu": 40.0}


def run(editor: str, args: list[str], project: str, scene: str | None, frames: int, warmup: int) -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "result.json")
        cmd = [editor, "benchmark", "--project", project, "--frames", str(frames), "--warmup", str(warmup), "--out", out]
        if scene:
            cmd += ["--scene", scene]
        cmd += args
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
        with open(out) as f:
            return json.load(f)


T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228,
       12: 2.179, 15: 2.131, 20: 2.086, 25: 2.060, 30: 2.042, 40: 2.021, 60: 2.000, 120: 1.980}


def t95(df: int) -> float:
    for key in sorted(T95):
        if df <= key:
            return T95[key]
    return 1.960


def mean_ci(values: list[float]) -> tuple[float, float]:
    n = len(values)
    mean = statistics.fmean(values)
    if n < 2:
        return mean, float("nan")
    return mean, t95(n - 1) * statistics.stdev(values) / math.sqrt(n)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--editor", required=True)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--project", required=True)
    ap.add_argument("--scene")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--warmup", type=int, default=120)
    ap.add_argument("--configs", default="off,recording,recording_gpu", help="comma list of off, recording, recording_gpu")
    ap.add_argument("--seed", type=int, default=15)
    ap.add_argument("--json")
    ap.add_argument("--no-check", action="store_true")
    a = ap.parse_args()

    all_configs = {"baseline": (a.baseline, []), "off": (a.editor, []), "recording": (a.editor, ["--profile"]),
                   "recording_gpu": (a.editor, ["--profile", "--gpu-passes"])}
    names = ["baseline"] + [c for c in a.configs.split(",") if c in all_configs and c != "baseline"]
    results: dict[str, list[float]] = {name: [] for name in names}
    rng = random.Random(a.seed)
    for _ in range(a.runs):
        order = names[:]
        rng.shuffle(order)
        for name in order:
            editor, extra = all_configs[name]
            results[name].append(run(editor, extra, a.project, a.scene, a.frames, a.warmup)["avgMs"])
    medians = {name: statistics.median(values) for name, values in results.items()}
    report = {"runs": a.runs, "frames": a.frames, "avgMs": results, "medianMs": medians, "overheadPercent": {},
              "ci95Percent": {}}
    failed = False
    base = results["baseline"]
    print(f"baseline  mean {statistics.fmean(base):.4f} ms  median {medians['baseline']:.4f} ms  ({a.runs} rounds)")
    for name in names[1:]:
        paired = [(v - b) * 100.0 / b for v, b in zip(results[name], base) if b > 0]
        mean, half = mean_ci(paired)
        report["overheadPercent"][name] = mean
        report["ci95Percent"][name] = [mean - half, mean + half]
        # the larger of the relative bound and the floor, as a percentage of this baseline's frame
        bound = max(THRESHOLDS[name], FLOORS_US[name] / 1000.0 * 100.0 / statistics.fmean(base))
        report.setdefault("boundPercent", {})[name] = bound
        ok = mean <= bound
        failed = failed or not ok
        print(f"{name:14} mean {statistics.fmean(results[name]):.4f} ms  overhead {mean:+.3f} % "
              f"(95 % CI {mean - half:+.3f} .. {mean + half:+.3f}; <= {bound:.3f} %: {THRESHOLDS[name]} % or "
              f"{FLOORS_US[name]:.0f} us) {'ok' if ok else 'OVER'}")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(report, f, indent=1)
    return 1 if failed and not a.no_check else 0


if __name__ == "__main__":
    sys.exit(main())
