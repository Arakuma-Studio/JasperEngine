"""S94 (docs/specs/S94-rendering-anim-gaps.md AC15, AC19, AC20): the Environment Lighting Source
(ambient_mode and the gradient's colours), the vignette's Radii falloff and the LUT's Flip Green
through set_scene_settings: one undo step each, saved, reloaded and read in Lua."""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "SceneSettings"}
TIMEOUT = 600

SCENE = "scenes/AmbientGradient.scene"


def near(a, b, tolerance=1e-3) -> bool:
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


async def test_ambient_gradient(ed: Editor):
    ed.require_tools("create_scene", "get_scene_settings", "set_scene_settings", "save_scene", "undo", "eval_lua",
                     "close_scene", "open_scene")
    sid = (await ed.call("create_scene", name="AmbientGradient", type="3D"))["scene"]["id"]
    detail = await ed.call("get_scene_settings", scene=sid, names=["ambient_mode", "ambient_sky_color"], detail=True)
    ed.check_eq(detail["schema"]["ambient_mode"]["options"], ["color", "gradient"])
    ed.check_eq(detail["settings"]["ambient_mode"], "color", "Color is the default")

    await ed.call("set_scene_settings", scene=sid, settings={"ambient_mode": "gradient"})
    await ed.call("set_scene_settings", scene=sid, settings={"ambient_sky_color": [0.5, 0.75, 1.0],
                                                             "ambient_equator_color": [0.25, 0.5, 0.25],
                                                             "ambient_ground_color": [0.5, 0.25, 0.0]})
    got = (await ed.call("get_scene_settings", scene=sid, names=["ambient_mode", "ambient_sky_color", "ambient_ground_color"]))["settings"]
    ed.check_eq(got["ambient_mode"], "gradient")
    ed.check(near(got["ambient_sky_color"], [0.5, 0.75, 1.0]), f"sky colour: {got['ambient_sky_color']}")

    # undo: the colours, then the mode
    await ed.call("undo")
    got = (await ed.call("get_scene_settings", scene=sid, names=["ambient_mode", "ambient_sky_color"]))["settings"]
    ed.check(not near(got["ambient_sky_color"], [0.5, 0.75, 1.0]), "undo restores the colours")
    ed.check_eq(got["ambient_mode"], "gradient", "the mode is the step before")
    await ed.call("undo")
    ed.check_eq((await ed.call("get_scene_settings", scene=sid, names=["ambient_mode"]))["settings"]["ambient_mode"], "color")
    await ed.check_error("set_scene_settings", scene=sid, settings={"ambient_mode": "trilight"}, contains="gradient")

    await ed.call("set_scene_settings", scene=sid, settings={"ambient_mode": "gradient", "ambient_ground_color": [0.5, 0.25, 0.0]})
    await ed.call("save_scene", scene=sid, path=SCENE)
    text = (ed.project / SCENE).read_text()
    ed.check("ambientMode: gradient" in text, "the mode saved")
    ed.check("ambientGroundColor" in text, "the ground colour saved")
    ed.check("ambientSkyColor" not in text, "a default colour is not written")

    await ed.call("close_scene", scene=sid, unsaved="discard")
    sid = (await ed.call("open_scene", scene=SCENE))["scene"]["id"]
    got = (await ed.call("get_scene_settings", scene=sid, names=["ambient_mode", "ambient_ground_color"]))["settings"]
    ed.check_eq(got["ambient_mode"], "gradient", "reloaded")
    ed.check(near(got["ambient_ground_color"], [0.5, 0.25, 0.0]), f"reloaded ground colour: {got['ambient_ground_color']}")

    lua = (await ed.call("eval_lua", scene=sid, allow_edit_mode=True, code=(
        "local before = scene.ambientMode == AmbientMode.GRADIENT "
        "scene:setAmbientGradient(Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)) "
        "local sky = scene.ambientSkyColor "
        "scene.ambientMode = AmbientMode.COLOR "
        "return before, sky.x, sky.y, scene.ambientMode == AmbientMode.COLOR")))["values"]
    ed.check_eq(lua, [True, 1, 0, True], "Lua reads and writes the source and the gradient")


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
