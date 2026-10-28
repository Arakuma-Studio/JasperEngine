"""Stopping Play keeps the Scene view's own overlays (editor/Project.cpp getPlayKeptEntities).

The Scene view makes a gizmo or icon (SceneRender::overlay: a Rigidbody's collider lines, a light's
icon, ...) the first time it draws the object. When that happens after Play captured the scene's
entities (a body added just before Play, drawn on the first frames of the session), Stop used to
destroy the overlay as something the game made, and the next hide of the gizmos (the Game view pass)
threw on its missing Transform: "FATAL: unhandled exception: unordered_map::at".
"""

from __future__ import annotations

import asyncio

from harness import Editor

EDITOR = {"create": True, "name": "PlayStopOverlays"}
CI = True


async def test_overlay_made_during_play_survives_stop(ed: Editor):
    # a few rounds: each body's collider lines are first drawn while Play starts
    for round_ in range(4):
        name = f"Body{round_}"
        await ed.batch([
            ("create_entity", {"name": name, "type": "cube", "position": [round_ * 2.0, 1, 0]}),
            ("add_component", {"entity": name, "component": "Rigidbody"}),
            ("play", {"action": "start"}),
        ])
        await asyncio.sleep(2.0)
        await ed.call("play", action="stop")
        await asyncio.sleep(1.0)
        await ed.check_alive()
        await ed.call("get_entity", entity=name)
