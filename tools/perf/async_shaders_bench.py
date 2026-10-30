#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The render thread's share of shader and pipeline creation (docs/specs/S32-async-shaders.md
R21, R22, AC13), measured with the S15 profiler.

    uv run --no-project --with-requirements tools/mcp/requirements.txt \\
        python tools/perf/async_shaders_bench.py [--runs 5] [--toggles 10] [--json out.json]
    ... async_shaders_bench.py --mode sync            # the red control: creation on the render thread
    ... async_shaders_bench.py --editor build/linux/editor-vulkan-install/bin/jasperengine-editor

Each run opens a new project in a headless editor (MCP) with twelve lit spheres. It draws them until
nothing is pending, then for each toggle turns on a set of the built-in Lit's lobes on one sphere
that no sphere had before (a variant never built) and records 60 frames with capture_profile. For
every frame get_cpu_profile gives the frame time and the main thread's "GPU create submit",
"GPU create publish" and "GPU pipeline create" scopes (counted once where they nest). A run's
results:

  max_main_ms           the most creation work the render thread did in one frame
  worst_frame_ratio     the worst frame of the toggles over the median frame without one

and the reported value is the median of the runs. The budgets (tools/perf/bench-thresholds.json,
bench "async-shaders") are checked when --check is given. --mode sets JASPER_ASYNC_SHADERS for the
editor (worker, parallel, deferred, sync; empty: the platform's choice). The bench name carries the
mode, so the sync control is never checked against the async budgets.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mcp"))
from harness import Editor  # noqa: E402

SCOPES = {"GPU create submit", "GPU create publish", "GPU pipeline create"}
LOBES = ["clearcoat", "sheen", "anisotropy", "iridescence", "specular", "transmission"]
SPHERES = 12
FRAMES_PER_TOGGLE = 60
BASELINE_FRAMES = 120


def main_creation_ms(rows: list[dict]) -> float:
    """The creation scopes of one frame's main-thread tree, a nested one not counted again."""
    total = 0.0
    inside = None   # the depth of the scope being counted
    for row in rows:
        depth = row.get("depth", 0)
        if inside is not None and depth <= inside:
            inside = None
        if inside is None and row.get("name") in SCOPES:
            total += float(row.get("total_ms", 0.0))
            inside = depth
    return total


async def wait_idle(ed: Editor, timeout_frames: int = 3000) -> None:
    for _ in range(timeout_frames // 10):
        stats = (await ed.call("get_render_stats"))["shader_creation"]
        if stats["pending_shaders"] == 0 and stats["pending_pipelines"] == 0 and stats["pending_warm_draws"] == 0:
            return
        await ed.call("wait", frames=10, timeout_seconds=900)
    raise RuntimeError("shader creation never went idle")


async def frames_of(ed: Editor, frames: int) -> list[dict]:
    capture = await ed.call("capture_profile", frames=frames, gpu=False)
    # (frames are numbered as the editor counts them: the capture's first is first_frame)
    first = int(capture.get("first_frame", 0))
    out = []
    for index in range(first, first + int(capture.get("frames", frames))):
        frame = await ed.call("get_cpu_profile", capture_id=capture["capture_id"], frame=index, view="tree",
                              thread="Main Thread", filter="GPU", max_rows=200)
        out.append({"frame_ms": float(frame.get("frame_ms") or 0.0), "creation_ms": main_creation_ms(frame.get("rows", []))})
    return out


async def build_scene(ed: Editor) -> int:
    made = await ed.call("create_scene", name="AsyncShaders", type="3D", path="async_shaders")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    await ed.call("set_scene_settings", scene=scene, settings={"lit_quality": "standard"})
    for i in range(SPHERES):
        ball = await ed.call("create_entity", scene=scene, type="sphere", name=f"Ball{i}")
        await ed.call("set_transform", scene=scene, entity=ball["entity_id"],
                      position=[(i % 4) * 1.4 - 2.1, (i // 4) * 1.4 - 1.4, 0.0])
    cameras = (await ed.call("find_entities", scene=scene, component="camera"))["entities"]
    if cameras:
        await ed.call("set_transform", scene=scene, entity=cameras[0]["id"], position=[0.0, 0.0, 7.0])
    await ed.call("save_scene", scene=scene)
    return scene


async def one_run(args: argparse.Namespace, run: int) -> dict:
    env = {"JASPER_ASYNC_SHADERS": args.mode} if args.mode else {}
    command = [args.editor] if args.editor else None
    async with Editor(create=True, name=f"AsyncShaders{run}", env=env, command=command, label=f"async-shaders-{run}") as ed:
        scene = await build_scene(ed)
        await wait_idle(ed)
        baseline = await frames_of(ed, BASELINE_FRAMES)
        idle_median = statistics.median(f["frame_ms"] for f in baseline)
        toggled: list[dict] = []
        for toggle in range(args.toggles):
            # lobe set toggle+1 (bits over LOBES) on sphere toggle % SPHERES: a variant no sphere had
            bits = toggle + 1
            pbr = {lobe: {"enabled": True} for i, lobe in enumerate(LOBES) if bits >> i & 1}
            await ed.call("set_properties", scene=scene, entity=f"Ball{toggle % SPHERES}",
                          values={"mesh": {"submeshes[0].material": {"pbr": pbr}}})
            toggled.extend(await frames_of(ed, FRAMES_PER_TOGGLE))
            await wait_idle(ed)
        mode = (await ed.call("get_render_stats"))["shader_creation"]["mode"]
        result = {
            "mode": mode,
            "idle_median_ms": idle_median,
            "max_main_ms": max(f["creation_ms"] for f in toggled),
            "worst_frame_ms": max(f["frame_ms"] for f in toggled),
        }
        result["worst_frame_ratio"] = result["worst_frame_ms"] / idle_median if idle_median > 0 else 0.0
        print(f"run {run}: mode {mode}, idle median {idle_median:.1f} ms, max main creation {result['max_main_ms']:.2f} ms, "
              f"worst frame {result['worst_frame_ms']:.1f} ms ({result['worst_frame_ratio']:.2f}x)", flush=True)
        return result


async def main_async(args: argparse.Namespace) -> int:
    runs = [await one_run(args, run) for run in range(args.runs)]
    mode = runs[0]["mode"]
    summary = {
        "bench": "async-shaders" if mode != "sync" else "async-shaders-sync",
        "mode": mode,
        "runs": runs,
        "results": {
            "max_main_ms": statistics.median(r["max_main_ms"] for r in runs),
            "worst_frame_ratio": statistics.median(r["worst_frame_ratio"] for r in runs),
            "idle_median_ms": statistics.median(r["idle_median_ms"] for r in runs),
        },
    }
    print(json.dumps(summary["results"], indent=1))
    if args.json:
        Path(args.json).write_text(json.dumps(summary, indent=1) + "\n")
    if args.check and args.json and mode != "sync":
        return subprocess.call([sys.executable, str(ROOT / "tools" / "perf" / "check_bench.py"), args.json])
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--toggles", type=int, default=10)
    parser.add_argument("--mode", default="", help="JASPER_ASYNC_SHADERS for the editor")
    parser.add_argument("--editor", default="", help="editor binary (default: the harness's)")
    parser.add_argument("--json", default="")
    parser.add_argument("--check", action="store_true", help="check the median against tools/perf/bench-thresholds.json")
    args = parser.parse_args()
    if args.toggles >= 2 ** len(LOBES):
        parser.error("at most 63 toggles (lobe sets)")
    return asyncio.run(main_async(args))


if __name__ == "__main__":
    sys.exit(main())
