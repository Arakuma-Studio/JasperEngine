"""Water Excluders and Water Wakes as an agent drives and checks them
(docs/superpowers/specs/2026-11-04-water-boats-design.md): the components with their defaults and
undo, sample_water's excluded_by (enabled, hidden and inactive excluders), its report (the hulls,
which waters take the excluder and wake variants), the hole in the Game view (sample_view_color),
cargo kept dry while the boat itself floats, no underwater fade inside a dry room, a wake behind
a moving hull (none ahead of it; staying on its water is WaterInteractionTest.WakesStayOnTheirWater),
and the scene's wake settings with undo."""

from __future__ import annotations

from harness import Editor, find_key

EDITOR = {"create": True, "name": "WaterBoatsTest"}
CI = True
TIMEOUT = 900

SCENE = "scenes/Boats.scene"

S: dict = {}

RUN = """local Run = {}
function Run:init()
    self.go = GameObject(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end
function Run:onUpdate()
    local p = self.go.position
    self.go.position = Vector3(p.x, p.y, p.z + 5 * Engine.deltatime)
end
return Run
"""


async def lua(ed: Editor, code: str, edit: bool = False):
    extra = {"allow_edit_mode": True} if edit else {}
    return (await ed.call("eval_lua", code=code, **extra))["values"]


def entity_id(made) -> int:
    return find_key(made, "entity_id", "id")


async def sample(ed: Editor, *points, report: bool = False) -> dict:
    args = {"points": [list(p) for p in points]} if points else {}
    if report:
        args["report"] = True
    return await ed.call("sample_water", **args)


async def frame(ed: Editor):
    """One editor frame of the stopped scene (a headless editor draws one per screenshot)."""
    await ed.call_raw("screenshot", {"view": "game"})


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("create_scene", name="Boats", path=SCENE)
    # a lake: the menu's water (Gerstner waves of 0.1 m) with a Water Volume, over a red bed
    S["lake"] = entity_id(await ed.call("create_entity", type="water", name="Lake"))
    await ed.call("add_component", entity=S["lake"], component="WaterVolume", values={"watervolume": {"size": [20, 6, 20]}})
    S["bed"] = entity_id(await ed.call("create_entity", type="plane", name="Bed", position=[0, -2, 0], scale=[3, 1, 3],
                                       values={"mesh": {"submeshes[0].material.baseColorFactor": [1, 0, 0, 1]}}))
    # a boat (a 2 x 1 x 4 box)
    S["boat"] = entity_id(await ed.call("create_entity", type="cube", name="Boat", position=[4, 0, 0], scale=[2, 1, 4]))
    # a camera straight down over the origin
    camera = entity_id(await ed.call("create_entity", type="camera", name="Top", position=[0, 12, 0],
                                     rotation={"euler": [-90, 0, 0]}))
    await ed.call("set_main_camera", entity=camera)
    await ed.call("save_scene")
    return S


async def test_components_defaults_and_undo(ed: Editor):
    s = await setup(ed)
    await ed.call("add_component", entity=s["boat"], component="WaterExcluder")
    got = (await ed.call("get_entity", entity=s["boat"], components=["WaterExcluder"]))["components"]["waterexcluder"]
    ed.check_eq((got["shape"], got["center"], got["size"], got["enabled"]), ("convex_hull", [0, 0, 0], [1, 1, 1], True), f"{got}")

    await ed.call("add_component", entity=s["boat"], component="WaterWake")
    wake = (await ed.call("get_entity", entity=s["boat"], components=["WaterWake"]))["components"]["waterwake"]
    ed.check_eq((wake["foam"], wake["waves"], wake["width"], wake["length"]), (1, 1, 0, 0), f"{wake}")
    await ed.call("undo")
    gone = await ed.call("get_entity", entity=s["boat"])
    ed.check("waterwake" not in gone["components"], "undo removes the Water Wake")
    await ed.call("redo")
    back = await ed.call("get_entity", entity=s["boat"])
    ed.check("waterwake" in back["components"], "redo adds it again")


async def test_sample_water_excluded(ed: Editor):
    s = await setup(ed)
    inside, outside = (4, -0.2, 0), (-4, -0.2, 6)
    got = await sample(ed, inside, outside)
    a, b = got["samples"]
    ed.check_eq(a["in_water"], False, f"inside the boat's hull: dry {a}")
    ed.check_eq((a["excluded_by"] or {}).get("id"), s["boat"], f"{a}")
    ed.check(a["surface_height"] is not None, f"the waterline is still reported: {a}")
    ed.check_eq((b["in_water"], b["excluded_by"]), (True, None), f"{b}")

    # disabled, hidden, inactive: none keeps water out
    await ed.call("set_properties", entity=s["boat"], values={"waterexcluder": {"enabled": False}})
    ed.check_eq((await sample(ed, inside))["samples"][0]["excluded_by"], None, "a disabled excluder")
    await ed.call("set_properties", entity=s["boat"], values={"waterexcluder": {"enabled": True}})
    await ed.call("set_properties", entity=s["boat"], values={"transform": {"visible": False}})
    ed.check_eq((await sample(ed, inside))["samples"][0]["excluded_by"], None, "a hidden excluder")
    await ed.call("set_properties", entity=s["boat"], values={"transform": {"visible": True}})
    await ed.call("set_entity_state", entity=s["boat"], active=False)
    ed.check_eq((await sample(ed, inside))["samples"][0]["excluded_by"], None, "an inactive excluder")
    await ed.call("set_entity_state", entity=s["boat"], active=True)
    ed.check_eq((await sample(ed, inside))["samples"][0]["in_water"], False, "back on")

    # Lua: the same answer, and the handle
    dry, excluder, hull = await lua(ed, "local w = Water.sample(scene, Vector3(4, -0.2, 0)); "
                                        "local e = object('Boat'):getWaterExcluder(); return w.excluded, w.excluder, e.hullStatus",
                                    edit=True)
    ed.check_eq((dry, excluder, hull), (True, s["boat"], "ready"), "Lua Water.sample and WaterExcluder.hullStatus")


async def test_report_variants(ed: Editor):
    s = await setup(ed)
    await frame(ed)
    got = await sample(ed, report=True)
    excluders = got["excluders"]
    ed.check(len(excluders) == 1 and excluders[0]["active"] and excluders[0]["hull_status"] == "ready" and excluders[0]["hull_faces"] >= 6,
             f"the boat's hull: {excluders}")
    lake = [w for w in got["waters"] if w["entity"]["id"] == s["lake"]]
    ed.check(lake and lake[0]["exclusion"] and lake[0]["wake"], f"the lake draws with both variants: {got['waters']}")
    ed.check_eq(got["wake_map"]["active"], False, "no wake while stopped")
    await ed.check_error("sample_water", {}, contains="points")

    # without the components the water draws its plain variant again
    await ed.call("remove_component", entity=s["boat"], component="WaterWake")
    await ed.call("remove_component", entity=s["boat"], component="WaterExcluder")
    for _ in range(3):
        await frame(ed)
    lake = [w for w in (await sample(ed, report=True))["waters"] if w["entity"]["id"] == s["lake"]]
    ed.check(lake and not lake[0]["exclusion"] and not lake[0]["wake"], f"no excluder, no wake: no bits {lake}")
    await ed.call("undo")
    await ed.call("undo")


async def test_hole_in_view(ed: Editor):
    s = await setup(ed)
    # a dry room under the camera: the red bed shows through the water there
    room = entity_id(await ed.call("create_entity", type="empty", name="Room", position=[0, 0, 0]))
    await ed.call("add_component", entity=room, component="WaterExcluder",
                  values={"waterexcluder": {"shape": "box", "size": [3, 2, 3]}})
    for _ in range(3):
        await frame(ed)
    probe = await ed.call_raw("sample_view_color", {"view": "game", "x": 0, "y": 0})
    ed.check(not probe.is_error, f"the Game view has an image: {probe.text}")
    size = probe.value["size"]
    x, y = size["width"] // 2, size["height"] // 2
    hole = (await ed.call("sample_view_color", view="game", x=x, y=y))["linear"]
    ed.check(hole[0] > 0.5 and hole[1] < 0.2 and hole[2] < 0.2, f"the red bed through the hole: {hole}")

    # red control: the excluder off, the water covers the bed again
    await ed.call("set_properties", entity=room, values={"waterexcluder": {"enabled": False}})
    for _ in range(3):
        await frame(ed)
    covered = (await ed.call("sample_view_color", view="game", x=x, y=y))["linear"]
    ed.check(covered[2] > hole[2] + 0.05 or covered[0] < hole[0] - 0.1, f"water over the bed: {covered} (hole {hole})")
    await ed.call("set_properties", entity=room, values={"waterexcluder": {"enabled": True}})
    S["room"] = room


async def test_cargo_dry_boat_floats(ed: Editor):
    s = await setup(ed)
    # a floating boat with its hold as its own excluder, and cargo inside the room
    raft = entity_id(await ed.call("create_entity", type="cube", name="Raft", position=[-5, 0.2, -5], scale=[2, 1, 2]))
    await ed.call("edit_colliders", entity=raft, body_type="dynamic", add=[{"shape": "box", "density": 400}])
    await ed.call("add_component", entity=raft, component="Buoyancy")
    await ed.call("add_component", entity=raft, component="WaterExcluder", values={"waterexcluder": {"shape": "box"}})
    crate = entity_id(await ed.call("create_entity", type="cube", name="Crate", position=[0, -0.4, 0], scale=[0.6, 0.6, 0.6]))
    await ed.call("edit_colliders", entity=crate, body_type="dynamic", add=[{"shape": "box", "density": 300}])
    await ed.call("add_component", entity=crate, component="Buoyancy")
    free = entity_id(await ed.call("create_entity", type="cube", name="Free", position=[5, -0.4, 5], scale=[0.6, 0.6, 0.6]))
    await ed.call("edit_colliders", entity=free, body_type="dynamic", add=[{"shape": "box", "density": 300}])
    await ed.call("add_component", entity=free, component="Buoyancy")

    started = await ed.call("play", action="start", fixed_fps=60)
    ed.check_eq(started["state"], "playing", f"{started}")
    try:
        await ed.call("wait", game_seconds=4)
        y = {}
        for name in ("Raft", "Crate", "Free"):
            y[name] = find_key(await ed.call("get_runtime_entity", entity=name, components=["transform"]), "position")[1]
        ed.check(-0.6 < y["Raft"] < 0.6, f"the raft floats despite its own excluder: {y}")
        ed.check(-0.6 < y["Free"] < 0.6, f"the free crate floats: {y}")
        # it falls through the room until its bottom leaves it, a metre under, where the water takes it again
        ed.check(y["Crate"] < -0.5 and y["Crate"] < y["Free"] - 0.4, f"the crate in the dry room sinks through it: {y}")
    finally:
        await ed.call("play", action="stop", fixed_fps=0)


async def test_camera_in_room_sees_no_fade(ed: Editor):
    await setup(ed)
    await ed.call("play", action="start")
    try:
        # under the surface outside the room: the underwater pass switches on
        await lua(ed, "local cam = object('Top'); cam.position = Vector3(6, -0.8, 6); cam:setRotation(0, 0, 0); return true")
        await ed.call("wait", frames=5)
        on = await lua(ed, "return scene:isPostProcessPassEnabled(0), Water.isUnderwater(scene, Vector3(6, -0.8, 6))")
        ed.check_eq(on, [True, True], "under the water outside the room")
        # inside the dry room, as deep: off
        await lua(ed, "object('Top').position = Vector3(0, -0.8, 0); return true")
        await ed.call("wait", frames=5)
        off = await lua(ed, "return scene:isPostProcessPassEnabled(0), Water.isUnderwater(scene, Vector3(0, -0.8, 0))")
        ed.check_eq(off, [False, False], "inside the dry room: no fade, not underwater")
    finally:
        await ed.call("play", action="stop")


async def test_wake_behind_moving_hull(ed: Editor):
    s = await setup(ed)
    # a skiff with a wake, run along the lake by a script
    skiff = entity_id(await ed.call("create_entity", type="cube", name="Skiff", position=[7, 0, -8], scale=[1, 0.6, 3]))
    await ed.call("add_component", entity=skiff, component="WaterWake")
    await ed.call("create_script", kind="lua", name="Run", content=RUN, entity=skiff)
    await ed.call("play", action="start", fixed_fps=60)
    try:
        await ed.call("wait", game_seconds=2)
        got = await sample(ed, (7, 0, -4.5), (7, 0, 8), report=True)
        behind, ahead = got["samples"]
        wakes = [w for w in got["wakes"] if w["entity"]["name"] == "Skiff"]
        ed.check(wakes and wakes[0]["emitting"] and abs(wakes[0]["speed"] - 5) < 0.6, f"the skiff emits at 5 m/s: {got['wakes']}")
        ed.check_eq(got["wake_map"]["active"], True, "the wake map runs")
        ed.check(behind["wake"] and behind["wake"]["foam"] > 0.05, f"foam behind the stern: {behind}")
        ed.check(ahead["wake"] is not None and ahead["wake"]["foam"] == 0 and ahead["wake"]["height"] == 0, f"none ahead: {ahead}")
        live = await lua(ed, "local w = object('Skiff'):getWaterWake(); return w.emitting, w.speed > 4")
        ed.check_eq(live, [True, True], "Lua WaterWake.emitting and speed")
    finally:
        await ed.call("play", action="stop", fixed_fps=0)


async def test_wake_settings_with_undo(ed: Editor):
    await setup(ed)
    defaults = (await ed.call("get_scene_settings", names=["water_wake_resolution", "water_wake_area", "water_wake_fade"]))["settings"]
    ed.check_eq(defaults["water_wake_resolution"], "auto", f"{defaults}")
    ed.check_eq((defaults["water_wake_area"], defaults["water_wake_fade"]), (64, 8), f"{defaults}")
    await ed.call("set_scene_settings", settings={"water_wake_resolution": "high", "water_wake_area": 32, "water_wake_fade": 4})
    got = (await ed.call("get_scene_settings", names=["water_wake_resolution", "water_wake_area", "water_wake_fade"]))["settings"]
    ed.check_eq((got["water_wake_resolution"], got["water_wake_area"], got["water_wake_fade"]), ("high", 32, 4), f"{got}")
    await ed.check_error("set_scene_settings", {"settings": {"water_wake_area": 2}}, contains="8 to 1024")
    values = await lua(ed, "return scene.waterWakeResolution == WaterWakeResolution.HIGH, scene.waterWakeArea", edit=True)
    ed.check_eq(values, [True, 32], "Lua reads the scene's settings")
    await ed.call("undo")
    back = (await ed.call("get_scene_settings", names=["water_wake_resolution"]))["settings"]
    ed.check_eq(back["water_wake_resolution"], "auto", "one undo step restores them")
