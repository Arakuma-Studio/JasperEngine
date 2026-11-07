"""The S45 editor UI on the harness' display (docs/specs/S45-sprite-atlas.md R29-R31, AC15): the
Sprite Atlas Inspector (stats, pages, frame search), the texture importer's Sprite Mode (the
name@WxH grid, Packed in), the Sprite Inspector's frame picker, Sliced warning and legacy section.
Each view's whole editor window is saved under the test's scratch folder as evidence.
"""

from __future__ import annotations

import pngdiff
import ui_input
from harness import REPO, Editor, TestFailure

CI = True
EDITOR = {"create": True, "name": "SpriteAtlasEditor", "xvfb": "harness", "ui_probe": True}

S: dict = {}


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def icon(seed: int, size: int = 16) -> bytes:
    rgba = bytearray()
    for y in range(size):
        for x in range(size):
            inside = 2 <= x < size - 2 and 2 <= y < size - 2
            rgba += bytes(((x * 13 + seed * 60) & 255, (y * 11) & 255, seed * 80 & 255, 255 if inside else 0))
    return pngdiff.encode(size, size, bytes(rgba))


async def select_entry(ed: Editor, helper: ui_input.Ui, name: str) -> None:
    await ed.call("select_entities", entities=[])
    await ed.call("focus_window", window="project")
    for _ in range(40):
        status = await ed.call("editor_status")
        entries = status["ui"]["project_window"]["entries"]
        rect = next((e["rect"] for e in entries if e["name"] == name), None)
        if rect:
            await helper.click(helper.centre(rect))
            await ed.call("focus_window", window="inspector")
            await helper.frames(8)
            return
        await helper.frames(4)
    ed.fail(f"{name} is not in the Project window: {entries}")


async def wait_item(ed: Editor, helper: ui_input.Ui, item: str, frames: int = 120) -> list:
    for _ in range(frames // 4):
        items = ed.ui_items()
        if item in items:
            return items[item]
        await helper.frames(4)
    ed.fail(f"the UI item {item} never showed: {sorted(i for i in ed.ui_items() if 'sprite' in i.lower() or 'texture' in i.lower())}")


async def shot(ed: Editor, name: str) -> None:
    png = await ed.screenshot("editor", path=ed.scratch.root / f"{name}.png", max_width=0)
    check(png and not pngdiff.flat(pngdiff.decode(png)), f"{name}: the editor window draws")
    # also beside the build (not committed), for the review's evidence
    evidence = REPO / "build" / "s45-evidence"
    evidence.mkdir(parents=True, exist_ok=True)
    (evidence / f"{name}.png").write_bytes(png)


async def test_atlas_inspector(ed: Editor):
    helper = ui_input.Ui(ed)
    ed.project.mkdir(parents=True, exist_ok=True)
    for i, name in enumerate(("a.png", "b.png")):
        (ed.project / name).write_bytes(icon(i))
    (ed.project / "coin@8x8.png").write_bytes(pngdiff.encode(16, 8, bytes([200, 100, 50, 255] * 16 * 8)))
    await ed.call("sync_files", paths=["a.png", "b.png", "coin@8x8.png"])
    await ed.call("tool_groups", enable=["sprites"])
    r = await ed.call_raw("edit_sprite_atlas", {"path": "icons.spriteatlas", "create": True, "add": ["a.png", "b.png"], "output": True,
                                                "include_pages": False})
    check(not r.is_error, r.text)
    for _ in range(100):
        got = await ed.call("get_sprite_atlas", path="icons.spriteatlas")
        if got.get("build_state") == "built":
            break
        await ed.call("wait", seconds=0.2)
    check(got.get("build_state") == "built", f"built: {got}")
    await select_entry(ed, helper, "icons.spriteatlas")
    await helper.reveal("Objects for Packing", "Inspector")
    await helper.reveal("##max_size", "Inspector")
    await shot(ed, "sprite-atlas-inspector")
    await helper.reveal("\uf06e Pack Preview", "Inspector")
    await wait_item(ed, helper, "spriteAtlas.packPreview")
    await wait_item(ed, helper, "spriteAtlas.stats")
    await helper.reveal("##frame_search", "Inspector")
    await wait_item(ed, helper, "spriteAtlas.frameSearch")
    await shot(ed, "sprite-atlas-inspector-pages")


async def test_texture_importer_sprite_mode(ed: Editor):
    helper = ui_input.Ui(ed)
    await select_entry(ed, helper, "coin@8x8.png")
    await helper.reveal("Sprite Mode", "Inspector")
    await wait_item(ed, helper, "textureImporter.spriteMode")
    await wait_item(ed, helper, "textureImporter.gridFromFileName")
    await shot(ed, "texture-sprite-mode")
    await select_entry(ed, helper, "a.png")
    await helper.reveal("Sprite Mode", "Inspector")
    await wait_item(ed, helper, "textureImporter.packedIn")
    await shot(ed, "texture-packed-in")


async def test_sprite_inspector(ed: Editor):
    helper = ui_input.Ui(ed)
    made = await ed.call("set_sprite_frames", create={"name": "Iconic"}, sheet="icons.spriteatlas", frame="b")
    entity = made["sprite"]["id"]
    await ed.call("set_properties", entity=entity, changes=[{"component": "sprite", "property": "drawMode", "value": "sliced"},
                                                           {"component": "sprite", "property": "width", "value": 48},
                                                           {"component": "sprite", "property": "height", "value": 32}])
    await ed.call("select_entities", entities=[entity])
    await ed.call("focus_window", window="inspector")
    await helper.frames(8)
    await helper.reveal("##sheet_frame", "Inspector")
    await wait_item(ed, helper, "inspector.sprite.framePicker")
    await wait_item(ed, helper, "inspector.sprite.slicedWarning")
    await shot(ed, "sprite-inspector-sheet")
    legacy = await ed.call("set_sprite_frames", create={"name": "Legacy"}, texture="a.png", frames=[{"name": "all", "rect": [0, 0, 16, 16]}],
                           inline=True)
    await ed.call("select_entities", entities=[legacy["sprite"]["id"]])
    await helper.frames(8)
    await helper.reveal("\uf0ec Convert to Sprite Sheet", "Inspector")
    await wait_item(ed, helper, "inspector.sprite.convertToSheet")
    await shot(ed, "sprite-inspector-legacy")
