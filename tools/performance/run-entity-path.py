#!/usr/bin/env python3
"""Benchmark unchanged EntityPath.cpp with real EntityManager/ComponentArray/Transform.

The fixture replaces only the registry/SceneProject owner to avoid graphics/audio
initialization. This measures the path lookup kernel, not end-to-end RPC latency.
"""
import argparse
import csv
import os
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--subsets", action="store_true", help="measure late candidates from a 5,000-Transform scene")
    parser.add_argument("--output", type=Path, default=ROOT / "build/performance/entity-path/repro")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    includes = [ROOT / "tools/performance/entity_path_fixture", ROOT / "editor/util", ROOT / "engine/core", ROOT / "engine/core/ecs", ROOT / "engine/core/math"]
    math = [ROOT / f"engine/core/math/{name}.cpp" for name in ("Vector2", "Vector3", "Vector4", "Matrix3", "Matrix4", "Quaternion")]
    for variant, revision in ([("before", args.baseline_ref)] if args.baseline_ref else []) + [("after", None)]:
        source = output / f"EntityPath.{variant}.cpp"
        source.write_text(subprocess.check_output(["git", "show", f"{revision}:editor/util/EntityPath.cpp"], cwd=ROOT, text=True) if revision else (ROOT / "editor/util/EntityPath.cpp").read_text())
        binary = output / variant
        flags = ["-std=c++20", "-O2", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
        if args.sanitize:
            flags += ["-g", "-DCHECK_ONLY", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([os.environ.get("CXX", "c++"), *flags, *(f"-I{p}" for p in includes), str(source), str(ROOT / "tools/performance/entity_path.cpp"), *(str(p) for p in math), "-o", str(binary)], check=True)
        result = subprocess.check_output([str(binary), *(["--subsets"] if args.subsets else [])], text=True)
        (output / f"{variant}.csv").write_text(result)
        print(f"{variant}: behavior assertions passed", flush=True)
        rows = list(csv.reader(result.splitlines()))
        for key in sorted({tuple(row[:3]) for row in rows}, key=lambda k: (int(k[0]), k[1], k[2])):
            matches = [r for r in rows if tuple(r[:3]) == key]
            cpu = statistics.median(float(r[4]) for r in matches)
            wall = statistics.median(float(r[5]) for r in matches)
            print(f"{variant} {key}: {cpu:.3f} CPU us, {wall:.3f} wall us", flush=True)


if __name__ == "__main__":
    main()
