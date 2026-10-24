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


async def menu_create_lattice(ed: Editor):
    """GameObject > 3D Object > Lattice, by mouse."""
    await ls.click(ed, 162, 10)
    ls.run_x(ed, "xdotool", "mousemove", "155", "95")
    await ls.settle(ed, 4)
    ls.run_x(ed, "xdotool", "mousemove", "430", "95")
    await ls.settle(ed, 4)
    await ls.click(ed, 424, 295)
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
