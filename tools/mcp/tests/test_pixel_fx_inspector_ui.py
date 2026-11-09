"""S48 AC24 (docs/specs/S48-pixel-fx.md R37-R40): the Pixel FX editor UI, driven like a person (xdotool in the
harness's Xvfb, aimed where find_ui_item says a control is drawn now).

Contact Sheet (R40): the Game view's Contact Sheet... button is greyed until one GameObject of the shown scene is
selected; it opens the Contact Sheet window, whose Capture draws the sheet and its checks; Save As... answers a file
picker and writes the PNG as one project undo step.

Screenshots of each step go to $JE_EVIDENCE_DIR (else the scratch directory).
"""

from __future__ import annotations

import os
from pathlib import Path

import pngdiff
import ui_input
from harness import Editor

EDITOR = {"xvfb": "harness", "create": True, "name": "Pixel FX UI"}
CI = True
TIMEOUT = 900

HERO_PNG = "assets/sprites/hero.png"
M = "submeshes[0].material."
STATE: dict = {}


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root) / "pixel-fx-ui"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def shot(ed: Editor, name: str) -> None:
    await ed.screenshot("editor", evidence(ed) / f"{name}.png", focus=False, max_width=0)


def ui_of(ed: Editor) -> ui_input.Ui:
    if "ui" not in STATE:
        STATE["ui"] = ui_input.for_editor(ed)
    return STATE["ui"]


async def test_contact_sheet_setup(ed: Editor):
    await ed.call("get_project_summary")
    (ed.project / "assets" / "sprites").mkdir(parents=True, exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets", "graphics.pixels_per_unit": 16})
    rgba = b"".join(bytes((200, 40, 40, 255)) if (x + y) % 3 else bytes((40, 180, 60, 255)) for y in range(8) for x in range(8))
    (ed.project / HERO_PNG).write_bytes(pngdiff.encode(8, 8, rgba))
    await ed.call("sync_files")
    scene = (await ed.call("create_scene", name="Sheet UI", type="2D"))["scene"]["id"]
    made = await ed.call("create_entity", scene=scene, type="sprite", name="Hero", position=[0, 0, 0],
                         values={"mesh": {M + "baseColorTexture": {"path": HERO_PNG, "min_filter": "nearest", "mag_filter": "nearest"}},
                                 "sprite": {"pivotPreset": "center", "width": 8, "height": 8}})
    STATE["scene"], STATE["hero"] = scene, made["entity_id"]
    await ed.call("select_scene", scene=scene)
    await ed.call("select_entities", scene=scene, entities=[])
    await ed.call("focus_window", window="game")
    await ui_of(ed).frames(4)


async def test_contact_sheet_button_needs_a_selection(ed: Editor):
    ui = ui_of(ed)
    button = await ui.find("Contact Sheet...##GameContactSheet", "Game")
    ed.check(button is not None, "the Game view's toolbar draws Contact Sheet...")
    ed.check(button.get("enabled") is False, f"greyed without a selection: {button}")
    await ed.call("select_entities", scene=STATE["scene"], entities=[STATE["hero"]])
    for _ in range(60):
        button = await ui.find("Contact Sheet...##GameContactSheet", "Game", frames=4)
        if button and button.get("enabled", True):
            break
    ed.check(button and button.get("enabled", True), f"enabled with one GameObject selected: {button}")


async def test_contact_sheet_capture_and_save(ed: Editor):
    ui = ui_of(ed)
    await ui.click_item("Contact Sheet...##GameContactSheet", "Game")
    capture = await ui.find("Capture##cs_capture", "Contact Sheet")
    ed.check(capture is not None, "the Contact Sheet window opens with its Capture button")
    for name in ("Frames##cs_frames", "Duration##cs_duration", "Scale##cs_scale", "Lints##cs_lints", "Scene Mode##cs_scene"):
        ed.check(await ui.find(name, "Contact Sheet") is not None, f"the window has {name.split('##')[0]}")
    await shot(ed, "1-contact-sheet-window")
    await ui.click_item("Capture##cs_capture", "Contact Sheet")
    image = None
    for _ in range(300):
        image = await ui.find("##cs_image", "Contact Sheet", frames=4)
        if image:
            break
    ed.check(image is not None, "the sheet is drawn in the window")
    ed.check(await ui.find("##cs_checks", "Contact Sheet") is not None, "with the checks table")
    await shot(ed, "2-contact-sheet-captured")

    before = {e["id"] for e in (await ed.call("get_undo_history", scope="project", limit=500))["entries"]}
    await ui.click_item("Save As...##cs_save", "Contact Sheet")
    status = await ed.call("editor_status")
    ed.check(status.get("dialog", {}).get("kind") == "file_picker", f"Save As... asks where: {status.get('dialog')}")
    target = ed.project / "assets" / "sheets" / "hero_sheet.png"
    target.parent.mkdir(parents=True, exist_ok=True)
    await ed.call("respond_dialog", values={"path": str(target)})
    for _ in range(120):
        if target.exists():
            break
        await ui_input.frames(ed, 4)
    ed.check(target.exists(), "Save As... writes the PNG")
    width, height, _ = pngdiff.decode(target.read_bytes())
    ed.check(width > 24 and height > 24, f"a sheet, not a frame: {width}x{height}")
    added = [e for e in (await ed.call("get_undo_history", scope="project", limit=500))["entries"] if e["id"] not in before]
    ed.check_eq(len(added), 1, f"one undo step: {[e['name'] for e in added]}")
    await ed.call("undo", scope="project")
    ed.check(not target.exists(), "undo removes it")
    await shot(ed, "3-contact-sheet-saved")
