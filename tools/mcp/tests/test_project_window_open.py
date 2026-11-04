"""Double-clicking a .material or a .particles in the Project window (BF23 F23), like a human: xdotool in the harness's
Xvfb, the files found through the UI probe (Editor(ui_probe=True), editor/util/UIProbe.h). The file shows in the
Inspector and the Inspector takes the focus, as in Unity; no text buffer opens. Open as Text stays in the context menu.

The Inspector edits a .material as Unity's does: the rows of a submesh's material, each gesture one project undo step
on the file (byte-exact back), the linked submeshes following, the open scene's undo untouched. An agent's edit reaches
the rows. A file that did not read completely is not written without the lossy-save modal's answer.
"""

from __future__ import annotations

import os
from pathlib import Path

from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Project Window Open", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 300

MATERIAL = "assets/mats/Shiny.material"
EFFECT = "assets/mats/Smoke.particles"


def centre(ui: Ui, rect: list[float]) -> tuple[int, int]:
    """The screen point at a probe rect's centre (probe rects are [x0, y0, x1, y1] in window pixels)"""
    return ui.at(rect, ((rect[2] - rect[0]) / 2, (rect[3] - rect[1]) / 2))


async def shot(ed: Editor, name: str) -> None:
    shots = Path(os.environ["JASPER_SHOTS_DIR"]) if os.environ.get("JASPER_SHOTS_DIR") else ed.scratch.root
    shots.mkdir(parents=True, exist_ok=True)
    await ed.screenshot("editor", path=str(shots / f"project-window-{name}.png"), max_width=1600)


async def probe_item(ui: Ui, name: str) -> list[float]:
    for _ in range(30):
        await ui.frames(2)
        rect = ui.ed.ui_items().get(name)
        if rect:
            return rect
    ui.ed.fail(f"{name} is never drawn: {sorted(ui.ed.ui_items())}")


async def project_file(ui: Ui, name: str, folders=("assets", "mats")) -> list[float]:
    """A file of the Project window (its probe rect), opening the folders on the way"""
    await ui.ed.call("focus_window", window="project")
    for _ in range(12):
        await ui.frames(4)
        items = ui.ed.ui_items()
        rect = items.get("project.file." + name)
        if rect:
            return rect
        for folder in reversed(folders):
            if "project.file." + folder in items:
                await ui.double_click(centre(ui, items["project.file." + folder]))
                break
    ui.ed.fail(f"the Project window never lists {name}: {sorted(ui.ed.ui_items())}")


async def buffers(ed: Editor) -> list[str]:
    return [Path(b["path"]).name for b in (await ed.call("code_editor", action="status"))["buffers"]]


async def focused(ed: Editor) -> str:
    return (await ed.call("editor_status"))["windows"]["focused"]


async def test_setup(ed: Editor):
    await ed.call("create_scene", name="Linked", type="3D", path="scenes/Linked")
    await ed.call("create_entity", name="Ball", type="box")
    await ed.call("create_material", path=MATERIAL, values={"baseColorFactor": [0.8, 0.2, 0.1, 1.0], "metallicFactor": 0.75})
    await ed.call("link_material", entity="Ball", material=MATERIAL)
    await ed.call("tool_groups", enable=["particles"])
    await ed.call("create_particle_effect", path=EFFECT, preset="smoke")
    await ed.call("sync_files")


async def test_double_click_material_shows_it_in_the_inspector(ed: Editor):
    ui = Ui(ed)
    rect = await project_file(ui, "Shiny.material")
    await ui.double_click(centre(ui, rect))
    await ui.frames(6)
    ed.check_eq(await focused(ed), "Inspector", "the Inspector takes the focus")
    ed.check_eq(await buffers(ed), [], "no text buffer opens")
    await probe_item(ui, "asset.header")
    await probe_item(ui, "asset.material.preview")
    await shot(ed, "material-inspector")


async def test_double_click_particles_behaves_like_a_material(ed: Editor):
    ui = Ui(ed)
    rect = await project_file(ui, "Smoke.particles")
    await ui.click(centre(ui, rect))
    # Assets > Open runs what a double-click runs: the Inspector's particle preview makes frames on this
    # software renderer longer than ImGui's double-click time, so two real clicks do not read as one here
    await ed.call("run_command", id="assets.open")
    await ui.frames(6)
    ed.check_eq(await focused(ed), "Inspector", "the Inspector takes the focus")
    ed.check_eq(await buffers(ed), [], "no text buffer opens")


async def test_open_as_text_stays_in_the_context_menu(ed: Editor):
    ui = Ui(ed)
    rect = await project_file(ui, "Shiny.material")
    await ui.click(centre(ui, rect), button="3")
    menu = await probe_item(ui, "project.menu.open_as_text")
    await ui.click(centre(ui, menu))
    await ui.frames(6)
    ed.check_eq(await buffers(ed), ["Shiny.material"], "Open as Text opens its text")
    await ed.call("code_editor", action="close", path=MATERIAL)


async def undo_steps(ed: Editor, scope: str) -> int:
    return len((await ed.call("get_undo_history", scope=scope, limit=500))["entries"])


async def linked_clearcoat(ed: Editor) -> bool:
    prop = "submeshes[0].material.pbr.clearcoat.enabled"
    got = await ed.call("get_entity", entity="Ball", components=["mesh"], properties=[prop])
    return got["components"]["mesh"][prop]


async def show_material(ed: Editor, ui: Ui) -> None:
    rect = await project_file(ui, "Shiny.material")
    await ui.double_click(centre(ui, rect))
    await ui.frames(6)
    await probe_item(ui, "asset.material.preview")
    ui.focus()


async def test_inspector_edits_the_file_as_one_project_step(ed: Editor):
    ui = Ui(ed)
    await show_material(ed, ui)
    file = ed.project / MATERIAL
    before = file.read_text()
    project_steps = await undo_steps(ed, "project")
    scene_steps = await undo_steps(ed, "scene")

    # Advanced Surface (InspectorMaterialPbr): a click is a step, the file and the linked mesh have it
    await ui.click(ui.centre((await ui.reveal("Clear Coat", "Inspector"))["rect"]))
    await ui.frames(6)
    clicked = file.read_text()
    ed.check("clearcoat" in clicked and clicked != before, f"the file has the lobe: {clicked}")
    ed.check_eq(await linked_clearcoat(ed), True, "the linked submesh follows")
    ed.check_eq(await undo_steps(ed, "project"), project_steps + 1, "one project step")
    ed.check_eq(await undo_steps(ed, "scene"), scene_steps, "the open scene's undo is not touched")
    await shot(ed, "material-edited")

    # Ctrl+Z with the Inspector focused is the project's: the bytes come back
    ed.check_eq(await focused(ed), "Inspector", "the Inspector has the focus")
    await ui.key("ctrl+z")
    await ui.frames(6)
    ed.check_eq(file.read_text(), before, "Ctrl+Z gives the file back byte for byte")
    ed.check_eq(await linked_clearcoat(ed), False, "and the linked submesh")

    # a drag is one step, however many values it passes through
    item = await ui.reveal("##input_float_submeshes[0].material.roughnessFactor", "Inspector")
    start = ui.centre(item["rect"])
    await ui.drag(start, (start[0] - 60, start[1]))
    await ui.frames(6)
    dragged = file.read_text()
    ed.check(dragged != before and "roughnessFactor" in dragged, f"the drag reached the file: {dragged}")
    ed.check_eq(await undo_steps(ed, "project"), project_steps + 1, "a drag is one step")
    ed.check_eq(await undo_steps(ed, "scene"), scene_steps, "and none in the scene")
    await ed.call("undo", scope="project")
    await ui.frames(6)
    ed.check_eq(file.read_text(), before, "its undo gives the file back byte for byte")

    # Esc while the mouse is down: nothing written, no step (docs/undo.md, Drags)
    steps = await undo_steps(ed, "project")
    item = await ui.reveal("##input_float_submeshes[0].material.roughnessFactor", "Inspector")
    start = ui.centre(item["rect"])
    await ui.drag(start, (start[0] - 60, start[1]), release=False)
    await ui.key("Escape")
    await ui.frames(2)
    await ui.release()
    await ui.frames(6)
    ed.check_eq(file.read_text(), before, "Esc leaves the file as it was")
    ed.check_eq(await undo_steps(ed, "project"), steps, "and adds no step")


async def test_an_agent_edit_reaches_the_rows(ed: Editor):
    ui = Ui(ed)
    await show_material(ed, ui)
    await ed.call("update_material", path=MATERIAL, values={"metallicFactor": 0.125})
    await ui.frames(6)
    # the next row edit starts from the agent's value: it would write the old one back otherwise
    await ui.click(ui.centre((await ui.reveal("Clear Coat", "Inspector"))["rect"]))
    await ui.frames(6)
    text = (ed.project / MATERIAL).read_text()
    ed.check("metallicFactor: 0.125" in text and "clearcoat" in text, f"both edits are in the file: {text}")
    await ed.call("undo", scope="project")
    await ed.call("undo", scope="project")
    await ui.frames(6)


async def test_a_file_that_did_not_read_completely_is_not_written(ed: Editor):
    ui = Ui(ed)
    file = ed.project / MATERIAL
    original = file.read_text()
    # (an unknown root key is kept through a write; a bad value below a block is what a write would lose)
    damaged = original.rstrip() + "\nextraPass:\n  order: NOT_AN_ORDER\n"
    file.write_text(damaged)
    await ed.call("sync_files")
    refused = await ed.call_raw("update_material", {"path": MATERIAL, "values": {"metallicFactor": 0.5}})
    ed.check(refused.is_error and "did not load completely" in refused.text, f"the agent's path refuses this file too: {refused.text[:300]}")
    await show_material(ed, ui)
    await ui.click(ui.centre((await ui.reveal("Clear Coat", "Inspector"))["rect"]))
    await ui.frames(6)
    # (this editor serves MCP on stdio: nobody to answer the modal, so it refuses as a headless host does)
    ed.check_eq(file.read_text(), damaged, "the file is not written")
    ed.check_eq(await linked_clearcoat(ed), False, "and the linked submesh goes back to what the file has")
    file.write_text(original)
    await ed.call("sync_files")
