"""The Build Report window with real input (docs/specs/build-size-tools.md R27-R29, R31, AC14): two
exports around a texture that grew; Compare With... the earlier one; the Changes tab lists the grown
texture and the detail pane its Referenced By chain; the Unreferenced and Left Out tabs list the
project's files; the audio clip Inspector shows Force To Mono with Auto's hint. xdotool clicks the
rects find_ui_item reports; the screenshots go to SHOTS (default build/test-results/build-report-ui/,
JASPER_BUILD_REPORT_SHOTS overrides it).
"""

from __future__ import annotations

import json
import os
import struct
import zlib
from pathlib import Path

from harness import REPO, Editor
import ui_input

EDITOR = {"create": True, "name": "Report UI", "xvfb": "harness"}
TIMEOUT = 1500
CI = True
SHOTS = Path(os.environ.get("JASPER_BUILD_REPORT_SHOTS", REPO / "build" / "test-results" / "build-report-ui"))
WINDOW = "Build Report"


def noise_png(size: int, seed: int) -> bytes:
    state = seed * 2654435761 & 0xFFFFFFFF
    rows = bytearray()
    for _ in range(size):
        rows.append(0)
        for _ in range(size):
            state = (state * 1103515245 + 12345) & 0xFFFFFFFF
            rows += state.to_bytes(4, "little")

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 1)) + chunk(b"IEND", b""))


def format_bytes(n: int) -> str:
    """BuildReport::formatBytes"""
    if n >= 1 << 30:
        return f"{n / 1073741824.0:.2f} GB"
    if n >= 1 << 20:
        return f"{n / 1048576.0:.1f} MB"
    if n >= 1024:
        return f"{n / 1024.0:.1f} KB"
    return f"{n} B"


def history_label(report: dict) -> str:
    """the Report and Compare With... menus' label of a report"""
    created = report["created"]
    return f"{report['platform'].capitalize()} · {created[:10]} {created[11:16]} · {report['method']} · {format_bytes(report['total_bytes'])}"


async def shot(ed: Editor, name: str) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    await ed.screenshot("editor", SHOTS / f"{name}.png", focus=False, max_width=0)


async def export(ed: Editor, name: str) -> dict:
    out = ed.scratch.root / name
    job = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"], overwrite=True,
                        timeout_seconds=900)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    return json.loads((out / "build-report.json").read_text())


async def test_build_report_window(ed: Editor):
    ui = ui_input.Ui(ed)
    await ed.call("tool_groups", enable=["editor_ux"])
    await ed.call("create_entity", name="Box", type="cube")
    await ed.call("rename_scene", name="Main")
    for rel, data in (("textures/wall.png", noise_png(64, 1)), ("textures/orphan.png", noise_png(32, 2))):
        (ed.project / rel).parent.mkdir(parents=True, exist_ok=True)
        (ed.project / rel).write_bytes(data)
    (ed.project / "art").mkdir(exist_ok=True)
    (ed.project / "art" / "hero.aseprite").write_bytes((REPO / "editor" / "tests" / "fixtures" / "aseprite" / "s44-mcp" / "hero_idle.aseprite").read_bytes())
    await ed.call("sync_files", paths=["textures/wall.png", "textures/orphan.png", "art/hero.aseprite"])
    await ed.call("create_material", path="materials/wall.material", values={"baseColorTexture": "textures/wall.png"})
    await ed.call("link_material", entity="Box", material="materials/wall.material")
    await ed.call("save_project", untitled_scene_dir="scenes")
    first = await export(ed, "report-ui-1")
    (ed.project / "textures" / "wall.png").write_bytes(noise_png(256, 3))
    await ed.call("sync_files", paths=["textures/wall.png"])
    second = await export(ed, "report-ui-2")

    await ed.call("run_command", id="window.buildReport")
    await ui.find("Files", WINDOW, frames=240) or ed.fail("the window opens on the last export")
    await ui.click_item("Compare With...", WINDOW)
    await ui.click_item(history_label(first))
    await ui.click_item("Changes", WINDOW)
    # the texture grew: one row keyed by its source, its Referenced By chain beside it
    await ui.click_item("textures/wall.png", WINDOW)
    await ui.reveal("scenes/Main.scene", WINDOW)
    await ui.reveal("project.yaml  (root)", WINDOW)
    await ui.frames(6)
    await shot(ed, "1-changes-referenced-by")

    await ui.click_item("Unreferenced", WINDOW)
    orphan = next(f["path"] for f in second["files"] if f.get("source") == "textures/orphan.png")
    await ui.reveal(orphan, WINDOW)
    await ui.frames(6)
    await shot(ed, "2-unreferenced")

    await ui.click_item("Left Out", WINDOW)
    await ui.reveal("art/hero.aseprite", WINDOW)
    await ui.reveal("textures/wall.png", WINDOW)
    await ui.frames(6)
    await shot(ed, "3-left-out")
