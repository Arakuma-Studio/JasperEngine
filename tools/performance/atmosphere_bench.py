#!/usr/bin/env python3
"""The atmosphere's performance gate (docs/specs/S12-atmosphere.md R18, R19, AC10).

    uv run --no-project --with-requirements tools/mcp/requirements.txt \\
        python tools/performance/atmosphere_bench.py [--width 1920 --height 1080] [--frames 300] [--json out.json]
    ... atmosphere_bench.py --mobile          # 1280x720, the mobile tier (Low)
    ... atmosphere_bench.py --make-fixture    # (re)write tools/fixtures/atmosphere/bench.scene
    ... atmosphere_bench.py --runs 3 --relative --json out.json   # a software-rendering machine

Opens a copy of tools/fixtures/atmosphere/ in the editor (MCP), plays bench.scene at the given
render size without vsync, and for each configuration (nothing, linear fog, height fog, height
fog with sky fill, volumetric fog and clouds at each tier) measures the median frame time over
`frames` frames, the passes get_atmosphere_info reports and their memory. While the S15 pass
timings are not in the engine, a feature's cost is its frame time above the configuration
without it (run on an otherwise idle reference machine: GTX 1660 / RX 5500 class desktop, Adreno
650 / Mali-G78 class phone); with them each pass's own GPU time is read too (get_atmosphere_info
passes[].gpuMs). The budgets are checked and the table printed for docs/mcp/guide/atmosphere.md.

--runs N measures N times, each in a new editor, and takes each configuration's median.
--relative is the method for a machine whose GPU is not the reference class (the benchmark server
renders in software, llvmpipe): there a feature's absolute milliseconds say nothing about a GPU,
so its cost is taken relative to the frame without any atmosphere on the same machine,
(frame - baseline frame) / frame("none"), and checked against the budget's share of the
reference frame, budget / 16.67 ms: the reference GPU is taken to draw the bench scene without
atmosphere in the 60 fps frame the budgets are written for. The Game view renders at the editor
window's size, so VRAM is scaled to 1080p by the pixel ratio (the noise volumes, which do not
scale, are scaled too: conservative).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import re
import shutil
import statistics
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mcp"))
from harness import Editor  # noqa: E402

FIXTURE = ROOT / "tools" / "fixtures" / "atmosphere"
SCENE = "bench.scene"

# R18/R19 (desktop at 1080p; mobile at 720p): ms of GPU per feature, MB of memory per tier
BUDGETS_MS = {
    "desktop": {"height_fog": 0.10, "sky_composite": 0.15, "volumetric_medium": 1.0, "volumetric_high": 1.8,
                "clouds_medium": 0.8, "clouds_high": 2.5},
    "mobile": {"height_fog": 0.3, "volumetric_low": 1.5, "clouds_low": 0.6},
}
BUDGETS_MB = {"low": 4, "medium": 16, "high": 24}
# --relative: the reference frame the budgets are a share of (60 fps)
REFERENCE_FRAME_MS = 1000.0 / 60.0


async def make_fixture(ed: Editor) -> None:
    """The bench scene: a terrain-like ground, rows of buildings, a sun with shadows, a height fog,
    three Local Volumetric Fog volumes and clouds (switched by the measurement)."""
    made = await ed.call("create_scene", name="AtmosphereBench", type="3D", path="bench")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    ground = await ed.call("create_entity", scene=scene, type="plane", name="Ground")
    await ed.call("set_transform", scene=scene, entity=ground["entity_id"], scale=[200, 1, 200])
    for row in range(6):
        for column in range(6):
            box = await ed.call("create_entity", scene=scene, type="box", name=f"Building{row}{column}")
            height = 4 + ((row * 7 + column * 3) % 9) * 3
            await ed.call("set_transform", scene=scene, entity=box["entity_id"],
                          position=[-60 + column * 24, height / 2, -20 - row * 40], scale=[8, height, 8])
    # the new 3D scene's sun
    sun = (await ed.call("find_entities", scene=scene, component="light"))["entities"][0]
    await ed.call("set_properties", scene=scene, entity=sun["id"],
                  values={"light": {"direction": [-0.4, -0.35, -1.0], "intensity": 2.0, "shadows": True}})
    fog = await ed.call("create_entity", scene=scene, type="fog", name="Fog")
    await ed.call("apply_atmosphere_preset", scene=scene, entity=fog["entity_id"], preset="mountain_valley")
    for i in range(3):
        volume = await ed.call("create_entity", scene=scene, type="local_volumetric_fog", name=f"Volume{i}")
        await ed.call("set_transform", scene=scene, entity=volume["entity_id"], position=[-20 + i * 20, 3, -30 - i * 15],
                      scale=[3, 1, 3])
    await ed.call("create_entity", scene=scene, type="volumetric_clouds", name="Clouds")
    camera = await ed.call("create_entity", scene=scene, type="camera", name="Main Camera")
    await ed.call("set_properties", scene=scene, entity=camera["entity_id"],
                  values={"camera": {"type": "perspective", "target": [0, 8, -200], "farClip": 5000}})
    await ed.call("set_transform", scene=scene, entity=camera["entity_id"], position=[0, 10, 30])
    await ed.call("set_main_camera", scene=scene, entity=camera["entity_id"])
    await ed.call("save_scene", scene=scene)


CONFIGS = [
    # (name, fog values or None for no fog, clouds on, tier)
    ("none", None, False, "high"),
    ("linear_fog", {"type": "linear"}, False, "high"),
    ("height_fog", {"type": "height", "skyFill": False, "volumetric": False}, False, "high"),
    ("sky_composite", {"type": "height", "skyFill": True, "volumetric": False}, False, "high"),
    ("volumetric_low", {"type": "height", "skyFill": True, "volumetric": True}, False, "low"),
    ("volumetric_medium", {"type": "height", "skyFill": True, "volumetric": True}, False, "medium"),
    ("volumetric_high", {"type": "height", "skyFill": True, "volumetric": True}, False, "high"),
    ("clouds_low", {"type": "height", "skyFill": True, "volumetric": False}, True, "low"),
    ("clouds_medium", {"type": "height", "skyFill": True, "volumetric": False}, True, "medium"),
    ("clouds_high", {"type": "height", "skyFill": True, "volumetric": False}, True, "high"),
]
# the configuration each feature is measured against
BASELINE = {"height_fog": "linear_fog", "sky_composite": "height_fog", "volumetric_low": "sky_composite",
            "volumetric_medium": "sky_composite", "volumetric_high": "sky_composite", "clouds_low": "sky_composite",
            "clouds_medium": "sky_composite", "clouds_high": "sky_composite"}


async def render_size(ed: Editor) -> list[int]:
    """The Game view's render size (its screenshot's source size)."""
    r = await ed.call_raw("screenshot", {"view": "game"})
    m = re.search(r"screenshot (\d+)x(\d+)(?: \(scaled from (\d+)x(\d+)\))?", r.text)
    if not m:
        return [0, 0]
    return [int(m.group(3)), int(m.group(4))] if m.group(3) else [int(m.group(1)), int(m.group(2))]


async def measure(ed: Editor, frames: int) -> dict:
    results = {}
    scene = (await ed.call("open_scene", path=SCENE))["scene"]["id"]
    for name, fog, clouds, tier in CONFIGS:
        await ed.call("set_properties", scene=scene, entity="Fog",
                      values={"fog": fog if fog else {"type": "linear", "density": 0.0, "linearStart": 1e6, "linearEnd": 2e6}})
        await ed.call("configure_game_view", atmosphere_quality=tier)
        await ed.call("play", action="start", scene=scene)
        try:
            await ed.call("eval_lua", code=f"local e = scene:findEntity('Clouds'); scene:setEntityActive(e, {str(clouds).lower()})")
            await ed.call("wait", frames=60)  # the noise bake, the Sky Map, the shader cache
            times = []
            for _ in range(max(frames // 30, 1)):
                start = time.perf_counter()
                await ed.call("wait", frames=30)
                times.append((time.perf_counter() - start) * 1000.0 / 30.0)
            info = await ed.call("get_atmosphere_info", view="game")
            size = await render_size(ed)
        finally:
            await ed.call("play", action="stop")
        results[name] = {"frame_ms": statistics.median(times), "passes": info["passes"], "vram_mb": info["vramBytes"] / 2 ** 20,
                         "tier": tier, "render_size": size}
        print(f"{name:18s} {results[name]['frame_ms']:7.3f} ms  {results[name]['vram_mb']:6.2f} MB  "
              f"{', '.join(p['label'] for p in info['passes'])}")
    return results


def vram_at_1080p(result: dict, platform: str) -> float:
    w, h = result.get("render_size", [0, 0])
    target = (1280 * 720) if platform == "mobile" else (1920 * 1080)
    return result["vram_mb"] * (target / (w * h)) if w and h else result["vram_mb"]


def report(results: dict, platform: str, relative: bool = False) -> bool:
    ok = True
    if relative:
        print(f"\nrelative method: cost = (frame - baseline) / frame(none) = {results['none']['frame_ms']:.2f} ms; "
              f"budget share = budget / {REFERENCE_FRAME_MS:.2f} ms; render size {results['none'].get('render_size')}")
        print("\n| Feature | Frame (ms) | Baseline | Cost (ms) | Cost share | Budget (ms) | Budget share | VRAM@res (MB) | Budget (MB) |"
              "\n|---|---|---|---|---|---|---|---|---|")
    else:
        print("\n| Feature | Cost (ms) | Budget (ms) | VRAM (MB) | Budget (MB) |\n|---|---|---|---|---|")
    for feature, budget in BUDGETS_MS[platform].items():
        if feature not in results:
            continue
        cost = results[feature]["frame_ms"] - results[BASELINE[feature]]["frame_ms"]
        gpu = [p["gpuMs"] for p in results[feature]["passes"] if "gpuMs" in p]
        if gpu and not relative:
            cost = sum(gpu)
        vram = vram_at_1080p(results[feature], platform)
        vram_budget = BUDGETS_MB.get(results[feature]["tier"], 0)
        if relative:
            share = cost / results["none"]["frame_ms"]
            budget_share = budget / REFERENCE_FRAME_MS
            passed = share <= budget_share and vram <= vram_budget
            print(f"| {feature} | {results[feature]['frame_ms']:.2f} | {BASELINE[feature]} | {cost:.2f} | {share * 100:.1f}% | "
                  f"{budget} | {budget_share * 100:.1f}% | {vram:.1f} | {vram_budget} |{'' if passed else ' over budget'}")
        else:
            passed = cost <= budget and vram <= vram_budget
            print(f"| {feature} | {cost:.3f} | {budget} | {vram:.1f} | {vram_budget} |{'' if passed else ' over budget'}")
        ok = ok and passed
    return ok


def median_of_runs(runs: list[dict]) -> dict:
    merged = {}
    for name in runs[0]:
        merged[name] = dict(runs[0][name])
        merged[name]["frame_ms"] = statistics.median(r[name]["frame_ms"] for r in runs)
        merged[name]["runs_ms"] = [r[name]["frame_ms"] for r in runs]
        merged[name]["vram_mb"] = max(r[name]["vram_mb"] for r in runs)
    return merged


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--make-fixture", action="store_true")
    ap.add_argument("--mobile", action="store_true", help="1280x720 and the mobile budgets")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--json", help="also write the measurements here")
    ap.add_argument("--runs", type=int, default=1, help="measure this many times (a new editor each) and take the medians")
    ap.add_argument("--relative", action="store_true",
                    help="cost relative to the no-atmosphere frame on this machine (a software renderer)")
    args = ap.parse_args()

    if args.make_fixture:
        async with Editor(create=True, name="AtmosphereBench") as ed:
            await make_fixture(ed)
            project = Path(ed.project)
            FIXTURE.mkdir(parents=True, exist_ok=True)
            shutil.copy(project / "project.yaml", FIXTURE / "project.yaml")
            shutil.copy(project / SCENE, FIXTURE / SCENE)
        print(f"wrote {FIXTURE / SCENE}")
        return 0

    width, height = (1280, 720) if args.mobile else (args.width, args.height)
    runs = []
    for run in range(max(args.runs, 1)):
        print(f"== run {run + 1}/{max(args.runs, 1)}")
        with tempfile.TemporaryDirectory() as tmp:
            project = Path(tmp) / "atmosphere"
            shutil.copytree(FIXTURE, project)
            async with Editor(project=project) as ed:
                await ed.call("set_project_settings", values={"quality.vsync": False, "graphics.canvas_width": width,
                                                                "graphics.canvas_height": height})
                await ed.call("configure_game_view", aspect="project", maximize_on_play=False)
                runs.append(await measure(ed, args.frames))
    results = median_of_runs(runs)
    platform = "mobile" if args.mobile else "desktop"
    ok = report(results, platform, args.relative)
    if args.json:
        Path(args.json).write_text(json.dumps({"platform": platform, "relative": args.relative, "runs": len(runs),
                                               "frames": args.frames, "passed": ok, "results": results}, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
