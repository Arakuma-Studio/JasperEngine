"""S12 atmosphere in the editor (docs/specs/S12-atmosphere.md AC12, AC13): the three GameObjects
the menus create, every new field settable with one undo step each, the presets (list, apply as
one named undo step), the Scene view's Fog and Clouds toggles (that view only), the Inspector's
height fog foldouts and the project's per-platform Atmosphere Quality.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import sys
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).parent))
from test_runtime_regressions import image_difference  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Atmosphere Editor CI"}
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="AtmosphereEdit", type="3D", path="scenes/AtmosphereEdit")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("create_entity", scene=S["scene"], type="box", name="Box")
    await ed.call("create_entity", scene=S["scene"], type="directional_light", name="Sun")
    return S


async def values(ed: Editor, entity, component: str, *names: str) -> dict:
    args = {"scene": S["scene"], "entity": entity, "components": [component]}
    if names:
        args["properties"] = list(names)
    return (await ed.call("get_entity", **args))["components"][component]


async def test_create_entity_types(ed: Editor):
    ed.require_tools("create_entity", "get_entity")
    s = await setup(ed)
    for type_, component, name in (("fog", "fog", "Fog"), ("local_volumetric_fog", "localvolumetricfog", "Local Volumetric Fog"),
                                   ("volumetric_clouds", "volumetricclouds", "Volumetric Clouds")):
        made = await ed.call("create_entity", scene=s["scene"], type=type_, name=name)
        s[type_] = made["entity_id"]
        got = await ed.call("get_entity", scene=s["scene"], entity=made["entity_id"], detail="summary")
        ed.check_in(component, [c.lower().replace(" ", "") for c in got["components"]], f"{type_} has its component")
    # the defaults are today's behaviour (a linear fog) and the spec's
    fog = await values(ed, s["fog"], "fog", "type", "volumetric", "attenuationDistance", "skyFill")
    ed.check_eq(fog["type"], "linear")
    ed.check_eq(fog["volumetric"], False)
    ed.check_eq(fog["attenuationDistance"], 400.0)
    clouds = await values(ed, s["volumetric_clouds"], "volumetricclouds", "preset", "rendering", "bottomAltitude")
    ed.check_eq(clouds["preset"], "cloudy")
    ed.check_eq(clouds["rendering"], "auto")
    ed.check_eq(clouds["bottomAltitude"], 1200.0)


async def test_set_properties_and_undo(ed: Editor):
    ed.require_tools("set_properties", "undo")
    s = await setup(ed)
    edits = [
        (s["fog"], "fog", {"type": "height"}),
        (s["fog"], "fog", {"attenuationDistance": 55.0}),
        (s["fog"], "fog", {"colorMode": "sky"}),
        (s["fog"], "fog", {"volumetric": True}),
        (s["fog"], "fog", {"wind": [1.0, 0.0, 2.0]}),
        (s["fog"], "fog", {"qualityOverride": "medium"}),
        (s["local_volumetric_fog"], "localvolumetricfog", {"shape": "sphere"}),
        (s["local_volumetric_fog"], "localvolumetricfog", {"size": [2.0, 3.0, 4.0]}),
        (s["volumetric_clouds"], "volumetricclouds", {"rendering": "screen"}),
        (s["volumetric_clouds"], "volumetricclouds", {"coverage": 0.8}),
    ]
    for entity, component, change in edits:
        name, value = next(iter(change.items()))
        before = (await values(ed, entity, component, name))[name]
        made = await ed.call("set_properties", scene=s["scene"], entity=entity, values={component: change})
        ed.check(made.get("undo", {}).get("id"), f"{component}.{name}: one undo step ({made})")
        after = (await values(ed, entity, component, name))[name]
        ed.check(after == value or (isinstance(value, list) and [round(v, 4) for v in after] == value), f"{name} set: {after}")
        await ed.call("undo", scope="timeline", scene=s["scene"])
        ed.check_eq((await values(ed, entity, component, name))[name], before, f"undo restores {component}.{name}")
    # the Light's volumetric intensity
    sun = (await ed.call("get_entity", scene=s["scene"], entity="Sun", detail="summary"))["id"]
    await ed.call("set_properties", scene=s["scene"], entity=sun, values={"light": {"volumetricIntensity": 2.0}})
    ed.check_eq((await values(ed, sun, "light", "volumetricIntensity"))["volumetricIntensity"], 2.0)


async def test_presets(ed: Editor):
    ed.require_tools("list_atmosphere_presets", "apply_atmosphere_preset", "get_undo_history")
    s = await setup(ed)
    fog_presets = await ed.call("list_atmosphere_presets", kind="fog")
    ed.check_eq([p["id"] for p in fog_presets["presets"]],
                ["morning_haze", "ground_mist", "mountain_valley", "horizon_haze", "night"], "five fog looks")
    cloud_presets = await ed.call("list_atmosphere_presets", kind="clouds")
    ed.check_eq([p["id"] for p in cloud_presets["presets"]], ["custom", "sparse", "cloudy", "overcast", "stormy"],
                "Custom and four cloud looks")

    before = await values(ed, s["fog"], "fog")
    applied = await ed.call("apply_atmosphere_preset", scene=s["scene"], entity=s["fog"], preset="ground_mist")
    ed.check(applied["changed"], "the preset changed the fog")
    after = await values(ed, s["fog"], "fog")
    ed.check_eq(after["type"], "height", "a fog preset makes a height fog")
    ed.check(after["attenuationDistance"] != before["attenuationDistance"], "the look's fields")
    top = (await ed.call("get_undo_history", scene=s["scene"]))["steps"][0]
    ed.check("Apply Atmosphere Preset" in top["name"], f"one named step: {top['name']}")
    await ed.call("undo", scope="timeline", scene=s["scene"])
    ed.check_eq(await values(ed, s["fog"], "fog"), before, "one undo restores every field")

    await ed.call("apply_atmosphere_preset", scene=s["scene"], entity=s["volumetric_clouds"], preset="overcast")
    clouds = await values(ed, s["volumetric_clouds"], "volumetricclouds", "preset", "coverage")
    ed.check_eq(clouds["preset"], "overcast")
    await ed.check_error("apply_atmosphere_preset", scene=s["scene"], entity=s["fog"], preset="no_such_look", contains="presets:")
    await ed.check_error("apply_atmosphere_preset", scene=s["scene"], entity="Box", preset="night", contains="neither")


async def test_scene_view_toggles(ed: Editor):
    """The Scene view's Fog and Clouds toggles change that view only."""
    ed.require_tools("set_scene_camera", "get_atmosphere_info", "screenshot")
    s = await setup(ed)
    await ed.call("set_properties", scene=s["scene"], entity=s["fog"],
                  values={"fog": {"type": "height", "volumetric": True, "attenuationDistance": 60.0}})
    await ed.call("set_scene_camera", scene=s["scene"], display={"disable_fog": False, "disable_clouds": False})

    async def info(view: str) -> dict:
        await ed.screenshot(view)
        await ed.call("wait", frames=4)
        await ed.screenshot(view)
        return await ed.call("get_atmosphere_info", scene=s["scene"], view=view)

    # (the clouds need their noise: give it time)
    for _ in range(30):
        got = await info("scene")
        if got["clouds"]["active"]:
            break
        await ed.call("wait", frames=10)
    ed.check(got["clouds"]["active"], f"the Scene view draws the clouds: {got['clouds']}")
    with_fog = await ed.screenshot("scene")

    await ed.call("set_scene_camera", scene=s["scene"], display={"disable_fog": True, "disable_clouds": True})
    scene_off = await info("scene")
    ed.check(not scene_off["clouds"]["active"], "the Clouds toggle removes the clouds")
    ed.check(not scene_off["fog"]["volumetric"]["active"], "the Fog toggle removes the volumetric fog")
    ed.check(image_difference(with_fog, await ed.screenshot("scene")) > 0.5, "the Scene view lost its fog")
    game = await info("game")
    ed.check(game["clouds"]["active"], "the Game view keeps the clouds")
    ed.check(game["fog"]["volumetric"]["active"], "the Game view keeps the fog")
    await ed.call("set_scene_camera", scene=s["scene"], display={"disable_fog": False, "disable_clouds": False})


async def test_inspector_shows_the_height_fog_foldouts(ed: Editor):
    """Type Height adds the Height Fog, Directional Inscattering, Sky and Volumetric Fog foldouts."""
    ed.require_tools("select_entities", "screenshot", "focus_window")
    s = await setup(ed)
    await ed.call("set_properties", scene=s["scene"], entity=s["fog"], values={"fog": {"type": "linear"}})
    await ed.call("select_entities", scene=s["scene"], entities=[s["fog"]])
    await ed.call("focus_window", window="inspector")
    await ed.call("wait", frames=5)
    linear = await ed.screenshot("editor")
    await ed.call("set_properties", scene=s["scene"], entity=s["fog"], values={"fog": {"type": "height"}})
    await ed.call("wait", frames=5)
    height = await ed.screenshot("editor")
    ed.check(image_difference(linear, height) > 0.2, "the Inspector shows other rows for a height fog")


async def test_project_atmosphere_quality(ed: Editor):
    ed.require_tools("get_project_settings", "set_project_settings")
    got = await ed.call("get_project_settings", keys=["quality.atmosphere_quality_desktop", "quality.atmosphere_quality_mobile",
                                                      "quality.atmosphere_quality_web"])
    settings = got.get("settings", got)
    ed.check_eq(settings["quality.atmosphere_quality_desktop"], "high", "desktop default")
    ed.check_eq(settings["quality.atmosphere_quality_mobile"], "low", "mobile default")
    ed.check_eq(settings["quality.atmosphere_quality_web"], "low", "web default")
    await ed.call("set_project_settings", settings={"quality.atmosphere_quality_mobile": "off"})
    got = await ed.call("get_project_settings", keys=["quality.atmosphere_quality_mobile"])
    ed.check_eq(got.get("settings", got)["quality.atmosphere_quality_mobile"], "off")
    await ed.check_error("set_project_settings", settings={"quality.atmosphere_quality_web": "ultra"})
    await ed.call("set_project_settings", settings={"quality.atmosphere_quality_mobile": "low"})
