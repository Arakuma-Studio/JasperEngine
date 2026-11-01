#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The render thread's share of shader and pipeline creation (docs/specs/S32-async-shaders.md
R21, R22, AC13), measured with the S15 profiler.

    uv run --no-project --with-requirements tools/mcp/requirements.txt \\
        python tools/perf/async_shaders_bench.py [--runs 5] [--toggles 10] [--json out.json]
    ... async_shaders_bench.py --mode sync            # the red control: creation on the render thread
    ... async_shaders_bench.py --editor build/linux/editor-vulkan-install/bin/jasperengine-editor
    ... async_shaders_bench.py --mode worker --warm-draws on   # GL warm draws (a hardware GL)

Each run opens a new project in a headless editor (MCP) with twelve lit spheres. It draws them until
nothing is pending, then for each toggle turns on a set of the built-in Lit's lobes on one sphere
that no sphere had before (a variant never built) and records frames with capture_profile (60 at a
time) until that sphere draws its new variant, and 10 more. For
every frame get_cpu_profile gives the frame time and the main thread's "GPU create submit",
"GPU create publish" and "GPU pipeline create" scopes (counted once where they nest). A run's
results:

  max_main_ms           the most creation work the render thread did in one frame
  worst_frame_ratio     the worst captured frame of the toggles over the median frame without one
  r22_worst_frame_ratio R22 (spec §7 AC13): the worst of every frame from the first toggle on (the
                        editor's frame-time ring read every 0.25 s, so the captures' gaps miss
                        nothing), the frames that ran an MCP tool (the edit, this bench's own calls)
                        apart; edit_worst_frame_ms, tool_worst_frame_ms and worker_worst_frame_ms
                        (the frames the worker created in) are reported beside it

Controls: --control (the same edits between variants built before), --idle (the same recording
with no edit: the editor's own frame-time noise) and --driver-cache DIR (the drivers' shader disk
caches on, so runs after the first start warm).

and the reported value is the median of the runs. Mesa's shader disk cache is off for the editor
(MESA_SHADER_CACHE_DISABLE), so every run compiles cold. The synchronous creation (--mode sync) is
timed with the same scopes. The budgets (tools/perf/bench-thresholds.json,
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
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mcp"))
from harness import Editor  # noqa: E402

sys.path.insert(0, str(ROOT / "tools" / "perf"))
import gpu_view  # noqa: E402

SCOPES = {"GPU create submit", "GPU create publish", "GPU pipeline create"}
LOBES = ["clearcoat", "sheen", "anisotropy", "iridescence", "specular", "transmission"]
SPHERES = 12
FRAMES_PER_TOGGLE = 60
TOGGLE_WINDOWS = 8
BASELINE_FRAMES = 120
# --idle: windows a "toggle" records (about a toggle's frames on a GPU)
IDLE_WINDOWS = 5


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


async def wait_idle(ed: Editor, timeout_seconds: float = 300.0) -> None:
    # (a wall-clock bound: at a GPU's 1 ms frames a frame count runs out long before a slow
    # creation, e.g. JASPER_ASYNC_SHADERS_DELAY_MS, is done)
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        stats = (await ed.call("get_render_stats"))["shader_creation"]
        if stats["pending_shaders"] == 0 and stats["pending_pipelines"] == 0 and stats["pending_warm_draws"] == 0:
            return
        await ed.call("wait", frames=10, timeout_seconds=900)
    raise RuntimeError("shader creation never went idle")


async def frames_of(ed: Editor, frames: int, during=None) -> list[dict]:
    """`frames` frames recorded with capture_profile; `during` (a coroutine) runs once the
    recording has started, so what it causes falls inside it"""
    recording = asyncio.ensure_future(ed.call("capture_profile", frames=frames, gpu=False))
    if during is not None:
        await ed.call("wait", frames=2, timeout_seconds=900)
        await during
    capture = await recording
    # (frames are numbered as the editor counts them: the capture's first is first_frame)
    first = int(capture.get("first_frame", 0))
    out = []
    for index in range(first, first + int(capture.get("frames", frames))):
        frame = await ed.call("get_cpu_profile", capture_id=capture["capture_id"], frame=index, view="tree",
                              thread="Main Thread", max_rows=400)
        rows = frame.get("rows", [])
        # what the creation worker did in that frame (its own lane; absent on a thread-less mode)
        worker = await ed.call_raw("get_cpu_profile", {"capture_id": capture["capture_id"], "frame": index, "view": "tree",
                                                        "thread": "GPU Create Worker", "max_rows": 50})
        value = getattr(worker, "value", None)
        worker_rows = value.get("rows", []) if not worker.is_error and isinstance(value, dict) else []
        worker_ms = sum(float(r.get("total_ms", 0.0)) for r in worker_rows
                        if r.get("name") == "GPU create worker" and int(r.get("depth", 0)) == 0)
        # the frame's heaviest scopes two levels down, for the report of the worst frame
        top = sorted((r for r in rows if r.get("depth", 0) <= 2), key=lambda r: -float(r.get("total_ms", 0.0)))[:6]
        out.append({"frame": index, "frame_ms": float(frame.get("frame_ms") or 0.0), "creation_ms": main_creation_ms(rows),
                    # the agent's edit ran in this frame (the "MCP tool" scope): the editor's cost of
                    # the edit, outside R22 (spec S32 §7 AC13); the worker created in this frame
                    "edit": any(r.get("name") == "MCP tool" for r in rows), "worker_ms": worker_ms,
                    "top": [(r.get("name"), round(float(r.get("total_ms", 0.0)), 1)) for r in top],
                    # the frame's scopes over 0.5 ms at any depth, in tree order (--tree prints the worst's)
                    "tree": [("  " * int(r.get("depth", 0)) + str(r.get("name")), round(float(r.get("total_ms", 0.0)), 2))
                             for r in rows if float(r.get("total_ms", 0.0)) >= 0.2 or str(r.get("name")).startswith("DBG")]})
    return out


async def poll_spikes(ed: Editor, threshold_ms: float, spikes: dict, stop: asyncio.Event) -> None:
    """every frame over threshold_ms, continuously (the editor's 600-frame ring, read every 0.25 s),
    {frame number: (frame ms, cpu ms, tool)}: the captures record windows with gaps between them,
    which missed NVIDIA's ~780 ms first-draw compiles in the GPU campaign (perf2). tool: the frame
    ran an MCP tool (this bench's own calls, the edit), whose time is not the creation's"""
    while not stop.is_set():
        r = await ed.call("get_frame_times", window=600, min_frame_ms=threshold_ms, frames=0)
        for spike in r.get("spikes", []) or []:
            spikes[int(spike.get("frame", 0))] = (float(spike.get("frame_ms", 0.0)), float(spike.get("cpu_ms", 0.0)),
                                                  bool(spike.get("tool", False)))
        try:
            await asyncio.wait_for(stop.wait(), 0.25)
        except asyncio.TimeoutError:
            pass


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
    # the drivers' shader disk caches off (Mesa's, NVIDIA's): every run compiles as a first start on
    # a user's machine does. --driver-cache keeps them on (their default), in a directory of this
    # bench's own: the first run fills it, the later ones start as a user's second start does
    if args.driver_cache:
        env = {"MESA_SHADER_CACHE_DIR": args.driver_cache, "__GL_SHADER_DISK_CACHE": "1",
               "__GL_SHADER_DISK_CACHE_PATH": args.driver_cache, "__GL_SHADER_DISK_CACHE_SKIP_CLEANUP": "1"}
    else:
        env = {"MESA_SHADER_CACHE_DISABLE": "true", "__GL_SHADER_DISK_CACHE": "0"}
    if args.mode:
        env["JASPER_ASYNC_SHADERS"] = args.mode
    # (absolute: the editor runs in its scratch directory)
    command = [str(Path(args.editor).resolve())] if args.editor else None
    # the editor's vsync off: on a GPU the idle frame is far under a refresh period, which would
    # otherwise pace it (and hide a spike of up to a period from R22)
    async with Editor(create=True, name=f"AsyncShaders{run}", env=env, command=command, label=f"async-shaders-{run}",
                      settings={"editor": {"vsync": False}}) as ed:
        if args.warm_draws is not None:
            await ed.call("set_project_settings", values={"shader_creation.gl_warm_draws": args.warm_draws == "on"})
        scene = await build_scene(ed)
        # focused, so a frame is not paced to the unfocused period (a GPU frame is far under it)
        await gpu_view.focus_editor(ed)
        await wait_idle(ed)
        baseline = await frames_of(ed, BASELINE_FRAMES)
        idle_median = statistics.median(f["frame_ms"] for f in baseline)
        idle_over = sorted((round(f["frame_ms"], 1) for f in baseline if f["frame_ms"] > 2 * idle_median), reverse=True)
        toggled: list[dict] = []
        spikes: dict[int, tuple[float, float, bool]] = {}
        if args.control:
            # the control: the same edits between two variants built before the measurement, so no
            # toggle creates anything (what a toggle costs besides the creation: the edit, the reload)
            for bits in (1, 2):
                pbr = {lobe: {"enabled": True} for i, lobe in enumerate(LOBES) if bits >> i & 1}
                await ed.call("set_properties", scene=scene, entity="Ball0", values={"mesh": {"submeshes[0].material": {"pbr": pbr}}})
                await ed.call("set_properties", scene=scene, entity="Ball1", values={"mesh": {"submeshes[0].material": {"pbr": pbr}}})
                await wait_idle(ed)
                await ed.call("wait", frames=30)
        # the frames before the toggles (the editor's start, the scene's build, the control's
        # variants) are not theirs
        start_frame = int((await ed.call("get_frame_times", window=1, frames=1))["newest"][-1]["frame"])
        for toggle in range(args.toggles):
            # lobe set toggle+1 (bits over LOBES) on sphere toggle % SPHERES: a variant no sphere had
            # (--control: lobe set 1 or 2, built already, on sphere 0)
            bits = toggle + 1 if not args.control else 2 - toggle % 2
            pbr = {lobe: {"enabled": True} for i, lobe in enumerate(LOBES) if bits >> i & 1}
            ball = f"Ball{toggle % SPHERES}" if not args.control else "Ball0"
            # --idle: the same recording with no edit at all (the editor's own frame-time noise
            # over as many frames: the floor under R22)
            change = (ed.call("set_properties", scene=scene, entity=ball, values={"mesh": {"submeshes[0].material": {"pbr": pbr}}})
                      if not args.idle else ed.call("wait", frames=1))
            # recorded from before the change until the sphere draws its new variant (its compile,
            # creation and first draw all inside the recording, in either mode), at most
            # TOGGLE_WINDOWS windows
            first_of_toggle = len(toggled)
            stop = asyncio.Event()
            poller = asyncio.ensure_future(poll_spikes(ed, 2.0 * idle_median, spikes, stop))
            for window in range(TOGGLE_WINDOWS):
                toggled.extend(await frames_of(ed, FRAMES_PER_TOGGLE, change if window == 0 else None))
                variant = await ed.call("get_material_variant", scene=scene, entity=ball)
                if args.idle and window < IDLE_WINDOWS - 1:
                    continue
                if args.idle or variant.get("ready") or variant.get("failed"):
                    toggled.extend(await frames_of(ed, 10))
                    break
            await wait_idle(ed)
            await ed.call("wait", frames=30)
            stop.set()
            await poller
            if args.tree:
                over = [(i - first_of_toggle, round(f["frame_ms"], 1)) for i, f in enumerate(toggled[first_of_toggle:], first_of_toggle)
                        if f["frame_ms"] > 2 * idle_median]
                print(f"  toggle {toggle}: {len(toggled) - first_of_toggle} frames, over 2x idle at (frame, ms) {over}", flush=True)
        stats = await ed.call("get_render_stats")
        mode = stats["shader_creation"]["mode"]
        result = {
            "mode": mode,
            "device": stats.get("gpu", {}).get("device", ""),
            "warm_draw_count": stats["shader_creation"].get("warm_draws", 0),
            "idle_median_ms": idle_median,
            "max_main_ms": max(f["creation_ms"] for f in toggled),
            "worst_frame_ms": max(f["frame_ms"] for f in toggled),
        }
        result["worst_frame_ratio"] = result["worst_frame_ms"] / idle_median if idle_median > 0 else 0.0
        # R22 as the spec measures it since perf2: the toggles' frames without the agent's edit (its
        # undo record, scene change and reload request are the editor's, not the creation's); the
        # edit frames and the frames the worker created in reported apart
        creation = [f for f in toggled if not f["edit"]]
        edit_frames = {f["frame"] for f in toggled if f["edit"]}
        spikes = {frame: spike for frame, spike in spikes.items() if frame > start_frame}
        continuous = [ms for frame, (ms, cpu, tool) in spikes.items() if frame not in edit_frames and not tool]
        tool_frames = [ms for frame, (ms, cpu, tool) in spikes.items() if tool]
        edits = [f for f in toggled if f["edit"]]
        working = [f for f in creation if f["worker_ms"] > 0.0]
        # R22: every frame of the toggles (continuous), the edit frames apart
        result["r22_worst_frame_ms"] = max([f["frame_ms"] for f in creation] + continuous, default=0.0)
        result["windowed_worst_frame_ms"] = max((f["frame_ms"] for f in creation), default=0.0)
        result["spikes"] = sorted(round(ms, 1) for ms in continuous)
        result["r22_worst_frame_ratio"] = result["r22_worst_frame_ms"] / idle_median if idle_median > 0 else 0.0
        result["edit_worst_frame_ms"] = max((f["frame_ms"] for f in edits), default=0.0)
        # the frames an MCP tool ran in (the edit, this bench's captures and queries): apart
        result["tool_worst_frame_ms"] = max(tool_frames, default=0.0)
        result["spike_detail"] = sorted(([frame, round(ms, 2), round(cpu, 2), tool] for frame, (ms, cpu, tool) in spikes.items()
                                         if not tool), key=lambda e: -e[1])[:40]
        result["edit_frames"] = len(edits)
        result["worker_frames"] = len(working)
        result["worker_worst_frame_ms"] = max((f["frame_ms"] for f in working), default=0.0)
        result["idle_frames_over_2x"] = idle_over
        worst = max(toggled, key=lambda f: f["frame_ms"])
        print(f"run {run}: {result['device']}, mode {mode}, warm draws done {result['warm_draw_count']}, idle median {idle_median:.1f} ms, max main creation {result['max_main_ms']:.2f} ms, "
              f"worst frame {result['worst_frame_ms']:.1f} ms ({result['worst_frame_ratio']:.2f}x), "
              f"{len(toggled)} toggle frames; worst frame's scopes {worst['top']}", flush=True)
        print(f"  R22 (edit frames apart): worst {result['r22_worst_frame_ms']:.1f} ms ({result['r22_worst_frame_ratio']:.2f}x); "
              f"{result['edit_frames']} edit frames, worst {result['edit_worst_frame_ms']:.1f} ms; "
              f"{result['worker_frames']} frames with the worker creating, worst {result['worker_worst_frame_ms']:.1f} ms", flush=True)
        if args.tree:
            print(f"  idle frames over 2x the idle median: {idle_over}", flush=True)
            for spike in sorted(toggled, key=lambda f: -f["frame_ms"])[:args.tree]:
                print(f"  frame {spike['frame_ms']:.1f} ms:\n" + "\n".join(f"    {n} {ms}" for n, ms in spike["tree"]), flush=True)
        return result


async def main_async(args: argparse.Namespace) -> int:
    runs = [await one_run(args, run) for run in range(args.runs)]
    mode = runs[0]["mode"]
    summary = {
        "bench": ("async-shaders" if mode != "sync" else "async-shaders-sync") + ("-control" if args.control else "") + ("-idle" if args.idle else ""),
        "mode": mode,
        "device": runs[0].get("device", ""),
        "warm_draws": args.warm_draws,
        "driver_cache": bool(args.driver_cache),
        "runs": runs,
        "results": {
            "max_main_ms": statistics.median(r["max_main_ms"] for r in runs),
            "worst_frame_ratio": statistics.median(r["worst_frame_ratio"] for r in runs),
            "r22_worst_frame_ratio": statistics.median(r["r22_worst_frame_ratio"] for r in runs),
            "edit_worst_frame_ms": statistics.median(r["edit_worst_frame_ms"] for r in runs),
            "tool_worst_frame_ms": statistics.median(r["tool_worst_frame_ms"] for r in runs),
            "worker_worst_frame_ms": statistics.median(r["worker_worst_frame_ms"] for r in runs),
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
    parser.add_argument("--warm-draws", choices=["on", "off"], default=None,
                        help="set the project's GL warm draws (shader_creation.gl_warm_draws); default: the project's")
    parser.add_argument("--idle", action="store_true",
                        help="record as the toggles do with no edit: the editor's frame-time noise (R22's floor)")
    parser.add_argument("--control", action="store_true",
                        help="toggle between two variants built beforehand: the edit's cost without any creation")
    parser.add_argument("--driver-cache", default="",
                        help="keep the drivers' shader disk caches on, in this (new) directory: runs after the first are warm")
    parser.add_argument("--tree", type=int, default=0, help="print the scope trees of the N worst toggle frames")
    parser.add_argument("--json", default="")
    parser.add_argument("--check", action="store_true", help="check the median against tools/perf/bench-thresholds.json")
    args = parser.parse_args()
    if args.driver_cache:
        args.driver_cache = str(Path(args.driver_cache).resolve())
        Path(args.driver_cache).mkdir(parents=True, exist_ok=True)
    if args.toggles >= 2 ** len(LOBES):
        parser.error("at most 63 toggles (lobe sets)")
    return asyncio.run(main_async(args))


if __name__ == "__main__":
    sys.exit(main())
