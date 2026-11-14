"""S94 AC22, AC23 (docs/specs/S94-rendering-anim-gaps.md F22, F23): the Outline component through the
existing tools. add_component outline with values, get_entity reads its fields back, set_properties
changes the mode and one undo restores it, a Scene view screenshot shows outline-coloured pixels
around a cube only while the Outline is enabled; an Outline on the root of an imported skinned FBX
outlines its mesh; Lua's Outline handle drives it in Play; apply_material_preset on a GameObject
without a Mesh applies to the meshes below it and its next names the Outline component.

Runs on an owned empty project.
"""

from __future__ import annotations

from pathlib import Path

from harness import Editor, find_key
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "Outline Test"}
CI = True
REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "editor" / "tests" / "fixtures" / "rigged_cube.fbx"
MAGENTA = [1, 0, 1, 1]

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Outlines", type="3D", path="scenes/Outlines")
    S["scene"] = made["scene"]["id"]
    await ed.call("create_entity", scene=S["scene"], type="cube", name="Hero", select=False)
    await ed.call("set_scene_camera", scene=S["scene"], position=[0, 0.8, 4.0], target=[0, 0, 0])
    await ed.call("save_scene", scene=S["scene"])
    return S


async def magenta_pixels(ed: Editor) -> int:
    """the Scene view's pixels of the outline colour"""
    await ed.call("wait", frames=4)
    png = await ed.screenshot(view="scene", max_width=320)
    width, height, channels, rows = decode_png(png)
    count = 0
    for row in rows:
        for x in range(0, width * channels, channels):
            r, g, b = row[x], row[x + 1], row[x + 2]
            if r > 200 and b > 200 and g < 80:
                count += 1
    return count


async def test_outline_component(ed: Editor):
    s = await setup(ed)
    before = await magenta_pixels(ed)
    ed.check_eq(before, 0, "nothing is magenta before the Outline")
    await ed.call("add_component", scene=s["scene"], entity="Hero", component="outline",
                  values={"outline": {"color": MAGENTA, "width": 4, "mode": "outline_visible"}})
    hero = await ed.call("get_entity", scene=s["scene"], entity="Hero")
    outline = hero["components"].get("outline")
    ed.check(outline is not None, f"get_entity lists the Outline: {list(hero['components'])}")
    ed.check_eq(outline["mode"], "outline_visible", f"the mode reads back: {outline}")
    ed.check_eq(outline["width"], 4, f"the width reads back: {outline}")
    ed.check_eq(outline["widthUnits"], "pixels", f"the width units default to Pixels: {outline}")
    ed.check_eq(outline["includeChildren"], True, f"Include Children defaults on: {outline}")
    drawn = await magenta_pixels(ed)
    ed.check(drawn > 40, f"the outline draws round the cube: {drawn} magenta pixels")

    await ed.call("set_properties", scene=s["scene"], entity="Hero", values={"outline": {"mode": "silhouette_only"}})
    hero = await ed.call("get_entity", scene=s["scene"], entity="Hero", properties=["mode"])
    ed.check_eq(hero["components"]["outline"]["mode"], "silhouette_only", "set_properties changes the mode")
    # nothing covers the cube: a silhouette alone shows nothing
    ed.check_eq(await magenta_pixels(ed), 0, "Silhouette Only of an uncovered cube draws nothing")
    await ed.call("undo", scene=s["scene"])
    hero = await ed.call("get_entity", scene=s["scene"], entity="Hero", properties=["mode"])
    ed.check_eq(hero["components"]["outline"]["mode"], "outline_visible", "one undo restores the mode")

    await ed.call("set_properties", scene=s["scene"], entity="Hero", values={"outline": {"enabled": False}})
    ed.check_eq(await magenta_pixels(ed), 0, "a disabled Outline draws nothing")
    await ed.call("set_properties", scene=s["scene"], entity="Hero", values={"outline": {"enabled": True}})

    bad = await ed.call_raw("set_properties", {"scene": s["scene"], "entity": "Hero", "values": {"outline": {"mode": "glow"}}})
    ed.check(bad.is_error and "outline_all" in bad.text, f"an unknown mode names the valid ones: {bad.text}")


async def test_outline_on_skinned_model_root(ed: Editor):
    s = await setup(ed)
    summary = await ed.call("get_project_summary")
    assets = summary["assets_dir"].rstrip("/") or "."
    models = f"{assets}/models" if assets != "." else "models"
    incoming = ed.scratch.root / "incoming"
    incoming.mkdir(parents=True, exist_ok=True)
    (incoming / "rigged_cube.fbx").write_bytes(FIXTURE.read_bytes())
    await ed.call("import_files", sources=[str(incoming / "rigged_cube.fbx")], to_dir=models)
    # the cube out of the way
    await ed.call("set_properties", scene=s["scene"], entity="Hero", values={"outline": {"enabled": False}})
    await ed.call("set_properties", scene=s["scene"], entity="Hero", values={"transform": {"position": [0, -50, 0]}})
    made = await ed.call("set_model", scene=s["scene"], create={"name": "Rig"}, path=f"{models}/rigged_cube.fbx")
    rig = find_key(made["model"], "id", "entity_id")
    ed.check_eq(await magenta_pixels(ed), 0, "the model alone has no magenta")
    await ed.call("add_component", scene=s["scene"], entity=rig, component="outline", values={"outline": {"color": MAGENTA, "width": 4}})
    drawn = await magenta_pixels(ed)
    ed.check(drawn > 20, f"the Outline on the model root outlines its skinned mesh: {drawn} magenta pixels")
    await ed.call("remove_component", scene=s["scene"], entity=rig, component="outline")
    await ed.call("set_properties", scene=s["scene"], entity="Hero", values={"transform": {"position": [0, 0, 0]}})
    await ed.call("delete_entity", scene=s["scene"], entity=rig)


LUA_OUTLINE = """
local hero = scene:findEntity("Hero")
local outline = Outline(scene, hero)
outline.mode = OutlineMode.OUTLINE_ALL
outline.widthUnits = OutlineWidthUnits.WORLD
outline.width = 0.05
outline.color = Vector4(0, 1, 0, 1)
return {outline.mode == OutlineMode.OUTLINE_ALL, outline.widthUnits == OutlineWidthUnits.WORLD, outline.width, outline.color.y}
"""


async def test_outline_from_lua(ed: Editor):
    s = await setup(ed)
    await ed.call("save_scene", scene=s["scene"])
    started = await ed.call("play", action="start", scene=s["scene"])
    ed.check_eq(started["state"], "playing", "the scene plays")
    try:
        values = (await ed.call("eval_lua", code=LUA_OUTLINE))["values"][0]
        ed.check(values[0] is True and values[1] is True, f"the enums read back: {values}")
        ed.check(abs(values[2] - 0.05) < 1e-6 and abs(values[3] - 1) < 1e-4, f"width and colour read back: {values}")
        hero = await ed.call("get_entity", entity="Hero", properties=["widthUnits", "width"])
        ed.check_eq(hero["components"]["outline"]["widthUnits"], "world", "Lua's units reach the component")
    finally:
        await ed.call("play", action="stop")


async def test_preset_on_model_root_descends(ed: Editor):
    s = await setup(ed)
    await ed.call("create_entity", scene=s["scene"], type="empty", name="Root", select=False)
    await ed.call("create_entity", scene=s["scene"], type="cube", name="Child", parent="Root", select=False)
    r = await ed.call("apply_material_preset", scene=s["scene"], entity="Root", preset="outline_hull")
    ed.check_eq(len(r["applied"]), 1, f"the preset reaches the child mesh: {r['applied']}")
    ed.check("Outline component" in r.get("next", ""), f"next suggests the Outline component: {r.get('next')}")
    child = await ed.call("get_entity", scene=s["scene"], entity="Child", properties=["submeshes[0].material.extraPass"])
    ed.check("outline" in str(child["components"]["mesh"]), f"the child got the hull pass: {child['components']['mesh']}")
    await ed.call("undo", scene=s["scene"])
    await ed.call("create_entity", scene=s["scene"], type="empty", name="Lonely", select=False)
    bad = await ed.call_raw("apply_material_preset", {"scene": s["scene"], "entity": "Lonely", "preset": "outline_hull"})
    ed.check(bad.is_error and "nor any below it" in bad.text, f"nothing below: the error says so: {bad.text}")
