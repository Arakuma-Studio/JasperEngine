"""Device tiers (spec S56 AC4, AC5): get_device_info in edit mode and in Play, the Game view's
device simulation (tier, platform family, thermal state) applying the tier's overrides in Play
and ending with it, and the Device Tier table through get/set_project_settings
(quality.device_tiers.<group>.<tier>) with undo. With JASPER_DEVICE_SHOTS set, the Game view at
Device Low and High is saved there (the spec's evidence screenshots)."""

from __future__ import annotations

import os
import sys
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from htmlui_pixels import image_difference  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Device Tiers"}
TIMEOUT = 600

TIERS = ["low", "medium", "high", "ultra"]


async def settings_of(ed: Editor, *keys: str) -> dict:
    got = await ed.call("get_project_settings", keys=list(keys))
    return got.get("settings", got)


async def scale_becomes(ed: Editor, sid: int, scale: float) -> dict:
    """The applied render scale once it is `scale` (a new uber variant builds first), or the last status."""
    status = {}
    for _ in range(60):
        status = await ed.call("get_post_processing_status", scene=sid)
        if abs(status["render_scale_applied"] - scale) < 1e-3:
            break
        await ed.call("wait", frames=10)
    return status


async def test_device_info_in_edit_mode(ed: Editor):
    ed.require_tools("get_device_info")
    info = await ed.call("get_device_info")
    ed.check(info["detected_tier"] in TIERS, f"a tier is detected: {info}")
    ed.check(info["reason"] and "tier " in info["reason"], f"with its reason: {info['reason']}")
    ed.check(info["rule"] in ("device", "name", "family", "vendor", "heuristic"), f"rule {info['rule']}")
    ed.check_eq(info["tier_applied"], False, "the editor applies no detected tier")
    ed.check_eq(info["tier_source"], "none")
    ed.check_eq(info["applied_overrides"], [], "nothing applies in edit mode")
    ed.check_eq(info["thermal_state"], "nominal")
    ed.check_eq(info["platform"], "desktop")
    ed.check_eq(info["upload_budget_mb"], 32.0, "the desktop upload budget")
    ed.check_eq(info["playing"], False)


async def test_device_tier_settings_with_undo(ed: Editor):
    ed.require_tools("get_project_settings", "set_project_settings", "undo", "redo")
    low_key = "quality.device_tiers.mobile.low"
    got = await settings_of(ed, low_key, "quality.device_tiers.mobile.medium", "quality.device_tiers.mobile.high",
                            "quality.thermal_response.mobile", "quality.thermal_response.desktop",
                            "graphics.upload_budget_mb.web", "quality.lod_bias")
    low = got[low_key]
    # R3's defaults: Low halves the scale, Low shadows, LOD Bias 0.5, no volumetrics or SSAO
    ed.check_eq(low["render_scale"], 0.5)
    ed.check_eq(low["shadow_quality"], "low")
    ed.check_eq(low["lod_bias"], 0.5)
    ed.check_eq(low["ssao"], False)
    ed.check_eq(low["volumetrics"], False)
    ed.check_eq(low["msaa_samples"], None, "an empty cell inherits")
    ed.check(abs(got["quality.device_tiers.mobile.medium"]["render_scale"] - 0.85) < 1e-6, "Medium renders at 0.85")
    ed.check(all(v is None for v in got["quality.device_tiers.mobile.high"].values()), "High is the project's values")
    ed.check_eq(got["quality.thermal_response.mobile"], True)
    ed.check_eq(got["quality.thermal_response.desktop"], False)
    ed.check_eq(got["graphics.upload_budget_mb.web"], 6)
    ed.check_eq(got["quality.lod_bias"], 1.0)

    yaml = ed.project / "project.yaml"
    ed.check("device:" not in yaml.read_text(), "a default project saves no device block")

    # a merge: the listed cells change, null inherits, the rest stays
    await ed.call("set_project_settings", values={low_key: {"render_scale": 0.6, "msaa_samples": 2, "ssao": None},
                                                  "quality.lod_bias": 0.8})
    low = (await settings_of(ed, low_key))[low_key]
    ed.check(abs(low["render_scale"] - 0.6) < 1e-6, f"render scale set: {low}")
    ed.check_eq(low["msaa_samples"], 2)
    ed.check_eq(low["ssao"], None, "null inherits again")
    ed.check_eq(low["volumetrics"], False, "untouched cells stay")
    ed.check("device:" in yaml.read_text(), "saved to project.yaml")

    # one call is one step of the project history
    await ed.call("undo", scope="project")
    ed.check_eq((await settings_of(ed, "quality.lod_bias"))["quality.lod_bias"], 1.0, "the LOD bias undone")
    low = (await settings_of(ed, low_key))[low_key]
    ed.check_eq(low["render_scale"], 0.5, "undo restores the cell")
    ed.check_eq(low["ssao"], False)
    ed.check_eq(low["msaa_samples"], None)
    ed.check("device:" not in yaml.read_text(), "back to the defaults on disk")
    await ed.call("redo", scope="project")
    got = await settings_of(ed, low_key, "quality.lod_bias")
    ed.check(abs(got[low_key]["render_scale"] - 0.6) < 1e-6 and abs(got["quality.lod_bias"] - 0.8) < 1e-6, f"redo: {got}")
    await ed.call("undo", scope="project")

    await ed.check_error("set_project_settings", values={low_key: {"render_scale": 2}}, contains="render_scale")
    await ed.check_error("set_project_settings", values={low_key: {"ssao": True}}, contains="off")
    await ed.check_error("set_project_settings", values={low_key: {"sparkle": 1}}, contains="not a cell")
    await ed.check_error("set_project_settings", values={"graphics.upload_budget_mb.mobile": 0})


async def test_device_simulation_in_play(ed: Editor):
    ed.require_tools("set_device_simulation", "get_device_info", "play", "wait", "create_entity", "get_post_processing_status",
                     "configure_game_view", "focus_window", "list_scenes")
    listed = await ed.call("list_scenes")
    sid = listed["selected_scene_id"]
    await ed.call("create_entity", scene=sid, name="Ball", type="sphere")
    await ed.call("create_entity", scene=sid, name="Floor", type="plane")

    out = await ed.call("set_device_simulation", tier="low", platform="mobile")
    ed.check_eq(out["playing"], False)
    ed.check_eq(out["simulation"], {"tier": "low", "platform": "mobile", "thermal_state": "device"})
    info = await ed.call("get_device_info")
    ed.check_eq(info["tier_applied"], False, "a simulation applies in Play only")

    shots = os.environ.get("JASPER_DEVICE_SHOTS")
    await ed.call("configure_game_view", aspect="16:9", scale=1, maximize_on_play=True)
    await ed.call("play", action="start", scene=sid)
    await ed.call("focus_window", window="game")
    try:
        await ed.call("wait", frames=30)
        info = await ed.call("get_device_info")
        ed.check_eq(info["tier_applied"], True, f"applied in Play: {info}")
        ed.check_eq(info["tier"], "low")
        ed.check_eq(info["tier_source"], "simulated")
        ed.check_eq(info["platform"], "mobile")
        applied = {o["setting"]: o for o in info["applied_overrides"]}
        ed.check(applied.get("render_scale", {}).get("value") == 0.5, f"Low's render scale: {applied}")
        ed.check(applied.get("ssao", {}).get("value") is False, f"Low's SSAO: {applied}")
        status = await scale_becomes(ed, sid, 0.5)
        ed.check_eq(status["render_scale_applied"], 0.5, "the renderer draws at Low's scale")
        low_shot = await ed.screenshot("game", Path(shots) / "device-low.png" if shots else None)

        out = await ed.call("set_device_simulation", tier="high")
        ed.check_eq(out["tier"], "high", "the reply is the device info after the change")
        ed.check_eq(out["applied_overrides"], [], "High is the project's values")
        status = await scale_becomes(ed, sid, 1.0)
        ed.check_eq(status["render_scale_applied"], 1.0)
        high_shot = await ed.screenshot("game", Path(shots) / "device-high.png" if shots else None)
        difference = image_difference(low_shot, high_shot)
        print(f"Game view Low vs High: mean RGB difference {difference:.3f}")
        ed.check(difference > 0.05, f"Low and High look different ({difference:.3f})")

        # a Serious thermal state on mobile: 20 % off the render scale, at most 30 fps
        out = await ed.call("set_device_simulation", tier="medium", thermal_state="serious")
        ed.check_eq(out["thermal_state"], "serious")
        ed.check_eq(out["thermal_response"], "serious")
        status = await scale_becomes(ed, sid, 0.85 * 0.8)
        ed.check(abs(status["render_scale_applied"] - 0.85 * 0.8) < 1e-3, f"Medium's 0.85 times 0.8: {status['render_scale_applied']}")
        sources = {(o["setting"], o["source"]) for o in out["applied_overrides"]}
        ed.check(("target_frame_rate", "thermal") in sources, f"the frame cap: {out['applied_overrides']}")
        # cooling down waits 30 s (hysteresis)
        out = await ed.call("set_device_simulation", thermal_state="nominal")
        ed.check_eq(out["thermal_state"], "nominal")
        ed.check_eq(out["thermal_response"], "serious", "the response waits before it recovers")
        ed.check(out["recovery_seconds"] > 25, f"recovery {out['recovery_seconds']}")
    finally:
        await ed.call("play", action="stop")

    await ed.call("wait", frames=10)
    info = await ed.call("get_device_info")
    ed.check_eq(info["tier_applied"], False, "Play's end ends the simulation")
    ed.check_eq(info["thermal_state"], "nominal")
    ed.check_eq(info["thermal_response"], "nominal")
    ed.check_eq(info["simulation"]["tier"], "medium", "the toolbar keeps its choice for the next Play")
    await ed.call("set_device_simulation", tier="auto", platform="auto", thermal_state="device")
    await ed.check_error("set_device_simulation", tier="extreme")
