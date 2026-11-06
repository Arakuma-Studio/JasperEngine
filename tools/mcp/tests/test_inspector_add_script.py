"""The Inspector's Add Script and script drops, driven like a human with xdotool in the harness's
Xvfb (scripting catch-up, Doriax 9844a94a7): Add Script lists the project's scripts and attaches the
one picked (a Lua one too: it used to add an empty C++ entry), one undo step; a Lua entry's edit
popup shows its Module name read-only; a script dragged from the Project window onto a GameObject
in the Hierarchy is attached to it."""

from __future__ import annotations

from harness import Editor
import ui_input

EDITOR = {"create": True, "name": "Inspector Add Script", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 600
CI = True

ENEMY = """local Enemy = {
    properties = {
        { name = "speed", type = "float", default = 3.0 },
    }
}
return Enemy
"""


async def scripts_of(ed: Editor, entity: str) -> list:
    return (await ed.call("list_scripts", entity=entity))["scripts"]


def centre(rect) -> tuple[int, int]:
    """the middle of a UI probe rectangle [x0, y0, x1, y1] (editor window pixels; ui.at makes it a screen point)"""
    return int((rect[0] + rect[2]) / 2), int((rect[1] + rect[3]) / 2)


async def reveal(ed: Editor, ui, name: str) -> list:
    """A probed Inspector widget's rectangle, scrolling the Inspector from its top until it is drawn whole."""
    await ui.frames()
    rect = ed.ui_items().get(name)
    if rect:
        return rect
    panel = await ed.ui_rect("inspector")
    ed.check(panel, "the inspector is drawn")
    at = ui.at(list(centre(panel)))
    await ui.wheel(at, -60)
    for _ in range(80):
        await ui.frames()
        rect = ed.ui_items().get(name)
        if rect:
            return rect
        await ui.wheel(at, 2)
    ed.fail(f"{name} is never drawn in the inspector: {sorted(ed.ui_items())}")


async def test_add_script_picks_a_lua_script(ed: Editor):
    ui = ui_input.for_editor(ed)
    ui.focus()
    await ed.call("create_script", kind="lua", name="Enemy", content=ENEMY, directory="ai")
    # an empty GameObject: the Script component is drawn near the Inspector's top
    await ed.call("create_entity", name="Grunt", type="empty")
    await ed.call("add_component", entity="Grunt", component="script")
    await ed.call("select_entities", entities=["Grunt"])
    await ed.call("focus_window", window="inspector")
    await ui.frames(6)

    await ui.click(ui.at(list(centre(await reveal(ed, ui, "inspector.addScript")))))
    await ui.frames(4)
    await ui.type("ene")
    await ui.frames(4)
    row = await ed.ui_rect("inspector.addScript.row.Enemy")
    ed.check(row, f"the picker lists Enemy: {sorted(ed.ui_items())}")
    await ui.click(ui.at(list(centre(row))))
    await ui.frames(6)

    scripts = await scripts_of(ed, "Grunt")
    ed.check_eq(len(scripts), 1, f"one script attached: {scripts}")
    ed.check_eq(scripts[0]["type"], "lua", scripts[0])
    ed.check(scripts[0]["path"].endswith("ai/Enemy.lua"), scripts[0])
    ed.check_eq([p["name"] for p in scripts[0]["properties"]], ["speed"], "its properties are read")

    # the edit popup of a Lua entry: the module name is the file name, not edited
    await ui.click_revealed("##edit_name_0", window="Inspector")
    await ui.frames(4)
    ed.check(await ui.find("Module", frames=10), "the popup shows Module")
    ed.check(not await ui.find("Class Name", frames=4), "no Class Name for a Lua script")
    await ui.key("Escape")
    await ui.frames(4)

    await ed.call("undo", scope="scene")
    ed.check_eq(await scripts_of(ed, "Grunt"), [], "one undo step takes it off")


async def test_drop_on_hierarchy(ed: Editor):
    ui = ui_input.for_editor(ed)
    ui.focus()
    await ed.call("create_entity", name="Target", type="box")
    await ed.call("focus_window", window="project")
    await ui.frames(6)

    def node(state, path):
        return next((n for n in state["tree"] if n["path"] == path), None)

    def entry(state):
        return next((e["rect"] for e in state["entries"] if e["name"] == "Enemy.lua"), None)

    state = (await ed.call("editor_status"))["ui"]["project_window"]
    for _ in range(10):
        if state["directory"] == "ai" and entry(state):
            break
        target = node(state, "ai")
        ed.check(target, f"ai in the tree: {[n['path'] for n in state['tree']]}")
        await ui.click(ui.centre(target["rect"]))
        await ui.frames()
        state = (await ed.call("editor_status"))["ui"]["project_window"]
    source = entry(state)
    ed.check(source, f"Enemy.lua listed: {[e['name'] for e in state['entries']]}")

    row = await ui.find("Target", window="Hierarchy")
    ed.check(row, "the Hierarchy row is drawn")
    await ui.drag(ui.centre(source), ui.centre(row["rect"]), steps=16)
    await ui.frames(8)

    scripts = await scripts_of(ed, "Target")
    ed.check(len(scripts) == 1 and scripts[0]["path"].endswith("ai/Enemy.lua"), f"dropped script attached: {scripts}")
