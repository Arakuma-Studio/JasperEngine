#!/usr/bin/env python3
"""Reproduce the audit's software-rendered scene integration comparison.

Requires Linux, Ninja, a Release editor build in build/validation (including
compile_commands.json), dbus-run-session, Xvfb and Mesa. Run `baseline` first,
then `compare`. Only the five audited translation units are rolled back to the
baseline revision; all unchanged objects and dependencies are shared with the
current build. Baseline objects, binaries and copied fixtures stay under build/.
Frame results check integration and rendering invariants, not CPU kernel gains.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/validation"
BASE = BUILD / "baseline"
OUT = BUILD / "integration"
CHANGED = [
    "engine/core/math/Matrix4.cpp",
    "engine/core/pose/PoseAnimatorRuntime.cpp",
    "engine/core/registry/EntityRegistry.cpp",
    "engine/core/util/TextShaper.cpp",
    "editor/window/widget/CustomTextEditor.cpp",
]


def build_baseline(revision):
    BASE.mkdir(exist_ok=True)
    entries = json.loads((BUILD / "compile_commands.json").read_text())
    objects = {}
    provenance = {}
    for relative in CHANGED:
        original = ROOT / relative
        row = next(row for row in entries if row["file"] == str(original)
                   and "lattice-tests.dir" not in row["command"])
        arguments = shlex.split(row["command"])
        original_object = arguments[arguments.index("-o") + 1]
        old_source = BASE / "source" / relative
        old_source.parent.mkdir(parents=True, exist_ok=True)
        content = subprocess.check_output(["git", "show", f"{revision}:{relative}"], cwd=ROOT)
        old_source.write_bytes(content)
        old_object = BASE / "objects" / original_object
        old_object.parent.mkdir(parents=True, exist_ok=True)
        arguments[arguments.index("-o") + 1] = str(old_object)
        arguments[arguments.index(str(original))] = str(old_source)
        arguments += ["-iquote", str(original.parent)]
        print("baseline compile:", relative, flush=True)
        subprocess.run(arguments, cwd=row["directory"], check=True)
        objects[original_object] = str(old_object)
        provenance[relative] = {"baseline_sha256": hashlib.sha256(content).hexdigest(),
                                "current_sha256": hashlib.sha256(original.read_bytes()).hexdigest()}

    for target, filename in [("jasperengine", "libjasperengine.so"),
                              ("jasperengine-editor", "jasperengine-editor")]:
        lines = subprocess.check_output(["ninja", "-C", str(BUILD), "-t", "commands", target],
                                        text=True).splitlines()
        command = next(line for line in lines if f" -o {filename} " in line)
        arguments = shlex.split(command)
        arguments = arguments[2:-2]  # CMake's leading ': &&' and trailing '&& :'.
        arguments = [objects.get(arg, arg) for arg in arguments]
        arguments[arguments.index("-o") + 1] = str(BASE / filename)
        arguments = ["-Wl,-rpath,$ORIGIN" if arg.startswith("-Wl,-rpath,") else arg
                     for arg in arguments]
        if target == "jasperengine-editor":
            arguments = [str(BASE / "libjasperengine.so") if arg == "libjasperengine.so" else arg
                         for arg in arguments]
        print("baseline link:", filename, flush=True)
        subprocess.run(arguments, cwd=BUILD, check=True)
    if not (BASE / "engine").exists():
        (BASE / "engine").symlink_to(BUILD / "engine", target_is_directory=True)
    (BASE / "provenance.json").write_text(json.dumps({"revision": revision, "sources": provenance}, indent=2))
    subprocess.run(["ldd", str(BASE / "jasperengine-editor")], check=True)


def compare():
    OUT.mkdir(exist_ok=True)
    records = []
    fixture = ROOT / "tools/fixtures/pose-animation"
    for sample, order in enumerate((("before", "after"), ("after", "before"), ("before", "after"))):
        for variant in order:
            run = OUT / f"{sample}-{variant}"
            if run.exists():
                shutil.rmtree(run)
            run.mkdir()
            project = run / "project"
            shutil.copytree(fixture, project)
            binary = (BASE if variant == "before" else BUILD) / "jasperengine-editor"
            result = run / "result.json"
            environment = os.environ.copy()
            environment.update(LIBGL_ALWAYS_SOFTWARE="1", LP_NUM_THREADS="2",
                               LD_LIBRARY_PATH=str(binary.parent),
                               XDG_CONFIG_HOME=str(run / "config"),
                               XDG_CACHE_HOME=str(run / "cache"))
            command = ["dbus-run-session", "--", "xvfb-run", "-a", "-s", "-screen 0 1600x900x24", str(binary),
                       "benchmark", "--project", str(project), "--scene", "Main Scene",
                       "--warmup", "60", "--frames", "180", "--mesh-lod", "off",
                       "--depth-prepass", "off", "--name", f"{sample}-{variant}", "--out", str(result)]
            print("integration run:", sample, variant, flush=True)
            with (run / "console.log").open("w") as log:
                subprocess.run(command, cwd=run, env=environment, check=True,
                               stdout=log, stderr=subprocess.STDOUT, timeout=240)
            record = json.loads(result.read_text())
            record.update(variant=variant, sample=sample)
            records.append(record)
            print(json.dumps(record), flush=True)
    invariants = ["viewport", "frames", "warmupFrames", "meshLod", "depthPrepass",
                  "avgDrawCalls", "avgInstances", "avgTriangles"]
    for field in invariants:
        assert all(row[field] == records[0][field] for row in records), (field, records)
    summary = {"renderer": "Mesa llvmpipe (LIBGL_ALWAYS_SOFTWARE=1, LP_NUM_THREADS=2)",
               "scene": "pose-animation/Main Scene", "invariants": invariants,
               "records": records}
    for variant in ("before", "after"):
        summary[variant] = {"median_avgMs": statistics.median(r["avgMs"] for r in records if r["variant"] == variant),
                            "median_wallFps": statistics.median(r["wallFps"] for r in records if r["variant"] == variant)}
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("baseline", "compare"))
    parser.add_argument("--baseline-ref", default="139d300f2")
    args = parser.parse_args()
    if args.command == "baseline":
        build_baseline(args.baseline_ref)
    else:
        compare()
