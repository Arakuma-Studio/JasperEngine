"""Lattice editor review (docs/superpowers/specs/2026-10-23-lattice-deformer-design.md §9): regressions
found by the review of the editor side, each driven the way a person does it (real X input on the
harness' Xvfb, xdotool) and read back over MCP.

- GameObject > 3D Object > Lattice with a group and its child selected binds the lattice once: the
  group (Include Children) deforms the child, a second binding on the child would deform it twice.
- The Lattice inspector's Select All / Select None show in the Scene view at once (it draws only
  when asked).
- In a 2D view, Space + click pans: it does not pick a lattice point.

The editor window fills the 1600x900 Xvfb screen in the default layout; menu and inspector positions
below are for that layout (the same one test_lattice_scene.py drives).
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import test_lattice_scene as ls  # noqa: E402  (helpers only: its tests are not collected here)
from harness import Editor  # noqa: E402

TIMEOUT = 900
EDITOR = {"create": True, "xvfb": "harness"}

S: dict = {}

# the Scene view's image in the default layout (inside its tab, under its toolbar)
SCENE_VIEW = (300, 100, 1238, 605)


def scene_blobs(img, color=ls.ORANGE):
    """ls.blobs over the Scene view's area only."""
    width, height, rgba = img
    x0, y0, x1, y1 = SCENE_VIEW
    out = bytearray()
    for y in range(y0, y1):
        out += rgba[(y * width + x0) * 4:(y * width + x1) * 4]
    return ls.blobs((x1 - x0, y1 - y0, bytes(out)), color)


async def latticemodifier(ed: Editor, scene: int, entity: int):
    got = await ed.call("get_entity", scene=scene, entity=entity, components=["latticemodifier"])
    return (got.get("components") or {}).get("latticemodifier")


async def lattice_state(ed: Editor, scene: int, entity: int) -> dict:
    got = await ed.call("get_entity", scene=scene, entity=entity, components=["lattice"],
                        properties=["resolutionX", "resolutionY", "resolutionZ", "offsets", "poses"])
    return got["components"]["lattice"]


async def menu_create_lattice(ed: Editor):
    """GameObject > 3D Object > Lattice, by mouse (the last item, after Water)."""
    await ls.click(ed, 162, 10)
    ls.run_x(ed, "xdotool", "mousemove", "155", "95")
    await ls.settle(ed, 4)
    ls.run_x(ed, "xdotool", "mousemove", "430", "95")
    await ls.settle(ed, 4)
    await ls.click(ed, 424, 314)
    await ls.settle(ed, 8)


async def test_setup(ed: Editor):
    ls.need_tools()
    made = await ed.call("create_scene", name="Review", type="3D", path="scenes/Review")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    await ed.call("focus_window", window="scene", scene=S["scene"])
    ls.focus_editor(ed)
    await ls.settle(ed, 10)


async def test_create_around_group_and_child_binds_once(ed: Editor):
    sid = S["scene"]
    group = (await ed.call("create_entity", type="empty", scene=sid, name="Group", position=[-3, 0, -2]))["entity_id"]
    box = (await ed.call("create_entity", type="box", scene=sid, name="Box", parent=group))["entity_id"]
    # the group and its child, as a Shift-click range in the Hierarchy selects them
    await ed.call("select_entities", scene=sid, entities=[group, box])
    ls.focus_editor(ed)
    await ls.settle(ed, 6)
    await menu_create_lattice(ed)

    sel = await ed.call("get_selection", scene=sid)
    ed.check_eq(len(sel["entities"]), 1, f"the new lattice is the selection: {sel}")
    lattice = sel["entities"][0]["id"]
    ed.check(lattice not in (group, box), "a new GameObject")

    on_group = await latticemodifier(ed, sid, group)
    ed.check(on_group is not None, "the group gets a Lattice Modifier")
    ed.check_eq([b["lattice"] for b in on_group["bindings"]], [lattice], "the group is bound to the lattice")
    ed.check(on_group["includeChildren"] is True, "the group (no mesh of its own) deforms its children")

    on_box = await latticemodifier(ed, sid, box)
    own = [b["lattice"] for b in (on_box or {}).get("bindings", [])]
    ed.check(lattice not in own, f"the child inherits the lattice from the group and is not bound again "
                                 f"(it would be deformed twice): its own bindings {own}")

    # one undo step takes the lattice and the binding away
    await ed.call("undo", scene=sid)
    ed.check(await latticemodifier(ed, sid, group) is None, "undo removes the group's modifier")
    ed.check(await latticemodifier(ed, sid, box) is None, "undo leaves the child without one")
    await ed.call("delete_entities", scene=sid, entities=[group])


async def test_inspector_select_all_shows_in_scene_view(ed: Editor):
    sid = S["scene"]
    cage = (await ed.call("create_entity", type="empty", scene=sid, name="Cage"))["entity_id"]
    await ed.call("add_component", scene=sid, entity=cage, component="lattice")
    await ed.call("set_transform", scene=sid, entity=cage, scale=[2, 2, 2])
    await ed.call("set_scene_camera", scene=sid, position=[3.2, 2.4, 5.0], target=[0, 0, 0])
    await ed.call("select_entities", scene=sid, entities=[cage])
    S["cage"] = cage
    ls.focus_editor(ed)
    await ls.settle(ed, 10)
    ed.check_eq(scene_blobs(ls.screen(ed)), [], "no point is selected yet")

    # Inspector > Lattice > Points > Select All; the mouse stays over the Inspector
    await ls.click(ed, 1335, 496)
    await ls.settle(ed, 10)
    points = scene_blobs(ls.screen(ed))
    ed.check_eq(len(points), 26, f"the Scene view shows the 27 selected points (the centre one under the gizmo) "
                                 f"without the mouse going over it, saw {len(points)}")

    # Select None, same rule
    await ls.click(ed, 1500, 496)
    await ls.settle(ed, 10)
    ed.check_eq(scene_blobs(ls.screen(ed)), [], "Select None clears them in the Scene view at once")


async def test_2d_space_click_pans_without_picking(ed: Editor):
    made = await ed.call("create_scene", name="Review 2D", type="2D", path="scenes/Review2D")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    await ed.call("focus_window", window="scene", scene=sid)
    lattice = (await ed.call("create_entity", type="empty", scene=sid, name="Cage 2D", position=[50, 50, 0]))["entity_id"]
    await ed.call("add_component", scene=sid, entity=lattice, component="lattice")
    await ed.call("set_transform", scene=sid, entity=lattice, scale=[160, 160, 20])
    await ed.call("set_scene_camera", scene=sid, center=[50, 50], zoom=0.5)
    await ed.call("select_entities", scene=sid, entities=[lattice])
    ls.focus_editor(ed)
    await ls.settle(ed, 10)

    await ls.key(ed, "ctrl+a")
    spots = [(round(x), round(y)) for x, y, _ in scene_blobs(ls.screen(ed))]
    ed.check(len(spots) >= 8, f"the points show: {spots}")
    await ls.key(ed, "Escape")
    ed.check_eq(scene_blobs(ls.screen(ed)), [], "none selected")

    x, y = max(spots, key=lambda p: p[0] - p[1])
    x += SCENE_VIEW[0]
    y += SCENE_VIEW[1]
    # Space held: the click is the start of a pan, not a pick
    await ls.click(ed, x, y, modifier="space")
    await ls.settle(ed, 6)
    ed.check_eq(scene_blobs(ls.screen(ed)), [], "Space + click picks no lattice point")
    # without Space the same click picks it
    await ls.click(ed, x, y)
    ed.check(len(scene_blobs(ls.screen(ed))) >= 1, "a plain click picks the point")


async def test_inspector_resolution_undo_restores_offsets_and_poses(ed: Editor):
    sid = S["scene"]
    await ed.call("select_scene", scene=sid)
    await ed.call("focus_window", window="scene", scene=sid)
    cage = S["cage"]
    await ed.call("set_properties", scene=sid, entity=cage, values={"lattice": {
        "offsets[26]": [0.1, 0.2, -0.1], "poses": [{"name": "Lean", "offsets": [[0.05, 0, 0]] * 27}]}})
    await ed.call("select_entities", scene=sid, entities=[cage])
    ls.focus_editor(ed)
    await ls.settle(ed, 8)
    before = await lattice_state(ed, sid, cage)

    # Inspector > Lattice > Resolution X: Ctrl+click types a value, Enter applies it
    await ls.click(ed, 1427, 327, modifier="ctrl")
    await ls.key(ed, "ctrl+a")
    ls.run_x(ed, "xdotool", "type", "5")
    await ls.key(ed, "Return")
    await ls.settle(ed, 6)
    after = await lattice_state(ed, sid, cage)
    ed.check_eq([after["resolutionX"], after["resolutionY"], after["resolutionZ"]], [5, 3, 3], "the X field resamples to 5")
    ed.check_eq(len(after["offsets"]), 45, "offsets laid out for 5x3x3")
    ed.check_eq(len(after["poses"][0]["offsets"]), 45, "the pose is resampled too")

    await ed.call("undo", scene=sid)
    undone = await lattice_state(ed, sid, cage)
    ed.check_eq(undone["resolutionX"], 3, "one undo step restores the resolution")
    ed.check_eq(undone["offsets"], before["offsets"], "and every offset exactly")
    ed.check_eq(undone["poses"], before["poses"], "and the poses exactly")
    await ed.call("redo", scene=sid)
    redone = await lattice_state(ed, sid, cage)
    ed.check_eq(redone["offsets"], after["offsets"], "redo gives the same resampled offsets")
    await ed.call("undo", scene=sid)


async def type_resolution(ed: Editor, axis: int, value: int, y: int = 327):
    """Inspector > Lattice > Resolution field `axis` (0..2): Ctrl+click, type, Enter."""
    await ls.click(ed, (1427, 1489, 1551)[axis], y, modifier="ctrl")
    await ls.key(ed, "ctrl+a")
    ls.run_x(ed, "xdotool", "type", str(value))
    await ls.key(ed, "Return")
    await ls.settle(ed, 6)


async def test_prefab_instance_resolution_keeps_its_grid(ed: Editor):
    # a prefab instance changes its X resolution in the Inspector (its offsets list follows); the
    # prefab then changes Y. The instance's grid is its own: it must not take the prefab's Y with
    # offsets laid out for its old grid (2x4x3 = 24 points, 18 offsets: no cage, no edits)
    sid = S["scene"]
    await ed.call("select_scene", scene=sid)
    cage = (await ed.call("create_entity", type="empty", scene=sid, name="Instanced"))["entity_id"]
    await ed.call("add_component", scene=sid, entity=cage, component="lattice")
    await ed.call("create_prefab", scene=sid, entity=cage, path="prefabs/InstancedCage.prefab")
    await ed.call("select_entities", scene=sid, entities=[cage])
    ls.focus_editor(ed)
    await ls.settle(ed, 8)
    # a prefab instance's inspector has the Prefab rows above the Lattice component
    await type_resolution(ed, 0, 2, y=438)
    state = await lattice_state(ed, sid, cage)
    ed.check_eq([state["resolutionX"], len(state["offsets"])], [2, 18], "the instance is 2x3x3")

    st = await ed.call("prefab_mode", action="open", prefab="prefabs/InstancedCage.prefab", auto_save=False)
    stage = st["scene_id"]
    await ed.call("select_entities", scene=stage, entities=[st["root"]["id"]])
    ls.focus_editor(ed)
    await ls.settle(ed, 8)
    await type_resolution(ed, 1, 4)
    ed.check_eq(len((await lattice_state(ed, stage, st["root"]["id"]))["offsets"]), 36, "the prefab is 3x4x3")
    await ed.call("prefab_mode", action="close", unsaved="save")
    await ed.call("select_scene", scene=sid)

    state = await lattice_state(ed, sid, cage)
    res = [state["resolutionX"], state["resolutionY"], state["resolutionZ"]]
    ed.check_eq(len(state["offsets"]), res[0] * res[1] * res[2],
                f"the instance's offsets fit its resolution {res}: {len(state['offsets'])} offsets")
    ed.check_eq(res, [2, 3, 3], "the instance keeps the grid it was given")

    # undo of the instance's resolution change drops its overrides: the prefab's grid comes back
    await ed.call("undo", scene=sid)
    state = await lattice_state(ed, sid, cage)
    res = [state["resolutionX"], state["resolutionY"], state["resolutionZ"]]
    ed.check_eq(len(state["offsets"]), res[0] * res[1] * res[2], f"after undo too: {res}, {len(state['offsets'])} offsets")


# -- Lattice Follower in the editor (LatticeSystem drives its Transform every frame) ---------------


def transform_overrides(info, entity: int) -> list[str]:
    """The Transform properties `entity` overrides (get_prefab_info lists the whole instance's)."""
    return [p for e in info.get("overrides", []) if e.get("kind") == "property" and e.get("component") == "transform"
            and (e.get("entity") or {}).get("id") == entity for p in e.get("properties", [])]


async def world_x(ed: Editor, scene: int, entity: int) -> float:
    return (await ed.call("get_entity", scene=scene, entity=entity))["world"]["position"][0]


async def unsaved(ed: Editor, scene: int) -> bool:
    status = await ed.call("editor_status")
    return scene in status["unsaved"]["scene_ids"]


async def test_follower_setup_and_no_spurious_state(ed: Editor):
    made = await ed.call("create_scene", name="Followers", type="3D", path="scenes/Followers")
    sid = made["scene"]["id"]
    S["fscene"] = sid
    await ed.call("select_scene", scene=sid)
    await ed.call("focus_window", window="scene", scene=sid)
    rig = (await ed.call("create_entity", type="empty", scene=sid, name="Rig"))["entity_id"]
    cage = (await ed.call("create_entity", type="empty", scene=sid, name="Cage", parent=rig, scale=[2, 2, 2]))["entity_id"]
    await ed.call("add_component", scene=sid, entity=cage, component="lattice")
    rest = (await ed.call("create_entity", type="empty", scene=sid, name="Rest", parent=rig, position=[0.4, 0, 0]))["entity_id"]
    follower = (await ed.call("create_entity", type="box", scene=sid, name="Follower", parent=rig, position=[0.4, 0, 0],
                              scale=[0.2, 0.2, 0.2]))["entity_id"]
    await ed.call("add_component", scene=sid, entity=follower, component="latticefollower")
    await ed.call("set_properties", scene=sid, entity=follower, values={"latticefollower": {"lattice": cage, "rest": rest}})
    S.update(rig=rig, fcage=cage, rest=rest, follower=follower)
    await ed.call("create_prefab", scene=sid, entity=rig, path="prefabs/Rig.prefab")

    # a point of the instance's cage moved: the follower follows, nothing else is an override
    await ed.call("set_properties", scene=sid, entity=cage, values={"lattice": {"offsets[14]": [0.25, 0, 0]}})
    await ed.call("wait", frames=10)
    moved = await world_x(ed, sid, follower)
    ed.check(moved > 0.45, f"the follower follows the cage: x {moved}")
    info = await ed.call("get_prefab_info", scene=sid, entity=follower, usage=False)
    ed.check_eq(transform_overrides(info, follower), [], f"the driven Transform is not an override of the instance: {info.get('overrides')}")

    # undo of the cage edit: the follower goes back to its rest, redo moves it again
    await ed.call("undo", scene=sid)
    await ed.call("wait", frames=10)
    ed.check(abs(await world_x(ed, sid, follower) - 0.4) < 1e-4, "undo of the cage edit puts the follower back")
    await ed.call("redo", scene=sid)
    await ed.call("wait", frames=10)
    ed.check(abs(await world_x(ed, sid, follower) - moved) < 1e-4, "redo moves it again")
    info = await ed.call("get_prefab_info", scene=sid, entity=follower, usage=False)
    ed.check_eq(transform_overrides(info, follower), [], "still no override on the follower")

    # saved, the scene stays saved while the follower is driven
    await ed.call("save_scene", scene=sid)
    await ed.call("wait", frames=20)
    ed.check(not await unsaved(ed, sid), "the driven follower does not mark the scene modified")
    info = await ed.call("get_prefab_info", scene=sid, entity=follower, usage=False)
    ed.check_eq(transform_overrides(info, follower), [], "nor after the save")


async def test_follower_gizmo_moves_the_rest(ed: Editor):
    sid, follower, rest = S["fscene"], S["follower"], S["rest"]
    await ed.call("select_scene", scene=sid)
    await ed.call("focus_window", window="scene", scene=sid)
    position = (await ed.call("get_entity", scene=sid, entity=follower))["world"]["position"]
    await ed.call("set_scene_camera", scene=sid, position=[position[0], position[1] + 1.5, position[2] + 4.0], target=position)
    await ed.call("select_entities", scene=sid, entities=[follower])
    ls.focus_editor(ed)
    await ls.settle(ed, 10)
    rest_before = await world_x(ed, sid, rest)
    follower_before = await world_x(ed, sid, follower)

    # the Move gizmo's centre sits on the follower, in the middle of the view (the camera's target)
    cx, cy = (SCENE_VIEW[0] + SCENE_VIEW[2]) // 2, (SCENE_VIEW[1] + SCENE_VIEW[3]) // 2
    await ls.drag(ed, (cx, cy), (cx + 80, cy))
    await ed.call("wait", frames=10)
    rest_after = await world_x(ed, sid, rest)
    follower_after = await world_x(ed, sid, follower)
    ed.check(rest_after > rest_before + 0.05, f"dragging a driven follower moves its rest: {rest_before} -> {rest_after}")
    ed.check(follower_after > follower_before + 0.05, f"and the follower follows it: {follower_before} -> {follower_after}")
    info = await ed.call("get_prefab_info", scene=sid, entity=follower, usage=False)
    ed.check_eq(transform_overrides(info, follower), [], f"no override on the driven follower: {info.get('overrides')}")
    info = await ed.call("get_prefab_info", scene=sid, entity=rest, usage=False)
    ed.check_in("position", transform_overrides(info, rest), "the rest's position is the instance's override")

    # one undo step puts the rest back, and the follower with it
    await ed.call("undo", scene=sid)
    await ed.call("wait", frames=10)
    ed.check(abs(await world_x(ed, sid, rest) - rest_before) < 1e-5, "undo puts the rest back")
    ed.check(abs(await world_x(ed, sid, follower) - follower_before) < 1e-4, "and the follower follows")
    info = await ed.call("get_prefab_info", scene=sid, entity=rest, usage=False)
    ed.check_eq(transform_overrides(info, rest), [], "and drops the rest's override")
