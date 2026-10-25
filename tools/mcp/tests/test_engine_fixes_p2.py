"""Engine fixes after the phase 2 review (sl/p2-fix-engine): the scene's sky is the first skybox
in the Hierarchy, live: moving another skybox first switches the sky at once, to the one a reload
of the saved scene shows (it used to change only after a reload or in an export)."""

from __future__ import annotations

from harness import Editor, find_key

from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "Live Sky Order"}


def image_difference(a: bytes, b: bytes) -> float:
    """Mean RGB difference; every capture contains the same scene and camera."""
    wa, ha, ca, ra = decode_png(a)
    wb, hb, cb, rb = decode_png(b)
    assert (wa, ha) == (wb, hb), "scene captures differ in size"
    return sum(abs(ra[y][x * ca + c] - rb[y][x * cb + c])
               for y in range(ha) for x in range(wa) for c in range(3)) / (wa * ha * 3)

TIMEOUT = 600

SCENE = "scenes/SkyOrder.scene"


async def make(ed: Editor, name: str, type_: str):
    made = await ed.call("create_entity", name=name, type=type_)
    eid = find_key(made, "entity_id", "id")
    ed.check(eid is not None, f"create_entity returned no id: {made!r}")
    return eid


async def settled_shot(ed: Editor) -> bytes:
    # The screenshot tool waits for loaded assets and a freshly rendered scene.
    shot = await ed.call_raw("screenshot", {"view": "scene", "max_width": 320})
    ed.check(not shot.is_error and shot.images, f"no screenshot: {shot.text[:300]}")
    return shot.images[0][0]


async def test_skybox_order_is_live(ed: Editor):
    ed.require_tools("create_scene", "create_entity", "set_properties", "move_entity", "screenshot", "save_scene",
                     "open_scene", "close_scene", "find_entities", "delete_entities")
    made = await ed.call("create_scene", name="SkyOrder", type="3d")
    scene_id = made["scene"]["id"]
    # a new 3D scene has a skybox of its own: only the two below
    for sky in (await ed.call("find_entities", scene=scene_id, component="skybox"))["entities"]:
        await ed.call("delete_entities", entities=[sky["id"]])
    gradient = await make(ed, "GradientSky", "skybox")
    procedural = await make(ed, "ProceduralSky", "skybox")
    await ed.call("set_properties", entity=gradient, values={"skybox": {"customShader": "library:gradient_sky"}})
    await ed.call("set_properties", entity=procedural, values={"skybox": {"customShader": "library:procedural_sky"}})
    await make(ed, "Ball", "sphere")

    first = await settled_shot(ed)
    # the procedural sky first in the Hierarchy: it is the scene's sky at once
    await ed.call("move_entity", entity=procedural, before=gradient)
    moved = await settled_shot(ed)
    ed.check(image_difference(first, moved) > 4.0, "moving the other skybox first changes the sky live")

    # what a reload of the saved scene shows
    await ed.call("save_scene", scene=scene_id, path=SCENE)
    await ed.call("close_scene", scene=scene_id)
    await ed.call("open_scene", scene=SCENE)
    reloaded = await settled_shot(ed)
    ed.check(image_difference(moved, reloaded) < 1.0,
             f"live and reloaded skies match (difference {image_difference(moved, reloaded):.2f})")

    # and back
    await ed.call("move_entity", entity="GradientSky", before="ProceduralSky")
    back = await settled_shot(ed)
    ed.check(image_difference(first, back) < 1.0, f"moving it back restores the first sky ({image_difference(first, back):.2f})")
