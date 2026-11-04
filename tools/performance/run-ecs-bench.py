#!/usr/bin/env python3
"""Run the S07 ECS benchmark (tools/performance/ecs_bench.cpp) and check its budgets.

Builds jasperengine-ecs-bench against the working tree's engine (Release, -O2) and, with
--baseline-ref, against the engine of that git revision (its engine/ directory exported with
git archive; the same benchmark source is compiled against it). Each benchmark runs --runs
times; medians are compared with the absolute budgets of R25 (working tree) and the relative
ones of R26 (working tree against the baseline). Bytes per component and per entity are
reported against R27. Exits non-zero when a budget is missed or a behaviour assertion of the
benchmark fails. Next to each budget it prints this machine's floor: the same workload on
plain vectors (no ECS), which shows a budget no storage can meet on the machine. The rows go
to <output>/ecs-bench.csv.

    tools/performance/run-ecs-bench.py [--baseline-ref oe-port] [--sizes 10000,100000] [--runs 7]
    tools/performance/run-ecs-bench.py --baseline-archive build/oe-port-engine.tar   # git archive oe-port engine

Run it inside the Linux build image (tools/build.sh exec linux ...).
"""
import argparse
import csv
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]

# R25: median ms (ns/op for the lookups) at 10k and 100k entities
ABSOLUTE = {
    "create": {10000: 3.0, 100000: 35.0},
    "destroy": {10000: 2.0, 100000: 25.0},
    "remove": {10000: 0.5, 100000: 6.0},
    "add": {10000: 1.5, 100000: 18.0},
    "index_loop": {10000: 0.15, 100000: 2.0},
    "view": {10000: 0.12, 100000: 1.5},
    "lookup": {10000: 15.0, 100000: 25.0},
    "move_branch": {10000: 0.3, 100000: 3.0},
}
PER_OP = {"lookup", "metadata", "floor_lookup", "floor_metadata"}

# R27
BYTES_PER_COMPONENT = 24.0
BYTES_PER_ENTITY = 96.0


def build(source_engine: Path, build_dir: Path, bench_cmake: Path, jobs: str) -> Path:
    command = ["cmake", "-S", str(source_engine), "-B", str(build_dir), "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG", "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG"]
    if shutil.which("ccache"):
        command += ["-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"]
    if bench_cmake is None:
        command.append("-DJASPERENGINE_ECS_BENCH=ON")
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    build_command = ["cmake", "--build", str(build_dir), "--target", "jasperengine-ecs-bench"]
    if jobs:
        build_command += ["-j", jobs]
    subprocess.run(build_command, check=True)
    return build_dir / "jasperengine-ecs-bench"


def export_baseline(ref: str, out: Path, archive_path: Path = None) -> Path:
    source = out / "baseline-src"
    if source.exists():
        shutil.rmtree(source)
    source.mkdir(parents=True)
    if archive_path:
        archive = archive_path.read_bytes()
    else:
        archive = subprocess.run(["git", "-C", str(ROOT), "archive", "--format=tar", ref, "engine"], check=True, capture_output=True).stdout
    subprocess.run(["tar", "-x", "-C", str(source)], input=archive, check=True)
    engine = source / "engine"
    # the working tree's benchmark target, built against this engine
    with open(engine / "CMakeLists.txt", "a", encoding="utf-8") as cmake:
        cmake.write(f'\ninclude("{ROOT / "tools/performance/EcsBench.cmake"}")\n')
    return engine


def run(binary: Path, sizes: str, runs: int, name: str, out: Path, ops: str = ""):
    print(f"Running {name}: {binary}", flush=True)
    command = [str(binary), "--sizes", sizes, "--runs", str(runs)]
    if ops:
        command += ["--ops", ops]
    result = subprocess.run(command, capture_output=True, text=True, cwd=out)
    (out / f"{name}.csv").write_text(result.stdout)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit(f"{name}: the benchmark failed (exit {result.returncode}): behaviour assertion or crash")
    times, metrics = {}, {}
    for row in csv.reader(result.stdout.splitlines()):
        if not row or row[0] in ("op", "#"):
            continue
        if row[0] == "metric":
            metrics[(row[2], int(row[1]))] = float(row[3])
            continue
        op, n, ms, ops = row[0], int(row[1]), float(row[3]), int(row[4])
        value = ms * 1e6 / ops if op in PER_OP else ms
        times.setdefault((op, n), []).append(value)
    return {key: statistics.median(values) for key, values in times.items()}, metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline-ref", help="git revision to compare with (R26)")
    parser.add_argument("--baseline-archive", type=Path,
                        help="instead of --baseline-ref: a tar of a revision's engine/ (git archive REV engine), "
                             "for a checkout without that revision (a build server's synced worktree)")
    parser.add_argument("--output", type=Path, default=ROOT / "build/performance-ecs")
    parser.add_argument("--sizes", default="10000,100000")
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--jobs", default=os.environ.get("JOBS", ""))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    after_binary = build(ROOT / "engine", out / "after-build", None, args.jobs)
    after, metrics = run(after_binary, args.sizes, args.runs, "after", out)
    before = None
    if args.baseline_ref or args.baseline_archive:
        baseline_engine = export_baseline(args.baseline_ref, out, args.baseline_archive)
        before_binary = build(baseline_engine, out / "before-build", baseline_engine, args.jobs)
        # only what R26 compares (the pre-S07 engine builds its worlds in quadratic time)
        before, _ = run(before_binary, args.sizes, args.runs, "before", out, "destroy,index_loop,lookup")

    failures = []
    rows = []
    # floor_<op>: the same work on plain vectors (ecs_bench.cpp runFloor), what this machine
    # allows at best; a budget under its floor cannot be met here by any storage
    floors = {(op[len("floor_"):], n): value for (op, n), value in after.items() if op.startswith("floor_")}
    floors[("index_loop_payload", 10000)] = floors.get(("view", 10000))
    floors[("index_loop_payload", 100000)] = floors.get(("view", 100000))
    print(f"\n{'op':<20}{'n':>8}{'after':>14}{'budget':>10}{'floor':>10}{'before':>14}{'ratio':>9}")
    for (op, n), value in sorted(after.items()):
        if op.startswith("floor_"):
            continue
        floor = floors.get((op, n))
        budget = ABSOLUTE.get(op, {}).get(n)
        old = before.get((op, n)) if before else None
        ratio = old / value if old and value > 0 else None
        unit = "ns/op" if op in PER_OP else "ms"
        status = ""
        if budget is not None and value > budget:
            below_floor = floor is not None and floor >= budget
            failures.append(f"R25 {op} at {n}: {value:.3f} {unit} > {budget} {unit}"
                            + (f" (this machine's floor is {floor:.3f} {unit})" if below_floor else ""))
            status = "  MISSED" + (" (budget under the machine's floor)" if below_floor else "")
        print(f"{op:<20}{n:>8}{value:>11.3f} {unit:<3}{(budget if budget is not None else ''):>9}"
              f"{(f'{floor:10.3f}' if floor is not None else ''):>10}"
              f"{(f'{old:11.3f}' if old is not None else ''):>14}{(f'{ratio:8.2f}x' if ratio else ''):>9}{status}")
        rows.append({"op": op, "n": n, "unit": unit, "after": value, "budget": budget, "floor": floor, "before": old, "speedup": ratio})

    if before:
        def ratio(op, n, base_op=None):
            old = before.get((base_op or op, n))
            new = after.get((op, n))
            return old / new if old and new else None
        checks = [
            ("R26 one-by-one destroy of 10k at least 20x faster", ratio("destroy", 10000), lambda r: r >= 20.0),
            ("R26 random metadata get at 100k at least 3x faster", ratio("metadata", 100000), lambda r: r >= 3.0),
            ("R26 index loop at most 15% slower (10k)", ratio("index_loop", 10000), lambda r: r >= 1 / 1.15),
            ("R26 index loop at most 15% slower (100k)", ratio("index_loop", 100000), lambda r: r >= 1 / 1.15),
            ("R26 view no slower than the baseline index loop (10k)", ratio("view", 10000, "index_loop_payload"), lambda r: r >= 1.0),
            ("R26 view no slower than the baseline index loop (100k)", ratio("view", 100000, "index_loop_payload"), lambda r: r >= 1.0),
        ]
        print()
        for name, value, ok in checks:
            if value is None:
                failures.append(f"{name}: not measured")
                continue
            passed = ok(value)
            print(f"{name}: {value:.2f}x {'ok' if passed else 'MISSED'}")
            if not passed:
                failures.append(f"{name}: {value:.2f}x")

    print()
    for (name, n), value in sorted(metrics.items()):
        limit = BYTES_PER_COMPONENT if name == "bytes_per_component" else BYTES_PER_ENTITY
        print(f"R27 {name} at {n}: {value:.2f} B (budget {limit:.0f} B){'' if value <= limit else '  OVER'}")
        rows.append({"op": name, "n": n, "unit": "B", "after": value, "budget": limit, "floor": None, "before": None, "speedup": None})

    with open(out / "ecs-bench.csv", "w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=["op", "n", "unit", "after", "budget", "floor", "before", "speedup"])
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nRows: {out / 'ecs-bench.csv'}")

    if failures:
        print("\nBudgets missed:\n  " + "\n  ".join(failures))
        return 1
    print("\nEvery budget met.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
