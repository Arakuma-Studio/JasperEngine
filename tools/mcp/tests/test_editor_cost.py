"""The editor's cost with a large scene (S98 REQ-30, REQ-31, REQ-41, AC24; ledger E11, E12/E42).

The fixture is generated: 1,500 GameObjects (100 point lights, 100 rigidbodies, 50 cameras, 50
particle systems, the rest cubes) under 12 groups, the Hierarchy fully expanded, nothing selected.
- Scene-view gizmos share their renderers per kind, so the scene takes no "No free system entity
  slot" error (the system range is 1..1000) when it is made or opened.
- The editor-side CPU scopes of a frame (the panels, the Scene view's gizmo update) stay within
  8 ms; in Play with the Game view maximised, the Hierarchy and the Scene view within 1 ms. Both
  are budgets: reported through harness.over_budget, enforced in the benchmark run.
- A light's icon is still picked where it is drawn."""

from __future__ import annotations

from harness import Editor, over_budget

CI = True
EDITOR = {"create": True, "name": "EditorCost"}
TIMEOUT = 1500

GROUPS = 12
KINDS = [("point_light", 100), ("rigidbody", 100), ("camera", 50), ("particle_system", 50)]
TOTAL = 1500
SLOT_ERROR = "No free system entity slot"

S: dict = {}


def fixture_calls(scene: int, groups: list[int]) -> list[dict]:
    """create_entity calls for every GameObject but the groups, spread over the groups: away from
    the origin (x >= 20), so the light the pick test aims at has the view's centre to itself."""
    kinds: list[str] = []
    for kind, count in KINDS:
        kinds += [kind] * count
    kinds += ["cube"] * (TOTAL - GROUPS - len(kinds) - 1)   # (one more light is the picked one)
    calls = []
    for i, kind in enumerate(kinds):
        position = [20.0 + (i % 40) * 3.0, 0.0, -60.0 + (i // 40) * 3.0]
        calls.append({"tool": "create_entity", "arguments": {
            "scene": scene, "type": kind, "name": f"{kind}_{i}", "parent": groups[i % len(groups)],
            "position": position, "select": False}})
    return calls


async def cost_scene(ed: Editor) -> int:
    if "scene" in S:
        return S["scene"]
    made = await ed.call("create_scene", name="EditorCost", type="3D", path="scenes/EditorCost")
    scene = made["scene"]["id"]
    groups = []
    for g in range(GROUPS):
        groups.append((await ed.call("create_entity", scene=scene, type="cube", name=f"Group {g}", select=False))["entity_id"])
    calls = fixture_calls(scene, groups)
    for at in range(0, len(calls), 200):
        batch = await ed.call("batch", calls=calls[at:at + 200], timeout=600)
        ed.check(batch["failed"] == 0, f"the fixture's GameObjects were made: {batch.get('results', [])[:2]}")
    S["light"] = (await ed.call("create_entity", scene=scene, type="point_light", name="PickLight",
                                position=[0, 0, 0], select=False))["entity_id"]
    found = await ed.call("find_entities", scene=scene, limit=100000)
    ed.check(len(found["entities"]) >= TOTAL, f"the scene holds {TOTAL} GameObjects: {len(found['entities'])}")
    await ed.call("save_scene", scene=scene)
    await ed.call("select_entities", scene=scene, entities=[])
    await ed.call("focus_window", window="hierarchy")
    await ed.call("set_scene_camera", scene=scene, position=[0, 0, 12], target=[0, 0, 0])
    await ed.call("wait", frames=10)
    S["scene"] = scene
    return scene


async def slot_errors(ed: Editor) -> list[str]:
    console = await ed.call("read_console", contains=SLOT_ERROR, limit=1000)
    return [e.get("message", "") for e in console["entries"]]


async def scope_ms(ed: Editor, capture: int, name: str) -> float:
    """total ms of the scopes named `name` in the capture's median frame (0 when it did not run)"""
    flat = await ed.call("get_cpu_profile", capture_id=capture, view="flat", frame="median", filter=name, max_rows=50)
    return sum(row["total_ms"] for row in flat.get("rows", []) if row["name"] == name)


async def test_large_scene_takes_no_system_slots(ed: Editor):
    """AC24 (a): making and reopening the fixture logs no system slot error."""
    scene = await cost_scene(ed)
    errors = await slot_errors(ed)
    ed.check(not errors, f"the gizmos of {TOTAL} GameObjects took no system entity slot each: {len(errors)} errors, {errors[:1]}")
    await ed.call("close_scene", scene=scene, unsaved="discard")
    S.pop("scene")
    reopened = (await ed.call("open_scene", scene="scenes/EditorCost.scene"))["scene"]["id"]
    await ed.call("focus_window", window="hierarchy")
    await ed.call("set_scene_camera", scene=reopened, position=[0, 0, 12], target=[0, 0, 0])
    await ed.call("wait", frames=10)
    S["scene"] = reopened
    S["light"] = (await ed.call("find_entities", scene=reopened, name="PickLight"))["entities"][0]["id"]
    errors = await slot_errors(ed)
    ed.check(not errors, f"opening the {TOTAL}-GameObject scene took no system entity slot per gizmo: {errors[:1]}")


async def test_light_icon_is_picked(ed: Editor):
    """AC24 (c): the light at the view's centre is what a click there selects."""
    scene = await cost_scene(ed)
    picked = await ed.call("pick_at", scene=scene, u=0.5, v=0.5)
    ed.check_eq((picked.get("would_select") or {}).get("entity"), S["light"], f"a click on the light's icon selects the light: {picked}")


async def test_editor_frame_budget(ed: Editor):
    """AC24 (b), REQ-41: the editor-side scopes of a frame with the fixture open, nothing selected."""
    await cost_scene(ed)
    await ed.call("focus_window", window="hierarchy")
    await ed.call("wait", frames=5)
    capture = (await ed.call("capture_profile", frames=60, gpu=False))["capture_id"]
    measured = {name: await scope_ms(ed, capture, name)
                for name in ("Editor UI", "Scene View Gizmos", "Hierarchy", "Hierarchy Rows", "Hierarchy Scene Sync",
                             "Hierarchy Search Order", "Hierarchy Popups", "Hierarchy Tree Rebuild", "Inspector", "Scene Window")}
    ed.check(measured["Hierarchy"] > 0 and measured["Editor UI"] > 0, f"the panels' scopes were recorded: {measured}")
    for name, ms in measured.items():
        print(f"     [bench] {name}: {ms:.3f} ms (median frame of 60, {TOTAL} GameObjects)")
    over = over_budget("editor-side frame scopes (Editor UI + Scene View Gizmos)",
                       {"editor": measured["Editor UI"] + measured["Scene View Gizmos"]}, 8.0, "ms")
    ed.check(not over, f"REQ-41: the editor's own scopes took {over} ms a frame (8 ms)")


async def test_play_maximized_budget(ed: Editor):
    """REQ-41: in Play with the Game view maximised, the Hierarchy and the Scene view stay out of
    the frame (at most 1 ms together): the Scene view updates no gizmo while it is not drawn."""
    scene = await cost_scene(ed)
    await ed.call("configure_game_view", maximize_on_play=True)
    await ed.call("play", action="start", scene=scene, fixed_fps=30, auto_pause=False)
    try:
        await ed.call("wait", frames=10)
        capture = (await ed.call("capture_profile", frames=60, gpu=False))["capture_id"]
        measured = {name: await scope_ms(ed, capture, name) for name in ("Hierarchy", "Scene View Gizmos", "Scene Window")}
        for name, ms in measured.items():
            print(f"     [bench] Play maximised, {name}: {ms:.3f} ms (median frame of 60)")
        ed.check(measured["Scene View Gizmos"] == 0.0,
                 f"the hidden Scene view updated its gizmos in Play with the Game view maximised: {measured}")
        over = over_budget("Play maximised: Hierarchy + Scene view", {"hierarchy_scene": sum(measured.values())}, 1.0, "ms")
        ed.check(not over, f"REQ-41: the Hierarchy and the Scene view took {over} ms a frame in maximised Play (1 ms)")
    finally:
        await ed.call("play", action="stop")
        await ed.call("configure_game_view", maximize_on_play=False)
