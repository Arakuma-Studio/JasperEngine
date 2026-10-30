"""S27 colours for agents (docs/specs/S27-editor-tools.md AC9, AC12, AC14; jasper://docs/editor-tools):
the colour forms set_properties takes (hex and {srgb} converted on a linear field, {linear}, HDR
intensity only on HDR fields), sample_view_color reading the Game view's pixel exactly, and the
swatch libraries edit_swatches keeps in the user's settings folder and the project's
EditorSettings/Swatches.
"""

import json

import harness
import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Color Tools"}

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["editor_tools"])
    made = await ed.call("create_scene", name="Colors", type="3D", path="scenes/Colors")
    S["scene"] = made["scene"]["id"]
    S["lamp"] = (await ed.call("create_entity", scene=S["scene"], type="point_light", name="Lamp"))["entity_id"]
    S["cube"] = (await ed.call("create_entity", scene=S["scene"], type="cube", name="Cube"))["entity_id"]
    return S


async def value_of(ed: Editor, entity, component: str, prop: str):
    got = await ed.call("get_entity", scene=S["scene"], entity=entity, components=[component], properties=[prop])
    value = harness.find_key(got["components"][component], prop)
    return value.get("value", value) if isinstance(value, dict) else value


def near(a, b, tol=1e-4) -> bool:
    return len(a) >= len(b) and all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))


async def test_set_color_forms(ed: Editor):
    await setup(ed)
    # "#FF8000" is sRGB: the light's colour is linear, so it stores (1, 0.2159, 0)
    await ed.call("set_properties", scene=S["scene"], entity=S["lamp"], values={"light": {"color": "#FF8000"}})
    color = await value_of(ed, S["lamp"], "light", "color")
    ed.check(near(color, [1.0, 0.21586, 0.0]), f"#FF8000 on a linear field: {color}")
    # {srgb} says the same; {linear} is stored as given
    await ed.call("set_properties", scene=S["scene"], entity=S["lamp"], values={"light": {"color": {"srgb": [0.5, 0.5, 0.5]}}})
    ed.check(near(await value_of(ed, S["lamp"], "light", "color"), [0.21404, 0.21404, 0.21404]), "srgb 0.5 is linear 0.214")
    await ed.call("set_properties", scene=S["scene"], entity=S["lamp"], values={"light": {"color": {"linear": [0.25, 0.5, 0.75]}}})
    ed.check(near(await value_of(ed, S["lamp"], "light", "color"), [0.25, 0.5, 0.75]), "linear stored unchanged")
    # a plain vector is the field's own space, unchanged
    await ed.call("set_properties", scene=S["scene"], entity=S["lamp"], values={"light": {"color": [0.1, 0.2, 0.3]}})
    ed.check(near(await value_of(ed, S["lamp"], "light", "color"), [0.1, 0.2, 0.3]), "a vector stays as given")
    # intensity: HDR fields only (the material's emission), 2^stops
    emission = "submeshes[0].material.emissiveFactor"
    await ed.call("set_properties", scene=S["scene"], entity=S["cube"], values={"mesh": {emission: {"hex": "#FF8000", "intensity": 2}}})
    ed.check(near(await value_of(ed, S["cube"], "mesh", emission), [4.0, 0.86344, 0.0], 1e-3), "emission x 2^2")
    await ed.check_error("set_properties", {"scene": S["scene"], "entity": S["lamp"],
                                            "values": {"light": {"color": {"hex": "#FF8000", "intensity": 1}}}},
                         contains="intensity")
    await ed.check_error("set_properties", {"scene": S["scene"], "entity": S["lamp"], "values": {"light": {"color": "#FF80"}}})
    # an sRGB field (the base colour of a UI image is not linear) takes hex as is: alpha too
    ui = (await ed.call("create_scene", name="ColorsUI", type="UI"))["scene"]["id"]
    image = (await ed.call("create_entity", scene=ui, type="image", name="Swatch"))["entity_id"]
    await ed.call("set_properties", scene=ui, entity=image, values={"ui": {"color": "#FF800080"}})
    got = await ed.call("get_entity", scene=ui, entity=image, components=["ui"], properties=["color"])
    color = harness.find_key(got["components"]["ui"], "color")
    color = color.get("value", color) if isinstance(color, dict) else color
    ed.check(near(color, [1.0, 128 / 255, 0.0, 128 / 255]), f"#FF800080 on an sRGB field: {color}")
    await ed.call("close_scene", scene=ui, unsaved="discard")
    await ed.call("select_scene", scene=S["scene"])


async def test_sample_view_color(ed: Editor):
    await setup(ed)
    # a 2D scene (unlit) filled with a red sprite: the Game view's pixels are #FF0000
    made = await ed.call("create_scene", name="RedView", type="2D")
    scene = made["scene"]["id"]
    await ed.call("set_project_settings", values={"graphics.canvas_width": 320, "graphics.canvas_height": 180})
    await ed.call("create_entity", scene=scene, type="sprite", name="Red", position=[0, 0, 0],
                  values={"sprite": {"width": 4000, "height": 4000},
                          "mesh": {"submeshes[0].material.baseColorFactor": [1, 0, 0, 1]}})
    await ed.call("select_scene", scene=scene)
    shot = await ed.screenshot(view="game", max_width=0)
    width, height, rgba = pngdiff.decode(shot)
    probe = await ed.call_raw("sample_view_color", {"view": "game", "x": 0, "y": 0})
    ed.check(not probe.is_error, f"the Game view has an image: {probe.text}")
    size = probe.value["size"]
    x, y = size["width"] // 2, size["height"] // 2
    got = await ed.call("sample_view_color", view="game", x=x, y=y)
    ed.check_eq(got["hex"], "#FF0000", f"the red sprite: {json.dumps(got)}")
    ed.check(near(got["linear"], [1.0, 0.0, 0.0]), "linear (1, 0, 0)")
    # the screenshot of the same view agrees where both show the sprite
    if (width, height) == (size["width"], size["height"]):
        i = (y * width + x) * 4
        ed.check_eq(list(rgba[i:i + 3]), got["bytes"][:3], "screenshot and sample_view_color read the same pixel")
    await ed.check_error("sample_view_color", {"view": "game", "x": size["width"], "y": 0}, contains="inside")
    await ed.call("close_scene", scene=scene, unsaved="discard")
    await ed.call("select_scene", scene=S["scene"])


async def test_edit_swatches(ed: Editor):
    await setup(ed)
    listed = await ed.call("edit_swatches", action="list", scope="user")
    ed.check(isinstance(listed.get("libraries"), list), f"libraries listed: {json.dumps(listed)[:400]}")
    await ed.call("edit_swatches", action="create_library", scope="project", library="Level Art")
    await ed.call("edit_swatches", action="add", scope="project", library="Level Art", name="Sky", color="#4080FF")
    await ed.call("edit_swatches", action="add", scope="project", library="Level Art", name="Lava",
                  color={"linear": [4.0, 0.5, 0.0], "intensity": 0})
    files = list((ed.project / "EditorSettings" / "Swatches").glob("*.colors"))
    ed.check_eq(len(files), 1, f"one project library: {files}")
    data = json.loads(files[0].read_text())
    ed.check_eq(data["name"], "Level Art")
    ed.check_eq([s["name"] for s in data["swatches"]], ["Sky", "Lava"])
    ed.check(near(data["swatches"][0]["linear"], [0.05127, 0.21586, 1.0, 1.0], 1e-3), f"Sky stored linear: {data['swatches'][0]}")
    ed.check(data["swatches"][1]["hdr"] is True, "an HDR swatch says so")
    await ed.call("edit_swatches", action="remove", scope="project", library="Level Art", index=0)
    data = json.loads(files[0].read_text())
    ed.check_eq([s["name"] for s in data["swatches"]], ["Lava"], "remove by index")
    # a library from a newer editor is read-only
    newer = files[0].parent / "Future.colors"
    newer.write_text(json.dumps({"version": 99, "name": "Future", "swatches": []}))
    await ed.call("wait", frames=2)
    listed = await ed.call("edit_swatches", action="list", scope="project")
    future = [lib for lib in listed["libraries"] if lib["name"] == "Future"]
    ed.check(future and future[0].get("read_only") is True, f"unknown version read-only: {json.dumps(listed)[:600]}")
    await ed.check_error("edit_swatches", {"action": "add", "scope": "project", "library": "Future", "name": "X", "color": "#FFFFFF"},
                         contains="read-only")
    # the user scope is the settings folder
    await ed.call("edit_swatches", action="create_library", scope="user", library="Mine")
    await ed.call("edit_swatches", action="add", scope="user", library="Mine", name="White", color="#FFFFFF")
    user_files = list(ed.scratch.home.rglob("swatches/*.colors"))
    ed.check(any(json.loads(f.read_text())["name"] == "Mine" for f in user_files), f"user library in the settings folder: {user_files}")
    await ed.call("edit_swatches", action="delete_library", scope="user", library="Mine")
    user_files = list(ed.scratch.home.rglob("swatches/*.colors"))
    ed.check(not any(json.loads(f.read_text())["name"] == "Mine" for f in user_files), "deleted")
