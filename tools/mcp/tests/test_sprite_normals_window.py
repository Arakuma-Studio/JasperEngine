"""The Sprite Normal Map window in the real editor (sprite normals Phase 4 spec 9.3), on a fresh project with a
32x32 shaded disk hero.png and a 4-frame 128x32 walk.png (written by a pure-Python PNG writer below), a 2D scene with
the Sprites Hero and Walker and a Light 2D built through MCP, driven by REAL X input (xdotool on the harness' Xvfb,
widgets found through the UI probe, Editor(ui_probe=True)) and the public MCP tools. The window's model (presets,
debounce, save and its undo steps, the GIF animation, the warnings and their fixes, Play, the lit preview) is the
editor unit test editor/tests/SpriteNormalsWindowTest.cpp.

Covered here, where the App's wiring, focus and real input matter:
- focus_window "sprite_normals" opens the window (its Lit view radio drawn);
- the Project window: a real right-click on hero.png > "Generate Normal Map..." opens the window on it, a real click
  on Save writes hero_normal.png, undo {scope: project} removes it;
- the Inspector: a real Shift-click on Hero's Normal Texture wand generates, saves and assigns hero_normal.png (one
  scene undo step: undo {scope: scene} clears the Normal Texture);
- real keys 3 then 1 switch the views (the selected radio's probe), and the editor screenshot is not flat;
- hero.png overwritten on disk then sync_files: the Project window's badge of hero_normal.png turns stale; a real
  right-click > "Regenerate Normal Map" makes it fresh again;
- while the scene plays the wand is disabled: a Shift-click writes nothing;
- Toon steps: real clicks on the Toon preset and the Toon steps buttons (their selected probes, the preview changes).
Set JASPER_SN_SHOTS=<dir> to keep the editor screenshots taken on the way.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import harness
from harness import Editor

EDITOR = {"create": True, "xvfb": "harness", "ui_probe": True}
CI_EXCLUDED = ("long-running (about 4 minutes on a loaded server: 11 tests of the window with real X input); "
               "in the full suite (MCP_SUITE=all) on the build servers")
TIMEOUT = 900

S: dict = {}
SHOTS = os.environ.get("JASPER_SN_SHOTS")


# ---------------------------------------------------------------------------- images

def png_bytes(width: int, height: int, pixels: list[tuple[int, int, int, int]]) -> bytes:
    """An RGBA8 PNG (zlib, filter 0 per row)."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(pixels[y * width + x])

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def disks(width: int, height: int, cells: int = 1, tint: int = 0) -> bytes:
    """Shaded disks (brighter up-left), one per cell, transparent around them."""
    pixels = []
    cw = width // cells
    for y in range(height):
        for x in range(width):
            c = x // cw
            cx, cy, r = c * cw + cw / 2.0, height / 2.0, min(cw, height) * 0.42
            dx, dy = x + 0.5 - cx, y + 0.5 - cy
            if dx * dx + dy * dy > r * r:
                pixels.append((0, 0, 0, 0))
                continue
            shade = max(0.15, min(1.0, 0.65 - (dx + dy) / (4.0 * r)))
            pixels.append((int(200 * shade + 40) ^ tint, int(120 * shade + 30), int(60 * shade + 20), 255))
    return png_bytes(width, height, pixels)


# ---------------------------------------------------------------------------- helpers

def xdo(ed: Editor, *args: str) -> str:
    if not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
        harness.skip("needs the harness' Xvfb (xvfb='harness')")
    if not shutil.which("xdotool"):
        harness.skip("xdotool is not installed (apt install xdotool)")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    return subprocess.run(["xdotool", *args], env=env, capture_output=True, text=True, timeout=20).stdout


async def focus_x(ed: Editor, window: str = "sprite_normals") -> None:
    await ed.call("focus_window", window=window)
    ids = xdo(ed, "search", "--onlyvisible", "--name", "JasperEngine").split()
    ed.check(ids, "the editor's X window")
    xdo(ed, "windowfocus", "--sync", ids[-1])
    await ed.call("wait", frames=6)


def center(rect):
    return (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0


async def click(ed: Editor, rect, button: str = "1", shift: bool = False) -> None:
    x, y = center(rect)
    xdo(ed, "mousemove", str(int(x)), str(int(y)))
    await asyncio.sleep(0.3)
    if shift:
        xdo(ed, "keydown", "shift")
        await ed.call("wait", frames=3)
    xdo(ed, "click", button)
    if shift:
        await ed.call("wait", frames=3)
        xdo(ed, "keyup", "shift")
    await ed.call("wait", frames=6)


async def rect_of(ed: Editor, name: str, what: str, panel: str | None = None, seconds: float = 10.0):
    """`name`'s rectangle; with `panel` (a probe name of the scrolling area), scrolled into view."""
    rect = await ed.ui_rect(name, timeout=seconds)
    if rect:
        return rect
    if panel:
        area = await ed.ui_rect(panel, timeout=5.0)
        if area:
            at = center(area)
            xdo(ed, "mousemove", str(int(at[0])), str(int(at[1])))
            for down in (True, False):
                for _ in range(30):
                    await ed.call("wait", frames=3)
                    rect = ed.ui_items().get(name)
                    if rect:
                        return rect
                    xdo(ed, "click", "5" if down else "4")
    ed.fail(f"{what} ({name}) is never drawn: {sorted(ed.ui_items())[:120]}")


async def wait_for(ed: Editor, predicate, what: str, seconds: float = 60.0) -> None:
    for _ in range(int(seconds * 4)):
        if predicate():
            return
        await ed.call("wait", frames=4)
        await asyncio.sleep(0.25)
    ed.fail(f"timed out waiting for {what}")


async def shot(ed: Editor, name: str) -> bytes:
    data = await ed.screenshot("editor", path=(Path(SHOTS) / f"{name}.png") if SHOTS else None)
    return data


async def open_assets_folder(ed: Editor) -> None:
    """The Project window showing assets/ (double-click the folder from the project root)."""
    await focus_x(ed, "project")
    if await ed.ui_rect("project.file.hero.png", timeout=2.0):
        return
    folder = await rect_of(ed, "project.file.assets", "the assets folder in the Project window")
    x, y = center(folder)
    xdo(ed, "mousemove", str(int(x)), str(int(y)))
    await asyncio.sleep(0.2)
    xdo(ed, "click", "--repeat", "2", "--delay", "80", "1")
    await ed.call("wait", frames=10)
    await rect_of(ed, "project.file.hero.png", "hero.png in the Project window")


async def normal_texture(ed: Editor, entity: str) -> str:
    got = await ed.call("get_entity", entity=entity, detail="full")
    found = harness.find_key(got, "submeshes[0].material.normalTexture")
    if found is None:
        text = str(got)
        return "hero_normal.png" if "hero_normal.png" in text else ""
    return str(found)


# ---------------------------------------------------------------------------- setup

async def test_setup(ed: Editor):
    ed.require_tools("focus_window", "sync_files", "create_scene", "create_entity", "set_properties", "set_sprite_frames",
                     "select_entities", "undo", "get_entity", "screenshot", "play")
    assets = ed.project / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    (assets / "hero.png").write_bytes(disks(32, 32))
    (assets / "walk.png").write_bytes(disks(128, 32, 4))
    await ed.call("sync_files")
    await ed.call("create_scene", name="Normals", type="2D", path="normals.scene", unsaved="discard")
    await ed.call("create_entity", type="sprite", name="Hero", position=[0, 0, 0],
                  values={"mesh": {"submeshes[0].material.baseColorTexture": "assets/hero.png"}, "sprite": {"width": 32, "height": 32}})
    await ed.call("set_sprite_frames", create={"name": "Walker", "position": [80, 0, 0]}, texture="assets/walk.png",
                  slice={"tile_size": [32, 32]}, inline=True)
    await ed.call("create_entity", type="light_2d", name="Light 2D", position=[-40, 40, 0])
    await ed.call("save_scene")


# ---------------------------------------------------------------------------- the window

async def test_focus_window_opens_it(ed: Editor):
    await focus_x(ed)
    await rect_of(ed, "spriteNormals.view.Lit", "the Lit view radio")
    await rect_of(ed, "spriteNormals.source", "the Source field")
    await shot(ed, "p4-empty")


async def test_project_window_generate_then_save(ed: Editor):
    await open_assets_folder(ed)
    await click(ed, await rect_of(ed, "project.file.hero.png", "hero.png"), button="3")
    item = await rect_of(ed, "project.menu.generateNormalMap", "Generate Normal Map... in the context menu")
    await shot(ed, "p4-project-menu")
    await click(ed, item)
    await rect_of(ed, "spriteNormals.canvas", "the window's canvas")
    # live: the preview arrives without Save; then a real click on Save
    out = ed.project / "assets" / "hero_normal.png"
    ed.check(not out.exists(), "nothing written before Save")
    await ed.call("wait", frames=60)
    await shot(ed, "p4-window-lit")
    await click(ed, await rect_of(ed, "spriteNormals.primary", "the Save button"))
    await wait_for(ed, out.exists, "Save to write hero_normal.png", 30)
    ed.check(out.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", "a PNG")
    ed.check(b"jasper.spritenormals" in out.read_bytes()[:4096], "with its recipe")
    await ed.call("undo", scope="project")
    await ed.call("wait", frames=6)
    ed.check(not out.exists(), "project undo removed it")


async def test_views_switch_with_keys(ed: Editor):
    await focus_x(ed)
    canvas = await rect_of(ed, "spriteNormals.canvas", "the canvas")
    # hover the window so it keeps the keyboard (a click on the canvas would move the light)
    x, y = center(canvas)
    xdo(ed, "mousemove", str(int(x)), str(int(y)))
    xdo(ed, "key", "3")
    await rect_of(ed, "spriteNormals.viewSelected.Lit", "Lit selected after the key 3")
    xdo(ed, "key", "1")
    await rect_of(ed, "spriteNormals.viewSelected.Color", "Color selected after the key 1")
    xdo(ed, "key", "2")
    await rect_of(ed, "spriteNormals.viewSelected.Normal", "Normal selected after the key 2")
    await ed.call("wait", frames=10)
    await shot(ed, "p4-window-normal")
    xdo(ed, "key", "4")
    await rect_of(ed, "spriteNormals.viewSelected.Split", "Split selected after the key 4")
    await ed.call("wait", frames=10)
    data = await shot(ed, "p4-window-split")
    ed.check(len(set(data[100:4000])) > 16, "the editor screenshot is not flat")
    xdo(ed, "key", "3")
    await rect_of(ed, "spriteNormals.viewSelected.Lit", "Lit again")


async def test_inspector_wand_shift_click_assigns(ed: Editor):
    await ed.call("select_entities", entities=["Hero"])
    await focus_x(ed, "inspector")
    # the Mesh's Material section opened (its arrow), then the Normal Texture row's wand
    if not await ed.ui_rect("inspector.normalMapWand.0", timeout=2.0):
        await click(ed, await rect_of(ed, "inspector.materialToggle.submeshes[0].material", "the Material's arrow", panel="inspector"))
    wand = await rect_of(ed, "inspector.normalMapWand.0", "the Normal Texture row's wand", panel="inspector", seconds=5.0)
    await shot(ed, "p4-inspector-wand")
    await click(ed, wand, shift=True)
    out = ed.project / "assets" / "hero_normal.png"
    await wait_for(ed, out.exists, "the Shift-click to write hero_normal.png", 30)
    for _ in range(40):
        if "hero_normal.png" in await normal_texture(ed, "Hero"):
            break
        await ed.call("wait", frames=5)
    ed.check("hero_normal.png" in await normal_texture(ed, "Hero"), "Hero's Normal Texture is the generated map")
    await ed.call("undo", scope="scene")
    await ed.call("wait", frames=6)
    ed.check("hero_normal.png" not in await normal_texture(ed, "Hero"), "one scene undo step clears it")
    ed.check(out.exists(), "the file stays until the project undo")


async def test_no_wand_on_a_lit_3d_mesh(ed: Editor):
    """A Lit box with a texture: its Normal Texture row has no wand (sprite normal maps are for Sprites, Tilemaps and
    Polygons, as check --normals)"""
    await ed.call("create_entity", type="box", name="Crate", position=[200, 0, 0],
                  values={"mesh": {"submeshes[0].material.baseColorTexture": "assets/hero.png"}})
    await ed.call("select_entities", entities=["Crate"])
    await focus_x(ed, "inspector")
    toggle = await ed.ui_rect("inspector.materialToggle.submeshes[0].material", timeout=5.0)
    if toggle and not await ed.ui_rect("inspector.normalMapWand.0", timeout=1.0):
        await click(ed, toggle)
    await ed.call("wait", frames=10)
    ed.check(toggle is not None, "the box's Material section is in the Inspector")
    ed.check("inspector.normalMapWand.0" not in ed.ui_items(), "no wand on a 3D mesh's Normal Texture row")
    await ed.call("delete_entities", entities=["Crate"])
    await ed.call("select_entities", entities=["Hero"])


async def test_stale_badge_and_regenerate(ed: Editor):
    out = ed.project / "assets" / "hero_normal.png"
    ed.check(out.exists(), "hero_normal.png from the Shift-click")
    await open_assets_folder(ed)
    await rect_of(ed, "project.generated.hero_normal.png", "the generated badge of hero_normal.png")
    ed.check("project.generatedStale.hero_normal.png" not in ed.ui_items(), "fresh at first")
    (ed.project / "assets" / "hero.png").write_bytes(disks(32, 32, tint=0x55))
    await ed.call("sync_files")
    # the watcher re-scans every 5 s of editor frames
    await wait_for(ed, lambda: "project.generatedStale.hero_normal.png" in ed.ui_items(), "the stale badge after hero.png changed", 40)
    await shot(ed, "p4-project-stale")
    # the Inspector's asset view of the stale map
    await click(ed, await rect_of(ed, "project.file.hero_normal.png", "hero_normal.png"))
    # (the Inspector keeps the scroll the wand's row took, test_inspector_wand_shift_click_assigns)
    await rect_of(ed, "snAsset.stale", "the asset view's stale line", panel="inspector", seconds=10.0)
    await shot(ed, "p4-inspector-asset")
    await click(ed, await rect_of(ed, "project.file.hero_normal.png", "hero_normal.png"), button="3")
    await click(ed, await rect_of(ed, "project.menu.regenerateNormalMap", "Regenerate Normal Map in the context menu"))
    for _ in range(80):
        await ed.call("wait", frames=10)
        if "project.generatedStale.hero_normal.png" not in ed.ui_items() and "project.generated.hero_normal.png" in ed.ui_items():
            break
    ed.check("project.generatedStale.hero_normal.png" not in ed.ui_items(), "fresh again after Regenerate")


async def test_playing_disables_the_wand(ed: Editor):
    out = ed.project / "assets" / "hero_normal.png"
    before = out.read_bytes() if out.exists() else b""
    await ed.call("select_entities", entities=["Hero"])
    await ed.call("play", action="start")
    await ed.call("wait", frames=20)
    await focus_x(ed, "inspector")
    wand = await ed.ui_rect("inspector.normalMapWand.0", timeout=5.0)
    if wand:
        await click(ed, wand, shift=True)
        await ed.call("wait", frames=40)
    after = out.read_bytes() if out.exists() else b""
    ed.check(after == before, "nothing written while playing")
    await ed.call("play", action="stop")
    await ed.call("wait", frames=20)


async def test_sprite_frames_button_opens_on_the_sprite(ed: Editor):
    """Inspector > Sprite > Sprite Frames: "Normal Map..." opens the window on Walker (its 4 frames), Assign on."""
    await ed.call("select_entities", entities=["Walker"])
    await focus_x(ed, "inspector")
    await click(ed, await rect_of(ed, "inspector.spriteNormalMap", "the Sprite Frames' Normal Map... button", panel="inspector"))
    await rect_of(ed, "spriteNormals.assign", "the Assign checkbox (an assign target)", panel="spriteNormals.settings")
    await rect_of(ed, "spriteNormals.grid", "the frame grid toggle (the Sprite's frames)")
    await rect_of(ed, "spriteNormals.play", "the Play button (more than one frame)")
    # the Advanced foldout and a region, drawn for real
    await click(ed, await rect_of(ed, "spriteNormals.advanced", "the Advanced foldout"))
    await click(ed, await rect_of(ed, "spriteNormals.addRegion", "+ Region", panel="spriteNormals.settings"))
    await rect_of(ed, "spriteNormals.region.0.pick", "the region's eyedropper", panel="spriteNormals.settings")
    await ed.call("wait", frames=30)
    await shot(ed, "p4-window-advanced")


async def test_batch_from_the_project_window(ed: Editor):
    """Two images selected (Ctrl+click), right-click > "Generate Normal Maps (2)...": the batch list."""
    await open_assets_folder(ed)
    await click(ed, await rect_of(ed, "project.file.hero.png", "hero.png"))
    walk = await rect_of(ed, "project.file.walk.png", "walk.png")
    x, y = center(walk)
    xdo(ed, "mousemove", str(int(x)), str(int(y)))
    await asyncio.sleep(0.2)
    xdo(ed, "keydown", "ctrl")
    xdo(ed, "click", "1")
    xdo(ed, "keyup", "ctrl")
    await ed.call("wait", frames=6)
    await click(ed, walk, button="3")
    await ed.call("wait", frames=6)
    await click(ed, await rect_of(ed, "project.menu.generateNormalMaps", "Generate Normal Maps (2)..."))
    await rect_of(ed, "spriteNormals.batch.hero.png", "hero.png in the batch list", panel="spriteNormals.settings")
    await rect_of(ed, "spriteNormals.batch.walk.png", "walk.png in the batch list", panel="spriteNormals.settings")
    await ed.call("wait", frames=40)
    await shot(ed, "p4-window-batch")


async def test_toon_steps(ed: Editor):
    """Settings > Toon: a real click on the preset selects Toon steps 8 and changes the preview; a click on "4" and
    "Off" moves the selection (the Advanced foldout draws Tilt rings, Flat below, Rotation)"""
    await open_assets_folder(ed)
    hero = await rect_of(ed, "project.file.hero.png", "hero.png")
    await click(ed, hero)   # only hero.png selected (the batch test selected two)
    await click(ed, hero, button="3")
    await click(ed, await rect_of(ed, "project.menu.generateNormalMap", "Generate Normal Map... in the context menu"))
    await rect_of(ed, "spriteNormals.canvas", "the window's canvas")
    await rect_of(ed, "spriteNormals.preset", "the presets", panel="spriteNormals.settings")
    if "spriteNormals.presetsSelected.Default" not in ed.ui_items():
        await click(ed, await rect_of(ed, "spriteNormals.presets.Default", "the Default preset", panel="spriteNormals.settings"))
    await rect_of(ed, "spriteNormals.toonStepsSelected.Off", "Toon steps Off on the Default preset", panel="spriteNormals.settings")
    await ed.call("wait", frames=60)
    before = await shot(ed, "q-window-off")
    await click(ed, await rect_of(ed, "spriteNormals.presets.Toon", "the Toon preset", panel="spriteNormals.settings"))
    await rect_of(ed, "spriteNormals.presetsSelected.Toon", "Toon selected", panel="spriteNormals.settings")
    await rect_of(ed, "spriteNormals.toonStepsSelected.8", "Toon steps 8 with the Toon preset", panel="spriteNormals.settings")
    await ed.call("wait", frames=60)
    after = await shot(ed, "q-window-wide")
    ed.check(before != after, "the preview changed")
    await click(ed, await rect_of(ed, "spriteNormals.toonSteps.4", "Toon steps 4", panel="spriteNormals.settings"))
    await rect_of(ed, "spriteNormals.toonStepsSelected.4", "4 selected", panel="spriteNormals.settings")
    await ed.call("wait", frames=60)
    await shot(ed, "q-window-4")
    # the Advanced values (Directions, Tilt rings, Flat below, Rotation), scrolled into view
    try:
        await rect_of(ed, "spriteNormals.toonTilts", "Tilt rings", panel="spriteNormals.settings", seconds=1.0)
    except harness.TestFailure:   # the Advanced foldout was closed
        await click(ed, await rect_of(ed, "spriteNormals.advanced", "the Advanced foldout", panel="spriteNormals.settings"))
        await rect_of(ed, "spriteNormals.toonTilts", "Tilt rings", panel="spriteNormals.settings")
    await ed.call("wait", frames=10)
    await shot(ed, "q-window-advanced")
    # 6 and 12 (only in the Advanced Directions combo) show as an extra, selected button on the Toon steps row
    for d in ("6", "12"):
        await click(ed, await rect_of(ed, "spriteNormals.toonDirections", "the Directions combo", panel="spriteNormals.settings"))
        await click(ed, await rect_of(ed, f"spriteNormals.toonDirections.{d}", f"{d} in the Directions combo"))
        await rect_of(ed, f"spriteNormals.toonStepsSelected.{d}", f"{d} selected on the Toon steps row", panel="spriteNormals.settings")
        row = [n for n in ed.ui_items() if n.startswith("spriteNormals.toonSteps")]
        ed.check(sorted(row) == sorted(["spriteNormals.toonSteps.Off", "spriteNormals.toonSteps.4", "spriteNormals.toonSteps.8",
                                        "spriteNormals.toonSteps.16", f"spriteNormals.toonStepsSelected.{d}"]), f"five buttons with {d}: {row}")
        await shot(ed, f"q-window-{d}")
    await click(ed, await rect_of(ed, "spriteNormals.toonSteps.4", "Toon steps 4", panel="spriteNormals.settings"))
    await rect_of(ed, "spriteNormals.toonStepsSelected.4", "4 selected again", panel="spriteNormals.settings")
    ed.check("spriteNormals.toonSteps.6" not in ed.ui_items() and "spriteNormals.toonSteps.12" not in ed.ui_items(), "back to four buttons")

    def toon_rows() -> tuple[float, float]:
        items = ed.ui_items()
        off, last = items.get("spriteNormals.toonSteps.Off"), items.get("spriteNormals.toonSteps.16")
        ed.check(off and last, f"the Toon steps buttons are drawn: {off} {last}")
        return off[1], last[1]

    await rect_of(ed, "spriteNormals.toonSteps.16", "Toon steps 16", panel="spriteNormals.settings")
    await rect_of(ed, "spriteNormals.toonSteps.Off", "Toon steps Off", panel="spriteNormals.settings")
    wide_off, wide_last = toon_rows()
    ed.check(abs(wide_off - wide_last) < 1, f"one Toon steps row at the default width: Off y {wide_off}, 16 y {wide_last}")
    # a narrower settings column (its border dragged left): the six presets wrap to rows instead of clipping
    area = await rect_of(ed, "spriteNormals.settings", "the settings panel")
    narrow = area
    for offset in (6, 10, 14, 18, 22, 3):   # the table's column border, just right of the panel's scroll area
        border_x, border_y = int(area[2]) + offset, int(area[1]) + 40
        xdo(ed, "mousemove", str(border_x), str(border_y))
        await ed.call("wait", frames=4)
        xdo(ed, "mousedown", "1")
        for step in range(1, 9):
            xdo(ed, "mousemove", str(border_x - step * 12), str(border_y))
            await ed.call("wait", frames=2)
        xdo(ed, "mouseup", "1")
        await ed.call("wait", frames=10)
        narrow = await rect_of(ed, "spriteNormals.settings", "the settings panel")
        if narrow[2] < area[2] - 30:
            break
    ed.check(narrow[2] < area[2] - 30, f"the column is narrower: {area} -> {narrow}")
    await rect_of(ed, "spriteNormals.presets.Soft", "Soft", panel="spriteNormals.settings")
    soft, chunky = ed.ui_items().get("spriteNormals.presets.Soft"), ed.ui_items().get("spriteNormals.presets.Chunky")
    ed.check(soft and chunky and chunky[1] > soft[3] - 1, f"the presets wrap to a second row: {soft} {chunky}")
    await rect_of(ed, "spriteNormals.toonSteps.16", "Toon steps 16", panel="spriteNormals.settings")
    await rect_of(ed, "spriteNormals.toonSteps.Off", "Toon steps Off", panel="spriteNormals.settings")
    narrow_off, narrow_last = toon_rows()
    ed.check(narrow_last > narrow_off + 1, f"the Toon steps buttons wrap to a second row: Off y {narrow_off}, 16 y {narrow_last}")
    await ed.call("wait", frames=10)
    await shot(ed, "q-window")
    await click(ed, await rect_of(ed, "spriteNormals.toonSteps.Off", "Toon steps Off", panel="spriteNormals.settings"))
    await rect_of(ed, "spriteNormals.toonStepsSelected.Off", "Off selected", panel="spriteNormals.settings")
    await click(ed, await rect_of(ed, "spriteNormals.presets.Default", "the Default preset", panel="spriteNormals.settings"))
