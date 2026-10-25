"""Lattice editor regression boundaries (lattice design §9/§12).

Space+click uses dynamically located cage-point handles and real X input;
follower-driven transforms are checked through public persisted editor state.
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import test_lattice_scene as ls  # noqa: E402  (helpers only: its tests are not collected here)
from harness import Editor  # noqa: E402

TIMEOUT = 900
EDITOR = {"create": True, "xvfb": "harness"}

async def test_2d_space_click_pans_without_picking(ed: Editor):
    ls.need_tools()
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
    spots = [(round(x), round(y)) for x, y, _ in ls.blobs(ls.screen(ed))]
    ed.check(len(spots) >= 8, f"the points show: {spots}")
    await ls.key(ed, "Escape")
    ed.check_eq(ls.blobs(ls.screen(ed)), [], "none selected")

    x, y = max(spots, key=lambda p: p[0] - p[1])
    # Space held: the click is the start of a pan, not a pick
    await ls.click(ed, x, y, modifier="space")
    await ls.settle(ed, 6)
    ed.check_eq(ls.blobs(ls.screen(ed)), [], "Space + click picks no lattice point")
    # without Space the same click picks it
    await ls.click(ed, x, y)
    ed.check(len(ls.blobs(ls.screen(ed))) >= 1, "a plain click picks the point")


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
