# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Scene integrity with real input (docs/superpowers/specs/2026-11-04-scene-integrity-design.md):
the Save Scene dialog refuses another scene's file and names that leave the folder, Save All asks
for untitled scenes, a project switch closes the old project's save dialogs, a child scene's
GameObject dragged in the Hierarchy is a reference only, and Escape gives a game-captured cursor
back to the editor while the game still gets the key.

Clicks and keys are xdotool in the harness's Xvfb, aimed where find_ui_item says a control is.
"""

from harness import Editor, TestFailure
from ui_input import Ui

EDITOR = {"xvfb": "harness", "create": True, "name": "Scene Integrity UI"}
CI = True
TIMEOUT = 1200


async def dialog(ed: Editor) -> dict:
    return (await ed.call("editor_status")).get("dialog") or {}


async def wait_for_dialog(ed: Editor, kind: str, frames: int = 40) -> dict:
    for _ in range(frames):
        open_dialog = await dialog(ed)
        if open_dialog.get("kind") == kind:
            return open_dialog
        await ed.call("wait", frames=2)
    raise TestFailure(f"no {kind} dialog opened: {await ed.call('editor_status')}")


async def item(ed: Editor, label: str, window: str | None = None) -> dict:
    args = {"label": label}
    if window:
        args["window"] = window
    items = (await ed.call("find_ui_item", **args)).get("items", [])
    if not items:
        raise TestFailure(f"find_ui_item: no visible {label!r}" + (f" in {window!r}" if window else ""))
    return items[0]


async def test_save_dialog_refuses_another_scenes_file(ed: Editor):
    ui = Ui(ed)
    ui.focus()
    await ed.call("create_scene", name="Taken", type="3D", path="Taken")
    taken = (ed.project / "Taken.scene").read_bytes()
    draft = (await ed.call("create_scene", name="Draft", type="3D"))["scene"]["id"]
    await ed.call("select_scene", scene=draft)

    await ed.call("run_command", command="file.save")
    await wait_for_dialog(ed, "save_dialog")
    name = await item(ed, "##fileName", "Save Scene")
    await ui.click(ui.centre(name))
    await ui.key("ctrl+a")
    await ui.type("Taken")
    await ui.frames(4)
    save = await item(ed, "Save", "Save Scene")
    ed.check_eq(save.get("enabled"), False, f"Save is disabled over another scene's file: {save}")
    await ui.click(ui.centre(save))
    ed.check_eq((ed.project / "Taken.scene").read_bytes(), taken, "Taken.scene is untouched")

    await ui.click(ui.centre(name))
    await ui.key("ctrl+a")
    await ui.type("../Outside")
    await ui.frames(4)
    ed.check_eq((await item(ed, "Save", "Save Scene")).get("enabled"), False, "a name leaving the folder is refused")

    await ed.call("respond_dialog", choice="cancel")
    await ed.call("wait", frames=4)
    ed.check_eq(await dialog(ed), {}, "cancelled")


async def test_save_all_asks_for_untitled_scenes(ed: Editor):
    await ed.call("create_scene", name="Untouched", type="3D")
    await ed.call("run_command", command="file.saveAll")
    open_dialog = await wait_for_dialog(ed, "save_dialog")
    ed.check_eq(open_dialog.get("title"), "Save Scene", f"Save All asks where to save the untitled scene: {open_dialog}")
    await ed.call("respond_dialog", choice="cancel")
    await ed.call("wait", frames=4)


async def test_project_switch_closes_save_dialogs(ed: Editor):
    await ed.call("create_scene", name="Pending", type="3D")
    await ed.call("run_command", command="file.save")
    await wait_for_dialog(ed, "save_dialog")
    other = ed.project.parent / (ed.project.name + "-other")
    await ed.call("open_project", path=str(other), create=True, name="Other", unsaved="discard")
    await ed.call("wait", frames=6)
    ed.check_eq(await dialog(ed), {}, "the old project's Save Scene dialog closed with it")


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
    await ui.drag(ui.centre(await item(ed, "Gamma", "Hierarchy")), ui.centre(await item(ed, "Beta", "Hierarchy")))
    await ui.frames(4)
    ed.check_eq(await ed.call("get_scene_tree", scene=parent), before, "the parent scene's hierarchy is unchanged")

    # onto the Project window: no prefab is made from a reference
    project = (await ed.call("focus_window", window="project")).get("rect") or {}
    if project:
        target = ui.at(project, (project["width"] * 0.7, project["height"] * 0.7))
        await ed.call("focus_window", window="hierarchy")
        await ui.drag(ui.centre(await item(ed, "Gamma", "Hierarchy")), target)
        await ui.frames(4)
        prefabs = list(ed.project.rglob("*.prefab"))
        ed.check_eq(prefabs, [], f"no prefab from a child scene's GameObject: {prefabs}")


CAPTURE = """local capture = {}
function capture:init()
    RegisterEngineEvent(self, "onKeyDown")
    self.escapes = 0
    Engine.mouseMode = MouseMode.CAPTURED
end
function capture:onKeyDown(key, rep, mods)
    if key == Input.KEY_ESCAPE then self.escapes = self.escapes + 1 end
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
        await ui.click(ui.centre(game))
        await ui.frames(4)
        cursor = (await ed.call("get_play_state"))["input"]["cursor"]
        ed.check_eq(cursor, {"mode": "captured", "released_to_editor": False}, "the game holds the cursor")

        await ui.key("Escape")
        await ui.frames(4)
        cursor = (await ed.call("get_play_state"))["input"]["cursor"]
        ed.check_eq(cursor["released_to_editor"], True, f"Escape released it: {cursor}")
        live = await ed.call("get_runtime_entity", entity="Capture")
        ed.check(live["scripts"][0]["state"]["escapes"] >= 1, f"the game got the Escape key too: {live['scripts'][0]['state']}")

        await ui.click(ui.centre(game))
        await ui.frames(4)
        ed.check_eq((await ed.call("get_play_state"))["input"]["cursor"]["released_to_editor"], False, "a click took it back")
        await ed.call("run_command", command="playmode.releaseCursor")
        await ui.frames(2)
        ed.check_eq((await ed.call("get_play_state"))["input"]["cursor"]["released_to_editor"], True, "the command releases it")
    finally:
        await ed.call("play", action="stop")
