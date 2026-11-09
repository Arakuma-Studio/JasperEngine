"""S52 AC13 (docs/specs/S52-sprite-instancing.md R34, R37, R15): 2D sorting and Sprite Batch through
the existing tools. The setting tags_and_layers.sorting_layers reads [{id, name}] in draw order and
takes names or {id, name} (a rename keeps the id, a removed id is never given again; Default
stays), graphics.transparency_sort_mode / _axis the Custom Axis;
set_properties takes sortingLayer as a name or an id and get_entity reads it back as {id, name};
an unknown name fails with the fix; a Game view screenshot shows the order change; a Sorting Group
is an ordinary component (undo removes it); a Sprite Batch filled by eval_lua in Play reports its
count and draws its instances.

Runs on an owned empty project.
"""

from __future__ import annotations

from harness import Editor
from htmlui_pixels import decode_png

EDITOR = {"create": True, "name": "Sorting 2D"}
CI = True

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="Sorting", type="2D", path="scenes/Sorting")
    S["scene"] = made["scene"]["id"]
    await ed.call("create_entity", scene=S["scene"], type="camera", name="Main Camera")
    await ed.call("set_main_camera", scene=S["scene"], entity="Main Camera")
    await ed.call("create_entity", scene=S["scene"], type="sprite", name="Red", position=[0, 0, 0])
    await ed.call("create_entity", scene=S["scene"], type="sprite", name="Green", position=[30, 30, 0])
    await ed.call("configure_game_view", resolution="640x360")
    await ed.call("save_scene", scene=S["scene"])
    return S


async def settings(ed: Editor, *keys: str) -> dict:
    return (await ed.call("get_project_settings", keys=list(keys)))["settings"]


def names(layers: list) -> list:
    return [layer["name"] for layer in layers]


async def test_sorting_layers_setting(ed: Editor):
    await setup(ed)
    got = await settings(ed, "tags_and_layers.sorting_layers", "graphics.transparency_sort_mode", "graphics.transparency_sort_axis")
    ed.check_eq(got["tags_and_layers.sorting_layers"], [{"id": 0, "name": "Default"}], "a new project has Default alone")
    ed.check_eq(got["graphics.transparency_sort_mode"], "default", "the mode is Default")
    ed.check_eq(got["graphics.transparency_sort_axis"], [0, 1, 0], "the axis is (0, 1, 0)")

    await ed.call("set_project_settings", values={"tags_and_layers.sorting_layers": ["Background", "Default", "Foreground"]})
    got = await settings(ed, "tags_and_layers.sorting_layers")
    ed.check_eq(names(got["tags_and_layers.sorting_layers"]), ["Background", "Default", "Foreground"], "the order reads back")

    bad = await ed.call_raw("set_project_settings", {"values": {"tags_and_layers.sorting_layers": ["Foreground"]}})
    ed.check(bad.is_error and "Default" in bad.text, f"Default cannot be removed: {bad.text}")
    bad = await ed.call_raw("set_project_settings", {"values": {"graphics.transparency_sort_mode": "sideways"}})
    ed.check(bad.is_error and "custom_axis" in bad.text, f"the mode's values are named: {bad.text}")

    await ed.call("set_project_settings", values={"graphics.transparency_sort_mode": "custom_axis",
                                                  "graphics.transparency_sort_axis": [0, 1, -0.25]})
    got = await settings(ed, "graphics.transparency_sort_mode", "graphics.transparency_sort_axis")
    ed.check_eq(got["graphics.transparency_sort_mode"], "custom_axis", "Custom Axis set")
    ed.check(abs(got["graphics.transparency_sort_axis"][2] + 0.25) < 1e-6, f"the axis set: {got}")
    # undo scope project restores the mode
    await ed.call("undo", scope="project")
    await ed.call("undo", scope="project")
    got = await settings(ed, "graphics.transparency_sort_mode")
    ed.check_eq(got["graphics.transparency_sort_mode"], "default", "undone")


async def test_sorting_layer_ids_kept(ed: Editor):
    """R1: a renderer keeps its layer through a rename ({id, name}), and a removed layer's id is
    never given to a later one (its renderers draw on Default, not on the newcomer)"""
    s = await setup(ed)
    await ed.call("set_project_settings", values={"tags_and_layers.sorting_layers": ["Default", "Back", "Front"]})
    layers = (await settings(ed, "tags_and_layers.sorting_layers"))["tags_and_layers.sorting_layers"]
    front = next(layer for layer in layers if layer["name"] == "Front")
    await ed.call("set_properties", scene=s["scene"], entity="Red", values={"mesh": {"sortingLayer": "Front"}})
    # rename Front to Foreground by its id: Red stays on it
    renamed = [layer if layer["name"] != "Front" else {"id": front["id"], "name": "Foreground"} for layer in layers]
    await ed.call("set_project_settings", values={"tags_and_layers.sorting_layers": renamed})
    red = await ed.call("get_entity", scene=s["scene"], entity="Red", properties=["sortingLayer"])
    ed.check_eq(red["components"]["mesh"]["sortingLayer"], {"id": front["id"], "name": "Foreground"}, "the rename keeps the renderer's layer")
    # remove Foreground, add Clouds: Clouds takes a new id, Red draws on Default
    await ed.call("set_project_settings", values={"tags_and_layers.sorting_layers": ["Default", "Back"]})
    await ed.call("set_project_settings", values={"tags_and_layers.sorting_layers": ["Default", "Back", "Clouds"]})
    layers = (await settings(ed, "tags_and_layers.sorting_layers"))["tags_and_layers.sorting_layers"]
    clouds = next(layer for layer in layers if layer["name"] == "Clouds")
    ed.check(clouds["id"] != front["id"], f"a removed layer's id is not given again: {layers}")
    red = await ed.call("get_entity", scene=s["scene"], entity="Red", properties=["sortingLayer"])
    ed.check(red["components"]["mesh"]["sortingLayer"]["name"] != "Clouds", f"Red is not moved onto Clouds: {red['components']['mesh']}")
    bad = await ed.call_raw("set_project_settings", {"values": {"tags_and_layers.sorting_layers": ["Default", {"id": 999, "name": "Nope"}]}})
    ed.check(bad.is_error and "999" in bad.text, f"an unknown id is refused: {bad.text}")
    await ed.call("set_properties", scene=s["scene"], entity="Red", values={"mesh": {"sortingLayer": "Default"}})


async def test_sorting_layer_property(ed: Editor):
    s = await setup(ed)
    await ed.call("set_project_settings", values={"tags_and_layers.sorting_layers": ["Background", "Default", "Foreground"]})
    await ed.call("set_properties", scene=s["scene"], entity="Red", values={"mesh": {"sortingLayer": "Foreground", "sortingOrder": 5}})
    red = await ed.call("get_entity", scene=s["scene"], entity="Red", properties=["sortingLayer", "sortingOrder"])
    mesh = red["components"]["mesh"]
    ed.check(mesh["sortingLayer"]["name"] == "Foreground" and isinstance(mesh["sortingLayer"]["id"], int),
             f"sortingLayer reads back as {{id, name}}: {mesh}")
    ed.check_eq(mesh["sortingOrder"], 5, "Order in Layer")
    # by id
    foreground = mesh["sortingLayer"]["id"]
    await ed.call("set_properties", scene=s["scene"], entity="Green", values={"mesh": {"sortingLayer": foreground}})
    green = await ed.call("get_entity", scene=s["scene"], entity="Green", properties=["sortingLayer"])
    ed.check_eq(green["components"]["mesh"]["sortingLayer"]["name"], "Foreground", "an id is accepted")
    # an unknown name names the fix, and changes nothing
    bad = await ed.call_raw("set_properties", {"scene": s["scene"], "entity": "Green", "values": {"mesh": {"sortingLayer": "Clouds"}}})
    ed.check(bad.is_error and "set_project_settings tags_and_layers.sorting_layers" in bad.text, f"the error names the fix: {bad.text}")
    await ed.call("set_properties", scene=s["scene"], entity="Green", values={"mesh": {"sortingLayer": "Default"}})


async def test_screenshot_order(ed: Editor):
    s = await setup(ed)
    await ed.call("set_project_settings", values={"tags_and_layers.sorting_layers": ["Background", "Default", "Foreground"]})
    # two overlapping sprites of their own colours: Green is later in the Hierarchy (on top by default)
    for name, color in (("Red", [1, 0, 0, 1]), ("Green", [0, 1, 0, 1])):
        await ed.call("set_properties", scene=s["scene"], entity=name,
                      values={"mesh": {"submeshes[0].material.baseColorFactor": color, "sortingLayer": "Default", "sortingOrder": 0}})
    await ed.call("wait", frames=3)

    async def overlap() -> tuple:
        png = await ed.screenshot(view="game", max_width=0)
        width, height, channels, rows = decode_png(png)
        counts = {"red": 0, "green": 0}
        for row in rows:
            for x in range(0, width * channels, channels):
                r, g, b = row[x], row[x + 1], row[x + 2]
                if r > 200 and g < 60 and b < 60:
                    counts["red"] += 1
                elif g > 200 and r < 60 and b < 60:
                    counts["green"] += 1
        return counts["red"], counts["green"]

    red_by_hierarchy, green_by_hierarchy = await overlap()
    ed.check(green_by_hierarchy > red_by_hierarchy, f"by the Hierarchy the green sprite covers the red: {red_by_hierarchy} {green_by_hierarchy}")
    await ed.call("set_properties", scene=s["scene"], entity="Red", values={"mesh": {"sortingLayer": "Foreground"}})
    await ed.call("wait", frames=3)
    red_by_layer, green_by_layer = await overlap()
    ed.check(red_by_layer > green_by_layer, f"the Foreground red covers the green: {red_by_layer} {green_by_layer}")
    await ed.call("set_properties", scene=s["scene"], entity="Red", values={"mesh": {"sortingLayer": "Default"}})


async def test_sorting_group_component(ed: Editor):
    s = await setup(ed)
    await ed.call("create_entity", scene=s["scene"], type="empty", name="Group")
    await ed.call("add_component", scene=s["scene"], entity="Group", component="sortinggroup")
    await ed.call("set_properties", scene=s["scene"], entity="Group", values={"sortinggroup": {"sortingLayer": "Default", "sortingOrder": 3}})
    group = await ed.call("get_entity", scene=s["scene"], entity="Group", properties=["sortingOrder", "sortingLayer"])
    ed.check_eq(group["components"]["sortinggroup"]["sortingOrder"], 3, "the group's Order in Layer")
    await ed.call("undo", scene=s["scene"])
    await ed.call("undo", scene=s["scene"])
    group = await ed.call("get_entity", scene=s["scene"], entity="Group")
    ed.check("sortinggroup" not in group["components"], f"undo removes the component: {list(group['components'])}")


FILL_LUA = """
local batch = SpriteBatch(scene, scene:findEntity("Bullets"))
local xy = {}
for i = 0, 99 do
    xy[#xy + 1] = (i % 10) * 20 - 100
    xy[#xy + 1] = math.floor(i / 10) * 20 - 100
end
batch:setPositions(xy)
local frames = {}
for i = 1, 100 do frames[i] = 0 end
frames[50] = 9
local ok, err = pcall(function() batch:setFrames(frames) end)
return {batch.count, ok, tostring(err)}
"""


async def test_sprite_batch_in_play(ed: Editor):
    s = await setup(ed)
    await ed.call("create_entity", scene=s["scene"], type="sprite", name="Bullets", position=[0, 0, 0])
    bad = await ed.call_raw("add_component", {"scene": s["scene"], "entity": "Main Camera", "component": "spritebatch"})
    ed.check(bad.is_error and "Sprite" in bad.text, f"a Sprite Batch goes on a Sprite: {bad.text}")
    await ed.call("add_component", scene=s["scene"], entity="Bullets", component="spritebatch")
    await ed.call("save_scene", scene=s["scene"])
    started = await ed.call("play", action="start", scene=s["scene"])
    ed.check_eq(started["state"], "playing", "the scene plays")
    try:
        values = (await ed.call("eval_lua", code=FILL_LUA))["values"][0]
        ed.check_eq(values[0], 100, f"the count: {values}")
        ed.check(values[1] is False and "frame_range" in values[2], f"a typed error at the call: {values}")
        await ed.call("wait", frames=4)
        bullets = await ed.call("get_entity", entity="Bullets")
        batch = bullets["components"]["spritebatch"]
        ed.check_eq(batch.get("count"), 100, f"get_entity reports count: {batch}")
        stats = await ed.call("get_render_stats", view="game")
        ed.check(stats.get("instances", 0) >= 100, f"get_render_stats counts the instances: {stats.get('instances')}")
    finally:
        await ed.call("play", action="stop")
