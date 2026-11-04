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
    ... gpu_campaign.py lighting  [--size 1920 1080]                            # S57 R7
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


async def play_and_time(ed: Editor, scene: str, size: tuple[int, int], frames: int, warmup: int = 120, probe=None) -> dict:
    """{label: GPU ms} of the Game view playing `scene` at `size`, plus _frame, _frames, _size; `probe`
    (a coroutine function of the editor) runs while playing, its result as _probe"""
    sid = (await ed.call("open_scene", scene=scene))["scene"]["id"]
    await ed.call("play", action="start", scene=sid)
    try:
        await settle(ed, warmup)
        got = await gpu_view.fit_view(ed, *size)
        await ed.call("wait", frames=30)
        out = await gpu_view.gpu_passes(ed, frames)
        out["_size"] = list(got)
        if probe is not None:
            out["_probe"] = await probe(ed)
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
    # not maximised: the Game tab then hides the Scene view, which would draw too (and on Vulkan a
    # second view's commit leaves the engine frame's passes untimed); fit_view sizes the view
    await ed.call("configure_game_view", maximize_on_play=False, aspect="free")
    return await gpu_view.device(ed)


def merge(runs: list[dict]) -> dict:
    """the median over runs of every case's every label"""
    out = {}
    for case in runs[0]:
        # a run whose capture timed none of the view's passes is left out (Vulkan in the editor
        # loses them at times: S15, docs/perf/gpu-campaign.md); _valid_runs counts the rest
        valid = [r for r in runs if r[case].get("_frame", 0.0) > 0.0] or runs
        labels = {label for r in valid for label in r[case] if not label.startswith("_") or label == "_frame"}
        out[case] = {label: statistics.median(r[case].get(label, 0.0) for r in valid) for label in sorted(labels)}
        out[case]["_size"] = runs[0][case].get("_size")
        out[case]["_runs_frame"] = [r[case].get("_frame", 0.0) for r in runs]
        out[case]["_valid_runs"] = sum(1 for r in runs if r[case].get("_frame", 0.0) > 0.0)
        out[case]["_clock_mhz"] = statistics.median(r[case].get("_clock_mhz", 0.0) for r in valid)
        if any("_live" in r[case] for r in valid):
            out[case]["_live"] = statistics.median(r[case].get("_live", 0) for r in valid)
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
    # the receiver row: the ground alone filling the view under the sun (no casters: the overdraw of
    # 2,000 pillars is not the receiver's), soft against no shadow
    "receiver_soft": {"SHADOWS_DYNAMIC": "0", "SHADOWS_STATIC": "0", "SHADOWS_LIGHTS": "sun", "SHADOWS_SUN": "soft"},
    "receiver_none": {"SHADOWS_DYNAMIC": "0", "SHADOWS_STATIC": "0", "SHADOWS_LIGHTS": "sun", "SHADOWS_SUN": "none"},
}


async def shadows_run(args: argparse.Namespace, run: int) -> tuple[dict, dict]:
    out, dev = {}, {}
    for tier in args.tiers.split(","):
        for case in args.cases.split(","):
            env = dict(SHADOW_CASES[case], SHADOWS_QUALITY=tier)
            if args.shadow_memory_mb:
                env["JASPERENGINE_SHADOW_MEMORY_MB"] = str(args.shadow_memory_mb)
            async with Editor(project=ROOT / "tools" / "fixtures" / "shadows", env=env, label=f"gpu-shadows-{tier}-{case}-{run}",
                              **editor_args(args)) as ed:
                dev = await prepare(ed)
                out[f"{tier}/{case}"] = got = await play_and_time(ed, "Main Scene.scene", TIERS[tier], args.frames)
                print(f"run {run} {tier}/{case}: frame {got['_frame']:.3f} ms, shadow passes {shadow_ms(got):.3f} ms "
                      f"at {got['_size']}", flush=True)
    return out, dev


def shadows_report(results: dict, dev: dict | None = None) -> list[str]:
    rows = ["| Tier | Resolution | Receiver, sun soft (ms) | Budget | Static, cached (ms) | Budget | Walking + 20 dynamic (ms) | Budget |",
            "|---|---|---|---|---|---|---|---|"]
    mark = lambda v, b: "-" if v is None else f"{v:.3f}" + ("" if v <= b else " **over**")  # noqa: E731
    for tier, (rb, sb, wb) in SHADOW_BUDGETS.items():
        got = {case: results.get(f"{tier}/{case}") for case in SHADOW_CASES}
        if not any(got.values()):
            continue
        receiver = None
        if got["receiver_soft"] and got["receiver_none"]:
            soft, none = got["receiver_soft"], got["receiver_none"]
            receiver = (soft["_frame"] - shadow_ms(soft)) - (none["_frame"] - shadow_ms(none))
        static = shadow_ms(got["static"]) if got["static"] else None
        walking = shadow_ms(got["walking"]) if got["walking"] else None
        size = next(v for v in got.values() if v)["_size"]
        rows.append(f"| {tier} | {size[0]}x{size[1]} | {mark(receiver, rb)} | {rb} | {mark(static, sb)} | {sb} | {mark(walking, wb)} | {wb} |")
    return rows


# ------------------------------------------------------------------ S11 lobes (AC11, R13)

LOBE_BUDGETS = {"Lobes Off": 0.02, "Lobe Clearcoat": 0.25, "Lobe Sheen": 0.15, "Lobe Anisotropy": 0.15,
                "Lobe Iridescence": 0.30, "Lobe Specular": 0.03, "Lobe Transmission": 0.20}


async def pbr_run(args: argparse.Namespace, run: int) -> tuple[dict, dict]:
    out, dev = {}, {}
    async with Editor(project=Path(args.project).resolve(), label=f"gpu-pbr-{run}", **editor_args(args)) as ed:
        dev = await prepare(ed)
        scenes = ["Lit Baseline", *LOBE_BUDGETS, "Lobe Hex", "Terrain Baseline", "Hex Terrain"]
        if args.scenes:
            scenes = [scene for scene in scenes if scene in args.scenes.split(",")]
        for quality in args.qualities.split(","):
            for scene in scenes:
                sid = (await ed.call("open_scene", scene=f"{scene}.scene"))["scene"]["id"]
                await ed.call("set_scene_settings", scene=sid, settings={"lit_quality": quality})
                got = await play_and_time(ed, f"{scene}.scene", tuple(args.size), args.frames)
                out[f"{quality}/{scene}"] = got
                print(f"run {run} {quality}/{scene}: frame {got['_frame']:.3f} ms at {got['_size']}", flush=True)
    return out, dev


# R13's 4-layer hex terrain over the Terrain Baseline at 4K, by backend (S11 R13, approved by the
# user 2026-11-02): the ratio less one, as LOBE_BUDGETS
HEX_TERRAIN_BUDGETS = {"vulkan": 0.40, "gl": 0.75}


def hex_terrain_budget(dev: dict | None) -> float:
    backend = (dev or {}).get("backend", "").lower()
    return HEX_TERRAIN_BUDGETS["vulkan" if "vulkan" in backend else "gl"]


def pbr_report(results: dict, dev: dict | None = None) -> list[str]:
    rows = ["| Scene | GPU frame, Standard (ms) | vs Lit Baseline | Budget | GPU frame, High (ms) | High / Standard |", "|---|---|---|---|---|---|"]
    base = results.get("standard/Lit Baseline", {}).get("_frame", 0.0)
    terrain = results.get("standard/Terrain Baseline", {}).get("_frame", 0.0)
    for key in [k for k in results if k.startswith("standard/")]:
        scene = key.split("/", 1)[1]
        std = results[key]["_frame"]
        high = results.get(f"high/{scene}", {}).get("_frame", 0.0)
        ref = terrain if scene == "Hex Terrain" else base
        ratio = std / ref - 1.0 if ref > 0 else 0.0
        budget = LOBE_BUDGETS.get(scene, hex_terrain_budget(dev) if scene == "Hex Terrain" else None)
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


def post_report(results: dict, dev: dict | None = None) -> list[str]:
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
                await ed.call("edit_particle_effect", scene=sid, entity=fx["particle_system"]["id"], patch=json.loads(PARTICLE_PATCH))
            cameras = (await ed.call("find_entities", scene=sid, component="camera"))["entities"]
            if cameras:
                await ed.call("set_transform", scene=sid, entity=cameras[0]["id"], position=[0.0, 3.0, 14.0])
            await ed.call("save_scene", scene=sid)
            async def live(e):
                world = (await e.call("get_particle_stats", scene=sid)).get("world", {})
                return {"particle_count": world.get("particle_count"), "budget": world.get("budget")}
            got = await play_and_time(ed, "particles.scene", (1920, 1080), args.frames, warmup=240, probe=live)
            got["_live"] = (got.get("_probe") or {}).get("particle_count") or 0
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
        # (the fire preset caps emission at 100 a tick: 6,000 a second at 60 Hz)
        "emission": {"rateOverTime": 50000, "maxPerTick": 100000},
    }})


def particles_report(results: dict, dev: dict | None = None) -> list[str]:
    cost = sum(ms for label, ms in results["ref_100k"].items() if not label.startswith("_")) - \
        sum(ms for label, ms in results["empty"].items() if not label.startswith("_"))
    live = results["ref_100k"].get("_live", 0)
    return ["| Case | GPU (ms) | Budget |", "|---|---|---|",
            f"| {live:.0f} live reference particles at 1080p (passes over the empty scene) | {cost:.3f} | ≤ 1.5 (100 k) |"]


# ------------------------------------------------------------------ S57 lighting (R7)

# each feature off and on in the same scene: (case, scene, what turns it on)
LIGHTING_CASES = ["tiles_per_object", "tiles_tiled", "capsules_off", "capsules_on", "contact_off", "contact_on",
                  "oit_sorted", "oit_on", "hair_standard", "hair_on"]


async def lighting_scene(ed: Editor, name: str) -> tuple[int, int, int]:
    made = await ed.call("create_scene", name=name, type="3D", path=name)
    sid = made["scene"]["id"]
    sun = (await ed.call("find_entities", scene=sid, component="light"))["entities"][0]["id"]
    # a new scene draws through the default camera, which has no GameObject: make one
    camera = (await ed.call("create_entity", scene=sid, type="camera", name="Main Camera", position=[0.0, 7.0, 9.0],
                            select=False, values={"camera": {"useTarget": True, "target": [0.0, 0.0, 0.0]}}))["entity_id"]
    await ed.call("set_main_camera", scene=sid, entity=camera)
    return sid, sun, camera


async def lighting_entity(ed: Editor, sid: int, kind: str, name: str, position, scale=None, values=None) -> int:
    args = {"scene": sid, "type": kind, "name": name, "position": position, "select": False}
    if scale is not None:
        args["scale"] = scale
    if values:
        args["values"] = values
    return (await ed.call("create_entity", **args))["entity_id"]


async def lighting_build(ed: Editor) -> None:
    """the five scenes of the campaign, saved (each case toggles its feature before playing)"""
    sid, sun, _ = await lighting_scene(ed, "tiles")
    await ed.call("set_properties", scene=sid, entity=sun, values={"light": {"intensity": 0.3}})
    await lighting_entity(ed, sid, "plane", "Ground", [0, 0, 0], scale=[2, 1, 2])
    for i in range(64):
        await lighting_entity(ed, sid, "point_light", f"Lamp{i}", [-7 + 2 * (i % 8), 0.6, -7 + 2 * (i // 8)],
                              values={"light": {"range": 2.5, "intensity": 2.0}})
    await ed.call("save_scene", scene=sid)

    sid, sun, _ = await lighting_scene(ed, "capsules")
    await ed.call("set_properties", scene=sid, entity=sun, values={"light": {"shadows": True}})
    await lighting_entity(ed, sid, "plane", "Ground", [0, 0, 0], scale=[2, 1, 2], values={"mesh": {"staticShadowCaster": True}})
    for i in range(4):
        body = await lighting_entity(ed, sid, "cube", f"Body{i}", [-3 + 2 * i, 0.9, 0], scale=[0.6, 1.8, 0.4])
        await ed.call("add_component", scene=sid, entity=body, component="capsuleshadowcaster")
        capsules = [{"bone": "", "radius": 0.15, "length": 0.4, "offset": [0, -0.4 + 0.12 * k, 0]} for k in range(8)]
        await ed.call("set_properties", scene=sid, entity=body,
                      values={"capsuleshadowcaster": {"castShadows": "capsules_only", "capsules": capsules}})
    await ed.call("save_scene", scene=sid)

    sid, sun, _ = await lighting_scene(ed, "contact")
    await ed.call("set_properties", scene=sid, entity=sun, values={"light": {"shadows": True, "contactShadowSampleCount": 16}})
    await lighting_entity(ed, sid, "plane", "Ground", [0, 0, 0], scale=[2, 1, 2])
    for i in range(20):
        await lighting_entity(ed, sid, "cube", f"Box{i}", [-6 + (i % 5) * 3, 0.25, -4 + (i // 5) * 2.5], scale=[0.5, 0.5, 0.5])
    await ed.call("save_scene", scene=sid)

    sid, sun, _ = await lighting_scene(ed, "oit")
    await lighting_entity(ed, sid, "plane", "Ground", [0, 0, 0], scale=[2, 1, 2])
    for i in range(24):
        # stacked, so the camera looks through them all
        await lighting_entity(ed, sid, "plane", f"Glass{i}", [-1 + (i % 6) * 0.4, 0.5 + i * 0.15, -1 + (i // 6) * 0.4], scale=[0.6, 1, 0.6],
                              values={"mesh": {"submeshes[0].material": {"baseColorFactor": [0.3, 0.6, 0.9, 0.4],
                                                                         "blendMode": "alpha", "cullMode": "off"}}})
    await ed.call("save_scene", scene=sid)

    sid, sun, camera = await lighting_scene(ed, "hair")
    await ed.call("set_transform", scene=sid, entity=camera, position=[0.0, 0.0, 2.2])
    await lighting_entity(ed, sid, "sphere", "Hair", [0, 0, 0], scale=[2, 2, 2])
    await ed.call("save_scene", scene=sid)


async def lighting_toggle(ed: Editor, case: str) -> str:
    """turns the case's feature on or off in its scene; returns the scene file"""
    scene_name, mode = case.rsplit("_", 1)
    scene_file = {"tiles": "tiles", "capsules": "capsules", "contact": "contact", "oit": "oit", "hair": "hair"}[scene_name.split("_")[0]]
    sid = (await ed.call("open_scene", scene=f"{scene_file}.scene"))["scene"]["id"]
    on = mode in ("tiled", "on")
    if scene_file == "tiles":
        await ed.call("set_project_settings", values={"graphics.light_mode": "tiled" if on else "per_object"})
    elif scene_file == "capsules":
        await ed.call("set_scene_settings", scene=sid, settings={"capsule_shadows_enabled": on})
    elif scene_file == "contact":
        sun = (await ed.call("find_entities", scene=sid, component="light"))["entities"][0]["id"]
        await ed.call("set_properties", scene=sid, entity=sun, values={"light": {"contactShadows": on}})
    elif scene_file == "oit":
        camera = (await ed.call("find_entities", scene=sid, component="camera"))["entities"][0]["id"]
        await ed.call("set_properties", scene=sid, entity=camera,
                      values={"camera": {"transparencyMode": "order_independent" if on else "sorted"}})
    elif scene_file == "hair":
        hair = (await ed.call("find_entities", scene=sid, name="Hair"))["entities"][0]["id"]
        await ed.call("set_properties", scene=sid, entity=hair, values={"mesh": {"submeshes[0].material": {"pbr": {"hair": {"enabled": on}}}}})
    await ed.call("save_scene", scene=sid)
    return f"{scene_file}.scene"


async def lighting_run(args: argparse.Namespace, run: int) -> tuple[dict, dict]:
    out, dev = {}, {}
    async with Editor(create=True, name=f"GpuLighting{run}", label=f"gpu-lighting-{run}", **editor_args(args)) as ed:
        dev = await prepare(ed)
        await lighting_build(ed)
        for case in args.lighting_cases.split(","):
            scene = await lighting_toggle(ed, case)
            got = await play_and_time(ed, scene, tuple(args.size), args.frames)
            out[case] = got
            print(f"run {run} lighting {case}: frame {got['_frame']:.3f} ms at {got['_size']}", flush=True)
        await ed.call("set_project_settings", values={"graphics.light_mode": "automatic"})
    return out, dev


def lighting_report(results: dict, dev: dict | None = None) -> list[str]:
    rows = ["| Feature | Off: GPU frame (ms) | On: GPU frame (ms) | Cost (ms) | Its own pass (ms) |", "|---|---|---|---|---|"]
    pairs = [("Tiled lights, 64 point lights (vs Per Object)", "tiles_per_object", "tiles_tiled", None),
             ("Capsule shadows, 4 casters x 8 capsules", "capsules_off", "capsules_on", None),
             ("Contact shadows, 16 samples", "contact_off", "contact_on", "contact shadows"),
             ("Order Independent, 24 overlapping planes (vs Sorted)", "oit_sorted", "oit_on", "oit accumulation"),
             ("Hair shading model, a sphere filling the view (vs Standard)", "hair_standard", "hair_on", None)]
    for label, off, on, pass_label in pairs:
        if off not in results or on not in results:
            continue
        a, b = results[off]["_frame"], results[on]["_frame"]
        own = f"{results[on].get(pass_label, 0.0):.3f}" if pass_label else "-"
        rows.append(f"| {label} | {a:.3f} | {b:.3f} | {b - a:+.3f} | {own} |")
    return rows


CASES = {"shadows": (shadows_run, shadows_report), "pbr": (pbr_run, pbr_report), "post": (post_run, post_report),
         "particles": (particles_run, particles_report), "lighting": (lighting_run, lighting_report)}


async def main_async(args: argparse.Namespace) -> int:
    run_one, report = CASES[args.case]
    runs, dev = [], {}
    for run in range(args.runs):
        got, dev = await run_one(args, run)
        runs.append(got)
    results = merge(runs)
    print(f"\ndevice: {dev}, {args.runs} runs, {args.frames} frames each, medians")
    rows = report(results, dev)
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
    parser.add_argument("--cases", default=",".join(SHADOW_CASES), help="shadows: the cases")
    parser.add_argument("--shadow-memory-mb", type=int, default=0,
                        help="shadows: JASPERENGINE_SHADOW_MEMORY_MB for the editor (the tier's memory row replaced)")
    parser.add_argument("--project", default=str(ROOT / "build" / "pbr-extended"), help="pbr: the generated project")
    parser.add_argument("--scenes", default="", help="pbr: only these scenes (comma separated names)")
    parser.add_argument("--qualities", default="standard,high", help="pbr: the Lit qualities")
    parser.add_argument("--size", type=int, nargs=2, default=[1920, 1080], metavar=("W", "H"),
                        help="pbr: the Game view's render size (a bigger one keeps the GPU at its clocks)")
    parser.add_argument("--lighting-cases", default=",".join(LIGHTING_CASES), help="lighting: the cases")
    parser.add_argument("--editor", default="", help="editor binary (default: the harness's)")
    parser.add_argument("--json", default="")
    return asyncio.run(main_async(parser.parse_args()))


if __name__ == "__main__":
    sys.exit(main())
