"""The Project window's file operations driven with real mouse and keyboard input
(docs/superpowers/specs/2026-11-04-project-window-design.md, section 10 M1): xdotool clicks the
rects editor_status reports (ui.project_window) and find_ui_item finds, the assertions read the
files and the editor's state back.

- Paste from a file's menu lands beside it, from a folder's menu inside it (Doriax 65c4c88d3)
- a cut pasted into itself is refused and stays on the clipboard
- Ctrl+D duplicates once per press and selects the copies; one undo removes them
- a press arriving with the pointer move drags only that file (Doriax 9050bbaca)
- Create > Lua Script from a folder's menu asks the name, writes the script there, undoes
- Create > HTML Document is one undo step
- the assets directory can't be cut
- F11 toggles Maximize On Play; an unnamed GameObject reads "GameObject <id>"

Screenshots of the steps go to build/test-results/project-window-ops/.
"""

from __future__ import annotations

import asyncio
from pathlib import Path

from harness import Editor
import ui_input

REPO = Path(__file__).resolve().parents[3]
EDITOR = {"project": REPO / "tools" / "fixtures" / "textures", "xvfb": "harness"}
TIMEOUT = 900
CI = True

SHOTS = REPO / "build" / "test-results" / "project-window-ops"


async def shot(ed: Editor, name: str) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    await ed.screenshot("editor", SHOTS / f"{name}.png", focus=False, max_width=0)


async def state(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]["project_window"]


async def wait_for(ed: Editor, predicate, what: str, timeout: float = 20.0) -> dict:
    last = None
    for _ in range(int(timeout / 0.25)):
        last = await state(ed)
        if predicate(last):
            return last
        await ui_input.frames(ed, 2)
    ed.fail(f"{what}; project window: {last}")


def entry(st: dict, name: str) -> list | None:
    for item in st["entries"]:
        if item["name"] == name:
            return item["rect"]
    return None


def menu_item(st: dict, label: str) -> list | None:
    for item in st["context_menu"]:
        if item["label"] == label:
            return item["rect"]
    return None


def tree_node(st: dict, path: str) -> dict | None:
    for node in st["tree"]:
        if node["path"] == path:
            return node
    return None


async def show_folder(ed: Editor, folder: str) -> dict:
    """The folder in the listing, picked in the folder tree (its parents opened with their arrows)"""
    ui = ui_input.for_editor(ed)
    await ed.call("focus_window", window="project")
    st = await wait_for(ed, lambda s: s["drawn"] and tree_node(s, "assets"), "the tree shows assets")
    if st["directory"] == folder:
        return st
    parts = folder.split("/")
    for depth in range(1, len(parts)):
        parent = "/".join(parts[:depth])
        child = "/".join(parts[:depth + 1])
        # the first click of a fresh editor can go to focusing its window: the arrow again until it opens
        for _ in range(3):
            if tree_node(st, child):
                break
            x, y = tree_node(st, parent)["arrow"]
            await ui.click(ui.at((x, y)))
            for _ in range(10):
                st = await state(ed)
                if tree_node(st, child):
                    break
                await ui_input.frames(ed, 2)
        if not tree_node(st, child):
            ed.fail(f"{parent} never opened in the tree: {st}")
    await ui.click(ui.centre(tree_node(st, folder)["rect"]))
    return await wait_for(ed, lambda s: s["directory"] == folder, f"{folder} shown")


async def context_menu(ed: Editor, name: str, label: str, submenu: str | None = None) -> None:
    """Right-click the entry, then the item (through a submenu when given)"""
    ui = ui_input.for_editor(ed)
    st = await state(ed)
    await ui.click(ui.centre(entry(st, name)), button="3")
    if submenu:
        st = await wait_for(ed, lambda s: menu_item(s, submenu), f"the menu of {name} shows {submenu}")
        await ui.move(ui.centre(menu_item(st, submenu)))
    st = await wait_for(ed, lambda s: menu_item(s, label), f"the menu of {name} shows {label}")
    await ui.click(ui.centre(menu_item(st, label)))


async def answer_dialog(ed: Editor) -> dict | None:
    status = await ed.call("editor_status")
    dialog = status.get("dialog")
    if dialog:
        await ed.call("respond_dialog", choice="ok")
    return dialog


async def test_paste_on_a_file_lands_in_its_folder(ed: Editor):
    st = await show_folder(ed, "assets/textures")
    ed.check(entry(st, "photo.png") and entry(st, "sprite.png"), f"listing: {[e['name'] for e in st['entries']]}")
    await context_menu(ed, "photo.png", "Copy")
    await context_menu(ed, "sprite.png", "Paste")
    await wait_for(ed, lambda s: entry(s, "photo 1.png"), "photo 1.png pasted beside sprite.png")
    ed.check((ed.project / "assets/textures/photo 1.png").read_bytes() == (ed.project / "assets/textures/photo.png").read_bytes(),
             "the copy holds the file")
    ed.check(not (ed.project / "assets/textures/sprite.png").is_dir(), "nothing under the file")
    st = await state(ed)
    ed.check_eq(st["selected"], "photo 1.png", "the copy is selected")
    await shot(ed, "paste-on-file")


async def test_paste_on_a_folder_goes_into_it(ed: Editor):
    await show_folder(ed, "assets")
    await context_menu(ed, "models", "Copy")
    await context_menu(ed, "textures", "Paste")
    for _ in range(40):
        if (ed.project / "assets/textures/models/panel.gltf").exists():
            break
        await ui_input.frames(ed, 2)
    ed.check((ed.project / "assets/textures/models/panel.gltf").exists(), "models pasted into textures")
    ed.check((ed.project / "assets/models/panel.gltf").exists(), "a copy leaves the source")


async def test_cut_paste_on_a_sibling_folder_moves_and_redraws(ed: Editor):
    st = await show_folder(ed, "assets/textures")
    ui = ui_input.for_editor(ed)
    # a folder listed before the images, and two images cut (more entries follow them: the old
    # loop read the destroyed ones, M2 under ASan)
    (ed.project / "assets/textures/aa_target").mkdir(exist_ok=True)
    await ed.call("sync_files", paths=["assets/textures/aa_target"])
    st = await wait_for(ed, lambda s: entry(s, "aa_target"), "aa_target listed")
    await ui.click(ui.centre(entry(st, "foliage.png")))
    await ui.click(ui.centre(entry(st, "gradient.png")), modifier="ctrl")
    await context_menu(ed, "gradient.png", "Cut")
    st = await state(ed)
    ed.check(st["clipboard"]["cut"] and len(st["clipboard"]["files"]) == 2, f"clipboard: {st['clipboard']}")
    await context_menu(ed, "aa_target", "Paste")
    st = await wait_for(ed, lambda s: not entry(s, "foliage.png") and not entry(s, "gradient.png"), "the cut files left")
    ed.check((ed.project / "assets/textures/aa_target/foliage.png").exists(), "foliage.png moved")
    ed.check((ed.project / "assets/textures/aa_target/gradient.png").exists(), "gradient.png moved")
    ed.check_eq(st["clipboard"]["files"], [], "a pasted cut leaves the clipboard")
    await ed.check_alive()


async def test_refused_cut_paste_keeps_the_clipboard(ed: Editor):
    st = await show_folder(ed, "assets")
    await context_menu(ed, "models", "Cut")
    await context_menu(ed, "models", "Paste")
    dialog = None
    for _ in range(20):
        dialog = await answer_dialog(ed)
        if dialog:
            break
        await ui_input.frames(ed, 2)
    ed.check(dialog is not None and "into itself" in dialog["message"], f"the refusal says why: {dialog}")
    st = await state(ed)
    ed.check(st["clipboard"]["cut"] and st["clipboard"]["files"] == ["assets/models"], f"the cut stays: {st['clipboard']}")
    ed.check(not (ed.project / "assets/models/models").exists(), "nothing went into itself")


async def test_ctrl_d_duplicates_once_and_selects_the_copies(ed: Editor):
    st = await show_folder(ed, "assets/textures")
    ui = ui_input.for_editor(ed)
    await ui.click(ui.centre(entry(st, "normal.png")))
    # held: one copy (no key repeat)
    ui.xdo("keydown", "ctrl")
    await ui.frames(1)
    ui.xdo("keydown", "d")
    await ui.settle(0.8, 4)
    ui.xdo("keyup", "d")
    ui.xdo("keyup", "ctrl")
    st = await wait_for(ed, lambda s: entry(s, "normal 1.png"), "normal 1.png made")
    await ui.frames(4)
    ed.check(not (ed.project / "assets/textures/normal 2.png").exists(), "a held Ctrl+D made more than one copy")
    st = await state(ed)
    ed.check_eq(st["selected"], "normal 1.png", "the copy is selected")
    await shot(ed, "duplicate")
    await ed.call("undo", scope="project")
    ed.check(not (ed.project / "assets/textures/normal 1.png").exists(), "undo removes the copy")


async def test_press_with_the_pointer_move_drags_only_that_file(ed: Editor):
    st = await show_folder(ed, "assets/textures")
    ui = ui_input.for_editor(ed)
    (ed.project / "assets/textures/zz_drop").mkdir(exist_ok=True)
    await ed.call("sync_files", paths=["assets/textures/zz_drop"])
    st = await wait_for(ed, lambda s: entry(s, "zz_drop") and entry(s, "photo.png"), "zz_drop listed")
    names = [e["name"] for e in st["entries"]]
    target = names[names.index("photo.png") + 1] if names.index("photo.png") + 1 < len(names) else "photo.png"
    # the pointer rests on empty space below the listing, then moves onto the file and presses in
    # one go: the press arrives in the frame the pointer reaches the file
    last = st["entries"][-1]["rect"]
    await ui.move(ui.at((last[0] + 4, last[1] + last[3] + 40)))
    x, y = ui.centre(entry(st, target))
    ui.xdo("mousemove", str(x), str(y), "mousedown", "1")
    ui.buttons_down.add("1")
    await ui.frames(2)
    drop = ui.centre(entry(st, "zz_drop"))
    await ui.drag_to((x, y), drop[0] - x, drop[1] - y, steps=16)
    await ui.release("1")
    await wait_for(ed, lambda s: not entry(s, target), f"{target} moved into zz_drop")
    moved = sorted(p.name for p in (ed.project / "assets/textures/zz_drop").iterdir() if p.suffix != ".meta")
    ed.check_eq(moved, [target], "only the pressed file was dragged")


async def test_create_lua_script_names_first_and_undoes(ed: Editor):
    ui = ui_input.for_editor(ed)
    # the Lua directory is scripts: a Lua script goes there (elsewhere the item is disabled)
    await show_folder(ed, "scripts")
    (ed.project / "scripts/enemies").mkdir(parents=True, exist_ok=True)
    await ed.call("sync_files", paths=["scripts/enemies"])
    st = await wait_for(ed, lambda s: entry(s, "enemies"), "scripts/enemies listed")
    await context_menu(ed, "enemies", "Lua Script", submenu="Create")
    await ui.frames(4)
    await ui.key("ctrl+a")
    await ui.type("Goblin")
    await ui.key("Return")
    script = ed.project / "scripts/enemies/Goblin.lua"
    for _ in range(40):
        if script.exists():
            break
        await ui.frames(2)
    ed.check(script.exists(), "the script is written into the right-clicked folder")
    text = script.read_text()
    ed.check_in("---@class Goblin : ScriptInstance", text)
    ed.check_in("return Goblin", text)
    await shot(ed, "create-lua-script")
    await ed.call("undo", scope="project")
    ed.check(not script.exists(), "undo removes the created script")


async def test_create_html_document_is_one_undo_step(ed: Editor):
    st = await show_folder(ed, "assets")
    ui = ui_input.for_editor(ed)
    await ui.click(ui.centre(entry(st, "textures")), button="3")
    st = await wait_for(ed, lambda s: menu_item(s, "Create"), "the folder menu shows Create")
    await ui.move(ui.centre(menu_item(st, "Create")))
    st = await wait_for(ed, lambda s: menu_item(s, "HTML Document"), "Create shows HTML Document")
    await ui.click(ui.centre(menu_item(st, "HTML Document")))
    await ui.frames(4)
    await ui.key("ctrl+a")
    await ui.type("Menu")
    await ui.key("Return")
    html = ed.project / "assets/textures/Menu.html"
    css = ed.project / "assets/textures/Menu.css"
    for _ in range(40):
        if html.exists() and css.exists():
            break
        await ui.frames(2)
    ed.check(html.exists() and css.exists(), "the document and its stylesheet")
    await ed.call("undo", scope="project")
    ed.check(not html.exists() and not css.exists(), "one undo removes both")


async def test_root_folder_cannot_be_cut(ed: Editor):
    st = await show_folder(ed, "assets")
    ui = ui_input.for_editor(ed)
    # the project root's listing: "assets" is the assets directory
    root = await wait_for(ed, lambda s: tree_node(s, "."), "the project root in the tree")
    await ui.click(ui.centre(tree_node(root, ".")["rect"]))
    await wait_for(ed, lambda s: s["directory"] in ("", ".") and entry(s, "assets"), "the project root listed")
    await context_menu(ed, "assets", "Cut")
    dialog = None
    for _ in range(20):
        dialog = await answer_dialog(ed)
        if dialog:
            break
        await ui.frames(2)
    ed.check(dialog is not None and "assets directory" in dialog["message"], f"the refusal names the setting: {dialog}")
    st = await state(ed)
    ed.check("assets" not in st["clipboard"]["files"], f"the assets directory was not cut: {st['clipboard']}")


async def test_unnamed_object_reads_gameobject_and_id(ed: Editor):
    await ed.call("play", action="start")
    try:
        made = await ed.call("eval_lua", code="return GameObject(scene)")
        handle = made["values"][0]
        entity = handle["entity"] if isinstance(handle, dict) else handle
        await ed.call("focus_window", window="hierarchy")
        ui = ui_input.for_editor(ed)
        item = await ui.find(f"GameObject {entity}", "Hierarchy", frames=120)
        ed.check(item is not None, f"the Hierarchy shows GameObject {entity}")
        await shot(ed, "unnamed-object")
    finally:
        await ed.call("play", action="stop", keep_changes="none")


async def test_f11_toggles_maximize_on_play(ed: Editor):
    ui = ui_input.for_editor(ed)
    before = (await ed.call("configure_game_view"))["maximize_on_play"]
    await ed.call("play", action="start")
    try:
        ui.focus()
        await ui.key("F11")
        view = await ed.call("configure_game_view")
        ed.check_eq(view["maximize_on_play"], not before, "F11 toggled the setting")
        await ui.frames(4)
        view = await ed.call("configure_game_view")
        ed.check_eq(view["maximized"], not before, "the Game view follows at once during Play")
        await shot(ed, "maximized")
        await ui.key("F11")
        view = await ed.call("configure_game_view")
        ed.check_eq(view["maximize_on_play"], before, "F11 again: back")
    finally:
        await ed.call("play", action="stop", keep_changes="none")
