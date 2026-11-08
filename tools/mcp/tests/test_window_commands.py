"""S61 by real input: a key does what the focused panel says, and only that.

Scene GameObjects selected, the Curve window focused: Delete, F, Ctrl+C and Ctrl+V reach the curve
through the one dispatcher and never the scene (the mouse over the Scene view too: its keys are
active under the pointer). The Edit menu, the palette and run_command follow the same claim, a
text field keeps its keys, the Curve and Color windows' keys rebind (AC3), and every other panel
with a claim leaves the scene alone (AC2; the Animation, Animator and Pose Animation windows have
theirs in test_animation_window.py, test_animator_window.py and test_pose_window.py). xdotool keys
in the harness's Xvfb, checked through MCP.
"""

from __future__ import annotations

import harness
from harness import Editor
import ui_input

EDITOR = {"xvfb": "harness", "create": True, "name": "Window Commands"}
CI = True
TIMEOUT = 900

Keys = ui_input.Ui

# three keys: a box drag over the whole graph selects them all
BUMP = [[0, 1, 0, 0], [0.5, 0.5, 0, 0], [1, 0, 0, 0]]


async def fresh_scene(ed: Editor, name: str) -> dict:
    """A saved scene with a cube that has a force field (its Falloff is a curve field), the cube
    selected: what the scene's Frame, Copy and Delete would act on."""
    made = await ed.call("create_scene", name=name, type="3D", path=f"scenes/{name}")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    await ed.call("save_scene", scene=scene, path=f"scenes/{name}.scene")
    cube = (await ed.call("create_entity", scene=scene, type="cube", name="Mover"))["entity_id"]
    await ed.call("add_component", scene=scene, entity=cube, component="forcefield")
    await ed.call("set_properties", scene=scene, entity=cube, values={"forcefield": {"falloff": BUMP}})
    await ed.call("select_entities", scene=scene, entities=[cube])
    return {"scene": scene, "cube": cube}


async def entity_count(ed: Editor, scene: int) -> int:
    found = await ed.call("find_entities", scene=scene, name="*")
    return len(harness.find_key(found, "entities") or [])


async def curve_keys(ed: Editor, s: dict) -> list:
    got = await ed.call("get_entity", scene=s["scene"], entity=s["cube"], components=["forcefield"], detail="full")
    curve = got["components"]["forcefield"]["falloff"]
    curve = curve.get("value", curve) if isinstance(curve, dict) else curve
    return [k[0] for k in curve]


async def open_curve_window(ed: Editor, keys: Keys) -> dict:
    """The Tween's curve field clicked in the Inspector: the Curve window opens with the focus.
    Returns the graph's item (its rect, its window's rect)."""
    await ed.call("focus_window", window="inspector")
    await keys.click_revealed("##curveField", "Inspector")
    graph = await keys.find("##graph", "Curve")
    if graph is None:
        ed.fail("the Curve window's graph is not drawn after a click on the field")
    for _ in range(25):
        status = await ed.call("editor_status")
        if "Curve" in (status["windows"]["focused"] or ""):
            return graph
        await keys.frames(2)
    ed.fail(f"the Curve window has no focus: {status['windows']}")
    return graph


async def box_select_all(keys: Keys, graph: dict) -> None:
    rect = graph["rect"]
    await keys.drag(keys.at(rect, (3, 3)), keys.at(rect, (rect["width"] - 3, rect["height"] - 3)))


async def over_the_scene_view(ed: Editor, keys: Keys, area: dict, graph: dict) -> None:
    """The pointer on the Scene view (area: its rect), outside the Curve window; the keyboard focus stays."""
    curve = graph["window_rect"]
    # (its lower corners: the upper ones hold the toolbars)
    corners = [(area["x"] + 24, area["y"] + area["height"] - 24),
               (area["x"] + area["width"] - 24, area["y"] + area["height"] - 24)]
    for x, y in corners:
        if not (curve["x"] <= x <= curve["x"] + curve["width"] and curve["y"] <= y <= curve["y"] + curve["height"]):
            await keys.move(keys.at({"x": x, "y": y}))
            return
    ed.fail(f"the Curve window {curve} covers the Scene view {area}")


async def test_delete_goes_to_the_curve_not_the_scene(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "DeleteHere")
    before = await entity_count(ed, s["scene"])
    graph = await open_curve_window(ed, keys)
    await box_select_all(keys, graph)
    await keys.key("Delete")
    ed.check_eq(await entity_count(ed, s["scene"]), before, "Delete in the Curve window left the GameObjects alone")
    # (a curve keeps its last key: curveops::deleteKeys)
    ed.check_eq(len(await curve_keys(ed, s)), 1, "and removed the selected curve keys")
    # one named step: Undo brings the keys back
    ed.check_in("Falloff", (await ed.call("editor_status"))["undo"]["undo_name"], "the step's name")
    await ed.call("undo")
    ed.check_eq(await curve_keys(ed, s), [0, 0.5, 1], "Undo brought the keys back")


async def test_frame_goes_to_the_curve_not_the_scene(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "FrameHere")
    await ed.call("set_scene_camera", scene=s["scene"], position=[50, 50, 50], target=[40, 40, 40])
    await keys.frames(4)
    camera = (await ed.call("set_scene_camera", scene=s["scene"]))["camera"]
    graph = await open_curve_window(ed, keys)
    await keys.wheel(keys.centre(graph["rect"]), -4)
    await keys.key("f")
    await keys.frames(60)
    ed.check_eq((await ed.call("set_scene_camera", scene=s["scene"]))["camera"], camera,
                "F in the Curve window left the Scene view's camera where it was")
    view = (await ed.call("editor_status"))["ui"]["curve_window"]["view"]
    ed.check(view["t0"] <= 0.0 and view["t1"] >= 1.0, f"and framed the curve: {view}")


async def test_copy_paste_go_to_the_curve_not_the_scene(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "CopyHere")
    before = await entity_count(ed, s["scene"])
    area = (await ed.call("focus_window", window="scene", scene=s["scene"]))["rect"]
    # A wrong scene Paste must have a live clipboard to act on.
    await ed.call("run_command", id="edit.copy")
    await ed.call("run_command", id="edit.paste")
    ed.check_eq(await entity_count(ed, s["scene"]), before + 1, "the scene clipboard can paste the selected GameObject")
    await ed.call("undo", scene=s["scene"])
    ed.check_eq(await entity_count(ed, s["scene"]), before, "Undo restored the scene before the curve's keys")
    await ed.call("select_entities", scene=s["scene"], entities=[s["cube"]])
    graph = await open_curve_window(ed, keys)
    await box_select_all(keys, graph)
    await over_the_scene_view(ed, keys, area, graph)
    await keys.key("ctrl+c")
    # The original key count must require Paste to change its destination.
    await keys.key("Delete")
    ed.check_eq(len(await curve_keys(ed, s)), 1, "one destination key remains before Paste")
    await keys.key("ctrl+v")
    await keys.frames(10)
    ed.check_eq(await entity_count(ed, s["scene"]), before, "Ctrl+C / Ctrl+V left the GameObjects alone")
    ed.check(len(await curve_keys(ed, s)) >= 3, "and the curve keeps its keys")
    ed.check_eq((await ed.call("editor_status"))["ui"]["curve_window"]["clipboard_keys"], 3, "the curve's clipboard took the keys")


# ---- R2, R3: the Edit menu and the palette say and do what the panel says

async def test_edit_menu_and_palette_follow_the_claim(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "MenuHere")
    before = await entity_count(ed, s["scene"])
    graph = await open_curve_window(ed, keys)

    # nothing selected in the curve: Edit > Delete is off, it does not fall through to the scene
    await keys.click(keys.at(graph["rect"], (graph["rect"]["width"] / 2, 3)))
    await ed.check_error("run_command", {"id": "edit.delete"}, contains="disabled")
    await keys.key("Delete")
    ed.check_eq(await entity_count(ed, s["scene"]), before, "no key selected: Delete does nothing")
    ed.check_eq(len(await curve_keys(ed, s)), 3, "and removes nothing")

    # Edit > Frame Selected, a real click in the menu: the curve's view moves
    await keys.wheel(keys.centre(graph["rect"]), -6)
    zoomed = (await ed.call("editor_status"))["ui"]["curve_window"]["view"]
    await keys.click_item("Edit")
    await keys.click_item("Frame Selected")
    await keys.frames(4)
    view = (await ed.call("editor_status"))["ui"]["curve_window"]["view"]
    ed.check(view["t1"] - view["t0"] > zoomed["t1"] - zoomed["t0"], f"Edit > Frame Selected framed the curve: {zoomed} -> {view}")

    # the palette: the same command, the same place
    await keys.wheel(keys.centre(graph["rect"]), -6)
    zoomed = (await ed.call("editor_status"))["ui"]["curve_window"]["view"]
    await keys.key("ctrl+k")
    await keys.type(">Frame Selected")
    await keys.frames(6)
    results = []
    for _ in range(25):
        results = (await ed.call("editor_status"))["ui"]["search_palette"]["results"]
        if "command: Frame Selected" in results:
            break
        await keys.frames(2)
    for _ in range(results.index("command: Frame Selected")):
        await keys.key("Down")
    await keys.key("Return")
    await keys.frames(4)
    view = (await ed.call("editor_status"))["ui"]["curve_window"]["view"]
    ed.check(view["t1"] - view["t0"] > zoomed["t1"] - zoomed["t0"], f"the palette's Frame Selected framed the curve: {zoomed} -> {view}")

    # Edit > Delete, with keys selected: a real click on the claimed item
    await box_select_all(keys, graph)
    await keys.click_item("Edit")
    await keys.click_item("Delete")
    await keys.frames(4)
    ed.check_eq(len(await curve_keys(ed, s)), 1, "Edit > Delete removed the curve's keys")
    ed.check_eq(await entity_count(ed, s["scene"]), before, "and no GameObject")

    # an agent's run_command too
    await ed.call("undo")
    await keys.frames(4)
    await box_select_all(keys, graph)
    await ed.call("run_command", id="edit.delete")
    await keys.frames(4)
    ed.check_eq(len(await curve_keys(ed, s)), 1, "run_command edit.delete removed the curve's keys")
    ed.check_eq(await entity_count(ed, s["scene"]), before, "and no GameObject")


# ---- R6: a focused input keeps its keys

async def test_text_fields_keep_their_keys(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "TypeHere")
    before = await entity_count(ed, s["scene"])
    graph = await open_curve_window(ed, keys)
    # one key selected: a box around the middle key, from where the view puts it; its Value field shows
    await keys.key("f")
    view = (await ed.call("editor_status"))["ui"]["curve_window"]["view"]
    rect = graph["rect"]
    x = rect["width"] * (0.5 - view["t0"]) / (view["t1"] - view["t0"])
    y = rect["height"] * (view["v1"] - 0.5) / (view["v1"] - view["v0"])
    await keys.drag(keys.at(rect, (x - 14, y - 14)), keys.at(rect, (x + 14, y + 14)))
    ed.check_eq((await ed.call("editor_status"))["ui"]["curve_window"]["selected_keys"], 1, "the middle key selected")
    field = await keys.find("##keyValue", "Curve")
    ed.check(field, "the Value field is drawn")
    await keys.double_click(keys.centre(field["rect"]))
    await keys.frames(4)
    await keys.key("Delete", "BackSpace", "f", "ctrl+c", "ctrl+v")
    ed.check_eq(await entity_count(ed, s["scene"]), before, "typing: no GameObject deleted")
    ed.check_eq(len(await curve_keys(ed, s)), 3, "typing: no key deleted or pasted")
    ed.check_eq((await ed.call("editor_status"))["ui"]["curve_window"]["clipboard_keys"], 0, "typing: Ctrl+C stayed in the field")
    await keys.key("Escape")


# ---- R5, AC3: rebinding the Curve window's and the Color window's keys

async def test_rebound_curve_frame(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "RebindHere")
    graph = await open_curve_window(ed, keys)
    listed = {i["id"]: i for i in (await ed.call("list_shortcuts", context="curve"))["shortcuts"]}
    ed.check_eq(sorted(listed), ["curve.copy", "curve.delete", "curve.frame", "curve.paste"], "the Curve context's commands")
    ed.check_eq(listed["curve.delete"]["chords"], ["Delete", "Backspace"], "with the keys they had")
    await ed.call("set_shortcut", id="curve.frame", chords=["J"])
    try:
        await keys.wheel(keys.centre(graph["rect"]), -6)
        zoomed = (await ed.call("editor_status"))["ui"]["curve_window"]["view"]
        await keys.key("f")
        await keys.frames(4)
        ed.check_eq((await ed.call("editor_status"))["ui"]["curve_window"]["view"], zoomed, "the old key no longer frames")
        await keys.key("j")
        await keys.frames(4)
        view = (await ed.call("editor_status"))["ui"]["curve_window"]["view"]
        ed.check(view["t1"] - view["t0"] > zoomed["t1"] - zoomed["t0"], f"the new key frames: {zoomed} -> {view}")
        menu = {i["id"]: i for i in (await ed.call("list_shortcuts", group="Edit"))["shortcuts"]}
        ed.check_eq(menu["edit.frameSelected"]["menu_shortcut"], "J", "Edit > Frame Selected shows the panel's key")
    finally:
        await ed.call("set_shortcut", id="curve.frame", chords=None)
    await ed.call("select_entities", scene=s["scene"], entities=[])


async def test_color_picker_eyedropper_and_edit_keys(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "ColorHere")
    before = await entity_count(ed, s["scene"])
    await ed.call("set_scene_camera", scene=s["scene"], position=[50, 50, 50], target=[40, 40, 40])
    await keys.frames(4)
    camera = (await ed.call("set_scene_camera", scene=s["scene"]))["camera"]
    await ed.call("run_command", id="window.colorPicker")
    for _ in range(25):
        picker = (await ed.call("editor_status"))["ui"]["color_picker"]
        if picker["focused"]:
            break
        await keys.frames(2)
    ed.check(picker["focused"], f"the Color window has the focus: {picker}")

    # a window with no edit keys: they do nothing, and do not reach the scene
    await keys.key("Delete", "f")
    await keys.frames(30)
    ed.check_eq(await entity_count(ed, s["scene"]), before, "Delete in the Color window: no GameObject deleted")
    ed.check_eq((await ed.call("set_scene_camera", scene=s["scene"]))["camera"], camera, "F in the Color window: the Scene view stays")

    async def sampling() -> bool:
        await keys.frames(2)
        return (await ed.call("editor_status"))["ui"]["color_picker"]["sampling"]

    async def picker_focused() -> None:
        # (Esc that stops the eyedropper closes the window too: open it again)
        await ed.call("run_command", id="window.colorPicker")
        for _ in range(25):
            if (await ed.call("editor_status"))["ui"]["color_picker"]["focused"]:
                return
            await keys.frames(2)
        ed.fail("the Color window has no focus")

    await keys.key("i")
    ed.check(await sampling(), "I starts the eyedropper")
    await keys.key("Escape")
    ed.check(not await sampling(), "Esc stops it")
    await picker_focused()
    await ed.call("set_shortcut", id="colorPicker.eyedropper", chords=["J"])
    try:
        await keys.key("i")
        ed.check(not await sampling(), "the old key no longer starts it")
        await keys.key("j")
        ed.check(await sampling(), "the new key does")
        await keys.key("Escape")
        ed.check(not await sampling(), "Esc stops it")
    finally:
        await ed.call("set_shortcut", id="colorPicker.eyedropper", chords=None)
    await keys.key("Escape")


# ---- AC2: every panel with a claim: its Delete affects it, not the scene

async def press_in_panel(ed: Editor, keys: Keys, s: dict, count: int, what: str, *combo: str) -> None:
    await keys.key(*combo)
    await keys.frames(6)
    ed.check_eq(await entity_count(ed, s["scene"]), count, f"{what}: no GameObject deleted")


async def project_entry(ed: Editor, keys: Keys, name: str) -> dict:
    """The Project window focused, in the assets folder, with this file drawn."""
    await ed.call("focus_window", window="project")
    await keys.frames(6)
    state = {}
    for _ in range(25):
        state = (await ed.call("editor_status"))["ui"]["project_window"]
        entry = next((e for e in state["entries"] if e["name"] == name), None)
        if entry:
            return entry
        folder = next((n for n in state["tree"] if n["path"] == "assets"), None)
        if folder:
            await keys.click(keys.centre(folder["rect"]))
        await keys.frames(4)
    ed.fail(f"{name} in the Project window: {state.get('directory')} {[e['name'] for e in state.get('entries', [])]}")
    return {}


async def test_console_list_leaves_the_scene_alone(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "ConsoleHere")
    count = await entity_count(ed, s["scene"])
    console = await ed.call("focus_window", window="console")
    await keys.frames(2)
    await keys.click(keys.centre(console["rect"]))
    ed.check_eq((await ed.call("editor_status"))["windows"]["focused"], "Console", "the click stayed in the Console")
    await press_in_panel(ed, keys, s, count, "Console", "Delete", "ctrl+d")


async def test_project_window_deletes_assets(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "ProjectHere")
    count = await entity_count(ed, s["scene"])
    await ed.call("write_file", path="assets/s41_delete_me.txt", content="x")
    await ed.call("sync_files")
    entry = await project_entry(ed, keys, "s41_delete_me.txt")
    await keys.click(keys.centre(entry["rect"]))
    await keys.key("Delete")
    await keys.frames(6)
    # its confirmation, drawn by the window: the asset goes, the GameObjects stay
    await keys.click_item("Yes", "Delete Confirmation")
    await keys.frames(6)
    ed.check(not (ed.project / "assets" / "s41_delete_me.txt").exists(), "Delete removed the asset")
    ed.check_eq(await entity_count(ed, s["scene"]), count, "Project: no GameObject deleted")


async def test_input_actions_window_deletes_its_map(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "InputHere")
    count = await entity_count(ed, s["scene"])
    path = "assets/s41.inputactions"
    await ed.call("edit_input_actions", path=path, ops=[{"op": "add_map", "name": "Gameplay"}, {"op": "add_map", "name": "Menu"}])
    await ed.call("sync_files")
    entry = await project_entry(ed, keys, "s41.inputactions")
    # (Assets > Open, as a double-click does: a double-click's timing is the host's on a loaded machine)
    await keys.click(keys.centre(entry["rect"]))
    await ed.call("run_command", id="assets.open")
    await keys.click_item("Gameplay", "Input Actions")
    await keys.frames(4)
    await press_in_panel(ed, keys, s, count, "Input Actions", "Delete")
    text = (ed.project / path).read_text()
    ed.check('"Gameplay"' not in text and '"Menu"' in text, f"Delete removed the selected map: {text}")
    # the window selects the next map: Delete goes on with it. No map left, nothing is selected:
    # it does nothing, and does not fall through to the scene
    await press_in_panel(ed, keys, s, count, "Input Actions, the next map", "Delete")
    text = (ed.project / path).read_text()
    ed.check('"Menu"' not in text, f"Delete removed the next map: {text}")
    await press_in_panel(ed, keys, s, count, "Input Actions, nothing selected", "Delete")
    ed.check_eq((ed.project / path).read_text(), text, "and changes nothing")
    await ed.call("focus_window", window="scene", scene=s["scene"])


# (the floating panels go last: they stay over the docked ones the tests before them click)

async def test_jobs_memory_leaves_the_scene_alone(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "JobsHere")
    count = await entity_count(ed, s["scene"])
    await ed.call("focus_window", window="jobs_memory")
    await keys.frames(4)
    await press_in_panel(ed, keys, s, count, "Jobs & Memory", "Delete", "BackSpace", "ctrl+d")


async def test_gradient_editor_leaves_the_scene_alone(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    made = await ed.call("create_scene", name="GradientHere", type="3D", path="scenes/GradientHere")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    line = (await ed.call("create_entity", scene=scene, type="line_renderer", name="Line"))["entity_id"]
    await ed.call("select_entities", scene=scene, entities=[line])
    count = await entity_count(ed, scene)
    await ed.call("focus_window", window="inspector")
    await keys.click_revealed("##gradientField", "Inspector")
    for _ in range(25):
        focused = (await ed.call("editor_status"))["windows"]["focused"] or ""
        if "Gradient" in focused:
            break
        await keys.frames(2)
    ed.check("Gradient" in focused, f"the Gradient Editor has the focus: {focused}")
    await press_in_panel(ed, keys, {"scene": scene}, count, "Gradient Editor", "Delete", "ctrl+d")
    await keys.key("Escape")


async def test_sprite_normals_window(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    s = await fresh_scene(ed, "SpriteHere")
    count = await entity_count(ed, s["scene"])
    await ed.call("set_scene_camera", scene=s["scene"], position=[50, 50, 50], target=[40, 40, 40])
    await keys.frames(4)
    camera = (await ed.call("set_scene_camera", scene=s["scene"]))["camera"]
    await ed.call("focus_window", window="sprite_normals")
    await keys.frames(4)
    await press_in_panel(ed, keys, s, count, "Sprite Normal Map", "Delete", "f")
    await keys.frames(30)
    ed.check_eq((await ed.call("set_scene_camera", scene=s["scene"]))["camera"], camera, "F there: the Scene view stays")
    await ed.call("focus_window", window="scene", scene=s["scene"])
