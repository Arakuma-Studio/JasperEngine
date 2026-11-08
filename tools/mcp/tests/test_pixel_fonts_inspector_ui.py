"""The Bitmap Font Inspector and the Text Inspector's Pixel Font rows with real input (docs/specs/
S51-pixel-fonts.md R22-R25, AC18, AC19): selecting hud.bitmapfont in the Project window shows its
glyph grid; a click selects a cell, a drag on its advance edge writes one advance override (one undo
step, merged while dragging), a drag on the baseline writes the baseline, a kerning pair is added and
removed, the preview draws; Create Bitmap Font on a PNG sheet, a .fnt and a pixel TTF writes a
.bitmapfont; a sheet named *@WxH.png shows the banner; a Text with a bitmap font shows Font Size in
multiples, the Outline Width combo and no Softness or Dilate, and a font dropped in the Scene view
snaps the size in one undo step. ui_input clicks the rects find_ui_item and editor_status report;
the screenshots of each step go to build/test-results/pixel-fonts-ui/.
"""

from __future__ import annotations

import json
import shutil

from harness import REPO, Editor, skip
import ui_input

EDITOR = {"create": True, "name": "Pixel Fonts UI", "xvfb": "harness"}
TIMEOUT = 900
CI = True
SHOTS = REPO / "build" / "test-results" / "pixel-fonts-ui"
FIXTURES = REPO / "engine/tests/core/fixtures/fonts"
FONT = "assets/fonts/hud.bitmapfont"


async def ui(ed: Editor) -> dict:
    return (await ed.call("editor_status"))["ui"]


async def wait_ui(ed: Editor, predicate, what: str, timeout: float = 20.0) -> dict:
    state = None
    for _ in range(int(timeout / 0.25)):
        state = await ui(ed)
        if predicate(state):
            return state
        await ui_input.frames(ed, 2)
    ed.fail(f"{what}; ui: {state}")


def tree_node(state: dict, path: str) -> dict | None:
    return next((node for node in state["project_window"]["tree"] if node["path"] == path), None)


def entry(state: dict, name: str) -> list | None:
    return next((item["rect"] for item in state["project_window"]["entries"] if item["name"] == name), None)


async def item(ed: Editor, label: str, window_name: str | None = None) -> list | None:
    args = {"label": label}
    if window_name:
        args["window"] = window_name
    items = [i for i in (await ed.call("find_ui_item", **args))["items"] if i.get("visible", True)]
    if not items:
        return None
    r = items[0]["rect"]
    return [r["x"], r["y"], r["width"], r["height"]]


async def wait_item(ed: Editor, label: str, window_name: str | None = None, timeout: float = 30.0) -> list:
    for _ in range(int(timeout / 0.25)):
        rect = await item(ed, label, window_name)
        if rect:
            return rect
        await ui_input.frames(ed, 2)
    ed.fail(f"no drawn UI item {label!r}")


async def click_at(ed: Editor, x: float, y: float, button: str = "1") -> None:
    driver = ui_input.for_editor(ed)
    await driver.click(driver.at((x, y)), button=button)


async def click(ed: Editor, rect: list, button: str = "1") -> None:
    x, y, w, h = rect
    await click_at(ed, x + w / 2, y + h / 2, button)


async def drag(ed: Editor, start: tuple[float, float], end: tuple[float, float]) -> None:
    driver = ui_input.for_editor(ed)
    await driver.drag(driver.at(start), driver.at(end), steps=8)


async def shot(ed: Editor, name: str) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    await ed.screenshot("editor", SHOTS / f"{name}.png", focus=False, max_width=0)


async def open_in_project(ed: Editor, folder: str, name: str) -> None:
    await ed.call("focus_window", window="project")
    state = await wait_ui(ed, lambda s: s["project_window"]["drawn"] and tree_node(s, "assets"), "the tree shows assets")
    if not tree_node(state, folder):
        x, y = tree_node(state, "assets")["arrow"]
        await click_at(ed, x, y)
        state = await wait_ui(ed, lambda s: tree_node(s, folder), f"{folder} in the tree")
    await click(ed, tree_node(state, folder)["rect"])
    state = await wait_ui(ed, lambda s: entry(s, name), f"{name} listed")
    await click(ed, entry(state, name))


def font_doc(ed: Editor) -> dict:
    return json.loads((ed.project / FONT).read_text())


async def setup_files(ed: Editor) -> None:
    fonts = ed.project / "assets/fonts"
    fonts.mkdir(parents=True, exist_ok=True)
    for name in ("grid@6x9.png", "bm.fnt", "bm_0.png", "pixel8.ttf"):
        shutil.copy(FIXTURES / name, fonts / name)
    (ed.project / FONT).write_text(json.dumps({"format": "jasperengine.bitmapfont", "version": 1,
                                               "source": {"type": "grid", "image": "grid@6x9.png"}}))
    await ed.call("sync_files", full=False)


async def test_bitmap_font_inspector(ed: Editor):
    if not ed._xvfb or not ed._xvfb.display:
        skip("needs the harness Xvfb")
    await ed.call("tool_groups", enable=["editor_ux"])
    await setup_files(ed)
    await open_in_project(ed, "assets/fonts", "hud.bitmapfont")
    grid = await wait_item(ed, "##bfGrid", "Inspector")
    await wait_item(ed, "Glyph Grid", "Inspector")
    await ui_input.frames(ed, 6)
    await shot(ed, "1-glyph-grid")

    # zoom 4: the cell of 'A' (index 33: column 1, row 2 of 16 x 6) at 6x9 px
    zoom, cw, ch = 4, 6, 9
    x0, y0 = grid[0], grid[1]
    cell = (x0 + (33 % 16) * cw * zoom + 2, y0 + (33 // 16) * ch * zoom + 2)
    await click_at(ed, *cell)
    await wait_item(ed, "Character##bfChar", "Inspector")
    await shot(ed, "2-cell-selected")

    # its advance edge (auto: 3 for 'A') dragged 2 font px right: one override, one undo step
    edge = (x0 + (33 % 16) * cw * zoom + 3 * zoom, y0 + (33 // 16) * ch * zoom + ch * zoom // 2)
    await drag(ed, edge, (edge[0] + 2 * zoom, edge[1]))
    await ui_input.frames(ed, 4)
    doc = font_doc(ed)
    ed.check_eq(doc.get("glyphs", {}).get("A", {}).get("advance"), 5, f"the drag wrote the advance: {doc}")
    status = await ed.call("editor_status")
    ed.check("Bitmap Font" in json.dumps(status.get("undo", {})), f"one undo step names it: {status.get('undo')}")
    await shot(ed, "3-advance-dragged")
    await ed.call("undo", scope="project")
    ed.check("glyphs" not in font_doc(ed) or "A" not in font_doc(ed)["glyphs"], "one undo removes the whole drag")

    # the baseline line (row 7 of every cell) dragged to row 8
    base = (x0 + 1, y0 + 7 * zoom)
    await drag(ed, base, (base[0], base[1] + zoom))
    await ui_input.frames(ed, 4)
    ed.check_eq(font_doc(ed).get("baseline"), 8, "the baseline drag wrote the baseline")
    await ed.call("undo", scope="project")

    # kerning: add T o -1, then remove it (the Inspector scrolls to each row)
    driver = ui_input.for_editor(ed)
    await driver.click_revealed("Kerning", "Inspector")
    for label, text in (("##bfKernLeft", "T"), ("##bfKernRight", "o")):
        await driver.click_revealed(label, "Inspector")
        await driver.type(text)
    await driver.click_revealed("Add Pair##bfKernAdd", "Inspector")
    await ui_input.frames(ed, 4)
    ed.check_eq(font_doc(ed).get("kerning"), [["T", "o", -1]], "the pair was added")
    await shot(ed, "4-kerning")
    await driver.click_revealed("##bfKernRemove0", "Inspector")
    await ui_input.frames(ed, 4)
    ed.check("kerning" not in font_doc(ed), "the pair was removed")

    # the preview draws
    await driver.reveal("##bfPreviewText", "Inspector")
    await shot(ed, "5-preview")


async def test_create_and_banner(ed: Editor):
    if not ed._xvfb or not ed._xvfb.display:
        skip("needs the harness Xvfb")
    await setup_files(ed)
    # the banner of a sheet named *@WxH.png
    await open_in_project(ed, "assets/fonts", "grid@6x9.png")
    await wait_item(ed, "Create Bitmap Font##bfBanner", "Inspector")
    await shot(ed, "6-banner")
    # Create Bitmap Font from each source kind (the Project window's context menu)
    for name, made in (("grid@6x9.png", "grid.bitmapfont"), ("bm.fnt", "bm.bitmapfont"), ("pixel8.ttf", "pixel8.bitmapfont")):
        state = await ui(ed)
        await click(ed, entry(state, name), button="3")
        await click(ed, await wait_item(ed, "Create Bitmap Font"))
        await ui_input.frames(ed, 4)
        path = ed.project / "assets/fonts" / made
        ed.check(path.exists(), f"Create Bitmap Font on {name} wrote {made}")
        doc = json.loads(path.read_text())
        ed.check_eq(doc["format"], "jasperengine.bitmapfont")
        if name.endswith(".ttf"):
            ed.check_eq(doc["source"].get("pixel_size"), 8, "the detected pixel size is written")
    info = await ed.call("get_asset_info", path="assets/fonts/grid.bitmapfont")
    ed.check_eq(info["font"]["glyphs"], 95)


async def test_text_inspector(ed: Editor):
    if not ed._xvfb or not ed._xvfb.display:
        skip("needs the harness Xvfb")
    await setup_files(ed)
    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/Hud")
    scene = made["scene"]["id"]
    await ed.call("create_ui", scene=scene, root={"type": "text", "name": "Label", "text": "Hp 99", "font": FONT,
                                                  "font_size": 20, "layout": {"anchor": "center"}})
    await ed.call("select_entities", scene=scene, entities=["Label"])
    await ed.call("focus_window", window="inspector")
    await ui_input.for_editor(ed).reveal("Pixel Font", "Inspector")
    ed.check(await item(ed, "Shadow Softness", "Inspector") is None, "Softness is hidden for a bitmap font")
    ed.check(await item(ed, "Shadow Dilate", "Inspector") is None, "Dilate is hidden for a bitmap font")
    await ui_input.for_editor(ed).reveal("##textPixelOutline", "Inspector")
    await shot(ed, "7-text-inspector")
    # 20 on a 9 px font: Snap writes 18, + writes 27, each one undo step
    driver = ui_input.for_editor(ed)
    await driver.click_revealed("Snap##textPixelSizeSnap", "Inspector")
    await ui_input.frames(ed, 3)
    got = await ed.call("get_entity", scene=scene, entity="Label", components=["text"])
    ed.check_eq(got["components"]["text"]["fontSize"], 18)
    await driver.click_revealed("+##textPixelSizeUp", "Inspector")
    await ui_input.frames(ed, 3)
    got = await ed.call("get_entity", scene=scene, entity="Label", components=["text"])
    ed.check_eq(got["components"]["text"]["fontSize"], 27)
    await ed.call("undo", scene=scene)
    got = await ed.call("get_entity", scene=scene, entity="Label", components=["text"])
    ed.check_eq(got["components"]["text"]["fontSize"], 18, "one undo step per edit")
