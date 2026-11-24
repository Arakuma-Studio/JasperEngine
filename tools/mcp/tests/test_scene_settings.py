"""S94 (docs/specs/S94-rendering-anim-gaps.md AC19, AC20): the vignette's Radii falloff and the LUT's
Flip Green through set_scene_settings: one undo step each, and saved."""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "SceneSettings"}
TIMEOUT = 600

async def test_vignette_radii_and_lut_flip(ed: Editor):
    ed.require_tools("create_scene", "get_scene_settings", "set_scene_settings", "save_scene", "undo")
    sid = (await ed.call("create_scene", name="VignetteRadii", type="3D"))["scene"]["id"]
    detail = await ed.call("get_scene_settings", scene=sid, names=["post_processing.vignette_falloff"], detail=True)
    ed.check_eq(detail["schema"]["post_processing.vignette_falloff"]["options"], ["smoothness", "radii"])

    settings = {"post_processing.vignette_enabled": True, "post_processing.vignette_falloff": "radii",
                "post_processing.vignette_start": 0.4, "post_processing.vignette_end": 1.1,
                "post_processing.lut_flip_green": True}
    for name, value in settings.items():
        out = await ed.call("set_scene_settings", scene=sid, settings={name: value})
        ed.check_eq(out["settings"][name], value, name)
    await ed.call("save_scene", scene=sid, path="scenes/VignetteRadii.scene")
    text = (ed.project / "scenes/VignetteRadii.scene").read_text()
    for key in ("vignetteFalloff", "vignetteStart", "vignetteEnd", "lutFlipGreen"):
        ed.check(key in text, f"{key} saved")
    await ed.call("undo")
    got = (await ed.call("get_scene_settings", scene=sid, names=["post_processing"]))["settings"]["post_processing"]
    ed.check("lut_flip_green" not in got, f"undo: the last field only ({sorted(got)})")
    ed.check_eq(got.get("vignette_falloff"), "radii")
