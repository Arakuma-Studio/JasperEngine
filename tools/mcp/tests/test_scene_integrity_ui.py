# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Scene integrity with real input (docs/superpowers/specs/2026-11-04-scene-integrity-design.md):
a child scene's GameObject dragged in the Hierarchy is a reference only, and Escape gives a
game-captured cursor back to the editor (that the game gets the key on the same press is runtime-core's test).

Clicks and keys are xdotool in the harness's Xvfb, aimed where find_ui_item says a control is.
"""

from harness import Editor, TestFailure
from ui_input import Ui

EDITOR = {"xvfb": "harness", "create": True, "name": "Scene Integrity UI"}
CI = True
TIMEOUT = 1200


async def item(ed: Editor, label: str, window: str | None = None) -> dict:
    args = {"label": label}
    if window:
        args["window"] = window
    items = (await ed.call("find_ui_item", **args)).get("items", [])
    if not items:
        raise TestFailure(f"find_ui_item: no visible {label!r}" + (f" in {window!r}" if window else ""))
    return items[0]


async def test_child_scene_drag_is_reference_only(ed: Editor):
    ui = Ui(ed)
    ui.focus()
    parent = (await ed.call("create_scene", name="Parent", type="3D", path="Parent"))["scene"]["id"]
    first = (await ed.call("create_entity", scene=parent, type="cube", name="Alpha"))["entity_id"]
    await ed.call("create_entity", scene=parent, type="cube", name="Beta")
    child = (await ed.call("create_scene", name="Child", type="3D", path="Child"))["scene"]["id"]
    gamma = (await ed.call("create_entity", scene=child, type="cube", name="Gamma"))["entity_id"]
    ed.check_eq(gamma, first, "the child's GameObject has the id of one of the parent's")
    await ed.call("save_scene", scene=child)
    await ed.call("select_scene", scene=parent)
    await ed.call("set_child_scene", scene=parent, child=child, loaded_inline=True)
    await ed.call("save_scene", scene=parent)
    await ed.call("focus_window", window="hierarchy")
    before = await ed.call("get_scene_tree", scene=parent)

    # onto the parent's Beta: Alpha (the same id) used to move under it
    await ui.drag(ui.centre((await item(ed, "Gamma", "Hierarchy"))["rect"]), ui.centre((await item(ed, "Beta", "Hierarchy"))["rect"]))
    await ui.frames(4)
    ed.check_eq(await ed.call("get_scene_tree", scene=parent), before, "the parent scene's hierarchy is unchanged")

    # onto the Project window: no prefab is made from a reference
    project = (await ed.call("focus_window", window="project")).get("rect") or {}
    if project:
        target = ui.at(project, (project["width"] * 0.7, project["height"] * 0.7))
        await ed.call("focus_window", window="hierarchy")
        await ui.drag(ui.centre((await item(ed, "Gamma", "Hierarchy"))["rect"]), target)
        await ui.frames(4)
        prefabs = list(ed.project.rglob("*.prefab"))
        ed.check_eq(prefabs, [], f"no prefab from a child scene's GameObject: {prefabs}")


CAPTURE = """local capture = {}
function capture:init()
    Engine.mouseMode = MouseMode.CAPTURED
end
return capture
"""


async def test_escape_releases_a_captured_cursor(ed: Editor):
    ui = Ui(ed)
    ui.focus()
    scene = (await ed.call("create_scene", name="Look", type="3D", path="Look"))["scene"]["id"]
    await ed.call("create_entity", scene=scene, type="camera", name="Eye", position=[0, 1, 5])
    await ed.call("set_main_camera", scene=scene, entity="Eye")
    holder = (await ed.call("create_entity", scene=scene, type="empty", name="Capture"))["entity_id"]
    await ed.call("create_script", kind="lua", name="Capture", content=CAPTURE, entity=holder, scene=scene)
    await ed.call("save_scene", scene=scene)

    await ed.call("play", action="start", scene=scene)
    try:
        game = (await ed.call("focus_window", window="game")).get("rect") or {}
        if not game:
            raise TestFailure("no Game view rect")
        # focus_window's focus lands on a later frame: the click on the game image comes after it,
        # and gives the image (not only its window) the focus
        await ui.frames(6)
        lock = ""
        for _ in range(3):
            await ui.click(ui.centre(game))
            await ui.frames(4)
            lock = (await ed.call("get_input_state", fields=["raw"]))["raw"]["pointer_lock"]
            if lock == "locked":
                break
        ed.check_eq(lock, "locked", "the game image has the focus: the game's pointer is locked")
        cursor = (await ed.call("get_play_state"))["input"]["cursor"]
        ed.check_eq(cursor, {"mode": "captured", "released_to_editor": False}, "the game holds the cursor")

        await ui.key("Escape")
        await ui.frames(4)
        cursor = (await ed.call("get_play_state"))["input"]["cursor"]
        ed.check_eq(cursor["released_to_editor"], True, f"Escape released it: {cursor}")
        lock = (await ed.call("get_input_state", fields=["raw"]))["raw"]["pointer_lock"]
        ed.check_eq(lock, "pending", "the game's pointer lock is let go (still requested: a click gives it back)")

        await ui.click(ui.centre(game))
        await ui.frames(4)
        ed.check_eq((await ed.call("get_play_state"))["input"]["cursor"]["released_to_editor"], False, "a click took it back")
        await ed.call("run_command", id="playmode.releaseCursor")
        await ui.frames(2)
        ed.check_eq((await ed.call("get_play_state"))["input"]["cursor"]["released_to_editor"], True, "the command releases it")
    finally:
        await ed.call("play", action="stop")
