"""Stopping Play right after it starts, with every Scene view gizmo map populated.

The Scene view keeps an editor gizmo per annotated GameObject (SceneRender3D: light, camera and
audio icons; body, joint, bone, line, polygon, track and reflection probe lines). The Game view pass
hides them all (SceneRender::beginGameViewPass -> hideAllGizmos) before it draws, so a gizmo whose
own entity went away with Stop threw "FATAL: unhandled exception: unordered_map::at" from
GameObject::setVisible. Each cycle plays and stops right away (during the startup, before the game
runs; at once; after a few frames in which the game spawns and destroys annotated GameObjects), once
with the Scene view and once with the Game view in front (only the Game view pass draws the scene),
on the pose fixture (two skinned models) plus one GameObject of each gizmo kind. The scene's System
entities (the editor's gizmos among them) stay as many across the cycles, but for the body added
each cycle: none is lost or left over.
"""

from __future__ import annotations

import harness
from harness import Editor

EDITOR = {"project": harness.REPO / "tools" / "fixtures" / "pose-animation"}
CI = True
TIMEOUT = 600

# (variant, view in front): every variant with each view
CYCLES = [(variant, view) for view in ("scene", "game") for variant in ("starting", "at_once", "after_frames")]

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

# the edited scene's System entities (ids 1..1000): the editor camera, the gizmos and icons
SYSTEM_COUNT = """
    local n = 0
    for e = 1, 1000 do if scene:isEntityCreated(e) then n = n + 1 end end
    return n
"""

S: dict = {}


async def shoot(ed: Editor, view: str) -> None:
    image = await ed.call_raw("screenshot", {"view": view, "max_width": 160})
    ed.check(not image.is_error and image.images, image.text)


async def batch_ok(ed: Editor, calls: list[tuple[str, dict]], **args) -> None:
    results = (await ed.batch(calls, **args))["results"]
    bad = [r for r in results if not r["ok"]]
    ed.check(not bad and len(results) == len(calls), f"batch: {bad or results}")


async def system_entities(ed: Editor) -> int:
    # a Scene view frame first: update() makes and prunes the gizmos
    await ed.call("focus_window", window="scene")
    await shoot(ed, "scene")
    return (await ed.call("eval_lua", code=SYSTEM_COUNT, allow_edit_mode=True))["values"][0]


async def test_build_scene_with_every_gizmo(ed: Editor):
    calls = [("create_entity", {"name": name, "type": kind, "position": [i * 1.5, 1, -3]})
             for i, (kind, name) in enumerate(KINDS)]
    calls += [("create_entity", {"name": name, "type": kind}) for kind, name in UNPLACED]
    calls += [
        ("create_entity", {"name": "Body", "type": "cube", "position": [0, 1, 3]}),
        ("add_component", {"entity": "Body", "component": "Rigidbody"}),
    ]
    await batch_ok(ed, calls)
    # every gizmo exists before the first Play
    S["system"] = await system_entities(ed)
    ed.check(S["system"] > 20, f"the scene's System entities: {S['system']}")


async def test_stop_right_after_start(ed: Editor):
    for cycle, (variant, view) in enumerate(CYCLES):
        await ed.call("focus_window", window=view)
        # a body added in the same frame as Play: its gizmo is made while the game runs
        late = [("create_entity", {"name": f"Late{cycle}", "type": "cube", "position": [cycle, 2, 0],
                                   "components": ["Rigidbody"]})]
        if variant == "starting":
            # stopped while still starting (the game never ran)
            await batch_ok(ed, late + [("play", {"action": "start", "wait": False})])
            await ed.call("play", action="stop")
        elif variant == "at_once":
            await batch_ok(ed, late + [("play", {"action": "start"})])
            await ed.call("play", action="stop")
        else:
            await batch_ok(ed, late + [("play", {"action": "start", "auto_pause": True, "fixed_fps": 60})])
            try:
                spawned = (await ed.call("eval_lua", code=SPAWN))["values"][0]
                ed.check(spawned >= 8, f"the game copied the annotated GameObjects: {spawned}")
                await ed.call("focus_window", window="scene")
                await ed.call("wait", frames=3)
            finally:
                await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
        # the first frames after Stop: the Game view pass hides every gizmo
        await shoot(ed, "game")
        # one body gizmo more per cycle; the copies' gizmos went, the authored ones stayed
        count = await system_entities(ed)
        ed.check_eq(count, S["system"] + cycle + 1, f"cycle {cycle} ({variant}, {view}): System entities")
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
    await batch_ok(ed, [("create_entity", {"scene": sid, "name": name, "type": kind, "position": [i * 40, 0, 0]})
                        for i, (kind, name) in enumerate(kinds)]
                   + [("create_entity", {"scene": sid, "name": "Audio2D", "type": "audio_source"})])
    for cycle, view in enumerate(("scene", "game")):
        await ed.call("focus_window", window=view, scene=sid)
        late = [("create_entity", {"scene": sid, "name": f"Late2D{cycle}", "type": "rigidbody2d",
                                   "position": [cycle * 30, 60, 0]})]
        await batch_ok(ed, late + [("play", {"scene": sid, "action": "start", "wait": cycle == 1})])
        await ed.call("play", action="stop")
        await shoot(ed, "game")
        await shoot(ed, "scene")
    await ed.check_alive()
    await ed.call("close_scene", scene=sid, unsaved="discard")
