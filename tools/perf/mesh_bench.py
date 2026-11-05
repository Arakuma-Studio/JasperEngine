#!/usr/bin/env python3
"""The mesh benchmark gate (docs/specs/S55-draw-submission.md R8-R10, AC6).

    python3 tools/perf/mesh_bench.py --editor <jasperengine-editor> --project <dir> [--json out.json] [--check]

<dir> is a project tools/fixtures/mesh-bench/generate.py wrote. The scene plays through
`jasperengine-editor benchmark --play`: with GPU Instancing off (every draw alone, the
submission before S55) and on, and on again with the project's Shadow LOD Bias at 1.0 (what a
project without the key reads: shadows at the colour pass's detail) and at 0.25. The result
("bench": "mesh-bench") holds the deterministic counters of the on run (draw calls by pass,
triangles, uniform bytes, buffers: gated exactly), the drops R9 gates by its minimums (draws and
uniform bytes from off to on, shadow triangles from bias 1.0 to the project's 0.5) and the times
(informational). --check runs tools/perf/check_bench.py on it against
tools/perf/bench-thresholds.json.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
# a run that never ends must not hold the machine
RUN_TIMEOUT_SECONDS = 900


def run(editor: str, project: str, instancing: str, frames: int, warmup: int, scratch: Path, name: str) -> dict:
    # an editor of its own: it reads and writes settings.yaml and editor.log in its working directory
    cwd = scratch / name
    cwd.mkdir()
    (cwd / "settings.yaml").write_text("{}\n")
    out = cwd / "result.json"
    command = [editor, "benchmark", "--project", project, "--play", "--frames", str(frames), "--warmup", str(warmup),
               "--gpu-instancing", instancing, "--name", "mesh-bench-" + name, "--out", str(out)]
    print(" ".join(command), flush=True)
    subprocess.run(command, check=True, cwd=cwd, timeout=RUN_TIMEOUT_SECONDS)
    return json.loads(out.read_text())


def project_with_bias(project: str, bias: float, scratch: Path) -> str:
    """A copy of the project with another Shadow LOD Bias."""
    copy = scratch / f"project-bias-{bias}"
    shutil.copytree(project, copy)
    yaml = copy / "project.yaml"
    text = yaml.read_text()
    assert "shadow_lod_bias: 0.5" in text, "the bench project sets Shadow LOD Bias 0.5"
    yaml.write_text(text.replace("shadow_lod_bias: 0.5", f"shadow_lod_bias: {bias}"))
    return str(copy)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--editor", required=True)
    parser.add_argument("--project", required=True)
    parser.add_argument("--frames", type=int, default=120)
    parser.add_argument("--warmup", type=int, default=60)
    parser.add_argument("--json", help="where to write the result")
    parser.add_argument("--check", action="store_true", help="check the result against bench-thresholds.json")
    args = parser.parse_args()

    editor = os.path.abspath(args.editor)
    project = os.path.abspath(args.project)
    with tempfile.TemporaryDirectory() as directory:
        scratch = Path(directory)
        off = run(editor, project, "off", args.frames, args.warmup, scratch, "off")
        on = run(editor, project, "on", args.frames, args.warmup, scratch, "on")
        full = run(editor, project_with_bias(project, 1.0, scratch), "on", args.frames, args.warmup, scratch, "bias1")
        quarter = run(editor, project_with_bias(project, 0.25, scratch), "on", args.frames, args.warmup, scratch, "bias025")

    r_off, r_on = off["render"], on["render"]
    ratio = lambda a, b: a / b if b else 0.0
    drop = lambda a, b: 1.0 - b / a if a else 0.0
    results = {
        # the on run's counters (the same every frame of a still scene)
        "main_draw_calls": r_on["mainDrawCalls"],
        "shadow_draw_calls": r_on["shadowDrawCalls"],
        "depth_draw_calls": r_on["depthDrawCalls"],
        "main_triangles": r_on["mainTriangles"],
        "shadow_triangles": r_on["shadowTriangles"],
        "instanced_draws": r_on["instancedDraws"],
        "batched_instances": r_on["batchedInstances"],
        "visible": r_on["visible"],
        "culled": r_on["culled"],
        "uniform_bytes": r_on["uniformBytes"],
        "alive_buffers": r_on["aliveBuffers"],
        "alive_images": r_on["aliveImages"],
        # R9: off to on
        "main_draw_call_ratio": ratio(r_off["mainDrawCalls"], r_on["mainDrawCalls"]),
        "shadow_triangle_drop": drop(full["render"]["shadowTriangles"], r_on["shadowTriangles"]),
        "uniform_byte_drop": drop(r_off["uniformBytes"], r_on["uniformBytes"]),
        "render_cpu_ms_drop": drop(off["renderCpuMs"]["median"], on["renderCpuMs"]["median"]),
        # informational: times and the off run's counters
        "render_cpu_ms_off": off["renderCpuMs"]["median"],
        "render_cpu_ms_on": on["renderCpuMs"]["median"],
        "cpu_ms_off": off["cpuMs"]["median"],
        "cpu_ms_on": on["cpuMs"]["median"],
        "main_draw_calls_off": r_off["mainDrawCalls"],
        "shadow_draw_calls_off": r_off["shadowDrawCalls"],
        "shadow_triangles_off": r_off["shadowTriangles"],
        "shadow_triangles_bias_1": full["render"]["shadowTriangles"],
        "shadow_triangles_bias_025": quarter["render"]["shadowTriangles"],
        "main_triangles_bias_1": full["render"]["mainTriangles"],
        "uniform_bytes_off": r_off["uniformBytes"],
    }
    summary = {"bench": "mesh-bench", "results": results, "off": off, "on": on, "bias_1": full, "bias_025": quarter}
    print(json.dumps(results, indent=1))
    path = Path(args.json) if args.json else None
    if path:
        path.write_text(json.dumps(summary, indent=1) + "\n")
    if args.check:
        if not path:
            print("--check needs --json")
            return 2
        return subprocess.call([sys.executable, str(ROOT / "tools" / "perf" / "check_bench.py"), str(path)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
