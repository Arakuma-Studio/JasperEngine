"""S42 AC13, AC5 (docs/specs/S42-pixel-art-mode.md R18-R20, R7), driven like a person (xdotool in the
harness's Xvfb, aimed where find_ui_item says a control is drawn now) on a copy of
tools/fixtures/pixel-art-bad:
1. Window > 2D > Pixel Art Check lists the findings grouped by rule;
2. clicking a row selects its GameObject;
3. Fix on the filtering row changes that reference only, and one undo puts it back;
4. Fix All's preview counts the changes check_pixel_art {fix: ["all"]} plans;
5. the texture importer's Apply to 3 References sets three references to its Filter Mode in one step;
6. Project Settings > Pixel Art > Apply Pixel Art Preset... previews and is answered like any modal.
Screenshots of the window, the preset dialog and the importer's Pixel Art section go to
$JE_EVIDENCE_DIR (else the scratch directory).
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

import ui_input
from harness import REPO, Editor, skip

CI = True
TIMEOUT = 900
EDITOR = {"project": REPO / "tools" / "fixtures" / "pixel-art-bad", "xvfb": "harness", "ui_probe": True}
WINDOW = "Pixel Art Check"
M = "submeshes[0].material."
STATE: dict = {}


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root) / "pixel-art-check"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def shot(ed: Editor, name: str) -> None:
    await ed.screenshot("editor", evidence(ed) / f"{name}.png", focus=False, max_width=0)


def ui_of(ed: Editor) -> ui_input.Ui:
    if "ui" not in STATE:
        STATE["ui"] = ui_input.for_editor(ed)
    return STATE["ui"]


async def wait_item(ed: Editor, label: str, window: str | None = WINDOW, frames: int = 600) -> dict:
    """drawn, and scrolled into its window's view"""
    return await ui_of(ed).reveal(label, window, frames=frames)


async def click(ed: Editor, label: str, window: str | None = WINDOW) -> None:
    await ui_of(ed).click_revealed(label, window)


async def texture(ed: Editor, entity: str, scene: str = "scenes/Main.scene"):
    got = await ed.call("get_entity", scene=scene, entity=entity, components=["mesh"])
    mesh = got["components"]["mesh"]
    return mesh.get(M + "baseColorTexture") or mesh["submeshes"][0]["material"]["baseColorTexture"]


async def test_window_lists_findings(ed: Editor):
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness Xvfb")
    await ed.call("tool_groups", enable=["pixel_art", "editor_ux", "assets_pipeline"])
    await ed.call("open_scene", scene="scenes/Main.scene")
    await ed.call("run_command", id="window.pixelArtCheck")
    for rule in ("filtering", "texel_size", "off_grid", "rotation", "mipmaps_compression"):
        await wait_item(ed, f"###rule_{rule}")
    await shot(ed, "1-window")


async def test_row_selects_and_fix_changes_one_reference(ed: Editor):
    result = await ed.call("check_pixel_art", rules=["filtering"])
    finding = next(f for f in result["findings"] if f["target"].get("entity") == "/Bilinear")
    STATE["filtering_key"] = finding["key"]
    await click(ed, "##" + finding["key"])
    selection = await ed.call("get_selection")
    ed.check("Bilinear" in str(selection), f"clicking the row selects the GameObject: {selection}")
    others = {name: await texture(ed, name) for name in ("Hero", "Scaled", "OffGrid")}
    await click(ed, "Fix##" + finding["key"])
    for _ in range(100):
        fixed = await texture(ed, "Bilinear")
        if isinstance(fixed, dict) and fixed.get("min_filter") == "nearest":
            break
        await ui_input.frames(ed, 4)
    ed.check(fixed.get("min_filter") == "nearest" and fixed.get("mag_filter") == "nearest", f"Fix set the reference nearest: {fixed}")
    for name, before in others.items():
        ed.check_eq(await texture(ed, name), before, f"{name}'s reference is untouched")
    await ed.call("undo", scene="scenes/Main.scene")
    back = await texture(ed, "Bilinear")
    # (the default sampler, linear, reads back as the path)
    ed.check(back == "sprites/hero.png" or (isinstance(back, dict) and back.get("min_filter") == "linear"), f"one undo puts it back: {back}")


async def test_fix_all_preview_counts_the_plan(ed: Editor):
    plan = await ed.call("check_pixel_art", fix=["all"])
    fixes = len(plan["fix_plan"])
    changes = sum(len(item["changes"]) for item in plan["fix_plan"])
    await click(ed, "Run")
    await click(ed, f"Fix All ({fixes})...")
    dialog = None
    for _ in range(100):
        dialog = (await ed.call("editor_status")).get("dialog")
        if dialog:
            break
        await ui_input.frames(ed, 4)
    ed.check(dialog and f"applies {changes} change(s) of {fixes} finding(s)" in dialog.get("message", ""),
             f"the preview counts the plan ({changes} changes): {dialog}")
    await shot(ed, "2-fix-all-preview")
    await ed.call("respond_dialog", choice="cancel")


async def test_importer_applies_filter_mode_to_references(ed: Editor):
    # three references to offpalette.png, then its Filter Mode bilinear: they differ from it
    for name, x in (("Copy1", 18), ("Copy2", 20)):
        await ed.call("create_entity", scene="scenes/Main.scene", type="sprite", name=name, position=[x, 10, 0],
                      values={"mesh": {M + "baseColorTexture": "sprites/offpalette.png"}})
    await ed.call("set_texture_import_settings", paths=["sprites/offpalette.png"], settings={"filter_mode": "bilinear"})
    await ed.call("focus_window", window="project")
    state = (await ed.call("editor_status"))["ui"]
    tree = {node["path"]: node for node in state["project_window"]["tree"]}
    ed.check("sprites" in tree, f"the sprites folder: {list(tree)[:10]}")
    ui = ui_of(ed)
    x, y, w, h = tree["sprites"]["rect"]
    await ui.click(ui.at((x + w / 2, y + h / 2)))
    for _ in range(100):
        state = (await ed.call("editor_status"))["ui"]
        entry = next((i for i in state["project_window"]["entries"] if i["name"] == "offpalette.png"), None)
        if entry:
            break
        await ui_input.frames(ed, 4)
    x, y, w, h = entry["rect"]
    await ui.click(ui.at((x + w / 2, y + h / 2)))
    await wait_item(ed, "Apply to 3 References", "Inspector")
    await shot(ed, "3-importer-pixel-art")
    before = await ed.call("get_undo_history", limit=200)
    await click(ed, "Apply to 3 References", "Inspector")
    for name in ("OffPalette", "Copy1", "Copy2"):
        got = await texture(ed, name)
        ed.check(got == "sprites/offpalette.png" or (isinstance(got, dict) and got.get("min_filter") == "linear"),
                 f"{name} now follows the Filter Mode (bilinear): {got}")
    after = await ed.call("get_undo_history", limit=200)
    added = [e for e in after["entries"] if e["id"] not in {b["id"] for b in before["entries"]}]
    ed.check(len({e.get("link_id") or e["id"] for e in added}) == 1, f"one undo step: {[e['name'] for e in added]}")


async def test_preset_dialog(ed: Editor):
    await ed.call("run_command", id="edit.projectSettings", payload="Pixel Art")
    await wait_item(ed, "Apply Pixel Art Preset...", "Project Settings")
    await shot(ed, "4-project-settings-pixel-art")
    await click(ed, "Apply Pixel Art Preset...", "Project Settings")
    dialog = None
    for _ in range(100):
        dialog = (await ed.call("editor_status")).get("dialog")
        if dialog and "Pixel Art Preset" in dialog.get("title", ""):
            break
        await ui_input.frames(ed, 4)
    ed.check(dialog and "change(s)" in dialog.get("message", ""), f"the preset's preview counts its changes: {dialog}")
    await shot(ed, "5-preset-dialog")
    await ed.call("respond_dialog", choice="cancel")
    await ed.call("respond_dialog", choice="cancel")
