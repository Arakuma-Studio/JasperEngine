#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Helpers for the real-GPU benches (docs/perf/gpu-campaign.md).

focus_editor(ed)     gives the editor's window the focus (an unfocused editor paces to 60 Hz).
fit_view(ed, w, h)   resizes the editor's main window until the Game view (or the Scene view) renders
                     at exactly w x h, so a fragment-bound pass is timed at the reference resolution.
                     The window is resized with xdotool on the bench's own X server (Xvfb or the
                     headless NVIDIA Xorg of tools/gpu/xorg-headless.sh; neither has a window
                     manager), one editor per display.
device(ed)           the backend and device the editor renders with (get_render_stats.gpu).
gpu_passes(ed, n)    n frames recorded with GPU timing; {label: median ms per frame} of the view.
"""

from __future__ import annotations

import re
import subprocess

from harness import Editor  # noqa: F401  (the caller's sys.path has tools/mcp)


async def view_size(ed, view: str = "game") -> tuple[int, int]:
    r = await ed.call_raw("screenshot", {"view": view})
    m = re.search(r"screenshot (\d+)x(\d+)(?: \(scaled from (\d+)x(\d+)\))?", r.text)
    if not m:
        return 0, 0
    return (int(m.group(3)), int(m.group(4))) if m.group(3) else (int(m.group(1)), int(m.group(2)))


def _largest_top_window() -> tuple[str, int, int] | None:
    """The biggest mapped top-level window of $DISPLAY (the editor's main window)."""
    try:
        out = subprocess.run(["xwininfo", "-root", "-children"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    best = None
    for m in re.finditer(r"^\s+(0x[0-9a-f]+) .*?: \(.*?\)\s+(\d+)x(\d+)\+", out, re.M):
        wid, w, h = m.group(1), int(m.group(2)), int(m.group(3))
        if best is None or w * h > best[1] * best[2]:
            best = (wid, w, h)
    return best


async def focus_editor(ed) -> bool:
    """Give the editor's main window the input focus. An X server without a window manager (Xvfb,
    the headless Xorg) focuses no window, and the editor paces an unfocused window's frames to its
    idle period (16.7 ms), which would hide a GPU frame's real time and a spike under one period."""
    win = _largest_top_window()
    if not win:
        return False
    subprocess.run(["xdotool", "windowfocus", "--sync", win[0]], capture_output=True, timeout=10)
    await ed.call("wait", frames=5)
    return True


async def fit_view(ed, width: int, height: int, view: str = "game", tries: int = 6) -> tuple[int, int]:
    w, h = await view_size(ed, view)
    for _ in range(tries):
        if (w, h) == (width, height) or not (w and h):
            break
        win = _largest_top_window()
        if not win:
            break
        wid, ww, wh = win
        subprocess.run(["xdotool", "windowsize", wid, str(ww + width - w), str(wh + height - h)],
                       capture_output=True, timeout=10)
        await ed.call("wait", frames=10)
        w, h = await view_size(ed, view)
    await focus_editor(ed)
    return w, h


async def device(ed) -> dict:
    gpu = (await ed.call("get_render_stats")).get("gpu", {})
    return {"backend": gpu.get("backend", ""), "device": gpu.get("device", ""), "semantics": gpu.get("semantics", "")}


async def gpu_passes(ed, frames: int = 300, view: str = "game") -> dict:
    """{label: median GPU ms per frame} over `frames` recorded frames, plus "_frame" (the engine
    frame's GPU time). A capture keeps each commit's view tag: while playing the Game view's passes
    are the engine frame's (-1) on GL, but its own tag (2) on Vulkan with the view maximised, so
    every tag is read and the labels merged (a sample has one tag: nothing counts twice)."""
    capture = await ed.call("capture_profile", frames=frames, gpu=True)
    out: dict = {}
    frames_seen = 0
    for tag in (-1, 1, 2):
        passes = await ed.call("get_gpu_passes", view=tag, capture_id=capture["capture_id"])
        for p in passes.get("passes", []):
            out[p["label"]] = out.get(p["label"], 0.0) + float((p.get("gpu_ms") or {}).get("median") or 0.0)
        if tag == -1:
            out["_frame"] = float((passes.get("frame_gpu_ms") or {}).get("median") or 0.0)
        frames_seen = max(frames_seen, int(passes.get("frames_with_gpu", 0)))
    out["_frames"] = frames_seen
    return out
