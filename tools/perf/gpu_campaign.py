#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The real-GPU budgets of the rendering streams, timed with the S15 GPU pass timers
(docs/perf/gpu-campaign.md). Each case plays a scene in a headless editor (MCP), sizes the Game
view to the case's resolution (gpu_view.fit_view), lets shader creation go idle, records `--frames`
frames with GPU timing and reads every pass label's median GPU ms per frame (get_gpu_passes). Each
run opens new editors; the reported value is the median of the runs.

    uv run --no-project --with-requirements tools/mcp/requirements.txt \\
        python tools/perf/gpu_campaign.py shadows   [--runs 5] [--json out.json]   # S10 AC14
    ... gpu_campaign.py pbr --project build/pbr-extended                       # S11 AC11 (R13)
    ... gpu_campaign.py post                                                   # S13 presets (desktop)
    ... gpu_campaign.py particles                                              # S20 R23
    ... --editor build/linux/editor-vulkan-install/bin/jasperengine-editor     # another build

On a GPU host: tools/gpu/xorg-headless.sh start, then inside the image `. tools/gpu/env.sh` and
JASPER_XVFB=none (the editor on the headless NVIDIA X server; Vulkan needs only env.sh).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mcp"))
sys.path.insert(0, str(ROOT / "tools" / "perf"))
from harness import Editor  # noqa: E402
import gpu_view  # noqa: E402

SHADOW_PASSES = ("shadow", "point shadow", "shadow static", "point shadow static")


async def settle(ed: Editor, frames: int = 60) -> None:
    """wait until nothing is pending in shader creation, then `frames` more"""
    await ed.call("wait", frames=frames, timeout_seconds=900)
    for _ in range(300):
        stats = (await ed.call("get_render_stats"))["shader_creation"]
        if stats["pending_shaders"] == 0 and stats["pending_pipelines"] == 0 and stats["pending_warm_draws"] == 0:
            break
        await ed.call("wait", frames=10, timeout_seconds=900)
    await ed.call("wait", frames=frames, timeout_seconds=900)


async def play_and_time(ed: Editor, scene: str, size: tuple[int, int], frames: int, warmup: int = 120) -> dict:
    """{label: GPU ms} of the Game view playing `scene` at `size`, plus _frame, _frames, _size"""
    sid = (await ed.call("open_scene", scene=scene))["scene"]["id"]
    await ed.call("play", action="start", scene=sid)
    try:
        await settle(ed, warmup)
        got = await gpu_view.fit_view(ed, *size)
        await ed.call("wait", frames=30)
        out = await gpu_view.gpu_passes(ed, frames)
        out["_size"] = list(got)
    finally:
        await ed.call("play", action="stop")
    return out


def editor_args(args: argparse.Namespace) -> dict:
    # the editor's own vsync off too (a GPU frame is far under a refresh period)
    out = {"settings": {"editor": {"vsync": False}}}
    if args.editor:
        out["command"] = [str(Path(args.editor).resolve())]
    return out


async def prepare(ed: Editor) -> dict:
    await ed.call("set_project_settings", values={"quality.vsync": False})
    await ed.call("configure_game_view", maximize_on_play=True, aspect="free")
    return await gpu_view.device(ed)


def merge(runs: list[dict]) -> dict:
    """the median over runs of every case's every label"""
    out = {}
    for case in runs[0]:
        labels = {label for r in runs for label in r[case] if not label.startswith("_") or label == "_frame"}
        out[case] = {label: statistics.median(r[case].get(label, 0.0) for r in runs) for label in sorted(labels)}
        out[case]["_size"] = runs[0][case].get("_size")
        out[case]["_runs_frame"] = [r[case].get("_frame", 0.0) for r in runs]
    return out


def shadow_ms(case: dict) -> float:
    return sum(ms for label, ms in case.items() if label in SHADOW_PASSES)


# ------------------------------------------------------------------ S10 shadows (AC14)

TIERS = {"low": (1920, 1080), "medium": (1920, 1080), "high": (2560, 1440), "ultra": (2560, 1440)}
# spec S10 §5: receiver, static cached, walking + 20 dynamic (ms)
SHADOW_BUDGETS = {"low": (1.0, 0.05, 1.5), "medium": (1.5, 0.05, 2.0), "high": (0.6, 0.05, 1.0), "ultra": (1.2, 0.05, 1.5)}
SHADOW_CASES = {
    "static": {"SHADOWS_DYNAMIC": "0"},
    "walking": {"SHADOWS_WALK": "1"},
    "receiver_soft": {"SHADOWS_DYNAMIC": "0", "SHADOWS_LIGHTS": "sun", "SHADOWS_SUN": "soft"},
    "receiver_none": {"SHADOWS_DYNAMIC": "0", "SHADOWS_LIGHTS": "sun", "SHADOWS_SUN": "none"},
}


async def shadows_run(args: argparse.Namespace, run: int) -> tuple[dict, dict]:
    out, dev = {}, {}
    for tier in args.tiers.split(","):
        for case, env in SHADOW_CASES.items():
            env = dict(env, SHADOWS_QUALITY=tier)
            async with Editor(project=ROOT / "tools" / "fixtures" / "shadows", env=env, label=f"gpu-shadows-{tier}-{case}-{run}",
                              **editor_args(args)) as ed:
                dev = await prepare(ed)
                out[f"{tier}/{case}"] = got = await play_and_time(ed, "Main Scene.scene", TIERS[tier], args.frames)
                print(f"run {run} {tier}/{case}: frame {got['_frame']:.3f} ms, shadow passes {shadow_ms(got):.3f} ms "
                      f"at {got['_size']}", flush=True)
    return out, dev


def shadows_report(results: dict) -> list[str]:
    rows = ["| Tier | Resolution | Receiver, sun soft (ms) | Budget | Static, cached (ms) | Budget | Walking + 20 dynamic (ms) | Budget |",
            "|---|---|---|---|---|---|---|---|"]
    for tier, (rb, sb, wb) in SHADOW_BUDGETS.items():
        if f"{tier}/static" not in results:
            continue
        soft, none = results[f"{tier}/receiver_soft"], results[f"{tier}/receiver_none"]
        receiver = (soft["_frame"] - shadow_ms(soft)) - (none["_frame"] - shadow_ms(none))
        static = shadow_ms(results[f"{tier}/static"])
        walking = shadow_ms(results[f"{tier}/walking"])
        size = results[f"{tier}/static"]["_size"]
        mark = lambda v, b: f"{v:.3f}" + ("" if v <= b else " **over**")  # noqa: E731
        rows.append(f"| {tier} | {size[0]}x{size[1]} | {mark(receiver, rb)} | {rb} | {mark(static, sb)} | {sb} | {mark(walking, wb)} | {wb} |")
    return rows


# ------------------------------------------------------------------ S11 lobes (AC11, R13)

LOBE_BUDGETS = {"Lobes Off": 0.02, "Lobe Clearcoat": 0.25, "Lobe Sheen": 0.15, "Lobe Anisotropy": 0.15,
                "Lobe Iridescence": 0.30, "Lobe Specular": 0.03, "Lobe Transmission": 0.20}


async def pbr_run(args: argparse.Namespace, run: int) -> tuple[dict, dict]:
    out, dev = {}, {}
    async with Editor(project=Path(args.project).resolve(), label=f"gpu-pbr-{run}", **editor_args(args)) as ed:
        dev = await prepare(ed)
        for quality in ("standard", "high"):
            for scene in ["Lit Baseline", *LOBE_BUDGETS, "Lobe Hex", "Terrain Baseline", "Hex Terrain"]:
                sid = (await ed.call("open_scene", scene=f"{scene}.scene"))["scene"]["id"]
                await ed.call("set_scene_settings", scene=sid, settings={"lit_quality": quality})
                got = await play_and_time(ed, f"{scene}.scene", (1920, 1080), args.frames)
                out[f"{quality}/{scene}"] = got
                print(f"run {run} {quality}/{scene}: frame {got['_frame']:.3f} ms at {got['_size']}", flush=True)
    return out, dev


def pbr_report(results: dict) -> list[str]:
    rows = ["| Scene | GPU frame, Standard (ms) | vs Lit Baseline | Budget | GPU frame, High (ms) | High / Standard |", "|---|---|---|---|---|---|"]
    base = results.get("standard/Lit Baseline", {}).get("_frame", 0.0)
    for key in [k for k in results if k.startswith("standard/")]:
        scene = key.split("/", 1)[1]
        std = results[key]["_frame"]
        high = results.get(f"high/{scene}", {}).get("_frame", 0.0)
        ref = results["standard/Terrain Baseline"]["_frame"] if scene == "Hex Terrain" else base
        ratio = std / ref - 1.0 if ref > 0 else 0.0
        budget = LOBE_BUDGETS.get(scene, 0.35 if scene == "Hex Terrain" else None)
        verdict = "" if budget is None or ratio <= budget else " **over**"
        rows.append(f"| {scene} | {std:.3f} | {ratio * 100:+.1f}%{verdict} | {'' if budget is None else f'+{budget * 100:.0f}%'} | "
                    f"{high:.3f} | {(high / std if std else 0):.3f} |")
    return rows


# ------------------------------------------------------------------ S13 post presets (desktop)

POST_PRESETS = ["none", "mobile", "console", "film", "vhs"]


async def post_run(args: argparse.Namespace, run: int) -> tuple[dict, dict]:
    out, dev = {}, {}
    async with Editor(project=ROOT / "tools" / "fixtures" / "post-processing", label=f"gpu-post-{run}", **editor_args(args)) as ed:
        dev = await prepare(ed)
        sid = (await ed.call("open_scene", scene="Main Scene.scene"))["scene"]["id"]
        for preset in POST_PRESETS:
            await ed.call("apply_post_processing_preset", scene=sid, preset=preset)
            got = await play_and_time(ed, "Main Scene.scene", (1920, 1080), args.frames)
            out[preset] = got
            print(f"run {run} post {preset}: frame {got['_frame']:.3f} ms at {got['_size']}", flush=True)
    return out, dev


def post_report(results: dict) -> list[str]:
    rows = ["| Preset | GPU frame (ms) | Over `mobile` (ms) | R14 phone budget over `mobile` |", "|---|---|---|---|"]
    mobile = results["mobile"]["_frame"]
    for preset in POST_PRESETS:
        frame = results[preset]["_frame"]
        budget = "≤ 1.0 (Mali-G57 / Adreno 610: pending a phone)" if preset in ("film", "vhs") else ""
        rows.append(f"| {preset} | {frame:.3f} | {frame - mobile:+.3f} | {budget} |")
    return rows


# ------------------------------------------------------------------ S20 particles (R23)

async def particles_run(args: argparse.Namespace, run: int) -> tuple[dict, dict]:
    """100 k live reference-stack particles (50 k/s, 1.5-2.5 s lives), each at most 16 px wide at 1080p"""
    out, dev = {}, {}
    async with Editor(create=True, name=f"GpuParticles{run}", label=f"gpu-particles-{run}", **editor_args(args)) as ed:
        dev = await prepare(ed)
        await ed.call("tool_groups", enable=["particles"])
        made = await ed.call("create_scene", name="Particles", type="3D", path="particles")
        sid = made["scene"]["id"]
        await ed.call("select_scene", scene=sid)
        for name, count in (("empty", 0), ("ref_100k", 100_000)):
            if count:
                fx = await ed.call("create_particles", scene=sid, name="Ref", preset="fire", position=[0, 0, 0])
                await ed.call("edit_particle_effect", scene=sid, entity=fx["entity_id"], patch=json.loads(PARTICLE_PATCH))
            cameras = (await ed.call("find_entities", scene=sid, component="camera"))["entities"]
            if cameras:
                await ed.call("set_transform", scene=sid, entity=cameras[0]["id"], position=[0.0, 3.0, 14.0])
            await ed.call("save_scene", scene=sid)
            got = await play_and_time(ed, "particles.scene", (1920, 1080), args.frames, warmup=240)
            got["_live"] = (await ed.call("get_render_stats", view="game")).get("instances", 0)
            out[name] = got
            print(f"run {run} particles {name}: frame {got['_frame']:.3f} ms at {got['_size']}", flush=True)
    return out, dev


# the reference stack of the budgets (engine/tests/core/bench/ParticleReferenceStack.h) at 100 k live
PARTICLE_PATCH = json.dumps({
    "maxParticles": 131072, "duration": 5, "looping": True, "simulationSpace": "world",
    "modules": {
        "main": {"startLifetime": {"mode": "twoConstants", "constant": 1.5, "constantMax": 2.5},
                 "startSpeed": {"mode": "twoConstants", "constant": 2, "constantMax": 4},
                 "startSize": {"mode": "twoConstants", "constant": 0.05, "constantMax": 0.15}, "gravityModifier": 0.3},
        "emission": {"rateOverTime": 50000},
    }})


def particles_report(results: dict) -> list[str]:
    cost = sum(ms for label, ms in results["ref_100k"].items() if not label.startswith("_")) - \
        sum(ms for label, ms in results["empty"].items() if not label.startswith("_"))
    return ["| Case | GPU (ms) | Budget |", "|---|---|---|",
            f"| 100 k reference particles at 1080p (passes over the empty scene) | {cost:.3f} | ≤ 1.5 |"]


CASES = {"shadows": (shadows_run, shadows_report), "pbr": (pbr_run, pbr_report), "post": (post_run, post_report),
         "particles": (particles_run, particles_report)}


async def main_async(args: argparse.Namespace) -> int:
    run_one, report = CASES[args.case]
    runs, dev = [], {}
    for run in range(args.runs):
        got, dev = await run_one(args, run)
        runs.append(got)
    results = merge(runs)
    print(f"\ndevice: {dev}, {args.runs} runs, {args.frames} frames each, medians")
    rows = report(results)
    print("\n".join(rows))
    if args.json:
        Path(args.json).write_text(json.dumps({"case": args.case, "device": dev, "runs": args.runs, "frames": args.frames,
                                               "results": results, "table": rows, "raw": runs}, indent=1) + "\n")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("case", choices=sorted(CASES))
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--tiers", default="low,medium,high,ultra", help="shadows: the tiers")
    parser.add_argument("--project", default=str(ROOT / "build" / "pbr-extended"), help="pbr: the generated project")
    parser.add_argument("--editor", default="", help="editor binary (default: the harness's)")
    parser.add_argument("--json", default="")
    return asyncio.run(main_async(parser.parse_args()))


if __name__ == "__main__":
    sys.exit(main())
