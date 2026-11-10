"""S43 AC18 (R32-R34): the Palette window and the Color window's palettes, driven like a person
(xdotool in the harness's Xvfb, aimed where find_ui_item says a control is drawn now).

1. A .palette opens in Window > 2D > Palette; dragging swatch 0 past swatch 3 reorders the file,
   and one project undo puts the old order back (the window reads the file again).
2. Generate Ramp... > Add to Palette appends a ramp of darker + 1 + lighter steps; the dialog's
   close button closes it (it stays open for more ramps, over the Palette window's buttons).
3. Create Variant adds "Variant 1"; clicking its hatched ("same as base") cell gives the cell the
   base colour with the Color window open on it, and Esc makes it "same as base" again.
4. Usage, pinned to a texture: each swatch's label carries its pixel count (the badges).
5. The Color window's Snap to Palette moves a typed colour (Tab out of Hexadecimal) to the nearest
   palette colour.
6. The material inspector: Detect sets the Source Palette; the lossy-texture warning's Fix makes the
   texture lossless and Nearest in one project undo step.

The swatch buttons' labels are "color <rrggbb>[ count <n>]###palette_swatch_<i>": the id after
### stays put while the text says what the swatch shows. find_ui_item reports a label as drawn, its
"##" part included; the checks read the text before it. Screenshots of each step go to
$JE_EVIDENCE_DIR (else the scratch directory).
"""

from __future__ import annotations

import json
import os
import struct
import zlib
from pathlib import Path

from harness import Editor
import ui_input

EDITOR = {"xvfb": "harness", "create": True, "name": "Palette Window"}
CI = True
TIMEOUT = 900

PALETTE = "assets/palettes/test.palette"
TEXTURE = "assets/textures/usage.png"
COLORS = ["#000000", "#ffffff", "#ff0000", "#00ff00", "#0000ff", "#ffff00"]
WINDOW = "Palette"
STATE: dict = {}


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root) / "palette-window"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


async def shot(ed: Editor, name: str) -> None:
    await ed.screenshot("editor", evidence(ed) / f"{name}.png", focus=False, max_width=0)


def png(width: int, height: int, pixels: list[tuple[int, int, int, int]]) -> bytes:
    """an RGBA8 PNG of the pixels, rows top first"""
    rows = b"".join(b"\x00" + bytes(c for p in pixels[y * width:(y + 1) * width] for c in p) for y in range(height))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def palette_file(ed: Editor) -> dict:
    return json.loads((ed.project / PALETTE).read_text())


async def until(ed: Editor, probe, what: str, frames: int = 1200):
    """probe() until it gives a truthy value, a few editor frames apart (frames: the hang guard)"""
    value = None
    for _ in range(frames // 4):
        value = probe()
        if value:
            return value
        await ui_input.frames(ed, 4)
    ed.fail(f"{what} (last: {value!r})")


def shown(item: dict) -> str:
    """an item's visible text: its label before the "##" id"""
    return item["label"].split("##", 1)[0]


async def label(ui: ui_input.Ui, name: str, window: str | None = WINDOW) -> str:
    item = await ui.find(name, window)
    ui.ed.check(item is not None, f"{name} is drawn")
    return shown(item)


async def label_until(ui: ui_input.Ui, name: str, predicate, what: str, window: str | None = WINDOW) -> str:
    text = ""
    for _ in range(300):
        item = await ui.find(name, window, frames=4)
        text = shown(item) if item else ""
        if item and predicate(text):
            return text
    ui.ed.fail(f"{what}: {name} is {text!r}")


def ui_of(ed: Editor) -> ui_input.Ui:
    if "ui" not in STATE:
        STATE["ui"] = ui_input.for_editor(ed)
    return STATE["ui"]


async def test_open_and_drag_reorder_undo(ed: Editor):
    ui = ui_of(ed)
    (ed.project / PALETTE).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / PALETTE).write_text(json.dumps({"format": "jasperengine.palette", "version": 1, "name": "Test", "columns": 8,
                                                  "colors": COLORS}, indent=2))
    await ed.call("tool_groups", enable=["editor_ux"])
    await ed.call("sync_files", paths=[PALETTE], full=False)
    await ed.call("run_command", id="window.palette", payload=PALETTE)

    for i, color in enumerate(COLORS):
        text = await label_until(ui, f"###palette_swatch_{i}", lambda t: t.startswith("color "), f"swatch {i} drawn")
        ed.check(text.startswith("color " + color[1:]), f"swatch {i} shows {color}: {text!r}")
    await shot(ed, "1-opened")

    # swatch 0 dropped on the right half of swatch 3: it lands after it
    first = await ui.find("###palette_swatch_0", WINDOW)
    fourth = await ui.find("###palette_swatch_3", WINDOW)
    rect = fourth["rect"]
    await ui.drag(ui.centre(first["rect"]), ui.at(rect, (rect["width"] * 0.8, rect["height"] / 2)), steps=16)
    moved = ["#ffffff", "#ff0000", "#00ff00", "#000000", "#0000ff", "#ffff00"]
    await until(ed, lambda: palette_file(ed)["colors"] == moved, "the drag reorders the file")
    await label_until(ui, "###palette_swatch_3", lambda t: t.startswith("color 000000"), "the moved swatch is drawn at 3")
    await shot(ed, "2-reordered")

    history = await ed.call("get_undo_history", scope="project")
    ed.check("Move Colour" in json.dumps(history), f"the reorder is a project undo step: {history}")
    await ed.call("undo", scope="project")
    await until(ed, lambda: palette_file(ed)["colors"] == COLORS, "undo restores the order")
    await label_until(ui, "###palette_swatch_0", lambda t: t.startswith("color 000000"), "the window reads the restored file")
    await shot(ed, "3-undone")


async def test_generate_ramp(ed: Editor):
    ui = ui_of(ed)
    await ui.click_item("Generate Ramp...", WINDOW)
    await ui.click_item("Add to Palette", "Generate Ramp")
    ramps = await until(ed, lambda: palette_file(ed).get("ramps"), "Add to Palette writes a ramp")
    ed.check_eq(len(ramps), 1)
    ed.check_eq(ramps[0]["name"], "Ramp 1")
    # the defaults: 3 darker, the base, 3 lighter
    ed.check_eq(len(ramps[0]["colors"]), 7)
    await label_until(ui, "###palette_ramp_0", lambda t: t.startswith("Ramp 1"), "the ramp's strip is drawn")
    await shot(ed, "4-ramp")
    # the dialog stays open for more ramps and floats over the Palette window's buttons: closed as a person would
    await ui.click_item("#CLOSE", "Generate Ramp")
    for _ in range(30):
        if await ui.find("Add to Palette", "Generate Ramp", frames=4) is None:
            break
    else:
        ed.fail("the Generate Ramp dialog closes on its close button")


async def test_create_variant(ed: Editor):
    ui = ui_of(ed)
    await ui.click_item("Create Variant", WINDOW)
    variants = await until(ed, lambda: palette_file(ed).get("variants"), "Create Variant writes a variant")
    ed.check_eq(variants[0]["name"], "Variant 1")

    cell = await ui.reveal("###palette_variant_0_2", WINDOW)
    ed.check(shown(cell).startswith("same as base"), f"a new variant's cells are the base colour: {cell['label']!r}")
    await ui.click(ui.centre(cell["rect"]))

    def cell_color():
        colors = palette_file(ed)["variants"][0].get("colors", [])
        return colors[2] if len(colors) > 2 else None

    await until(ed, lambda: cell_color() == "#ff0000", "a click gives the hatched cell the base colour")
    await label_until(ui, "###palette_variant_0_2", lambda t: t.startswith("color ff0000"), "the cell shows its colour")
    await shot(ed, "5-variant-cell")
    # Esc in the Color window: the cell is "same as base" again
    await ui.key("Escape")
    await until(ed, lambda: cell_color() is None, "Esc makes the cell the base colour again")


async def test_usage_badges(ed: Editor):
    ui = ui_of(ed)
    # 10 red, 4 white and 2 off-palette pixels
    pixels = [(255, 0, 0, 255)] * 10 + [(255, 255, 255, 255)] * 4 + [(0x12, 0x34, 0x56, 255)] * 2
    (ed.project / TEXTURE).parent.mkdir(parents=True, exist_ok=True)
    (ed.project / TEXTURE).write_bytes(png(4, 4, pixels))
    await ed.call("sync_files", paths=[TEXTURE], full=False)

    combo = await ui.reveal("##usage_texture", WINDOW)
    await ui.click(ui.centre(combo["rect"]))
    await ui.click_item(TEXTURE)
    red = COLORS.index("#ff0000")
    white = COLORS.index("#ffffff")
    await label_until(ui, f"###palette_swatch_{red}", lambda t: t.endswith(" count 10"), "the red swatch's badge counts 10")
    await label_until(ui, f"###palette_swatch_{white}", lambda t: t.endswith(" count 4"), "the white swatch's badge counts 4")
    text = await label(ui, f"###palette_swatch_{COLORS.index('#0000ff')}")
    ed.check(text.endswith(" count 0"), f"an unused colour counts 0: {text!r}")
    await shot(ed, "6-usage")


async def test_color_picker_snaps_to_the_palette(ed: Editor):
    ui = ui_of(ed)
    await ed.call("run_command", id="window.colorPicker")
    await ui.click_item("Snap to Palette", "Color")
    # "##hex" also finds the Copy##hex button beside the field: the field is the item of that whole label
    fields = [i for i in (await ed.call("find_ui_item", label="##hex", window="Color"))["items"] if i["label"] == "##hex" and i["visible"]]
    ed.check(fields, "the Hexadecimal field is drawn")
    await ui.click(ui.centre(fields[0]["rect"]))
    await ui.key("ctrl+a")
    await ui.type("F01010")
    # Tab leaves the field, which applies the hex; Enter would also close the window (S27: Enter commits)
    await ui.key("Tab")
    colors = palette_file(ed)["colors"]
    expected = f"Snapped: index {colors.index('#ff0000')} #ff0000"
    await label_until(ui, "###snap_result", lambda t: t.startswith(expected), "the typed colour snaps to the nearest palette colour", "Color")
    await shot(ed, "7-snap")
    await ui.key("Escape")


async def test_material_inspector_detect_and_fix(ed: Editor):
    """6. The material inspector's Palette Swap rows: Detect sets the Source Palette to the project
    palette holding every colour of the sprite's texture; the lossy-texture warning's Fix makes the
    texture lossless (no compression, no mipmaps) and Nearest, as one project undo step."""
    ui = ui_of(ed)
    detect_png = "assets/textures/detect.png"
    other = "assets/palettes/other.palette"
    (ed.project / other).write_text(json.dumps({"format": "jasperengine.palette", "version": 1, "name": "Other", "columns": 8,
                                                "colors": ["#123456", "#654321"]}, indent=2))
    pixels = [tuple(int(c[i:i + 2], 16) for i in (1, 3, 5)) + (255,) for c in COLORS[:4]] * 4
    (ed.project / detect_png).write_bytes(png(4, 4, pixels))
    await ed.call("sync_files", paths=[other, detect_png], full=False)

    scene = (await ed.call("create_scene", name="Inspector Rows", type="2D"))["scene"]["id"]
    m = "submeshes[0].material."
    entity = (await ed.call("create_entity", scene=scene, type="sprite", name="Hero",
                            values={"mesh": {m + "baseColorTexture": {"path": detect_png, "min_filter": "linear", "mag_filter": "linear"}},
                                    "sprite": {"width": 64, "height": 64}}))["entity_id"]
    await ed.call("set_properties", scene=scene, entity=entity, values={"mesh": {m + "shader": "library:sprite"}})
    await ed.call("set_properties", scene=scene, entity=entity, values={"mesh": {
        m + "shaderFeatures": ["PALETTE_SWAP"], m + "shaderPalettes": {"u_palette": {"source": other}}}})

    def source_of(got: dict) -> str:
        text = json.dumps(got)
        return "test.palette" if "test.palette" in text else ("other.palette" if "other.palette" in text else "")

    await ed.call("select_entities", scene=scene, entities=[entity])
    await ed.call("focus_window", window="inspector")
    await ui_input.frames(ed, 10)
    if not (await ed.call("find_ui_item", label="Detect##palette_detect_u_palette", window="Inspector"))["items"]:
        await ui.click_revealed("##toggle_mesh", "Inspector")
    ed.check_eq(source_of(await ed.call("get_entity", scene=scene, entity=entity, components=["mesh"])), "other.palette")
    await ui.click_revealed("Detect##palette_detect_u_palette", "Inspector")

    async def detected():
        got = await ed.call("get_entity", scene=scene, entity=entity, components=["mesh"])
        return source_of(got) == "test.palette"

    for _ in range(300):
        if await detected():
            break
        await ui_input.frames(ed, 4)
    else:
        ed.fail("Detect sets the Source Palette to the palette holding the texture's colours")
    await shot(ed, "8-detect")

    # Fix: the texture is compressed and filtered by default; one click makes it lossless and Nearest
    before = await ed.call("get_texture_import_settings", path=detect_png)
    history = len(json.dumps(await ed.call("get_undo_history", scope="project")))
    fix = await ui.reveal("Fix##palette_lossless_fix_u_palette", "Inspector", frames=600)
    await ui.click(ui.centre(fix["rect"]))

    async def fixed():
        settings = await ed.call("get_texture_import_settings", path=detect_png)
        stored = settings.get("stored") or {}
        return stored.get("quality") == "none" and stored.get("mipmaps") is False

    for _ in range(300):
        if await fixed():
            break
        await ui_input.frames(ed, 4)
    else:
        ed.fail(f"Fix makes the texture lossless (before: {before})")
    got = json.dumps(await ed.call("get_entity", scene=scene, entity=entity, components=["mesh"]))
    ed.check('"nearest"' in got, "Fix sets the sprite's filter to Nearest")
    ed.check(len(json.dumps(await ed.call("get_undo_history", scope="project"))) > history, "Fix is a project undo step")
    await shot(ed, "9-fixed")
    await ed.call("undo", scope="project")
    for _ in range(300):
        if not await fixed():
            break
        await ui_input.frames(ed, 4)
    else:
        ed.fail("one project undo puts the texture's settings back")
