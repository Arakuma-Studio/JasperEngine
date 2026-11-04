"""Scene view icons follow Active (runtime-core spec 6.4): a GameObject inactive in hierarchy draws no
icon or gizmo lines in the Scene view (Unity draws no gizmos for inactive GameObjects, and the icon
picker already refused them). A point light under a parent: its icon shows, the parent made inactive
hides it, active again shows it.
"""

from __future__ import annotations

from harness import Editor, find_key
from htmlui_pixels import image_difference

CI = True
EDITOR = {"create": True, "name": "Scene View Icons"}
TIMEOUT = 600


async def shot(ed: Editor) -> bytes:
    # (the screenshot tool waits for a freshly rendered Scene view)
    result = await ed.call_raw("screenshot", {"view": "scene", "max_width": 480})
    ed.check(not result.is_error and result.images, f"no screenshot: {result.text[:300]}")
    return result.images[0][0]


async def test_icons_of_inactive_game_objects_are_hidden(ed: Editor):
    made = await ed.call("create_scene", name="Icons", type="3d")
    scene = made["scene"]["id"]
    parent = find_key(await ed.call("create_entity", scene=scene, type="empty", name="Holder"), "entity_id", "id")
    light = find_key(await ed.call("create_entity", scene=scene, type="point_light", name="Bulb", parent=parent), "entity_id", "id")
    await ed.call("frame_entity", scene=scene, entity=light)
    await ed.call("select_entities", entities=[])
    await ed.call("wait", frames=4)

    shown = await shot(ed)
    await ed.call("set_entity_state", scene=scene, entity=parent, active=False)
    hidden = await shot(ed)
    await ed.call("set_entity_state", scene=scene, entity=parent, active=True)
    again = await shot(ed)

    ed.check(image_difference(shown, hidden, step=1) > 0.05, "the inactive light's icon is gone")
    ed.check(image_difference(shown, again, step=1) < 0.02, "active again, its icon is back")
