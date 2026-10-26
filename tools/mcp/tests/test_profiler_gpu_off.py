"""The GPU timer forced off (spec S15 AC5): with JASPERENGINE_GPU_TIMER=off the editor reports GPU
timing unavailable with the reason disabled, the pass list still has every pass (no times), and
nothing crashes."""

from __future__ import annotations

import os

from harness import Editor

# read at editor start: this file runs in its own process
os.environ["JASPERENGINE_GPU_TIMER"] = "off"

CI = True
EDITOR = {"create": True, "name": "Profiler GPU Off"}
TIMEOUT = 300


async def test_gpu_timer_disabled(ed: Editor):
    made = await ed.call("create_scene", name="Off", type="3D")
    scene = made["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="box", name="Box", select=False)
    await ed.call("wait", frames=10)
    r = await ed.call("get_gpu_passes")
    ed.check_eq(r["availability"]["state"], "unavailable", "unavailable")
    ed.check_eq(r["availability"]["reason"], "disabled", "the reason")
    ed.check(r["passes"] and all(p["gpu_ms"] is None and not p["valid"] for p in r["passes"]), "passes without times")
    stats = await ed.call("get_render_stats")
    ed.check(stats["gpu"]["available"] is False and stats["gpu"]["reason"] == "disabled", f"stats: {stats['gpu']}")
    capture = await ed.call("capture_profile", frames=10)
    ed.check_eq(capture["frames"], 10, "the CPU profiler still records")
    await ed.check_alive()
