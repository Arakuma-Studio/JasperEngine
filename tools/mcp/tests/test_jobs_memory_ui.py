"""S06 AC23, scripted: the Jobs & Memory panel and Project Settings > Player > Threading, driven
with xdotool in the harness's Xvfb (1600x900, default layout), and a 200-texture load storm.

1. Reserved Frame Workers changed in Project Settings: the panel's gate max follows at once,
   and Ctrl+Z in the panel reverts it.
2. 200 textures load at once: the Background lane runs at the gate's max (Heavy jobs, as
   texture decodes are, take at most heavy_max = max - 2 of it, R9), and the editor's frames
   keep their pace.

The clicks are real (xdotool), aimed where find_ui_item says the control is drawn now (by its
label, scrolling with the wheel until it is), so a layout change does not move them off target.
Screenshots go to $JE_EVIDENCE_DIR (else the scratch directory).
"""

from __future__ import annotations

import asyncio
import os
import shutil
import statistics
import struct
import subprocess
import time
import zlib
from pathlib import Path

from harness import Editor, TestFailure, find_key, skip
import ui_input

EDITOR = {"xvfb": "harness", "create": True, "name": "Jobs Memory UI"}
CI = True
TIMEOUT = 900

PROJECT_SETTINGS = "Project Settings##ProjectSettingsModal"

TEXTURES = 200
SIZE = 512


class Ui(ui_input.Ui):
    """The shared frame-paced input (tools/mcp/ui_input.py), clicking items it scrolls to."""

    async def click_item(self, label: str, window: str | None = None, scroll_in: dict | None = None):
        """clicks the item where it is drawn; with scroll_in (a window rect), wheels down through that
        window until the item shows"""
        for _ in range(60):
            item = await self.find(label, window, frames=2)
            if item:
                await self.click(self.centre(item["rect"]))
                return
            if scroll_in is None:
                await self.ed.call("wait", frames=5)
            else:
                await self.wheel(self.centre(scroll_in), 3)
        raise TestFailure(f"find_ui_item: no visible {label!r}" + (f" in {window!r}" if window else ""))


def evidence(ed: Editor) -> Path:
    folder = Path(os.environ.get("JE_EVIDENCE_DIR") or ed.scratch.root)
    folder.mkdir(parents=True, exist_ok=True)
    return folder


def needs_ui(ed: Editor) -> Ui:
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness's Xvfb")
    env = dict(os.environ, DISPLAY=ed._xvfb.display)
    windows = subprocess.run(["xdotool", "search", "--name", "."], env=env, capture_output=True, text=True,
                             timeout=20).stdout.split()
    if not windows:
        skip("no editor window found")
    ui = Ui(ed, windows[-1])
    ui.xdo("windowfocus", "--sync", ui.window)
    return ui


async def gate(ed: Editor) -> dict:
    return (await ed.call("get_job_system_stats", window_frames=1))["gate"]


async def test_reserved_frame_workers_from_project_settings(ed: Editor):
    ui = needs_ui(ed)
    out = evidence(ed)
    workers = (await ed.call("get_job_system_stats", window_frames=1))["workers"]
    before = await gate(ed)
    await ed.call("focus_window", window="jobs_memory")
    await ed.call("wait", frames=5)
    await ed.screenshot("editor", path=str(out / "ac23-1-before.png"))

    # Edit > Project Settings..., Player > Threading > Reserved Frame Workers: 3, OK
    await ui.click_item("Edit", window="##MainMenuBar")
    await ui.click_item("Project Settings...")
    ok = None
    for _ in range(20):
        ok = await ui.find("OK", window=PROJECT_SETTINGS)
        if ok:
            break
    ed.check(ok is not None, "Project Settings opened (its OK button is drawn)")
    page = ok["window_rect"]
    # down the page until the setting, or its closed Threading foldout, shows
    for _ in range(60):
        if await ui.find("##ReservedFrameWorkers", window=PROJECT_SETTINGS):
            break
        if await ui.find("Threading", window=PROJECT_SETTINGS):
            await ui.click_item("Threading", window=PROJECT_SETTINGS)
            break
        await ui.wheel(ui.centre(page), 3)
    await ui.click_item("##ReservedFrameWorkers", window=PROJECT_SETTINGS, scroll_in=page)
    await ui.click_item("3", window="##Combo")     # the open combo's popup
    await ed.screenshot("editor", path=str(out / "ac23-2-settings.png"))
    await ui.click_item("OK", window=PROJECT_SETTINGS)
    await ed.call("wait", frames=5)
    applied = await gate(ed)
    ed.check_eq(applied["reserve"], 3, "Reserved Frame Workers 3 set through the dialog")
    ed.check_eq(applied["max"], max(1, workers - 3), "the gate follows the setting at once")
    settings = await ed.call("get_project_settings", keys=["player.threading.reserved_frame_workers"])
    ed.check_eq(settings["settings"]["player.threading.reserved_frame_workers"], 3, "saved in the project")
    await ed.screenshot("editor", path=str(out / "ac23-3-applied.png"))

    # Ctrl+Z in the panel: the project history's step
    panel = (await ed.call("focus_window", window="jobs_memory"))["rect"]
    await ui.click(ui.centre(panel))
    # Ctrl held across frames (ui_input.Ui.key)
    await ui.key("ctrl+z")
    await ed.call("wait", frames=5)
    reverted = await gate(ed)
    await ed.screenshot("editor", path=str(out / "ac23-4-undone.png"))
    ed.check_eq((reverted["max"], reverted["reserve"]), (before["max"], before["reserve"]), "Ctrl+Z reverts it")
    print(f"gate max {before['max']} -> {applied['max']} (reserve 3) -> {reverted['max']} after Ctrl+Z")


def noise_png(size: int) -> bytes:
    """A PNG of random pixels: it compresses to nothing less, so decoding it takes real time."""
    rows = b"".join(b"\x00" + os.urandom(size * 4) for _ in range(size))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 1)) + chunk(b"IEND", b"")


async def frame_ms(ed: Editor) -> float:
    rate = (await ed.call("eval_lua", code="return Engine.framerate", allow_edit_mode=True))["values"][0]
    return 1000.0 / rate if rate and rate > 0 else 0.0


async def test_texture_storm_saturates_the_gate_and_keeps_frames(ed: Editor):
    out = evidence(ed)
    folder = ed.project / "assets" / "storm"
    folder.mkdir(parents=True, exist_ok=True)
    paths = []
    for k in range(TEXTURES):
        (folder / f"noise{k:03d}.png").write_bytes(noise_png(SIZE))
        paths.append(f"assets/storm/noise{k:03d}.png")
    await ed.call("sync_files")
    made = await ed.call("create_scene", name="Storm", type="3D", path="scenes/Storm")
    scene = made["scene"]["id"]
    created = await ed.batch([("create_entity", {"scene": scene, "type": "box", "name": f"Box{k}",
                                                  "position": [k % 20 * 1.5, 0, k // 20 * 1.5]}) for k in range(TEXTURES)])
    ids = [find_key(r, "entity_id", "id") for r in created["results"]]
    await ed.call("focus_window", window="jobs_memory")

    # the editor's own Background work (thumbnails of the new files) first
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        s = await ed.call("get_job_system_stats", window_frames=1)
        if s["asset_jobs"] == 0 and s["lanes"]["Background"]["running"] == 0 and s["lanes"]["Background"]["queued"] == 0:
            break
        await asyncio.sleep(0.2)
    idle = [await frame_ms(ed) for _ in range(40)]

    stats = await ed.call("get_job_system_stats", window_frames=1)
    heavy_max = stats["gate"]["heavy_max"]
    probe = "return {Engine.framerate, Jobs.getRunningCount(JobLane.Background), Engine.getQueuedResourceCount()}"
    await ed.batch([("set_properties", {"scene": scene, "entity": eid,
                                        "values": {"mesh": {"submeshes[0].material.baseColorTexture": path}}})
                    for eid, path in zip(ids, paths)])
    storm, running = [], []
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        rate, background, queued = (await ed.call("eval_lua", code=probe, allow_edit_mode=True))["values"][0]
        running.append(background)
        storm.append(1000.0 / rate if rate > 0 else 0.0)
        if queued == 0 and len(running) > 5:
            break
    # the gate's own high-water mark over the storm (sampled each frame), also in the panel
    peaks = (await ed.call("get_job_system_stats", window_frames=120))["gate"]
    await ed.screenshot("editor", path=str(out / "ac23-6-after-storm.png"))
    idle_p95 = sorted(idle)[int(len(idle) * 0.95) - 1]
    storm_p95 = sorted(storm)[int(len(storm) * 0.95) - 1]
    print(f"storm: {len(storm)} samples; Background running max {max(running)} sampled, gate peak {peaks['peak']} (Heavy {peaks['heavy_peak']} of {heavy_max}); "
          f"frame ms idle median {statistics.median(idle):.1f} p95 {idle_p95:.1f}, "
          f"storm median {statistics.median(storm):.1f} p95 {storm_p95:.1f}")
    ed.check_eq(peaks["heavy_peak"], heavy_max, "the decodes ran at the gate's Heavy max")
    ed.check(peaks["peak"] <= peaks["max"], "never above the gate's max")
    # the reference machine's budget is 33 ms; a software renderer's idle frame may already
    # be slower, so the storm may add at most half of the idle p95 there
    budget = 33.0 if idle_p95 <= 22.0 else idle_p95 * 1.5
    # a wall-clock budget: enforced in the benchmark run (JASPER_TEST_PERF_SLACK, on the
    # benchmark-only server E, docs/testing.md); a shared builder's load alone misses it
    slack = float(os.environ.get("JASPER_TEST_PERF_SLACK") or 0)
    if slack > 0:
        ed.check(storm_p95 <= budget * slack,
                 f"frames during the storm: p95 {storm_p95:.1f} ms (idle {idle_p95:.1f}, budget {budget * slack:.1f})")
    else:
        print(f"     [bench] storm frame p95 {storm_p95:.1f} ms against {budget:.1f} (not enforced: JASPER_TEST_PERF_SLACK unset)")
