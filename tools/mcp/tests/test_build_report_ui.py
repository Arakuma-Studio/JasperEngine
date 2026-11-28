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
import sys
import zlib
from pathlib import Path

import yaml

from harness import REPO, Editor
import ui_input

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import audio_refs as ar  # noqa: E402

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


def near_mono(seconds: float) -> list:
    """two channels whose side is 24 dB under their mid: Force To Mono's Auto folds them"""
    m = ar.sine(440.0, seconds, 44100, -12.0)
    side = ar.sine(1000.0, seconds, 44100, -36.0)
    return [[a + b for a, b in zip(m, side)], [a - b for a, b in zip(m, side)]]


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


async def click_drawn(ed: Editor, ui: ui_input.Ui, label: str) -> None:
    """a click on the first row of a scrolling table: it sits right under the frozen header row, where the
    probe reports it not visible though it is drawn in the open"""
    for _ in range(60):
        items = sorted((await ed.call("find_ui_item", label=label, window=WINDOW))["items"], key=lambda i: i["rect"]["y"])
        if items:
            await ui.click(ui.centre(items[0]["rect"]))
            return
        await ui.frames(2)
    ed.fail(f"no {label!r} drawn in {WINDOW}")


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
    # two near-mono clips: one set to Auto (it ships folded), one left Off (the report offers Auto)
    for clip in ("sfx/near.wav", "sfx/plain.wav"):
        ar.write_wav(ed.project / clip, near_mono(1.0), 44100, ar.PCM16)
    await ed.call("sync_files", paths=["textures/wall.png", "sfx/near.wav", "sfx/plain.wav"])
    await ed.call("analyze_audio", paths=["sfx/near.wav", "sfx/plain.wav"], image="none")
    await ed.call("set_audio_import_settings", paths=["sfx/near.wav"], settings={"force_to_mono": "auto"})
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
    # the rows are drawn (the first can sit under the frozen header row, where reveal cannot scroll it out)
    for label in ("art/hero.aseprite", "textures/wall.png"):
        for _ in range(60):
            if (await ed.call("find_ui_item", label=label, window=WINDOW))["items"]:
                break
            await ui.frames(2)
        else:
            ed.fail(f"the Left Out tab does not list {label}")
    await ui.frames(6)
    await shot(ed, "3-left-out")

    # an audio file the report says Auto would fold: Set Force To Mono: Auto is one undoable meta edit
    await ui.click_item("Files", WINDOW)
    await ui.click_item("##files_search", WINDOW)
    await ui.type("plain.wav")
    await ui.frames(4)
    await click_drawn(ed, ui, "##row")
    await ui.reveal("Set Force To Mono: Auto", WINDOW)
    await ui.frames(4)
    await shot(ed, "4-audio-detail")
    await ui.click_item("Set Force To Mono: Auto", WINDOW)
    for _ in range(60):
        meta = ed.project / "sfx" / "plain.wav.meta"
        if meta.is_file() and (yaml.safe_load(meta.read_text()) or {}).get("import", {}).get("audioImporter", {}).get("force_to_mono") == "auto":
            break
        await ui.frames(2)
    else:
        ed.fail("Set Force To Mono: Auto did not write the clip's import settings")
    await ed.call("undo", scope="project")

    # the folded clip's source opens its Inspector: the Force To Mono row with Auto's hint
    await ui.click_item("##files_search", WINDOW)
    await ui.key("ctrl+a")
    await ui.type("near.wav")
    await ui.frames(4)
    await click_drawn(ed, ui, "sfx/near.wav")
    await ui.frames(6)
    await shot(ed, "5-after-source-click")
    await ui.reveal("Force To Mono", "Inspector")
    await ui.frames(6)
    await shot(ed, "6-inspector-force-to-mono")
