"""S22 P3, water and buoyancy as an agent sees them (docs/specs/S22-gameplay.md AC16, R30-R32, R42).
A Water Volume on the GameObject menu's Water (a library:water grid with WAVES) and a crate with a
Buoyancy at half the water's density. In Play, the crate floats about half under. At one frame,
sample_water and Lua Water.sampleHeight give the same surface within 1e-5: both read the waves of
the material on the frame's shader clock.

Scene Buoyancy (3D). Play runs with auto_pause, so both reads happen in the same frame.
"""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Buoyancy CI"}

TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Buoyancy", type="3D", path="scenes/Buoyancy")
    S["scene"] = made["scene"]["id"]
    water = await ed.call("create_entity", scene=S["scene"], type="water", name="Lake")
    S["water"] = water["entity_id"]
    await ed.call("add_component", scene=S["scene"], entity=S["water"], component="WaterVolume",
                  values={"watervolume": {"size": [20, 6, 20]}})
    crate = await ed.call("create_entity", scene=S["scene"], type="cube", name="Crate", position=[2, 1, -3])
    S["crate"] = crate["entity_id"]
    await ed.call("edit_colliders", scene=S["scene"], entity=S["crate"], body_type="dynamic",
                  add=[{"shape": "box", "size": [1, 1, 1], "density": 500, "fit": False}])
    await ed.call("add_component", scene=S["scene"], entity=S["crate"], component="Buoyancy")
    await ed.call("save_scene", scene=S["scene"])
    return S


async def test_components_listed_with_defaults(ed: Editor):
    s = await setup(ed)
    got = await ed.call("get_entity", scene=s["scene"], entity=s["water"], components=["WaterVolume"])
    volume = got["components"]["watervolume"]
    ed.check_eq((volume["density"], volume["waves"], volume["infinite"]), (1000, "from_material", False), f"{volume}")
    ed.check_eq(volume["size"], [20, 6, 20], f"{volume}")
    got = await ed.call("get_entity", scene=s["scene"], entity=s["crate"], components=["Buoyancy"])
    buoyancy = got["components"]["buoyancy"]
    ed.check_eq((buoyancy["columns"], buoyancy["volumeScale"], buoyancy["dragScale"]), ("auto_2x2", 1, 1), f"{buoyancy}")


async def test_edit_mode_sample(ed: Editor):
    """Usable in edit mode: in the volume under the surface, above it, and outside it."""
    s = await setup(ed)
    got = await ed.call("sample_water", scene=s["scene"], points=[[1, -2, 2], [1, 3, 2], [40, -1, 0]])
    inside, above, outside = got["samples"]
    ed.check_eq(inside["in_water"], True, f"{inside}")
    ed.check(abs(inside["surface_height"]) <= 0.15 + 1e-4, f"the surface within the waves: {inside}")
    ed.check_eq(inside["volume"]["id"], s["water"], f"{inside}")
    ed.check_eq(above["in_water"], False, f"{above}")
    ed.check(abs(above["surface_height"] - inside["surface_height"]) < 1e-6, f"the same surface over x, z: {above}")
    ed.check_eq((outside["in_water"], outside["surface_height"]), (False, None), f"{outside}")


async def test_play_floats_and_samples_agree(ed: Editor):
    s = await setup(ed)
    st = await ed.call("get_play_state")
    if st["state"] != "stopped":
        await ed.call("play", action="stop")
    started = await ed.call("play", action="start", scene=s["scene"], auto_pause=True, fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"start: {started}")
    try:
        await ed.call("wait", frames=480)
        live = await ed.call("get_runtime_entity", entity=s["crate"], components=["Transform"])
        y = live["components"]["transform"]["position"][1]
        # half its density: about half under, so its centre near the (waving) surface
        ed.check(-0.35 < y < 0.35, f"the crate floats half under: y {y}")

        # one round-trip: both read the same frame's water clock (the scene clock)
        both = await ed.call("batch", calls=[
            {"tool": "sample_water", "arguments": {"points": [[1.5, 0, 2.5]]}},
            {"tool": "eval_lua", "arguments": {"code": "return Water.sampleHeight(scene, 1.5, 2.5), Water.getTime(scene)"}},
            {"tool": "eval_lua", "arguments": {"code": "local w = Water.sample(scene, Vector3(1.5, -1, 2.5)); "
                                                       "return w.inWater, w.surfaceHeight, w.volume"}}])
        ed.check_eq(both["failed"], 0, f"{both}")
        got = both["results"][0]["structured"]
        lua = both["results"][1]["structured"]["values"]
        tool = got["samples"][0]["surface_height"]
        ed.check(abs(tool - lua[0]) <= 1e-5, f"sample_water {tool} = Water.sampleHeight {lua[0]}")
        ed.check(abs(got["water_time"] - lua[1]) <= 1e-6, f"the same water time: {got['water_time']} {lua[1]}")
        sample = both["results"][2]["structured"]["values"]
        ed.check_eq(sample[0], True, f"Water.sample in water: {sample}")
        ed.check(abs(sample[1] - tool) <= 1e-5, f"Water.sample's surface: {sample}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
