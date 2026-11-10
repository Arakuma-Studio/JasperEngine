"""S90 R16 in the Project window (review I4): a baked PNG's badge turns stale when its graph changes, while the
folder stays open, and the sprite normal maps' own refresh does not wipe it.

Real input (xdotool in the harness's Xvfb) opens the folder; the badges are read through the UI probe
(`project.generated.<name>`, `project.generatedStale.<name>`), as test_sprite_normals_window.py reads its own.
"""

from __future__ import annotations

import asyncio
import json
import struct
import zlib

from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Procedural Material Badge CI", "xvfb": "harness", "ui_probe": True}
TIMEOUT = 900

GRAPH = {
    "formatVersion": 1, "kind": "procedural_material", "kindVersion": 1, "nextId": 4,
    "nodes": [{"id": "n1", "type": "perlin", "position": [0, 0]}, {"id": "out", "type": "material_output", "position": [300, 0]}],
    "edges": [{"id": "e2", "from": {"node": "n1", "port": "output"}, "to": {"node": "out", "port": "base_color"}},
              {"id": "e3", "from": {"node": "n1", "port": "output"}, "to": {"node": "out", "port": "height"}}],
}
BADGE = "project.generated.stone_base_color.png"
STALE = "project.generatedStale.stone_base_color.png"


def sprite(tint: int) -> bytes:
    """a 16 x 16 RGBA disk, its shade set by tint"""
    rows = b""
    for y in range(16):
        rows += b"\x00"
        for x in range(16):
            inside = (x - 7.5) ** 2 + (y - 7.5) ** 2 < 49
            rows += bytes([tint, 120, 200 - x * 4, 255 if inside else 0])

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows))
            + chunk(b"IEND", b""))


async def wait_for(ed: Editor, predicate, what: str, seconds: float = 60.0) -> None:
    for _ in range(int(seconds * 4)):
        if predicate():
            return
        await ed.call("wait", frames=4)
        await asyncio.sleep(0.25)
    shown = sorted(k for k in ed.ui_items() if k.startswith("project."))
    files = sorted(p.name for p in (ed.project / "assets/mats/stone").glob("*")) if (ed.project / "assets/mats/stone").is_dir() else []
    ed.fail(f"timed out waiting for {what}; the Project window shows {shown}; the folder holds {files}")


async def open_folder(ed: Editor, ui: Ui, name: str, inside: str) -> None:
    """double-click folder `name` until its item `inside` shows: a double click a loaded frame splits is two clicks"""
    for _ in range(6):
        if await ed.ui_rect(f"project.file.{inside}", timeout=1.0):
            return
        rect = await ed.ui_rect(f"project.file.{name}", timeout=20.0)
        ed.check(rect is not None, f"{name} in the Project window: {sorted(ed.ui_items())[:30]}")
        # (probe rects are window pixels of the harness's Xvfb, whose window sits at its origin: as
        # test_sprite_normals_window.py opens its folder)
        ui.xdo("mousemove", str(int((rect[0] + rect[2]) / 2)), str(int((rect[1] + rect[3]) / 2)))
        await ed.call("wait", frames=4)
        ui.xdo("click", "--repeat", "2", "--delay", "80", "1")
        await ed.call("wait", frames=10)
    ed.check(await ed.ui_rect(f"project.file.{inside}", timeout=5.0) is not None, f"{name} opened: {sorted(ed.ui_items())[:30]}")


async def test_the_badge_follows_the_graph_and_survives_the_sprite_normals_refresh(ed: Editor):
    target = ed.project / "assets/mats/stone.procmat"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(GRAPH, indent=2) + "\n")
    await ed.call("sync_files")
    await ed.call("bake_procedural_material", path="assets/mats/stone.procmat", size=64)
    # a sprite and its normal map in the same folder: their staleness drives the sprite-normals refresh
    (ed.project / "assets/mats/stone/rock.png").write_bytes(sprite(40))
    await ed.call("sync_files")
    await ed.call("generate_sprite_normals", source="assets/mats/stone/rock.png", output=True)

    ui = Ui(ed)
    await ed.call("focus_window", window="project")
    ui.focus()
    for folder, inside in (("assets", "mats"), ("mats", "stone"), ("stone", "stone_base_color.png")):
        await open_folder(ed, ui, folder, inside)
    await wait_for(ed, lambda: BADGE in ed.ui_items(), "the bake's badge")
    ed.check(STALE not in ed.ui_items(), "fresh after the bake")

    # the sprite changes: its normal map turns stale, and the bake's badge must stay
    (ed.project / "assets/mats/stone/rock.png").write_bytes(sprite(200))
    await ed.call("sync_files")
    await wait_for(ed, lambda: "project.generatedStale.rock_normal.png" in ed.ui_items(), "the sprite normal map's stale badge", 60)
    ed.check(BADGE in ed.ui_items(), "the sprite-normals refresh keeps the bake's badge")

    # the graph changes while the folder stays open
    await ed.call("edit_graph", path="assets/mats/stone.procmat", ops=[{"op": "setParam", "node": "n1", "name": "persistence", "value": 0.8}])
    await wait_for(ed, lambda: STALE in ed.ui_items(), "the bake's stale badge after the graph's edit", 30)
