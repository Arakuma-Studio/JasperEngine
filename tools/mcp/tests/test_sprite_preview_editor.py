"""The Aseprite Importer inspector and the Sprite Preview in the real editor (docs/specs/S44-aseprite-import.md
R30, R31; AC16), driven like a human with xdotool on the harness' Xvfb, on a new project holding asefile's
layers_and_tags (16x16, 4 frames, 6 layers, tags T1 0-1, T3, T2) as assets/sprites/hero.aseprite and asefile's
basic-16x16 as assets/sprites/tiles.aseprite, set to 16 pixels per unit (hero keeps the project's, 1):

- a click on hero.aseprite in the Project window shows the Aseprite Importer in the Inspector: the summary line
  (the import's "3 tags · 4 frames · ..."), the sections General, Layers, Animation, Slices and Texture, and the
  buttons;
- Window > 2D > Sprite Preview (run_command window.spritePreview) opens on the Project window's selection: the
  canvas draws the frame, the frame strip has a cell per frame and a band per tag, Source / Imported both draw;
- a click on a frame cell, then the `.` and Home keys, step the frames; Onion Skin draws the previous and next
  frames (none before frame 0);
- the tag dropdown lists the tags and picking one selects its band and its first frame;
- Compare at 1x with tiles as the reference draws it beside the frame and warns of the mixed pixel sizes; the
  warning goes when tiles takes the project's pixels per unit and comes back with the project undo; the choice
  is remembered in .jasperengine/user/sprite-preview.yaml;
- Exclude from Import with one layer unticked writes layers.exclude into hero's meta in one project undo step
  (one `undo {scope: "project"}` restores the meta and leaves tiles' earlier step alone).

Widgets are found through the UI probe (Editor(ui_probe=True), editor/util/UIProbe.h): the inspector's are
"asepriteImporter.*" (editor/window/AsepriteImporterView.cpp), the preview's "spritePreview.*"
(editor/window/SpritePreviewWindow.cpp), Project window files "project.file.<name>". The Inspector and the
preview's side panel are scrolled with real wheel notches, waited for through the probe's scroll. Screenshots of
the editor go to $JASPER_SHOTS_DIR when set (else the scratch directory's shots/).
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
import time
from pathlib import Path

import pngdiff
from harness import REPO, Editor

EDITOR = {"create": True, "name": "Sprite Preview", "xvfb": "harness", "ui_probe": True}
CI = True
TIMEOUT = 900

FIXTURES = REPO / "editor" / "tests" / "fixtures" / "aseprite" / "asefile"
HERO = "assets/sprites/hero.aseprite"
TILES = "assets/sprites/tiles.aseprite"
SECTIONS = ("General", "Layers", "Animation", "Slices", "Texture")
S: dict = {}


def center(rect):
    return (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0


class Ui:
    def __init__(self, ed: Editor):
        self.ed = ed
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    async def frames(self, n: int = 3):
        await self.ed.call("wait", frames=n)

    async def click(self, rect, double: bool = False):
        x, y = center(rect)
        self.xdo("mousemove", str(int(x)), str(int(y)))
        await asyncio.sleep(0.3)
        self.xdo("click", "--repeat", "2" if double else "1", "--delay", "80", "1")
        await asyncio.sleep(0.4)
        await self.frames(4)

    async def key(self, name: str):
        # the keys go to the editor's X window (the Sprite Preview has them once a click focused it)
        ids = self.xdo("search", "--onlyvisible", "--name", "JasperEngine").split()
        if ids:
            self.xdo("windowfocus", "--sync", ids[-1])
        self.xdo("key", name)
        await asyncio.sleep(0.3)
        await self.frames(4)

    async def wheel(self, at, down: bool):
        self.xdo("mousemove", str(int(at[0])), str(int(at[1])))
        await asyncio.sleep(0.1)
        self.xdo("click", "5" if down else "4")
        await asyncio.sleep(0.2)

    def items(self) -> dict:
        return self.ed.ui_items()

    async def item(self, name: str, what: str, seconds: float = 30.0) -> list[float]:
        """`name`'s rectangle once drawn and still for two polls (the layout settled)"""
        deadline = time.monotonic() + seconds
        last = None
        while time.monotonic() < deadline:
            await self.frames(2)
            rect = self.items().get(name)
            if rect and rect == last:
                return rect
            last = rect
        self.ed.fail(f"{what} ({name}) is never drawn: {sorted(self.items())}")

    async def gone(self, name: str, what: str, seconds: float = 30.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            await self.frames(2)
            if name not in self.items():
                return
        self.ed.fail(f"{what}: {name} is still drawn")

    def find(self, prefix: str, suffix: str) -> str | None:
        for name in self.items():
            if name.startswith(prefix) and name.endswith(suffix):
                return name
        return None

    async def reveal(self, name: str, panel: str, scroll: str, what: str) -> list[float]:
        """Wheels the panel (probe `panel`, its scroll `scroll`) a notch at a time until `name` is drawn whole:
        up to the top, down to the end, then up again (a notch that arrives late can carry it past)"""
        if name in self.items():
            return await self.item(name, what)
        area = await self.item(panel, f"the panel holding {what}")
        at = center(area)
        for down in (False, True, False):
            for _ in range(150):
                await self.frames(3)
                probe = self.ed.ui_probe_state()
                if name in probe.get("items", {}):
                    return await self.item(name, what)
                y, max_y = probe.get("scroll", {}).get(scroll, [0.0, 0.0])
                if (y <= 0.0 and not down) or (y >= max_y and down):
                    break
                await self.wheel(at, down)
        self.ed.fail(f"{what} ({name}) is never drawn in {panel}: {sorted(self.items())}")

    async def project_file(self, name: str, folders=("assets", "sprites")) -> list[float]:
        """The Project window's entry `name` of assets/sprites (opening the folders by double-click)"""
        await self.ed.call("focus_window", window="project")
        for _ in range(12):
            await self.frames(4)
            items = self.items()
            rect = items.get("project.file." + name)
            if rect:
                return rect
            for folder in reversed(folders):
                if "project.file." + folder in items:
                    await self.click(items["project.file." + folder], double=True)
                    break
        self.ed.fail(f"the Project window never lists {name}: {sorted(self.items())}")

    async def inspector(self, name: str, what: str) -> list[float]:
        return await self.reveal(name, "inspector", "inspector", what)

    async def panel(self, name: str, what: str) -> list[float]:
        return await self.reveal(name, "spritePreview.panel", "spritePreview.panel", what)


async def shot(ed: Editor, name: str):
    shots = Path(os.environ["JASPER_SHOTS_DIR"]) if os.environ.get("JASPER_SHOTS_DIR") else ed.scratch.root / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    await ed.call("wait", frames=4)
    await ed.screenshot("editor", path=str(shots / f"{name}.png"), max_width=1600)


async def asset_info(ed: Editor, path: str) -> dict:
    info = await ed.call("get_asset_info", path=path)
    return info.get("aseprite", info)


async def imported(ed: Editor, path: str, until=None, what: str = "") -> dict:
    """get_asset_info's aseprite part once the import is done (and until(info) holds)"""
    info = None
    for _ in range(300):
        info = await asset_info(ed, path)
        if info.get("import_state") in ("imported", "failed") and (until is None or until(info)):
            ed.check(info["import_state"] == "imported", f"{path} imports: {info}")
            return info
        await ed.call("wait", seconds=0.1)
    ed.fail(f"{path} never imported{' with ' + what if what else ''}: {info}")


def resolved(info: dict) -> dict:
    return info["import_settings"]["resolved"]


async def test_setup(ed: Editor):
    if not shutil.which("xdotool"):
        ed.fail("xdotool is needed for the real-input UI tests (apt install xdotool)")
    ed.require_tools("sync_files", "get_asset_info", "tool_groups", "run_command", "focus_window", "undo", "screenshot")
    (ed.project / "assets" / "sprites").mkdir(parents=True, exist_ok=True)
    shutil.copy(FIXTURES / "layers_and_tags.aseprite", ed.project / HERO)
    shutil.copy(FIXTURES / "basic-16x16.aseprite", ed.project / TILES)
    await ed.call("sync_files", paths=[HERO, TILES])
    S["hero"] = await imported(ed, HERO)
    await imported(ed, TILES)
    ed.check(S["hero"]["sprite"]["frame_count"] == 4, f"hero has 4 frames: {S['hero']['sprite'].get('frame_count')}")

    await ed.call("tool_groups", enable=["sprites"])
    r = await ed.call("set_sprite_import_settings", paths=[TILES], settings={"pixels_per_unit": 16}, output=True)
    ed.check(r["written"] is True, f"tiles' pixels per unit written: {r}")
    await imported(ed, TILES, until=lambda i: resolved(i)["pixels_per_unit"] == 16, what="16 pixels per unit")
    ed.check(resolved(S["hero"])["pixels_per_unit"] == "project", f"hero keeps the project's: {resolved(S['hero'])}")


async def test_importer_inspector(ed: Editor):
    ui = Ui(ed)
    summary = S["hero"]["import"]["summary"]
    ed.check("3 tags" in summary and "4 frames" in summary, f"the import's summary line: {summary}")
    rect = await ui.project_file("hero.aseprite")
    name = None
    for _ in range(4):
        await ui.click(rect)
        await ui.frames(5)
        name = ui.find("asepriteImporter.summary.", "hero.aseprite")
        if name:
            break
        rect = await ui.project_file("hero.aseprite")
    ed.check(name, f"the Aseprite Importer shows hero's summary line in the Inspector: {sorted(ui.items())}")
    await shot(ed, "01-importer-inspector-top")
    for section in SECTIONS:
        await ui.inspector("asepriteImporter.section." + section, f"the {section} section header")
    for button in ("apply", "revert", "openInAseprite", "openSpritePreview", "reimport", "extractClips"):
        await ui.inspector("asepriteImporter." + button, f"the {button} button")
    await shot(ed, "02-importer-inspector-bottom")


async def test_sprite_preview_from_the_window_menu(ed: Editor):
    ui = Ui(ed)
    r = await ed.call("run_command", id="window.spritePreview")
    ed.check(r.get("id") == "window.spritePreview", f"Window > 2D > Sprite Preview ran: {r}")
    await ui.item("spritePreview.canvas", "the preview's canvas")
    await ui.item("spritePreview.frameImage", "hero's frame on the canvas (the preview follows the Project selection)")
    await ui.panel("spritePreview.summary", "the import's summary in the side panel")
    await ui.item("spritePreview.frameStrip", "the frame strip")
    await ui.item("spritePreview.frameCurrent.0", "frame 0's cell, current")
    for f in (1, 2, 3):
        await ui.item(f"spritePreview.frame.{f}", f"frame {f}'s cell")
    for tag in S["hero"]["sprite"]["tags"]:
        await ui.item("spritePreview.tagBand." + tag["name"], f"tag {tag['name']}'s band")
    for name in ("tagDropdown", "play", "previousFrame", "nextFrame", "onionSkin", "slices", "pivot", "trim", "pixelGrid",
                 "background", "fit", "view.Source", "view.Imported"):
        await ui.item("spritePreview." + name, name)
    # Imported draws the import's sheet, Source the file
    await ui.click(ui.items()["spritePreview.view.Imported"])
    await ui.item("spritePreview.frameImage", "the imported frame")
    await ui.click(ui.items()["spritePreview.view.Source"])
    rect = await ui.item("spritePreview.frameImage", "the source frame")
    await shot(ed, "03-sprite-preview")
    # the sprite's pixels are squares at the integer zoom: a handful of colours (the sprite's, the
    # checkerboard's and the pixel grid's), where a linear sampler would blur them into gradients
    w, h, px = pngdiff.decode(await ed.screenshot("editor", max_width=0))
    x0, y0, x1, y1 = (int(v) for v in rect)
    colours = {bytes(px[(y * w + x) * 4:(y * w + x) * 4 + 3]) for y in range(max(y0 + 2, 0), min(y1 - 2, h))
               for x in range(max(x0 + 2, 0), min(x1 - 2, w))}
    ed.check(0 < len(colours) <= 64, f"the frame drawn with Nearest sampling: {len(colours)} colours in {rect}")


async def test_frame_stepping_and_onion_skin(ed: Editor):
    ui = Ui(ed)
    await ui.click(await ui.item("spritePreview.frame.1", "frame 1's cell"))
    await ui.item("spritePreview.frameCurrent.1", "a click on frame 1 shows it")
    await ui.key("period")
    await ui.item("spritePreview.frameCurrent.2", "`.` steps to frame 2")
    ed.check("spritePreview.onionDrawn.previous.1" not in ui.items(), "no onion skin while it is off")

    await ui.click(await ui.item("spritePreview.onionSkin", "the Onion Skin checkbox"))
    for name in ("onionPrevious", "onionNext", "onionOpacity"):
        await ui.item("spritePreview." + name, f"Onion Skin's {name}")
    await ui.item("spritePreview.onionDrawn.previous.1", "frame 1 drawn as the previous onion skin")
    await ui.item("spritePreview.onionDrawn.next.1", "frame 3 drawn as the next onion skin")
    await shot(ed, "04-onion-skin")

    await ui.key("Home")
    await ui.item("spritePreview.frameCurrent.0", "Home goes to the first frame")
    await ui.gone("spritePreview.onionDrawn.previous.1", "frame 0 has no previous frame to skin")
    await ui.item("spritePreview.onionDrawn.next.1", "frame 1 is still the next onion skin")
    await ui.click(ui.items()["spritePreview.onionSkin"])
    await ui.gone("spritePreview.onionPrevious", "Onion Skin off hides its settings")


async def test_tag_dropdown(ed: Editor):
    ui = Ui(ed)
    tags = {t["name"]: t for t in S["hero"]["sprite"]["tags"]}
    ed.check(set(tags) == {"T1", "T2", "T3"}, f"hero's tags: {list(tags)}")
    await ui.click(await ui.item("spritePreview.tagDropdown", "the tag dropdown"))
    await ui.item("spritePreview.tagAll", "All Frames in the dropdown")
    for name in tags:
        await ui.item("spritePreview.tagItem." + name, f"{name} in the dropdown")
    await ui.click(ui.items()["spritePreview.tagItem.T2"])
    await ui.item("spritePreview.tagBandSelected.T2", "T2's band selected")
    first = min(tags["T2"]["from"], tags["T2"]["to"])
    await ui.item(f"spritePreview.frameCurrent.{first}", "T2's first frame shown")
    await shot(ed, "05-tag-T2")
    await ui.click(await ui.item("spritePreview.tagDropdown", "the tag dropdown"))
    await ui.click(await ui.item("spritePreview.tagAll", "All Frames in the dropdown"))
    await ui.gone("spritePreview.tagBandSelected.T2", "All Frames selects no band")


async def test_compare_at_1x_warns_on_mixed_pixel_sizes(ed: Editor):
    ui = Ui(ed)
    await ui.click(await ui.panel("spritePreview.compare", "the Compare checkbox"))
    await ui.click(await ui.panel("spritePreview.compareSheet", "the reference sheet dropdown"))
    await ui.frames(5)
    candidate = ui.find("spritePreview.compareCandidate.", "tiles.aseprite")
    ed.check(candidate, f"tiles is a reference candidate: {sorted(ui.items())}")
    ed.check(ui.find("spritePreview.compareCandidate.", "hero.aseprite"), "every .aseprite file is a candidate")
    await ui.click(ui.items()[candidate])
    await ui.panel("spritePreview.compareFrame", "the reference frame dropdown")
    # hero 1 PPU (the project's), tiles 16 PPU
    await ui.panel("spritePreview.compareWarning", "the mixed pixel sizes warning")
    await ui.click(await ui.item("spritePreview.fit", "Fit"))
    await ui.item("spritePreview.compareImage", "the reference drawn beside the frame")
    await ui.item("spritePreview.frameImage", "the frame next to it")
    choice = ed.project / ".jasperengine" / "user" / "sprite-preview.yaml"
    ed.check(choice.exists() and "tiles.aseprite" in choice.read_text(), f"the reference is remembered for the project: {choice}")
    await shot(ed, "06-compare-at-1x")

    # the warning follows the sheets' pixels per unit, through the reimport
    await ed.call("set_sprite_import_settings", paths=[TILES], settings={"pixels_per_unit": "project"}, output=True)
    await imported(ed, TILES, until=lambda i: resolved(i)["pixels_per_unit"] == "project", what="the project's pixels per unit")
    await ui.gone("spritePreview.compareWarning", "one pixel size for both: no warning")
    await ed.call("undo", scope="project")
    await imported(ed, TILES, until=lambda i: resolved(i)["pixels_per_unit"] == 16, what="16 pixels per unit again")
    await ui.panel("spritePreview.compareWarning", "the warning back after the undo")


async def test_exclude_from_import_is_one_undo_step(ed: Editor):
    ui = Ui(ed)
    layers = S["hero"]["sprite"]["layers"]
    by_index = {layer["index"]: layer for layer in layers}

    def shown(layer) -> bool:
        while layer:
            if not layer["visible"]:
                return False
            layer = by_index.get(layer["parent"])
        return True

    candidates = [layer for layer in layers if layer["type"] == "image" and not layer.get("reference") and shown(layer)]
    ed.check(candidates, f"hero has a visible image layer: {layers}")
    path = candidates[0]["path"]
    meta = ed.project / (HERO + ".meta")
    before = meta.read_bytes() if meta.exists() else None
    ed.check(path not in resolved(await asset_info(ed, HERO))["layers"]["exclude"], "the layer is imported at first")

    await ui.click(await ui.panel("spritePreview.layerOn." + path, f"layer {path}'s toggle, ticked"))
    await ui.gone("spritePreview.layerOn." + path, f"layer {path} unticked")
    await ui.item("spritePreview.layer." + path, f"layer {path}'s toggle, unticked")
    await ui.click(await ui.panel("spritePreview.excludeFromImport", "Exclude from Import"))
    await ui.panel("spritePreview.status", "Exclude from Import says what it did")
    info = await imported(ed, HERO, until=lambda i: path in resolved(i)["layers"]["exclude"], what=f"{path} excluded")
    ed.check(meta.exists() and path in meta.read_text(), f"the meta names the excluded layer: {meta}")
    ed.check(resolved(info)["layers"]["exclude"] == [path], f"only that layer is excluded: {resolved(info)['layers']}")
    await shot(ed, "07-exclude-from-import")

    await ed.call("undo", scope="project")
    info = await imported(ed, HERO, until=lambda i: path not in resolved(i)["layers"]["exclude"], what="the exclude undone")
    now = meta.read_bytes() if meta.exists() else None
    if before is not None:
        ed.check(now == before, "one project undo restores hero's meta byte for byte")
    else:
        ed.check(now is None or path.encode() not in now, f"one project undo takes the exclude out: {now!r}")
    # the step before it (tiles' pixels per unit) is still there: Exclude from Import was one step
    ed.check(resolved(await asset_info(ed, TILES))["pixels_per_unit"] == 16, "tiles' earlier step is not undone")
