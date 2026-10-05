#!/usr/bin/env python3
"""Reproduce ECS sort/morph microbenchmarks, including behavior assertions.

Run once on each revision, or use --baseline-ref to compare a git revision with
the working tree. Samples include CPU time (avoids scheduler contention), wall
time and allocations per call. These are isolated CPU kernels, not frame times.
The morph fixture compiles the exact method/helpers extracted from ActionSystem
with the real component/interpolation headers and a minimal accumulator owner;
it intentionally does not initialize a graphics/audio Scene.
"""
import argparse
import csv
import os
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def source(path, revision=None):
    if revision:
        return subprocess.check_output(["git", "show", f"{revision}:{path}"], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def definition(text, marker):
    start = text.index(marker)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def run_variant(out, name, revision, sanitize, only):
    print(f"Compiling {name}: {revision or 'working tree'} ({'ASan/UBSan checks' if sanitize else 'release benchmark'})", flush=True)
    variant = out / name
    variant.mkdir(parents=True, exist_ok=True)
    header = variant / "ComponentArray.h"
    header.write_text(source("engine/core/ecs/ComponentArray.h", revision))
    action = source("engine/core/subsystem/ActionSystem.cpp", revision)
    methods = "\n\n".join(definition(action, marker) for marker in (
        "template<typename T>\nstatic bool hasCubicSplineTangents",
        "template<typename TracksComp>\nstatic bool trackIndexValid",
        "void ActionSystem::morphTracksUpdate"))
    morph = variant / "morph_tracks.cpp"
    morph.write_text((ROOT / "tools/performance/morph_tracks.cpp").read_text().replace("// PRODUCTION_METHODS", methods))
    includes = [ROOT / "engine/core"] + [p for p in (ROOT / "engine/core").iterdir() if p.is_dir()]
    includes += [ROOT / "engine/libs", ROOT / "engine/libs/lua", ROOT / "engine/libs/luabridge3", ROOT / "engine/libs/sokol", ROOT / "engine/renders"]
    flags = ["-std=c++17", "-O3", "-DNO_LUA_INIT", "-DSOKOL_GLCORE"]
    if sanitize:
        flags = ["-std=c++17", "-O1", "-g", "-DNO_LUA_INIT", "-DSOKOL_GLCORE", "-DCORE_CHECK_ONLY", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for kind, src in (("component", ROOT / "tools/performance/component_sort.cpp"), ("morph", morph)):
        if only and kind != only:
            continue
        binary = variant / kind
        command = [os.environ.get("CXX", "g++"), *flags, *(f"-I{p}" for p in includes)]
        if kind == "component":
            command.append(f'-DCOMPONENT_ARRAY_HEADER="{header}"')
        command += [str(src), "-o", str(binary)]
        subprocess.run(command, check=True, cwd=ROOT)
        output = subprocess.check_output([str(binary)], text=True, cwd=ROOT)
        print(f"{name} {kind}: behavior assertions passed", flush=True)
        (variant / f"{kind}.csv").write_text(output)
        rows = list(csv.reader(output.splitlines()))
        for case, size in sorted({(r[0], r[1]) for r in rows}):
            sample = [r for r in rows if (r[0], r[1]) == (case, size)]
            cpu = statistics.median(float(r[3]) for r in sample)
            allocs = statistics.median(float(r[4]) for r in sample)
            wall = statistics.median(float(r[5]) for r in sample)
            print(f"{name:8} {kind:9} {case:8} {size:>6}: {cpu:12.1f} CPU ns, {allocs:3.0f} allocs, {wall:12.1f} wall ns/call", flush=True)


def paired_component(out):
    # Rename only the namespace/header guard; both implementations otherwise
    # remain untouched and share exactly the same entity/log declarations.
    baseline = (out / "before/ComponentArray.h").read_text()
    baseline = baseline.replace("COMPONENTARRAY_H", "BASELINE_COMPONENTARRAY_H")
    baseline = baseline.replace("namespace jasperengine {", "namespace baseline { using jasperengine::Entity; using jasperengine::Log;")
    header = out / "ComponentArray.paired-baseline.h"
    header.write_text(baseline)
    binary = out / "component-paired"
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O3",
        f"-I{ROOT / 'engine/core'}", f"-I{ROOT / 'engine/core/ecs'}",
        f'-DBASELINE_COMPONENT_HEADER="{header}"',
        str(ROOT / "tools/performance/component_sort_paired.cpp"), "-o", str(binary)], check=True)
    output = subprocess.check_output([str(binary)], text=True)
    (out / "component-paired.csv").write_text(output)
    rows = list(csv.reader(output.splitlines()))
    for size in sorted({r[0] for r in rows}, key=int):
        samples = [r for r in rows if r[0] == size]
        before = statistics.median(float(r[2]) for r in samples)
        after = statistics.median(float(r[3]) for r in samples)
        ratio = statistics.median(float(r[4]) for r in samples)
        print(f"paired reversed {size:>6}: {before:.1f} -> {after:.1f} CPU ns/call; median paired ratio {ratio:.4f}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", help="git revision to measure before the working tree")
    parser.add_argument("--output", type=Path, default=ROOT / "build/performance-core/repro")
    parser.add_argument("--sanitize", action="store_true", help="run behavior assertions with ASan/UBSan, without timing loops")
    parser.add_argument("--only", choices=("component", "morph"), help="compile and run only one kernel")
    parser.add_argument("--paired-component", action="store_true", help="also compare reverse-order sorting in one process (requires --baseline-ref)")
    args = parser.parse_args()
    if args.paired_component and (not args.baseline_ref or args.sanitize):
        parser.error("--paired-component requires --baseline-ref and cannot be combined with --sanitize")
    out = args.output.resolve()
    if args.baseline_ref:
        run_variant(out, "before", args.baseline_ref, args.sanitize, args.only)
    run_variant(out, "after", None, args.sanitize, args.only)
    if args.paired_component:
        paired_component(out)


if __name__ == "__main__":
    main()
