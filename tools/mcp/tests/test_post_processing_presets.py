"""Post-processing presets and the cost readout (shader library phase 4, editor): list_post_processing_presets /
apply_post_processing_preset (a preset replaces the whole post_processing struct as one undo step), the estimated
cost (this engine against Unity URP for the same effects, the research cost model), the LUT image's path, and the
Scene view's post-processing toggle (a display setting kept in the workspace file)."""

from __future__ import annotations

import struct
import zlib

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Post Processing Presets"}
TIMEOUT = 600

SCENE = "scenes/Presets.scene"
# S43 added handheld_4 and pico_8 (Palette Quantize with a built-in palette)
PRESETS = ["none", "mobile", "mobile_adaptive", "console", "stylised", "crt_retro", "film", "vhs", "handheld_4", "pico_8"]


def png(width: int, height: int) -> bytes:
    """A grey RGBA PNG."""
    rows = b"".join(b"\x00" + bytes([128, 128, 128, 255]) * width for _ in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


async def current(ed: Editor, sid: int) -> dict:
    return (await ed.call("list_post_processing_presets", scene=sid))["current"]


async def test_presets_list_apply_undo(ed: Editor):
    ed.require_tools("list_post_processing_presets", "apply_post_processing_preset", "get_scene_settings", "undo", "redo")
    sid = (await ed.call("create_scene", name="Presets", type="3d"))["scene"]["id"]

    listed = await ed.call("list_post_processing_presets", scene=sid)
    ed.check_eq({p["id"] for p in listed["presets"]}, set(PRESETS))
    by_id = {p["id"]: p for p in listed["presets"]}
    ed.check_eq(by_id["none"]["settings"], {}, "none is the defaults")
    ed.check(by_id["mobile"]["settings"].get("fxaa_enabled") is True and by_id["mobile"]["settings"].get("bloom_enabled") is True,
             f"mobile: FXAA and bloom: {by_id['mobile']['settings']}")
    ed.check(by_id["stylised"]["settings"].get("pixelate_enabled") is True, "stylised pixelates")
    ed.check(by_id["crt_retro"]["settings"].get("crt_enabled") is True, "crt_retro is a CRT")
    ed.check_eq(listed["current"]["preset"], "none", "a new scene matches none")
    ed.check_eq(listed["current"]["cost"]["engine"]["passes"], 0, "nothing on: no pass")

    # one preset, one undo step: the whole struct
    out = await ed.call("apply_post_processing_preset", scene=sid, preset="mobile")
    ed.check(out["settings"]["fxaa_enabled"] and out["settings"]["bloom_enabled"], "mobile enables FXAA and bloom")
    got = (await ed.call("get_scene_settings", scene=sid, names=["post_processing"]))["settings"]["post_processing"]
    ed.check_eq(got["tonemap"], "neutral", "mobile uses neutral tone mapping")
    ed.check_eq(got["bloom_intensity"], 0.6, "mobile has faint bloom")
    ed.check(got["fxaa_enabled"] and got["bloom_enabled"], "mobile settings are applied")
    ed.check_eq((await current(ed, sid))["preset"], "mobile")

    await ed.call("apply_post_processing_preset", scene=sid, preset="Console")  # by name
    ed.check_eq((await current(ed, sid))["preset"], "console")
    await ed.call("undo")
    ed.check_eq((await current(ed, sid))["preset"], "mobile", "undo: the previous preset whole")
    await ed.call("undo")
    ed.check_eq((await current(ed, sid))["preset"], "none", "undo again: the defaults")
    await ed.call("redo")
    ed.check_eq((await current(ed, sid))["preset"], "mobile", "redo")

    # a field changed after a preset: custom
    await ed.call("set_scene_settings", scene=sid, settings={"post_processing.bloom_intensity": 2.0})
    ed.check((await current(ed, sid))["preset"] is None, "edited: no preset matches")

    # The documented stylised preset has warm amber shadows.
    lift = by_id["stylised"]["settings"]["lift"]
    ed.check(lift[0] > lift[2] and lift[1] >= lift[2], f"stylised warm lift: {lift}")

    # saved with the scene
    await ed.call("apply_post_processing_preset", scene=sid, preset="crt_retro")
    await ed.call("save_scene", scene=sid, path=SCENE)
    text = (ed.project / SCENE).read_text()
    for key in ("crtEnabled: true", "pixelateEnabled: true", "filmGrainEnabled: true"):
        ed.check(key in text, f"{key} saved")

    await ed.check_error("apply_post_processing_preset", scene=sid, preset="cinematic", contains="crt_retro")
    ui = (await ed.call("create_scene", name="PresetsUI", type="UI"))["scene"]["id"]
    await ed.check_error("apply_post_processing_preset", scene=ui, preset="mobile", contains="UI scene")


async def test_cost_readout(ed: Editor):
    sid = (await ed.call("create_scene", name="Cost", type="3d"))["scene"]["id"]

    async def cost(settings: dict | None) -> dict:
        await ed.call("set_scene_settings", scene=sid, settings={"post_processing": None})
        if settings:
            await ed.call("set_scene_settings", scene=sid, settings={"post_processing": settings})
        return (await current(ed, sid))["cost"]

    # T0 only: no pass here; URP bakes its LUT and runs its uber pass
    c = await cost({"tonemap": "aces", "color_grading_enabled": True, "vignette_enabled": True})
    ed.check(c["estimate"] is True, "labelled as an estimate")
    ed.check_eq(c["engine"]["passes"], 0, "T0: no pass")
    ed.check_eq(c["tiers"], {"in_shader": True, "uber_pass": False, "bloom_chain": False})
    ed.check_eq(c["unity_urp"]["passes"], 2, "URP: LUT + uber")

    # the research's reference set (spec section 1): 8 passes, 1 at full resolution, ~1.17 screens;
    # URP 19 passes, 2 at full resolution, ~2.77 screens
    c = await cost({"tonemap": "aces", "color_grading_enabled": True, "vignette_enabled": True,
                    "chromatic_aberration_enabled": True, "bloom_enabled": True, "film_grain_enabled": True,
                    "fxaa_enabled": True})
    ed.check_eq((c["engine"]["passes"], c["engine"]["full_resolution_passes"]), (8, 1))
    ed.check(abs(c["engine"]["screens"] - 1.166) < 0.01, f"engine screens {c['engine']['screens']}")
    ed.check_eq((c["unity_urp"]["passes"], c["unity_urp"]["full_resolution_passes"]), (19, 2))
    ed.check(abs(c["unity_urp"]["screens"] - 2.766) < 0.01, f"URP screens {c['unity_urp']['screens']}")

    # render scale alone: the uber pass upscales a quarter of the pixels
    c = await cost({"render_scale": 0.5})
    ed.check_eq(c["engine"]["passes"], 1)
    ed.check(abs(c["scene_shading"] - 0.25) < 1e-3, f"scene shading {c['scene_shading']}")

    # a user pass costs one more pass in both
    before = await cost({"fxaa_enabled": True})
    await ed.call("set_scene_settings", scene=sid, settings={"post_process": [{"shader": "", "enabled": True}]})
    after = (await current(ed, sid))["cost"]
    ed.check_eq(after["engine"]["passes"], before["engine"]["passes"] + 1, "user pass counted")
    ed.check_eq(after["unity_urp"]["passes"], before["unity_urp"]["passes"] + 1, "user pass counted for URP")


async def test_lut_texture_path(ed: Editor):
    # an assets directory below the project (as the fixtures have): MCP paths are project-relative,
    # the scene stores them relative to the assets directory, as the engine loads them
    sid = (await ed.call("create_scene", name="Lut", type="3d"))["scene"]["id"]
    (ed.project / "assets").mkdir(exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
    ed.check_eq((await ed.call("get_project_summary"))["assets_dir"], "assets")
    rel = "assets/textures/strip.png"
    (ed.project / rel).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / rel).write_bytes(png(16, 4))  # N*N x N, N = 4

    await ed.call("set_scene_settings", scene=sid, settings={"post_processing": {"color_grading_enabled": True, "lut_texture": rel}})
    got = (await ed.call("get_scene_settings", scene=sid, names=["post_processing.lut_texture"]))["settings"]
    ed.check_eq(got["post_processing.lut_texture"], rel, "read back project-relative")
    await ed.call("save_scene", scene=sid, path="assets/scenes/Lut.scene")
    text = (ed.project / "assets/scenes/Lut.scene").read_text()
    ed.check("lutTexture: textures/strip.png" in text, f"stored relative to the assets directory: {text[:400]}")
    await ed.check_error("set_scene_settings", scene=sid, settings={"post_processing.lut_texture": "textures/strip.png"},
                         contains="no file")


async def test_scene_view_toggle_persists(ed: Editor):
    ed.require_tools("set_scene_camera", "close_scene", "open_scene")
    sid = (await ed.call("create_scene", name="Toggle", type="3d"))["scene"]["id"]
    await ed.call("save_scene", scene=sid, path="scenes/Toggle.scene")

    view = await ed.call("set_scene_camera", scene=sid)
    ed.check_eq(view["display"]["show_post_processing"], False, "off by default")
    view = await ed.call("set_scene_camera", scene=sid, display={"show_post_processing": True})
    ed.check_eq(view["display"]["show_post_processing"], True)
    workspace = (ed.project / ".jasperengine" / "user" / "workspace.yaml").read_text()
    ed.check("showPostProcessing: true" in workspace, "saved in the workspace file")

    # kept across closing and reopening the scene
    await ed.call("create_scene", name="Other", type="3d")
    await ed.call("close_scene", scene=sid, unsaved="discard")
    opened = await ed.call("open_scene", scene="scenes/Toggle.scene")
    view = await ed.call("set_scene_camera", scene=opened["scene"]["id"])
    ed.check_eq(view["display"]["show_post_processing"], True, "restored from the workspace")

    view = await ed.call("set_scene_camera", scene=opened["scene"]["id"], display={"show_post_processing": False})
    workspace = (ed.project / ".jasperengine" / "user" / "workspace.yaml").read_text()
    ed.check("showPostProcessing: true" not in workspace, "off again: not written (the default)")
