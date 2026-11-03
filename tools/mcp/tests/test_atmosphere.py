"""S12 atmosphere (docs/specs/S12-atmosphere.md) through the MCP server: the zero-cost rule (AC2),
distance fogs untouched by the height fog's fields (AC1), the volumetric fog's tiers and the Local
Volumetric Fog caps reported by get_atmosphere_info (AC7), the clouds' modes (AC8) and the Lua
API in Play (AC14). The images themselves are checked by the engine's AtmosphereGpu goldens
(engine/tests/core/render/AtmosphereGpuTest.cpp).

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import sys
from pathlib import Path

from harness import Editor

sys.path.insert(0, str(Path(__file__).parent))
from test_runtime_regressions import image_difference  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Atmosphere CI"}
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> dict:
    """A 3D scene of its own with a ground, a box and a sun, selected."""
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="AtmosphereLab", type="3D", path="scenes/AtmosphereLab")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    # the goldens were drawn before S11's Lit Quality (a new scene is High): Standard is that shader
    await ed.call("set_scene_settings", scene=S["scene"], settings={"lit_quality": "standard"})
    ground = await ed.call("create_entity", scene=S["scene"], type="plane", name="Ground")
    await ed.call("set_transform", scene=S["scene"], entity=ground["entity_id"], scale=[50, 1, 50])
    await ed.call("create_entity", scene=S["scene"], type="box", name="Box")
    S["sun"] = (await ed.call("create_entity", scene=S["scene"], type="directional_light", name="Sun"))["entity_id"]
    return S


async def info(ed: Editor, view: str = "game") -> dict:
    """What the view ran, after it drew a few frames."""
    await ed.screenshot(view)
    await ed.call("wait", frames=4)
    await ed.screenshot(view)
    return await ed.call("get_atmosphere_info", scene=S["scene"], view=view)


async def set_values(ed: Editor, entity, component: str, **values):
    await ed.call("set_properties", scene=S["scene"], entity=entity, values={component: values})


async def test_zero_cost(ed: Editor):
    ed.require_tools("get_atmosphere_info", "create_entity", "set_properties")
    s = await setup(ed)
    got = await info(ed)
    ed.check_eq(got["passes"], [], "no atmosphere: no pass")
    ed.check_eq(got["vramBytes"], 0, "no atmosphere: no memory")

    fog = (await ed.call("create_entity", scene=s["scene"], type="fog", name="Fog"))["entity_id"]
    s["fog"] = fog
    await set_values(ed, fog, "fog", type="exponential", density=0.02)
    ed.check_eq((await info(ed))["passes"], [], "a distance fog runs no atmosphere pass")

    await set_values(ed, fog, "fog", type="height", skyFill=False)
    got = await info(ed)
    ed.check_eq(got["fog"]["type"], "height")
    ed.check_eq(got["passes"], [], "a height fog without sky fill runs no pass")
    ed.check_eq(got["vramBytes"], 0)

    await set_values(ed, fog, "fog", skyFill=True)
    labels = [p["label"] for p in (await info(ed))["passes"]]
    ed.check_eq(labels, ["Atmosphere/SkyComposite"], "the sky fill is one pass")


async def test_legacy_fog_identical(ed: Editor):
    """A distance fog ignores the height fog's fields: setting them changes no pixel."""
    s = await setup(ed)
    await set_values(ed, s["fog"], "fog", type="linear", linearStart=2.0, linearEnd=40.0)
    await ed.call("wait", frames=10)
    before = await ed.screenshot("game")
    await set_values(ed, s["fog"], "fog", attenuationDistance=33.0, baseHeight=5.0, maximumHeight=9.0, colorMode="sky",
                     volumetric=True, directionalInscattering=1.0)
    await ed.call("wait", frames=10)
    after = await ed.screenshot("game")
    ed.check_eq(image_difference(before, after), 0.0, "the linear fog renders the same")
    ed.check_eq((await info(ed))["passes"], [], "a linear fog marked volumetric still runs nothing")


async def test_volumetric_tiers(ed: Editor):
    s = await setup(ed)
    await set_values(ed, s["fog"], "fog", type="height", volumetric=True, attenuationDistance=120.0, maximumHeight=40.0)
    steps = {"low": 16, "medium": 32, "high": 64}
    for quality, count in steps.items():
        await ed.call("configure_game_view", atmosphere_quality=quality)
        got = await info(ed)
        vol = got["fog"]["volumetric"]
        ed.check(vol["active"], f"{quality}: the volumetric fog runs ({got})")
        ed.check_eq(got["quality"], quality)
        ed.check_eq(vol["steps"], count, f"{quality} steps")
        labels = [p["label"] for p in got["passes"]]
        for label in ("Atmosphere/DepthDownsample", "Atmosphere/VolumetricFog", "Atmosphere/Denoise",
                      "Atmosphere/VolumetricComposite"):
            ed.check_in(label, labels, quality)
        ed.check(got["vramBytes"] > 0, "the targets count")
    # Off turns it off
    await ed.call("configure_game_view", atmosphere_quality="off")
    got = await info(ed)
    ed.check(not got["fog"]["volumetric"]["active"], "Off: no volumetric fog")
    await ed.call("configure_game_view", atmosphere_quality="project")


async def test_local_volume_caps(ed: Editor):
    s = await setup(ed)
    for i in range(10):
        made = await ed.call("create_entity", scene=s["scene"], type="local_volumetric_fog", name=f"Volume{i}")
        await ed.call("set_transform", scene=s["scene"], entity=made["entity_id"], position=[-6 + i * 1.5, 1, -4 - i])
    for quality, cap in (("low", 2), ("medium", 4), ("high", 8)):
        await ed.call("configure_game_view", atmosphere_quality=quality)
        vol = (await info(ed))["fog"]["volumetric"]
        ed.check_eq(vol["volumes"]["used"], cap, f"{quality}: volumes used")
        ed.check_eq(vol["volumes"]["culled"], 10 - cap, f"{quality}: volumes left out")
    await ed.call("configure_game_view", atmosphere_quality="project")


async def test_clouds_modes(ed: Editor):
    s = await setup(ed)
    clouds = (await ed.call("create_entity", scene=s["scene"], type="volumetric_clouds", name="Clouds"))["entity_id"]
    s["clouds"] = clouds
    for quality, mode in (("low", "sky_map"), ("medium", "sky_map"), ("high", "screen")):
        await ed.call("configure_game_view", atmosphere_quality=quality)
        got = await info(ed)
        for _ in range(30):
            if got["clouds"]["noiseReady"] and got["clouds"]["active"]:
                break
            await ed.call("wait", frames=10)
            got = await info(ed)
        ed.check(got["clouds"]["active"], f"{quality}: the clouds draw ({got['clouds']})")
        ed.check_eq(got["clouds"]["mode"], mode, quality)
        labels = [p["label"] for p in got["passes"]]
        ed.check_in("Atmosphere/CloudsScreen" if mode == "screen" else "Atmosphere/CloudsSkyMap", labels, quality)
    # forced Sky Map at High
    await set_values(ed, clouds, "volumetricclouds", rendering="sky_map")
    ed.check_eq((await info(ed))["clouds"]["mode"], "sky_map", "rendering sky_map wins over the tier")
    await ed.call("configure_game_view", atmosphere_quality="project")


async def test_lua_api(ed: Editor):
    """AC14: a script makes the fog a volumetric height fog, adds clouds and turns the atmosphere off."""
    ed.require_tools("play", "eval_lua")
    s = await setup(ed)
    await set_values(ed, s["fog"], "fog", type="linear", volumetric=False)
    if s.get("clouds"):
        await ed.call("delete_entities", scene=s["scene"], entities=[s["clouds"]])
        s.pop("clouds")
    await ed.call("play", action="start", scene=s["scene"])
    try:
        await ed.call("wait", frames=10)
        await ed.call("eval_lua", code="local fog = object('Fog', 'Fog')\n"
                                       "fog.type = FogType.HEIGHT\nfog.volumetric = true\nfog.attenuationDistance = 80")
        await ed.call("wait", frames=10)
        got = await ed.call("get_atmosphere_info", view="game")
        ed.check_eq(got["fog"]["type"], "height", "Lua set the height fog")
        ed.check(got["fog"]["volumetric"]["active"], "Lua turned the volumetric fog on")

        await ed.call("eval_lua", code="clouds = VolumetricClouds(scene)\nclouds.preset = CloudsPreset.OVERCAST")
        await ed.call("wait", frames=20)
        got = await ed.call("get_atmosphere_info", view="game")
        ed.check(got["clouds"]["present"], "Lua made clouds")

        await ed.call("eval_lua", code="Engine.setAtmosphereQuality(AtmosphereQuality.OFF)")
        await ed.call("wait", frames=5)
        got = await ed.call("get_atmosphere_info", view="game")
        ed.check_eq(got["quality"], "off", "the runtime override")
        ed.check(not got["fog"]["volumetric"]["active"], "Off: no volumetric fog")
        ed.check(not got["clouds"]["active"], "Off: no clouds")
        quality = await ed.call("eval_lua", code="return Engine.getAtmosphereQuality() == AtmosphereQuality.OFF")
        ed.check("true" in str(quality).lower(), f"Engine.getAtmosphereQuality: {quality}")
    finally:
        await ed.call("eval_lua", code="Engine.resetAtmosphereQuality()")
        await ed.call("play", action="stop")
