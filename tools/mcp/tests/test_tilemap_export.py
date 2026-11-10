"""A tileset map in exports (docs/specs/S50-grid-tilemap.md R12, R53, AC15): the .tileset names its
image by GUID, so the image can move; the editor still draws the layer after the move, the image
imports as a sprite sheet (no mips), and a source export ships the image and a .tileset that names
it at its new path with no GUID (a game loads by path), with the scene's layer referencing it. With
JASPER_SLOW_BUILD=1 a Desktop game built with the prebuilt glcore player draws the layer (the
player's own screenshot).

Oracles: the exported files read by Python, get_tilemap's CPU image and get_texture_import_settings
in the editor, the player's screenshot. The asset index's .tileset edges (editor/asset/AssetIndex.cpp)
are what the inferred type and the export's texture list come from.
"""

from __future__ import annotations

import asyncio
import json
import os
import time
from pathlib import Path

import build_cache
import pngdiff
from harness import Editor, skip
from htmlui_pixels import count_near

PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "TilemapExport", "env": build_cache.player_env("glcore", "release")}
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "1800"))
CI = True

TEXTURE = "assets/art/tiles.png"
MOVED = "assets/moved/tiles.png"
TILESET = "assets/art/world.tileset"
RED, GREEN = (255, 0, 0), (0, 200, 0)


def write_tiles(ed: Editor) -> None:
    """two 16 x 16 tiles: red, green"""
    data = bytearray()
    for _y in range(16):
        for x in range(32):
            data.extend([*(RED if x < 16 else GREEN), 255])
    path = ed.project / TEXTURE
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pngdiff.encode(32, 16, bytes(data)))


def cooked_at(assets: Path, rel: str) -> bool:
    """the texture ships cooked: an index.json of assets/je-cooked/<family>/ (S14) names it"""
    return any(rel in index.read_text(errors="replace") for index in (assets / "je-cooked").rglob("index.json"))


async def wait_job(ed: Editor, result: dict, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    job = result
    while job["status"] == "running":
        if time.monotonic() > deadline:
            ed.fail(f"job {result['job_id']} still running after {timeout}s: {job}")
        await asyncio.sleep(5)
        job = await ed.call("get_job", job_id=result["job_id"], log_lines=10)
    return job


async def test_setup_and_move_the_image(ed: Editor):
    await ed.call("tool_groups", enable=["tilemaps"])
    write_tiles(ed)
    await ed.call("edit_tileset", path=TILESET, output=True, sources=[{"texture": TEXTURE, "tile_size": [16, 16], "skip_empty": False}])
    made = await ed.call("create_scene", name="Map", type="2d", path="scenes/Map")
    sid = made["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    painted = await ed.call("edit_tilemap", scene=sid, create={"name": "World"}, tileset=TILESET,
                            ops=[{"op": "fill", "tile": 0, "from": [-80, -50], "to": [80, 50]}], return_grid=False)
    layer = painted["tilemap"]["id"]
    await ed.call("save_scene", scene=sid)
    await ed.call("save_project")
    await ed.call("edit_build_scenes", action="set_start", scene=sid)

    await ed.call("create_folder", path=str(Path(MOVED).parent))
    moved = await ed.call("move_asset", path=TEXTURE, to=MOVED)
    ed.check(not (ed.project / TEXTURE).exists() and (ed.project / MOVED).exists(), f"the image moved: {moved}")
    ed.check(TEXTURE in (ed.project / TILESET).read_text(), "the .tileset still has the old path hint")
    image = await ed.call_raw("get_tilemap", {"scene": sid, "entity": layer, "image": True, "region": [0, 0, 1, 0]}, timeout=60)
    ed.check(not image.is_error and image.images, f"the layer still draws after the move: {image.text[:400]}")
    ed.check_eq(image.structured["image"].get("missing_cells", 0), 0, "every cell found its tile image by the image's GUID")
    ed.check(count_near(image.images[0][0], RED, step=1) >= 2 * 16 * 16, "in the tile's colour")

    settings = await ed.call("get_texture_import_settings", path=MOVED)
    ed.check_eq(settings["inferred"]["type"], "sprite", f"a tileset's image imports as a sprite sheet: {settings['inferred']}")
    ed.check_eq(settings["resolved"]["desktop"]["mipmaps"], False, "without mips")


async def test_source_export_ships_the_image_and_a_resolved_tileset(ed: Editor):
    out = ed.scratch.root / "export-tilemap"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"],
                      overwrite=True, timeout_seconds=900)
    ed.check_eq(r["status"], "succeeded", f"{str(r)[:2000]}")
    project = out / "project"
    assets = project / "assets"
    # paths below are relative to the exported assets root, as the editor's references hold them
    shipped = assets / TILESET
    ed.check(shipped.is_file(), f"the .tileset ships: {sorted(p.relative_to(assets).as_posix() for p in assets.rglob('*.tileset'))}")
    source = json.loads(shipped.read_text())["sources"][0]["texture"]
    ed.check_eq(source, {"path": MOVED}, "the image at its current path, no GUID")
    ed.check((assets / MOVED).is_file() or cooked_at(assets, MOVED), "the image ships")
    ed.check(not (assets / TEXTURE).exists() and not cooked_at(assets, TEXTURE), "nothing at the old path")
    referrers = [p for p in project.rglob("*") if p.is_file() and p != shipped and p.suffix not in (".png", ".ktx2")
                 and b"world.tileset" in p.read_bytes()]
    ed.check(referrers, "the exported scene's layer references the tileset")


async def test_desktop_player_draws_the_layer(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop game (the run builds its prebuilt runtime once)")
    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-tilemap-desktop"
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    job = await wait_job(ed, r, TIMEOUT)
    ed.check_eq(job["status"], "succeeded", f"{str(job)[:2000]}")
    ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
    raw = await ed.call_raw("run_build", {"after_ready": True, "fixed_fps": 60, "quit_after": 60})
    ed.check(not raw.is_error, raw.text)
    ed.check(not raw.value["crashed"], f"the player crashed: {raw.value}")
    ed.check(raw.images, f"the player's own screenshot: {raw.text[:2000]}")
    png = raw.images[0][0]
    evidence = os.environ.get("JASPER_TILEMAP_EVIDENCE")
    if evidence:
        Path(evidence).mkdir(parents=True, exist_ok=True)
        (Path(evidence) / "desktop-tilemap.png").write_bytes(png)
    ed.check(count_near(png, RED) > 500, f"the player draws the layer's tiles: {count_near(png, RED)} red samples")
    ed.check_eq(raw.value.get("exit_code"), 0, f"the player got ready and quit after its frames: {raw.value}")
