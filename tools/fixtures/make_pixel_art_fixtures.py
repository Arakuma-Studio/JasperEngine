#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Writes tools/fixtures/pixel-art and tools/fixtures/pixel-art-bad (docs/specs/S42-pixel-art-mode.md
§7): editor-made projects (driven over MCP, tools/mcp/harness.py) the pixel-art validator is tested
on. pixel-art is clean: Pixel Art Mode, a 320x180 base at whole scales, a Pixel Perfect Camera and
point-filtered lossless sprites on whole pixels. pixel-art-bad seeds one failure per rule:

    filtering            Bilinear: a bilinear reference
    mipmaps_compression  Compressed: a BC7, mipmapped sprite; Swap: Palette Swap on a BC7 texture
    texel_size           Scaled (1.5x) and PPU32 (a PPU-32 sprite in a PPU-16 project)
    off_grid             OffGrid at x = 10.3 units (164.8 base pixels)
    rotation             Rotated by 37 degrees
    font_antialiasing    Label: a vector font
    particles_grid       Sparks: stretched billboards
    anti_aliasing        FXAA on (Main)
    integer_scale        scenes/Fixed: fixed resolution 640x360 under letterbox (fit) scaling
    palette              OffPalette: colours a remap fixes, against the Default Palette
    sprite_variants      scenes/Variants: 30 library:sprite feature sets
    pixel_art_mode       letterbox scaling (not a whole-number layout)

The art is made here (generate_placeholder_art) in the palette's colours; it is CC0.

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/fixtures/make_pixel_art_fixtures.py

Needs a built editor (build/linux/editor, or JASPER_EDITOR) and runs it headless.
"""

from __future__ import annotations

import asyncio
import itertools
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "mcp"))

from harness import Editor  # noqa: E402

EXCLUDED = {".jasperengine", "build", "Recordings", ".trash", ".git", ".clangd", ".vscode", ".luarc.json", "AGENTS.md",
            "CLAUDE.md", "CMakeLists.txt", ".gitignore", ".gitattributes"}
M = "submeshes[0].material."
PALETTE = ["#000000", "#ffffff", "#ff0000", "#3d8fb0"]
NEAREST = {"min_filter": "nearest", "mag_filter": "nearest"}
FEATURES = ["OUTLINE", "HUE_SHIFT", "FLASH", "GRAYSCALE", "COLOR_REPLACE"]


def copy_project(src: Path, dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    for entry in sorted(src.iterdir()):
        if entry.name in EXCLUDED:
            continue
        if entry.is_dir():
            shutil.copytree(entry, dst / entry.name)
        else:
            shutil.copy2(entry, dst / entry.name)


async def scene(ed: Editor, name: str, first: bool = False) -> int:
    """a 2D scene saved as scenes/<name>.scene; the first replaces the new project's untitled scene"""
    if first:
        made = await ed.call("create_scene", name=name, type="2D", path=f"scenes/{name}", close_previous=True, unsaved="discard")
    else:
        made = await ed.call("create_scene", name=name, type="2D", path=f"scenes/{name}")
    return made["scene"]["id"]


async def art(ed: Editor, path: str, color: str, outline: str = "#000000", size: int = 16) -> str:
    made = await ed.call("generate_placeholder_art", path=path, width=size, height=size, shape="rect", color=color,
                         outline_color=outline, outline_width=1)
    return made["path"]


async def sprite(ed: Editor, sid: int, name: str, texture: str, position, values: dict | None = None) -> int:
    merged = {"sprite": {"width": 16, "height": 16}, "mesh": {M + "baseColorTexture": dict(path=texture, **NEAREST)}}
    for component, props in (values or {}).items():
        merged.setdefault(component, {}).update(props)
    made = await ed.call("create_entity", scene=sid, type="sprite", name=name, position=list(position), values=merged)
    return made["entity_id"]


async def wait_imports(ed: Editor, paths: list[str]) -> None:
    """the import settings the first cook writes (Pixel Art Mode's pixelArt: true) exist before the copy"""
    for _ in range(200):
        if all((ed.project / f"{p}.meta").exists() and "textureImporter" in (ed.project / f"{p}.meta").read_text() for p in paths):
            return
        await ed.call("wait", frames=10)
    raise SystemExit(f"the textures were not imported: {paths}")


async def build_clean(ed: Editor) -> None:
    await ed.call("set_project_settings", values={"pixel_art.enabled": True, "graphics.canvas_width": 320, "graphics.canvas_height": 180,
                                                   "graphics.scaling_mode": "integer", "graphics.pixels_per_unit": 1,
                                                   "graphics.default_filter_mode": "point", "player.window_width": 1280,
                                                   "player.window_height": 720})
    sid = await scene(ed, "Main", first=True)
    await ed.call("edit_build_scenes", action="set_start", scene=sid)
    hero = await art(ed, "sprites/hero.png", "#ff0000")
    tile = await art(ed, "sprites/tile.png", "#3d8fb0", "#ffffff")
    await sprite(ed, sid, "Hero", hero, [152, 82, 0])
    for i in range(4):
        await sprite(ed, sid, f"Tile{i}", tile, [i * 16, 0, 0])
    await wait_imports(ed, [hero, tile])
    await ed.call("save_scene", scene=sid)
    checked = await ed.call("check_pixel_art")
    if checked.get("verdict") != "clean":
        raise SystemExit(f"pixel-art is not clean: {checked}")


async def build_bad(ed: Editor) -> None:
    await ed.call("set_project_settings", values={"pixel_art.enabled": True, "graphics.canvas_width": 320, "graphics.canvas_height": 180,
                                                   "graphics.scaling_mode": "letterbox", "graphics.pixels_per_unit": 16,
                                                   "graphics.default_filter_mode": "point", "player.window_width": 1280,
                                                   "player.window_height": 720})
    made = await ed.call("edit_palette", {"path": "palettes/game.palette", "from": {"colors": PALETTE}, "output": True})
    await ed.call("set_project_settings", values={"palettes.default": made.get("path", "palettes/game.palette")})
    main = await scene(ed, "Main", first=True)
    await ed.call("edit_build_scenes", action="set_start", scene=main)
    hero = await art(ed, "sprites/hero.png", "#ff0000")
    lossy = await art(ed, "sprites/lossy.png", "#3d8fb0", size=64)
    swap = await art(ed, "sprites/swap.png", "#ffffff", size=64)
    offpalette = await art(ed, "sprites/offpalette.png", "#fe0202", "#010101")
    # BC7 with mipmaps: an explicit default type and quality win over Pixel Art
    for path in (lossy, swap):
        await ed.call("set_texture_import_settings", paths=[path],
                      settings={"pixel_art": False, "type": "default", "quality": "high", "mipmaps": True})

    await sprite(ed, main, "Hero", hero, [2, 2, 0])
    await sprite(ed, main, "Bilinear", hero, [4, 2, 0],
                 {"mesh": {M + "baseColorTexture": {"path": hero, "min_filter": "linear", "mag_filter": "linear"}}})
    await sprite(ed, main, "Compressed", lossy, [6, 2, 0], {"sprite": {"width": 64, "height": 64}})
    await sprite(ed, main, "Scaled", hero, [10, 4, 0])
    await ed.call("set_transform", scene=main, entity="Scaled", scale=[1.5, 1.5, 1])
    await sprite(ed, main, "PPU32", hero, [12, 4, 0], {"sprite": {"pixelsPerUnit": 32}})
    await sprite(ed, main, "OffGrid", hero, [10.3, 6, 0])
    await sprite(ed, main, "Rotated", hero, [14, 6, 0])
    await ed.call("set_transform", scene=main, entity="Rotated", rotation={"euler": [0, 0, 37]})
    await sprite(ed, main, "OffPalette", offpalette, [16, 6, 0])
    swapped = await sprite(ed, main, "Swap", swap, [16, 8, 0], {"sprite": {"width": 64, "height": 64}})
    await ed.call("set_properties", scene=main, entity=swapped, values={"mesh": {M + "shader": "library:sprite"}})
    await ed.call("set_properties", scene=main, entity=swapped, values={"mesh": {
        M + "shaderFeatures": ["PALETTE_SWAP"], M + "shaderPalettes": {"u_palette": {"source": "palettes/game.palette"}}}})
    await ed.call("create_entity", scene=main, type="text", name="Label", position=[2, 8, 0],
                  values={"text": {"text": "HELLO", "fontSize": 16}})
    await ed.call("create_particles", scene=main, name="Sparks", preset="sparks", position=[8, 8, 0],
                  modules={"renderer": {"renderMode": "stretched_billboard"}})
    await ed.call("set_scene_settings", scene=main, settings={"post_processing.fxaa_enabled": True})
    # the textures the scene draws cook, and the first cook writes the others' import settings
    await wait_imports(ed, [hero, lossy, swap, offpalette])
    await ed.call("save_scene", scene=main)

    # the fixed resolution scene: its camera without a Pixel Perfect Camera
    fixed = await scene(ed, "Fixed")
    await ed.call("remove_component", scene=fixed, entity="Main Camera", component="pixelperfectcamera")
    await ed.call("set_scene_settings", scene=fixed, settings={"fixed_resolution_enabled": True, "fixed_resolution_width": 640,
                                                               "fixed_resolution_height": 360})
    await ed.call("save_scene", scene=fixed)

    # 30 library:sprite feature sets (sprite_variants), clean sprites otherwise
    variants = await scene(ed, "Variants")
    sets = [combo for n in range(1, len(FEATURES) + 1) for combo in itertools.combinations(FEATURES, n)][:30]
    for i, features in enumerate(sets):
        entity = await sprite(ed, variants, f"Variant{i}", hero, [(i % 10) * 2, (i // 10) * 2, 0])
        await ed.call("set_properties", scene=variants, entity=entity, values={"mesh": {M + "shader": "library:sprite"}})
        await ed.call("set_properties", scene=variants, entity=entity, values={"mesh": {M + "shaderFeatures": list(features)}})
    await ed.call("save_scene", scene=variants)
    await ed.call("select_scene", scene=main)


async def make(name: str, builder) -> None:
    async with Editor(create=True, name=name, label=f"fixture-{name}") as ed:
        await ed.call("tool_groups", enable=["pixel_art", "particles", "assets_pipeline"])
        await builder(ed)
        await ed.call("save_project")
        target = REPO / "tools" / "fixtures" / name
        copy_project(ed.project, target)
        files = sorted(str(p.relative_to(target)) for p in target.rglob("*") if p.is_file())
        print(f"[fixtures] {name}: {len(files)} files")


async def main() -> int:
    wanted = sys.argv[1:] or ["pixel-art", "pixel-art-bad"]
    if "pixel-art" in wanted:
        await make("pixel-art", build_clean)
    if "pixel-art-bad" in wanted:
        await make("pixel-art-bad", build_bad)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
