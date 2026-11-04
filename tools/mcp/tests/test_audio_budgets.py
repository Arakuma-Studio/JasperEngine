"""S36 AC13 (R31, R32): the sound tools' wall-clock budgets. analyze_audio of a 1 s clip, uncached and cached; of a
3-minute OGG; search_sounds over the whole library with a warm cache. Times are measured around the tool call; a run
reports them through harness.over_budget, enforced with JASPER_MCP_PERF_SLACK on a quiet server."""

from __future__ import annotations

import shutil
import subprocess
import sys
import time
from pathlib import Path

from harness import Editor, over_budget

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import audio_refs as ar  # noqa: E402

CI = True
EDITOR = {"create": True}
TIMEOUT = 900
RATE = 48000


async def timed(ed: Editor, tool: str, **args) -> float:
    start = time.perf_counter()
    r = await ed.call_raw(tool, args, timeout=600)
    took = time.perf_counter() - start
    ed.check(not r.is_error, r.text[:400])
    return took


async def test_analyze_budgets(ed: Editor):
    uncached = []
    for i in range(5):
        path = f"assets/budget/one_{i}.wav"
        ar.write_wav(ed.project / path, [ar.sine(440.0 + i * 11, 1.0, RATE, -12.0)], RATE, ar.PCM16)
        uncached.append(await timed(ed, "analyze_audio", paths=[path], image="none"))
    # the round trip of a call that does nothing, so the budget is the tool's work
    floor = min([await timed(ed, "editor_status") for _ in range(5)])
    best = min(uncached) - floor
    print(f"[budget] 1 s clip uncached {best * 1000:.1f} ms (call floor {floor * 1000:.1f} ms)")
    ed.check(not over_budget("analyze_audio 1 s uncached", {"seconds": best}, 0.040), "over budget")
    cached = min([await timed(ed, "analyze_audio", paths=["assets/budget/one_0.wav"], image="none") for _ in range(5)]) - floor
    print(f"[budget] 1 s clip cached {cached * 1000:.1f} ms")
    ed.check(not over_budget("analyze_audio 1 s cached", {"seconds": cached}, 0.002), "over budget")


async def test_long_ogg_budget(ed: Editor):
    ffmpeg = shutil.which("ffmpeg")
    ed.check(ffmpeg, "ffmpeg is in the image")
    out = ed.project / "assets/budget/long.ogg"
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=180",
                    "-ac", "2", "-c:a", "libvorbis", "-q:a", "3", str(out)], check=True)
    took = await timed(ed, "analyze_audio", paths=["assets/budget/long.ogg"], image="none")
    print(f"[budget] 3 minute OGG {took:.2f} s")
    ed.check(not over_budget("analyze_audio 3 minute OGG", {"seconds": took}, 1.2), "over budget")


async def test_search_budget(ed: Editor):
    await timed(ed, "search_sounds", query="wood", scope="all", limit=20)
    warm = min([await timed(ed, "search_sounds", query="wood", scope="all", limit=20) for _ in range(3)])
    print(f"[budget] search_sounds, warm cache {warm * 1000:.1f} ms")
    ed.check(not over_budget("search_sounds warm", {"seconds": warm}, 0.080), "over budget")
