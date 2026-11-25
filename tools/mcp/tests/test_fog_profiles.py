"""Vapor's Fog Profiles and Fog Zones through MCP (docs/specs/S101-vapor.md R31-R35, R43, AC12, AC13):
linking a built-in makes its project copy in the same undo step, edit_fog_profile edits the file
as one undo step of the project, a Fog Zone's weight follows the camera in get_atmosphere_info,
the profile and zone diagnostics come and go with their cause, unlinking keeps the look,
set_properties refuses a field the profile sets and names both fixes, and in Play a Lua blendTo,
a clone and a script write act on the resolved look only, gone after Stop.

Runs in one new empty project; tests run in order and share the editor.
"""

from __future__ import annotations

import json

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Fog Profiles CI"}
TIMEOUT = 900

S: dict = {}
NIGHT = "FogProfiles/night.fogprofile"


async def setup(ed: Editor) -> dict:
    """A 3D scene of its own with a sun, a main camera at the origin and a linear Fog."""
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="FogLooks", type="3D", path="scenes/FogLooks")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("create_entity", scene=S["scene"], type="directional_light", name="Sun")
    S["camera"] = (await ed.call("create_entity", scene=S["scene"], type="camera", name="Main Camera", position=[0, 2, 0]))["entity_id"]
    await ed.call("set_main_camera", scene=S["scene"], entity=S["camera"])
    S["fog"] = (await ed.call("create_entity", scene=S["scene"], type="fog", name="Fog"))["entity_id"]
    return S


async def info(ed: Editor, **args) -> dict:
    """get_atmosphere_info of the Game view after it drew a few frames."""
    await ed.call("wait", frames=3)
    return await ed.call("get_atmosphere_info", scene=S["scene"], view="game", **args)


async def fog_values(ed: Editor) -> dict:
    got = await ed.call("get_entity", scene=S["scene"], entity=S["fog"], components=["fog"])
    return got["components"]["fog"]


def codes(got: dict, entity=None) -> list[str]:
    return [d["code"] for d in got["diagnostics"] if entity is None or d.get("entity") == entity]


def near(a: float, b: float, tolerance: float = 1e-3) -> bool:
    return abs(a - b) <= tolerance * max(1.0, abs(b))


async def test_group_lists_edit_fog_profile(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_fog_profile" not in default, "hidden until rendering_fx is enabled")
    await ed.call("tool_groups", enable=["rendering_fx"])
    ed.check("edit_fog_profile" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")


async def test_list_fog_profiles(ed: Editor):
    ed.require_tools("list_atmosphere_presets")
    await setup(ed)
    got = await ed.call("list_atmosphere_presets", kind="fog")
    ids = [p["id"] for p in got["presets"]]
    for wanted in ("morning_haze", "ground_mist", "mountain_valley", "horizon_haze", "night", "bright_haze", "murky", "infernal", "toxic"):
        ed.check_in(wanted, ids, "every built-in is listed")
    night = next(p for p in got["presets"] if p["id"] == "night")
    ed.check(night["overrides"] > 0 and night["name"] == "Night" and night["description"], f"its name, description, overrides: {night}")
    ed.check_eq(night["usedBy"], 0, "nothing uses it yet")
    ed.check("copy" not in night, "no project copy yet")
    ed.check_eq(got["profiles"], [], "the project has no profiles")


async def test_link_builtin_makes_its_copy(ed: Editor):
    ed.require_tools("apply_atmosphere_preset", "undo", "redo")
    s = await setup(ed)
    copy = ed.project / NIGHT
    linked = await ed.call("apply_atmosphere_preset", scene=s["scene"], entity=s["fog"], preset="night")
    ed.check_eq(linked["profile"], NIGHT, "linked to the project's copy")
    ed.check(linked["created"] and linked["typeChanged"], f"the copy made and the Fog made Height in that step: {linked}")
    ed.check(copy.is_file(), "the copy is written")
    ed.check("next" not in linked, "one user: no hint")
    ed.check_eq((await fog_values(ed))["type"], "height")
    got = await info(ed)
    ed.check_eq(got["fog"]["profile"], NIGHT)
    meta = (ed.project / (NIGHT + ".meta")).read_text()

    # one undo step: the link, the Type and the copy go together
    await ed.call("undo", scope="scene", scene=s["scene"])
    ed.check(not copy.exists(), "undo deletes the copy")
    values = await fog_values(ed)
    ed.check(not values.get("profile"), f"unlinked: {values.get('profile')}")
    ed.check_eq(values["type"], "linear")
    await ed.call("redo", scope="scene", scene=s["scene"])
    ed.check(copy.is_file(), "redo writes it again")
    ed.check_eq((ed.project / (NIGHT + ".meta")).read_text(), meta, "with the same GUID")

    listed = await ed.call("list_atmosphere_presets", kind="fog")
    night = next(p for p in listed["presets"] if p["id"] == "night")
    ed.check_eq(night.get("copy"), NIGHT)
    ed.check_eq(night["usedBy"], 1)
    ed.check_eq([p["path"] for p in listed["profiles"]], [NIGHT])

    # linking it again reuses the copy and changes nothing
    again = await ed.call("apply_atmosphere_preset", scene=s["scene"], entity=s["fog"], preset="builtin:night")
    ed.check_eq(again["changed"], False)
    error = await ed.call("apply_atmosphere_preset", scene=s["scene"], entity=s["fog"], preset="nope", expect_error=True)
    ed.check_in("morning_haze", error, "an unknown preset lists the built-ins")
    ed.check_in(NIGHT, error, "and the project's profiles")


async def test_edit_profile_and_undo(ed: Editor):
    ed.require_tools("edit_fog_profile", "undo")
    s = await setup(ed)
    before = (ed.project / NIGHT).read_text()
    edited = await ed.call("edit_fog_profile", path=NIGHT, set={"attenuationDistance": 33.0, "noiseIntensity": 0.25})
    ed.check(edited["changed"], f"{edited}")
    ed.check_eq(edited["fields"]["attenuationDistance"], 33)
    ed.check_eq(edited["fields"]["noiseIntensity"], 0.25, "set ticks a field the profile did not set")
    ed.check_eq(edited["users"], [{"scene": s["scene"], "sceneName": "FogLooks", "entity": s["fog"], "kind": "fog"}])
    ed.check_eq(edited["resolved"]["fields"]["attenuationDistance"], {"value": 33, "source": "profile"}, "the open scene shows it")
    ed.check_in('"attenuationDistance": 33', (ed.project / NIGHT).read_text())
    got = await info(ed, resolved=True)
    ed.check_eq(got["resolved"]["attenuationDistance"]["value"], 33)

    cleared = await ed.call("edit_fog_profile", path=NIGHT, clear=["noiseIntensity"])
    ed.check("noiseIntensity" not in cleared["fields"], "clear unticks")
    ed.check_eq(cleared["resolved"]["fields"]["noiseIntensity"]["source"], "fog", "the Fog's own value shows again")

    await ed.call("undo", scope="project")
    await ed.call("undo", scope="project")
    ed.check_eq((ed.project / NIGHT).read_text(), before, "two undo steps put the file back")
    got = await info(ed, resolved=True)
    ed.check_eq(got["resolved"]["attenuationDistance"], {"value": 200, "source": "profile"}, "and the look")

    bad = await ed.call("edit_fog_profile", path=NIGHT, set={"volumetric": True}, expect_error=True)
    ed.check_in("set_properties", bad, "technique fields stay on the Fog")
    bad = await ed.call("edit_fog_profile", path=NIGHT, set={"thickness": 2}, expect_error=True)
    ed.check_in("attenuationDistance", bad, "an unknown field lists the fields")
    bad = await ed.call("edit_fog_profile", path="FogProfiles/none.fogprofile", set={"density": 1}, expect_error=True)
    ed.check_in("from", bad, "a new file needs from")


async def test_set_properties_refuses_profile_fields(ed: Editor):
    ed.require_tools("set_properties")
    s = await setup(ed)
    error = await ed.call("set_properties", scene=s["scene"], entity=s["fog"], values={"fog": {"attenuationDistance": 50.0}},
                          expect_error=True)
    for part in (NIGHT, "used by 1", "edit_fog_profile {path: \"" + NIGHT + "\", set: {\"attenuationDistance\":50.0}}",
                 "from: {profile: \"" + NIGHT + "\"}", "apply_atmosphere_preset"):
        ed.check_in(part, error, "the refusal names the profile, its users and both fixes")
    ed.check_eq((await fog_values(ed))["attenuationDistance"], 400, "nothing changed")
    # a field the profile does not set writes as before
    await ed.call("set_properties", scene=s["scene"], entity=s["fog"], values={"fog": {"noiseIntensity": 0.5, "volumetric": True}})
    values = await fog_values(ed)
    ed.check_eq(values["noiseIntensity"], 0.5)
    await ed.call("set_properties", scene=s["scene"], entity=s["fog"], values={"fog": {"noiseIntensity": 0.0, "volumetric": False}})


async def test_duplicate_for_one_scene(ed: Editor):
    """A second scene's Fog linked to the same profile: the link says it is shared, a copy changes one scene only."""
    ed.require_tools("edit_fog_profile", "apply_atmosphere_preset")
    s = await setup(ed)
    other = (await ed.call("create_scene", name="FogCave", type="3D", path="scenes/FogCave"))["scene"]["id"]
    other_fog = (await ed.call("create_entity", scene=other, type="fog", name="Fog"))["entity_id"]
    shared = await ed.call("apply_atmosphere_preset", scene=other, entity=other_fog, preset=NIGHT)
    ed.check_eq(shared["usedBy"], 2)
    ed.check_in("edit_fog_profile", shared.get("next", ""), "a shared profile's next routes to edit_fog_profile")
    ed.check_in("from: {profile", shared.get("next", ""), "and to a copy for one scene")

    cave = "FogProfiles/cave.fogprofile"
    made = await ed.call("edit_fog_profile", path=cave, **{"from": {"profile": NIGHT}}, set={"attenuationDistance": 20.0})
    ed.check(made["created"] and made["usedBy"] == 0, f"{made}")
    ed.check_in("apply_atmosphere_preset", made.get("next", ""), "an unused profile's next says how to link it")
    linked = await ed.call("apply_atmosphere_preset", scene=other, entity=other_fog, preset=cave)
    ed.check_eq(linked["usedBy"], 1)
    got = await ed.call("get_atmosphere_info", scene=other, view="scene", resolved=True)
    ed.check_eq(got["resolved"]["attenuationDistance"]["value"], 20)
    mine = await info(ed, resolved=True)
    ed.check_eq(mine["resolved"]["attenuationDistance"]["value"], 200, "the first scene keeps the shared look")
    exists = await ed.call("edit_fog_profile", path=cave, **{"from": {"builtin": "murky"}}, expect_error=True)
    ed.check_in("exists", exists)
    await ed.call("select_scene", scene=s["scene"])


async def test_zone_weight_follows_the_camera(ed: Editor):
    ed.require_tools("create_entity", "set_transform", "get_atmosphere_info")
    s = await setup(ed)
    zone = (await ed.call("create_entity", scene=s["scene"], type="fog_zone", name="Cave"))["entity_id"]
    s["zone"] = zone
    await ed.call("set_properties", scene=s["scene"], entity=zone,
                  values={"fogzone": {"size": [10, 10, 10], "blendDistance": 10.0, "priority": 2, "weight": 1.0}})
    linked = await ed.call("apply_atmosphere_preset", scene=s["scene"], entity=zone, preset="ground_mist")
    ed.check_eq(linked["component"], "fogzone")
    for x, weight in ((0, 1.0), (10, 0.5), (30, 0.0)):
        await ed.call("set_transform", scene=s["scene"], entity=s["camera"], position=[x, 0, 0])
        got = await info(ed, resolved=True)
        zones = {z["entity"]: z for z in got["zones"]}
        ed.check(zone in zones, f"the zone is listed: {got['zones']}")
        ed.check(near(zones[zone]["weight"], weight, 0.02), f"camera at x={x}: weight {zones[zone]['weight']}, expected {weight}")
        ed.check_eq(zones[zone]["priority"], 2)
        source = got["resolved"]["attenuationDistance"]["source"]
        ed.check_eq(source, f"zone:{zone}" if weight > 0 else "profile", f"at x={x} the field comes from")
    await ed.call("set_transform", scene=s["scene"], entity=s["camera"], position=[0, 2, 0])


async def test_diagnostics_round_trip(ed: Editor):
    ed.require_tools("edit_fog_profile", "get_atmosphere_info")
    s = await setup(ed)
    zone = s["zone"]
    await ed.call("set_properties", scene=s["scene"], entity=zone, values={"fogzone": {"weight": 0.0}})
    got = await info(ed)
    ed.check_in("zone_no_effect", codes(got, zone), f"weight 0: {got['diagnostics']}")
    ed.check_in("zone_no_effect", got.get("next", ""), "next names it")
    await ed.call("set_properties", scene=s["scene"], entity=zone, values={"fogzone": {"weight": 1.0}})
    got = await info(ed)
    ed.check("zone_no_effect" not in codes(got), f"fixed: {got['diagnostics']}")

    # a profile with only a distance fog's field, on a zone over a Height fog
    dist = "FogProfiles/distance.fogprofile"
    await ed.call("edit_fog_profile", path=dist, **{"from": {"builtin": "night"}})
    every = list((await ed.call("edit_fog_profile", path=dist))["fields"])
    await ed.call("edit_fog_profile", path=dist, clear=every, set={"density": 0.2})
    await ed.call("apply_atmosphere_preset", scene=s["scene"], entity=zone, preset=dist)
    got = await info(ed)
    ed.check_in("profile_no_effect", codes(got, zone), f"{got['diagnostics']}")
    await ed.call("edit_fog_profile", path=dist, set={"attenuationDistance": 30.0})
    got = await info(ed)
    ed.check("profile_no_effect" not in codes(got), f"fixed: {got['diagnostics']}")

    # Fog Zones without a Fog
    lonely = (await ed.call("create_scene", name="FogLonely", type="3D", path="scenes/FogLonely"))["scene"]["id"]
    lone = (await ed.call("create_entity", scene=lonely, type="fog_zone", name="Zone"))["entity_id"]
    await ed.call("apply_atmosphere_preset", scene=lonely, entity=lone, preset=dist)
    got = await ed.call("get_atmosphere_info", scene=lonely, view="scene")
    ed.check_in("fog_missing", codes(got), f"{got['diagnostics']}")
    ed.check_in("create_entity", got.get("next", ""))
    await ed.call("create_entity", scene=lonely, type="fog", name="Fog")
    got = await ed.call("get_atmosphere_info", scene=lonely, view="scene")
    ed.check("fog_missing" not in codes(got), f"fixed: {got['diagnostics']}")
    await ed.call("select_scene", scene=s["scene"])


async def test_write_file_takes_fog_profiles(ed: Editor):
    """A .fogprofile is JSON text: write_file and read_file take it, and the project lists the profile."""
    ed.require_tools("write_file", "read_file", "list_atmosphere_presets")
    await setup(ed)
    path = "FogProfiles/written.fogprofile"
    text = json.dumps({"version": 1, "name": "Written", "fields": {"attenuationDistance": 75.0}}, indent=2) + "\n"
    await ed.call("write_file", path=path, content=text)
    got = await ed.call_raw("read_file", {"path": path})
    ed.check(not got.is_error and "attenuationDistance" in got.text, f"read_file reads it back: {got.text[:300]}")
    listed = await ed.call("list_atmosphere_presets", kind="fog")
    written = [p for p in listed["profiles"] if p["path"] == path]
    ed.check(written and written[0]["name"] == "Written" and written[0]["overrides"] == 1, f"listed: {listed['profiles']}")


async def test_unlink_keeps_the_look(ed: Editor):
    ed.require_tools("apply_atmosphere_preset", "get_atmosphere_info")
    s = await setup(ed)
    await ed.call("delete_entities", scene=s["scene"], entities=[s["zone"]])
    linked = await info(ed, resolved=True)
    unlinked = await ed.call("apply_atmosphere_preset", scene=s["scene"], entity=s["fog"], unlink=True)
    ed.check(unlinked["changed"] and unlinked["profile"] is None, f"{unlinked}")
    got = await info(ed, resolved=True)
    ed.check(got["fog"]["profile"] is None, "no profile")
    for field, value in linked["resolved"].items():
        ed.check_eq(got["resolved"][field]["value"], value["value"], f"{field} looks the same")
        ed.check_eq(got["resolved"][field]["source"], "fog", f"{field} is the Fog's own")
    await ed.call("undo", scope="scene", scene=s["scene"])
    got = await info(ed)
    ed.check_eq(got["fog"]["profile"], NIGHT, "undo links it again")


async def test_play_blend_clone_and_runtime_writes(ed: Editor):
    """AC13: in Play a script's blendTo, clone():set and writes reach the resolved look only."""
    ed.require_tools("play", "eval_lua", "get_atmosphere_info")
    s = await setup(ed)
    profile_file = ed.project / NIGHT
    file_before = profile_file.read_text()
    own = (await fog_values(ed))["attenuationDistance"]
    await ed.call("save_scene", scene=s["scene"])
    await ed.call("play", action="start", scene=s["scene"], fixed_fps=60, auto_pause=True)
    try:
        lua = "local fog = object('Fog', 'Fog')\n"
        resolved = "return object('Fog', 'Fog'):getResolved('attenuationDistance')"
        ed.check(near((await ed.call("eval_lua", code=resolved))["values"][0], 200), "the profile's value")

        # a write to a field the profile sets goes above it
        await ed.call("eval_lua", code=lua + "fog.attenuationDistance = 7")
        ed.check(near((await ed.call("eval_lua", code=resolved))["values"][0], 7), "the script's write shows")
        await ed.call("eval_lua", code=lua + "fog:clearOverrides()")

        # blendTo(nil) fades from the profile to the Fog's own fields over a second of game time
        await ed.call("eval_lua", code=lua + "fog:blendTo(nil, 1.0)")
        ed.check(near((await ed.call("eval_lua", code=resolved))["values"][0], 200, 0.02), "0%: the profile's look")
        await ed.call("play", action="step", frames=30)
        half = (await ed.call("eval_lua", code=resolved))["values"][0]
        expected = 1.0 / (0.5 / 200 + 0.5 / own)
        ed.check(near(half, expected, 0.05), f"50%: blended as extinction ({half}, expected {expected})")
        got = await ed.call("get_atmosphere_info", view="game")
        ed.check(got["blend"] and near(got["blend"]["progress"], 0.5, 0.05) and got["blend"]["target"] is None, f"{got['blend']}")
        await ed.call("play", action="step", frames=32)
        ed.check(near((await ed.call("eval_lua", code=resolved))["values"][0], own), "100%: the Fog's own value")
        ed.check_eq((await ed.call("eval_lua", code="return object('Fog', 'Fog').blending"))["values"][0], False)

        # a clone is the script's own: the asset file stays as it is
        await ed.call("eval_lua", code=lua + "local p = FogProfile.load('FogProfiles/night.fogprofile'):clone()\n"
                                             "p:set('attenuationDistance', 5)\nfog.profile = p")
        ed.check(near((await ed.call("eval_lua", code=resolved))["values"][0], 5), "the clone's value shows")
        ed.check_eq(profile_file.read_text(), file_before, "clone():set does not change the file")
        refused = await ed.call("edit_fog_profile", path=NIGHT, set={"density": 0.5}, expect_error=True)
        ed.check(refused, "edit_fog_profile is refused in Play")
    finally:
        await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
    got = await info(ed, resolved=True)
    ed.check_eq(got["fog"]["profile"], NIGHT, "after Stop the saved link is back")
    ed.check_eq(got["resolved"]["attenuationDistance"], {"value": 200, "source": "profile"}, "and no runtime layer")
    ed.check(got["blend"] is None, "no blend")
    ed.check_eq((await fog_values(ed))["attenuationDistance"], own, "the Fog's own field never changed")
    ed.check_eq(json.loads(profile_file.read_text())["fields"]["attenuationDistance"], 200)
