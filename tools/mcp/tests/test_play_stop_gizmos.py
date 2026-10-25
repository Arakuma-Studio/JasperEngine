"""Stopping Play right after it starts, with every Scene view gizmo map populated.

The Scene view keeps an editor gizmo per annotated GameObject (SceneRender3D: light, camera and
audio icons; body, joint, bone, line, polygon, track and reflection probe lines). The Game view pass
hides them all (SceneRender::beginGameViewPass -> hideAllGizmos) before it draws, so a gizmo whose
own entity went away with Stop threw "FATAL: unhandled exception: unordered_map::at" from
GameObject::setVisible. This loops play start -> immediate stop (also during the startup, before
the game runs, and after a few frames in which the game spawns and destroys annotated GameObjects)
on the pose fixture (two skinned models) plus one GameObject of each gizmo kind, with the Game view
(the Game tab in front: only the Game view pass draws the scene) and the Scene view drawing.
"""

from __future__ import annotations

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
TIMEOUT = 600

CYCLES = 12

KINDS = [
    ("directional_light", "Sun"), ("point_light", "Bulb"), ("spot_light", "Spot"),
    ("camera", "Cam2"), ("audio_source_3d", "Speaker"), ("reflection_probe", "Probe"),
    ("line_renderer", "Lines"), ("mesh_polygon", "Poly"),
]
# no Transform: created without a position
UNPLACED = [("joint3d", "Hinge"), ("translate_tracks", "Path")]

# what the game does in its first frames: copies of annotated GameObjects (gizmos made for
# entities Stop removes) and an authored one destroyed (Stop brings it back)
SPAWN = """
    local n = 0
    for _, name in ipairs({"Sun", "Bulb", "Spot", "Cam2", "Speaker", "Probe", "Lines", "Poly", "Body"}) do
        local e = scene:findEntity(name)
        if e and e ~= 0 then GameObject.instantiate(GameObject(scene, e)); n = n + 1 end
    end
    local bulb = scene:findEntity("Bulb")
    if bulb and bulb ~= 0 then GameObject.destroyImmediate(GameObject(scene, bulb)) end
    return n
"""


async def shoot(ed: Editor, view: str) -> None:
    image = await ed.call_raw("screenshot", {"view": view, "max_width": 160})
    ed.check(not image.is_error and image.images, image.text)


async def test_build_scene_with_every_gizmo(ed: Editor):
    calls = [("create_entity", {"name": name, "type": kind, "position": [i * 1.5, 1, -3]})
             for i, (kind, name) in enumerate(KINDS)]
    calls += [("create_entity", {"name": name, "type": kind}) for kind, name in UNPLACED]
    calls += [
        ("create_entity", {"name": "Body", "type": "cube", "position": [0, 1, 3]}),
        ("add_component", {"entity": "Body", "component": "Rigidbody"}),
    ]
    await ed.batch(calls)
    for _, name in KINDS + UNPLACED + [("", "Body")]:
        await ed.call("get_entity", entity=name)
    # every gizmo exists before the first Play
    await ed.call("focus_window", window="scene")
    await shoot(ed, "scene")


async def test_stop_right_after_start(ed: Editor):
    for cycle in range(CYCLES):
        # the Scene view in front on even cycles, the Game view (its own pass) on odd ones
        await ed.call("focus_window", window="game" if cycle % 2 else "scene")
        # a body added in the same frame as Play: its gizmo is made while the game runs
        late = [("create_entity", {"name": f"Late{cycle}", "type": "cube", "position": [cycle, 2, 0],
                                   "components": ["Rigidbody"]})]
        variant = cycle % 3
        if variant == 0:
            # stopped while still starting (the game never ran)
            await ed.batch(late + [("play", {"action": "start", "wait": False})])
        elif variant == 1:
            await ed.batch(late + [("play", {"action": "start"})])
        else:
            await ed.batch(late + [("play", {"action": "start", "auto_pause": True, "fixed_fps": 60})])
            try:
                spawned = (await ed.call("eval_lua", code=SPAWN))["values"][0]
                ed.check(spawned >= 8, f"the game copied the annotated GameObjects: {spawned}")
                await ed.call("focus_window", window="scene")
                await ed.call("wait", frames=3)
            finally:
                await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
        if variant != 2:
            await ed.call("play", action="stop")
        # the first frames after Stop: the Game view pass hides every gizmo
        await shoot(ed, "game")
        await shoot(ed, "scene")
    await ed.check_alive()
    for _, name in KINDS + UNPLACED + [("", "Body")]:
        await ed.call("get_entity", entity=name)
    state = await ed.call("get_play_state")
    ed.check_eq(state["state"], "stopped")


async def test_stop_right_after_start_2d(ed: Editor):
    """The same on a 2D scene (SceneRender2D: body, light, shadow caster, line, polygon, camera, audio)."""
    made = await ed.call("create_scene", name="Gizmos 2D", type="2D")
    sid = made["scene"]["id"]
    kinds = [("rigidbody2d", "Body2D"), ("light_2d", "Light2D"), ("shadow_caster_2d", "Caster"),
             ("line_renderer", "Lines2D"), ("polygon", "Poly2D"), ("camera", "Cam2D")]
    await ed.batch([("create_entity", {"scene": sid, "name": name, "type": kind, "position": [i * 40, 0, 0]})
                    for i, (kind, name) in enumerate(kinds)]
                   + [("create_entity", {"scene": sid, "name": "Audio2D", "type": "audio_source"})])
    for _, name in kinds + [("", "Audio2D")]:
        await ed.call("get_entity", scene=sid, entity=name)
    for cycle in range(4):
        await ed.call("focus_window", window="game" if cycle % 2 else "scene", scene=sid)
        late = [("create_entity", {"scene": sid, "name": f"Late2D{cycle}", "type": "rigidbody2d",
                                   "position": [cycle * 30, 60, 0]})]
        await ed.batch(late + [("play", {"scene": sid, "action": "start", "wait": cycle % 2 == 1})])
        await ed.call("play", action="stop")
        await shoot(ed, "game")
        await shoot(ed, "scene")
    await ed.check_alive()
    await ed.call("close_scene", scene=sid, unsaved="discard")
