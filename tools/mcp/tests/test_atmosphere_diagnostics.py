"""Vapor's atmosphere diagnostics and opacity through get_atmosphere_info (docs/specs/S101-vapor.md R17,
R18, R34, AC9, AC10): each state an agent meets when "the fog isn't showing" comes back as a
diagnostic naming the cause, its entity and its fix, with the tool route in `next`; fixing the
cause clears it. The opacity estimate thins and thickens with the fog and says it counts the
analytic fog only. The diagnostics themselves are unit-tested one per code
(engine/tests/core/render/AtmosphereDiagnosticsTest.cpp), the opacity against the GPU by
AtmosphereMathGpu.OpacityMatchesGpuAnalytic.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Atmosphere Diagnostics CI"}
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> dict:
    """A 3D scene of its own with a ground, a sun, a main camera and a volumetric height fog, selected."""
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="FogDoctor", type="3D", path="scenes/FogDoctor")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    ground = await ed.call("create_entity", scene=S["scene"], type="plane", name="Ground")
    await ed.call("set_transform", scene=S["scene"], entity=ground["entity_id"], scale=[50, 1, 50])
    S["sun"] = (await ed.call("create_entity", scene=S["scene"], type="directional_light", name="Sun"))["entity_id"]
    # the main camera the opacity is estimated along: 2 m up, looking at the origin
    camera = (await ed.call("create_entity", scene=S["scene"], type="camera", name="Main Camera", position=[0, 2, 10]))["entity_id"]
    await ed.call("set_main_camera", scene=S["scene"], entity=camera)
    S["fog"] = (await ed.call("create_entity", scene=S["scene"], type="fog", name="Fog"))["entity_id"]
    # no light shafts: whether the sun's shadow map draws is not what these tests are about
    await set_fog(ed, type="height", volumetric=True, attenuationDistance=120.0, maximumHeight=40.0, lightShafts=False)
    return S


async def set_fog(ed: Editor, **values):
    await ed.call("set_properties", scene=S["scene"], entity=S["fog"], values={"fog": values})


async def info(ed: Editor, view: str = "game") -> dict:
    """What the view ran, after it drew a few frames."""
    await ed.screenshot(view)
    await ed.call("wait", frames=4)
    await ed.screenshot(view)
    return await ed.call("get_atmosphere_info", scene=S["scene"], view=view)


def codes(got: dict, entity=None) -> list[str]:
    return [d["code"] for d in got["diagnostics"] if entity is None or d.get("entity") == entity]


def diagnostic(ed: Editor, got: dict, code: str) -> dict:
    found = [d for d in got["diagnostics"] if d["code"] == code]
    ed.check(found, f"{code} reported: {got['diagnostics']}")
    return found[0] if found else {}


async def test_opacity_round_trip(ed: Editor):
    ed.require_tools("get_atmosphere_info", "set_properties")
    await setup(ed)
    got = await info(ed)
    opacity = got["opacity"]
    ed.check(opacity is not None, f"a Fog and a main camera: an estimate ({got})")
    ed.check_eq(opacity["includes"], "analytic", "it says what it counts")
    ed.check(0.0 <= opacity["at10"] <= opacity["at50"] <= opacity["at200"] <= 1.0, f"thicker with distance: {opacity}")
    warnings = [d["code"] for d in got["diagnostics"] if d["severity"] != "info"]
    ed.check_eq(warnings, [], "a working volumetric fog has no warning")
    ed.check("next" not in got, "nothing to fix, no next")

    # far too thin: invisible, with the fix and its route
    await set_fog(ed, attenuationDistance=1000000.0)
    got = await info(ed)
    ed.check(got["opacity"]["at200"] < 0.02, f"thin: {got['opacity']}")
    thin = diagnostic(ed, got, "fog_invisible")
    ed.check_eq(thin.get("entity"), S["fog"])
    ed.check_in("Attenuation Distance", thin.get("fix", ""))
    ed.check_in("fog_invisible", got.get("next", ""), "next names the cause")

    # far too dense: opaque
    await set_fog(ed, attenuationDistance=0.5)
    got = await info(ed)
    ed.check(got["opacity"]["at10"] > 0.95, f"dense: {got['opacity']}")
    ed.check_in("fog_opaque", codes(got))
    ed.check("fog_invisible" not in codes(got))

    await set_fog(ed, attenuationDistance=120.0)
    got = await info(ed)
    ed.check("fog_opaque" not in codes(got) and "fog_invisible" not in codes(got), f"fixed: {got['diagnostics']}")


async def test_local_volumes_need_the_volumetric_fog(ed: Editor):
    ed.require_tools("create_entity", "get_atmosphere_info")
    s = await setup(ed)
    volume = (await ed.call("create_entity", scene=s["scene"], type="local_volumetric_fog", name="Cave Mist"))["entity_id"]
    s["volume"] = volume
    await ed.call("set_transform", scene=s["scene"], entity=volume, position=[0, 1, -4])
    await set_fog(ed, volumetric=False)
    got = await info(ed)
    needs = diagnostic(ed, got, "volumetric_needs_height")
    ed.check_eq(needs.get("entity"), s["fog"], "about the Fog, where the fix is")
    ed.check_in("Volumetric Fog", needs.get("fix", ""))

    await set_fog(ed, volumetric=True)
    got = await info(ed)
    ed.check("volumetric_needs_height" not in codes(got), f"fixed: {got['diagnostics']}")

    # a volume past the volumetric distance is culled, and says why
    far = (await ed.call("create_entity", scene=s["scene"], type="local_volumetric_fog", name="Far Mist"))["entity_id"]
    await ed.call("set_transform", scene=s["scene"], entity=far, position=[0, 1, -400])
    got = await info(ed)
    ed.check_in("volume_culled", codes(got, far), f"the far volume: {got['diagnostics']}")
    ed.check("volume_culled" not in codes(got, volume), "the near one is drawn")
    await ed.call("delete_entities", scene=s["scene"], entities=[far])


async def test_quality_off_names_the_game_view(ed: Editor):
    ed.require_tools("configure_game_view", "get_atmosphere_info")
    await setup(ed)
    await ed.call("configure_game_view", atmosphere_quality="off")
    got = await info(ed)
    off = diagnostic(ed, got, "volumetric_quality_off")
    ed.check_in("Game view", off.get("message", ""))
    ed.check_in("configure_game_view", got.get("next", ""), "next routes to the Game view's setting")
    await ed.call("configure_game_view", atmosphere_quality="project")
    got = await info(ed)
    ed.check("volumetric_quality_off" not in codes(got), f"fixed: {got['diagnostics']}")


async def test_scene_view_fog_toggle(ed: Editor):
    ed.require_tools("set_scene_camera", "get_atmosphere_info")
    s = await setup(ed)
    await ed.call("set_scene_camera", scene=s["scene"], display={"disable_fog": True})
    got = await info(ed)
    diagnostic(ed, got, "scene_view_fog_disabled")
    ed.check_in("set_scene_camera", got.get("next", ""), "next routes to the display setting")
    await ed.call("set_scene_camera", scene=s["scene"], display={"disable_fog": False})
    got = await info(ed)
    ed.check("scene_view_fog_disabled" not in codes(got), f"fixed: {got['diagnostics']}")


async def test_second_fog_ignored(ed: Editor):
    ed.require_tools("create_entity", "delete_entities", "get_atmosphere_info")
    s = await setup(ed)
    second = (await ed.call("create_entity", scene=s["scene"], type="fog", name="Second Fog"))["entity_id"]
    got = await info(ed)
    ignored = diagnostic(ed, got, "fog_second_ignored")
    ed.check_eq(ignored.get("entity"), second)
    await ed.call("delete_entities", scene=s["scene"], entities=[second])
    got = await info(ed)
    ed.check("fog_second_ignored" not in codes(got), f"fixed: {got['diagnostics']}")
