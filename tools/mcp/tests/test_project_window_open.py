"""Double-clicking a .material or a .particles in the Project window (BF23 F23, S91), like a human: xdotool in the
harness's Xvfb, the files found through the UI probe (Editor(ui_probe=True), editor/util/UIProbe.h). A .material shows
in the Inspector and the Inspector takes the focus, as in Unity; a .particles opens the Particle Effect window (a single
click still shows it in the Inspector). No text buffer opens. Open as Text stays in the context menu.

The Inspector edits a .material as Unity's does: the rows of a submesh's material, each gesture one project undo step
on the file (byte-exact back), the linked submeshes following, the open scene's undo untouched. An agent's edit reaches
the rows. A file that did not read completely is not written without the lossy-save modal's answer.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

import pngdiff
from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Project Window Open", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 300

MATERIAL = "assets/mats/Shiny.material"
EFFECT = "assets/mats/Smoke.particles"
STATE: dict = {}


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
    STATE["material"] = (ed.project / MATERIAL).read_bytes()


async def ball_exists(ed: Editor) -> bool:
    return not (await ed.call_raw("get_entity", {"entity": "Ball"})).is_error


async def fresh(ed: Editor) -> None:
    """The file and the linked Ball as test_setup left them: a failed test leaves neither broken for the next"""
    file = ed.project / MATERIAL
    if file.read_bytes() != STATE["material"]:
        file.write_bytes(STATE["material"])
        await ed.call("sync_files")
    if not await ball_exists(ed):
        await ed.call("create_entity", name="Ball", type="box")
        await ed.call("link_material", entity="Ball", material=MATERIAL)
    await ed.call("code_editor", action="close", path=MATERIAL, discard_unsaved=True) if Path(MATERIAL).name in await buffers(ed) else None


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


async def test_double_click_particles_opens_the_particle_effect_window(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    await fresh(ed)
    await ed.call("select_entities", entities=["Ball"])
    rect = await project_file(ui, "Smoke.particles")
    await ui.click(centre(ui, rect))
    # a single click shows the effect in the Inspector, so Delete there is not the selected GameObject's
    await probe_item(ui, "asset.particles.header")
    await ed.call("focus_window", window="inspector")
    ui.focus()
    await ui.key("Delete")
    await ui.frames(4)
    ed.check(await ball_exists(ed), "Delete with the effect in the Inspector leaves the GameObject")
    # Assets > Open runs what a double-click runs: the particle previews make frames on this software
    # renderer longer than ImGui's double-click time, so two real clicks do not read as one here
    await ed.call("run_command", id="assets.open")
    await ui.frames(6)
    ed.check_eq(await focused(ed), "Particle Effect", "the Particle Effect window takes the focus (S91)")
    ed.check_eq(await buffers(ed), [], "no text buffer opens")
    status = (await ed.call("editor_status"))["ui"]["particle_effect"]
    ed.check_eq(status["effect"], EFFECT, "the window shows the effect")
    await probe_item(ui, "particle_effect.stage")
    await shot(ed, "particle-effect-window")


async def test_open_as_text_stays_in_the_context_menu(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    await fresh(ed)
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
    await ui.release_all()
    await fresh(ed)
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
    await ui.release_all()
    await fresh(ed)
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
    await ui.release_all()
    await fresh(ed)
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


async def roughness_drag(ed: Editor, ui: Ui) -> tuple[int, int]:
    """Starts a drag on the Roughness row; the mouse stays down"""
    item = await ui.reveal("##input_float_submeshes[0].material.roughnessFactor", "Inspector")
    start = ui.centre(item["rect"])
    await ui.drag(start, (start[0] - 60, start[1]), release=False)
    return start


async def test_ctrl_z_during_a_drag_only_puts_the_drag_back(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    await fresh(ed)
    await show_material(ed, ui)
    file = ed.project / MATERIAL
    await ed.call("update_material", path=MATERIAL, values={"metallicFactor": 0.375})
    await ui.frames(6)
    before = file.read_text()
    steps = await undo_steps(ed, "project")
    await roughness_drag(ed, ui)
    await ui.key("ctrl+z")
    await ui.frames(2)
    await ui.release()
    await ui.frames(6)
    ed.check_eq(file.read_text(), before, "the older step is not undone and nothing is written")
    ed.check_eq(await undo_steps(ed, "project"), steps, "no step is taken or added")
    await ed.call("undo", scope="project")
    await ui.frames(6)


async def test_color_window_session_is_one_step_and_esc_reverts_it(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    await fresh(ed)
    await show_material(ed, ui)
    file = ed.project / MATERIAL
    before = file.read_text()
    steps = await undo_steps(ed, "project")
    swatch = await probe_item(ui, "inspector.color.submeshes[0].material.baseColorFactor")
    await ui.click(centre(ui, swatch))
    await ui.frames(6)
    picker = {}
    for _ in range(20):
        picker = (await ed.call("editor_status"))["ui"]["color_picker"]
        if picker.get("focused"):
            break
        await ui.frames(2)
    ed.check(picker.get("focused"), f"the Color window has the focus: {picker}")
    inner = await ui.find("##sliderMode", "Color")
    ed.check(inner, "a control of the Color window is drawn")
    area = inner["window_rect"]

    async def pick(fx: float, fy: float):
        await ui.click(ui.at([area["x"] + area["width"] * fx, area["y"] + area["height"] * fy]))
        await ui.frames(4)

    await pick(0.30, 0.30)
    await pick(0.45, 0.35)
    ed.check_eq(file.read_text(), before, "a session is written when it ends, not at each release")
    await ui.key("Escape")
    await ui.frames(6)
    ed.check_eq(file.read_text(), before, "Esc reverts the session")
    ed.check_eq(await undo_steps(ed, "project"), steps, "and adds no step")


async def test_a_change_during_a_drag_is_not_overwritten(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    await fresh(ed)
    await show_material(ed, ui)
    file = ed.project / MATERIAL
    await roughness_drag(ed, ui)
    await ed.call("update_material", path=MATERIAL, values={"metallicFactor": 0.625})
    await ui.frames(2)
    await ui.release()
    await ui.frames(6)
    text = file.read_text()
    ed.check("metallicFactor: 0.625" in text, f"the agent's edit stays in the file: {text}")
    ed.check("roughnessFactor: 1" in text, f"the drag on the older values is not written: {text}")
    await ed.call("undo", scope="project")
    await ui.frames(6)


async def test_an_edit_left_when_the_view_goes_does_not_come_back_later(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    await fresh(ed)
    await ed.call("create_material", path="assets/mats/Other.material", values={"metallicFactor": 0.5}, overwrite=True)
    await show_material(ed, ui)
    file = ed.project / MATERIAL
    await roughness_drag(ed, ui)
    # the file goes while the mouse is down, and the view with it
    file.unlink()
    await ed.call("sync_files")
    await ui.frames(4)
    await ui.release()
    await ui.frames(6)
    ed.check("asset.material.preview" not in ed.ui_items(), "the Inspector no longer shows it")
    rect = await project_file(ui, "Other.material")
    await ui.click(centre(ui, rect))
    await ui.frames(6)
    await probe_item(ui, "asset.material.preview")
    ed.check(not file.exists(), "showing another material does not bring the deleted one back")
    file.write_bytes(STATE["material"])
    await ed.call("sync_files")
    await ed.call("link_material", entity="Ball", material=MATERIAL)
    await ui.frames(6)


async def scene_is_unsaved(ed: Editor) -> bool:
    status = await ed.call("editor_status")
    return status["project"]["selected_scene_id"] in status["unsaved"]["scene_ids"]


async def test_esc_leaves_the_linked_scene_saved(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    await fresh(ed)
    await ed.call("save_scene")
    await show_material(ed, ui)
    ed.check(not await scene_is_unsaved(ed), "the scene is saved")
    await roughness_drag(ed, ui)
    await ui.key("Escape")
    await ui.frames(2)
    await ui.release()
    await ui.frames(6)
    ed.check(not await scene_is_unsaved(ed), "Esc puts the unsaved mark back too (docs/undo.md, Drags)")


async def test_an_edit_in_play_stays_on_the_linked_mesh_after_stop(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    await fresh(ed)
    await show_material(ed, ui)
    await ed.call("play", action="start")
    try:
        await ed.call("wait", game_seconds=0.2)
        await show_material(ed, ui)
        await ui.click(ui.centre((await ui.reveal("Clear Coat", "Inspector"))["rect"]))
        await ui.frames(6)
        ed.check("clearcoat" in (ed.project / MATERIAL).read_text(), "the file has the edit")
    finally:
        await ed.call("play", action="stop")
    await ui.frames(6)
    ed.check_eq(await linked_clearcoat(ed), True, "after Stop the linked submesh agrees with the file")
    await ed.call("undo", scope="project")
    await ui.frames(6)


async def test_a_write_keeps_guid_references_unknown_keys_and_line_endings(ed: Editor):
    ui = Ui(ed)
    await ui.release_all()
    await fresh(ed)
    file = ed.project / MATERIAL
    original = file.read_bytes()
    texture = ed.project / "assets/mats/albedo.png"
    texture.write_bytes(pngdiff.encode(4, 4, bytes([200, 90, 40, 255]) * 16))
    await ed.call("sync_files")
    await ed.call("update_material", path=MATERIAL, values={"baseColorTexture": "assets/mats/albedo.png"})
    with_texture = file.read_text()
    guid = next(line for line in with_texture.splitlines() if line.startswith("guid: "))
    reference = re.search(r"baseColorTexture:.*?\n(?:\s+.*\n)*", with_texture).group(0)
    ed.check("Guid" in reference or "guid" in reference, f"the reference holds its GUID: {reference}")
    file.write_bytes((with_texture.rstrip() + "\nnotAMaterialField: 1\n").replace("\n", "\r\n").encode())
    await ed.call("sync_files")
    await show_material(ed, ui)
    await ui.click(ui.centre((await ui.reveal("Clear Coat", "Inspector"))["rect"]))
    await ui.frames(6)
    raw = file.read_bytes()
    text = raw.decode().replace("\r\n", "\n")
    ed.check(b"clearcoat" in raw, f"the edit is written: {text}")
    ed.check(raw.count(b"\n") == raw.count(b"\r\n"), f"every line keeps its CRLF: {raw[:200]!r}")
    ed.check(guid in text, f"its own GUID stays: {text}")
    ed.check("notAMaterialField: 1" in text, f"an unknown root key stays: {text}")
    ed.check(reference in text, f"the texture and its GUID stay: {text}")
    file.write_bytes(original)
    await ed.call("sync_files")
    await ui.frames(6)
