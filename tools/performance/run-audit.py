#!/usr/bin/env python3
"""Run the performance comparisons sequentially and keep their evidence.

Run on a quiet host with --baseline-ref pointing to the original revision.
--sanitize runs behavior checks with ASan/UBSan instead of measuring time.
The individual runners compile production methods and real dependencies; these
are CPU kernel measurements, not end-to-end frame-rate measurements.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
RUNNERS = {
    "ancestry": ("run-ancestry.py", "--sanitize"),
    "pose": ("run_pose_settle.py", "--check"),
    "matrix": ("run_matrix_vectors.py", "--sanitize"),
    "tokenizer": ("editor_tokenize.py", "--sanitize"),
    "inference": ("editor_inference.py", "--sanitize"),
    "text": ("run-text-shaper.py", "--sanitize"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "build/performance-audit")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--only", choices=RUNNERS)
    args = parser.parse_args()
    out = args.output.resolve() / ("sanitize" if args.sanitize else "release")
    out.mkdir(parents=True, exist_ok=True)

    def git(*arguments):
        return subprocess.check_output(["git", *arguments], cwd=ROOT, text=True).strip()

    compiler = os.environ.get("CXX", "c++")
    environment = {
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
        "compiler": subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
        "baseline": git("rev-parse", args.baseline_ref),
        "head": git("rev-parse", "HEAD"),
        "tracked_diff_sha256": hashlib.sha256(git("diff", "HEAD").encode()).hexdigest(),
        "sanitize": args.sanitize,
    }
    for name, (script, check_flag) in RUNNERS.items():
        if args.only and name != args.only:
            continue
        log = out / f"{name}.log"
        (out / f"{name}.environment.json").write_text(json.dumps(environment, indent=2) + "\n")
        command = [sys.executable, str(ROOT / "tools/performance" / script),
                   "--baseline-ref", args.baseline_ref, "--output", str(out / name)]
        if args.sanitize:
            command.append(check_flag)
        print(f"Running {name}; evidence: {log}", flush=True)
        with log.open("w") as output:
            result = subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        if result.returncode:
            print(log.read_text(), file=sys.stderr)
            raise SystemExit(result.returncode)
        print(f"Passed {name}", flush=True)
    print(f"All requested comparisons passed. Evidence: {out}", flush=True)


if __name__ == "__main__":
    main()
