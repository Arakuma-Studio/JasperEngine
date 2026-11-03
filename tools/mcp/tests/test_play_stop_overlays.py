"""Stopping Play keeps the Scene view's own overlays (editor/Project.cpp getPlayKeptEntities).

The Scene view makes a gizmo or icon (SceneRender::overlay: a Rigidbody's collider lines, a light's
icon, ...) the first time it draws the object. When that happens after Play captured the scene's
entities (a body added just before Play, drawn on the first frames of the session), Stop used to
destroy the overlay as something the game made, and the next hide of the gizmos (the Game view pass)
threw on its missing Transform: "FATAL: unhandled exception: unordered_map::at".
"""

from __future__ import annotations

from harness import Editor

EDITOR = {"create": True, "name": "PlayStopOverlays"}
CI = True


async def test_overlay_made_during_play_survives_stop(ed: Editor):
    await ed.batch([
        ("create_entity", {"name": "Body", "type": "cube", "position": [0, 1, 0]}),
        ("add_component", {"entity": "Body", "component": "Rigidbody"}),
        ("play", {"action": "start", "auto_pause": True, "fixed_fps": 60}),
    ])
    try:
        await ed.call("wait", frames=3)
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    # Both views hide/show their own overlays after Stop; the old deletion crashed here.
    for view in ("scene", "game"):
        image = await ed.call_raw("screenshot", {"view": view, "max_width": 160})
        ed.check(not image.is_error and image.images, image.text)
    await ed.call("get_entity", entity="Body")
