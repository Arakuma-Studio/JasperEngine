"""A file saved outside the editor imports again on its own (docs/specs/S44-aseprite-import.md
R22, R23, R28; AC14), on a new project:

- an atomic replace and an in-place overwrite with another real Aseprite file give a new import
  (the cache key and the frame count change), and the Console has one "[Aseprite] Imported" line
  for it;
- a half-written file keeps the previous import (its outputs stay) and imports once it is whole;
- the latency from the write to the new import is reported against R28's 300 ms (p95 of 10 runs).
"""

from __future__ import annotations

import os
import shutil
import time
from pathlib import Path

from harness import REPO, Editor, TestFailure, over_budget

CI = True
EDITOR = {"create": True, "name": "AsepriteHotReload"}
TIMEOUT = 600

FIXTURES = REPO / "editor" / "tests" / "fixtures" / "aseprite"
FOUR_FRAMES = FIXTURES / "asefile" / "layers_and_tags.aseprite"       # 4 frames
TWO_FRAMES = FIXTURES / "aseprite-tests" / "2f-index-3x3.aseprite"      # 2 frames
FILE = "assets/sprites/live.aseprite"


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


async def import_info(ed: Editor) -> dict:
    return await ed.call("get_asset_info", path=FILE)


async def wait_for_key(ed: Editor, old_key: str | None, timeout: float = 10.0) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        info = await import_info(ed)
        key = (info.get("import") or {}).get("cache", {}).get("key")
        if info.get("import_state") == "imported" and key and key != old_key:
            return info
        await ed.call("wait", seconds=0.02)
    raise TestFailure(f"no new import of {FILE} within {timeout} s: {info.get('import_state')}")


def replace_atomically(source: Path, target: Path) -> None:
    tmp = target.with_suffix(".tmp-write")
    shutil.copy(source, tmp)
    os.replace(tmp, target)


async def test_saves_import_again(ed: Editor):
    target = ed.project / FILE
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(FOUR_FRAMES, target)
    await ed.call("sync_files", paths=[FILE])
    first = await wait_for_key(ed, None)
    check(first["import"]["frames"] == 4, f"first import: {first['import']['frames']}")

    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    replace_atomically(TWO_FRAMES, target)
    second = await wait_for_key(ed, first["import"]["cache"]["key"])
    check(second["import"]["frames"] == 2, f"after an atomic replace: {second['import']['frames']}")
    console = await ed.call("read_console", since=cursor, limit=200)
    lines = [e["message"] for e in console["entries"] if "[Aseprite] Imported" in e.get("message", "") and "live.aseprite" in e["message"]]
    check(len(lines) == 1, f"one Console line per import: {lines}")

    time.sleep(0.05)
    target.write_bytes(FOUR_FRAMES.read_bytes())     # in place
    third = await wait_for_key(ed, second["import"]["cache"]["key"])
    check(third["import"]["frames"] == 4, f"after an in-place write: {third['import']['frames']}")


async def test_half_written_keeps_the_previous_import(ed: Editor):
    target = ed.project / FILE
    before = await import_info(ed)
    key = before["import"]["cache"]["key"]
    data = TWO_FRAMES.read_bytes()
    target.write_bytes(data[: len(data) // 2])
    await ed.call("wait", seconds=0.8)
    during = await import_info(ed)
    check(during["import"]["cache"]["key"] == key and during["import"]["frames"] == 4,
          f"a half-written file keeps the previous import: {during.get('import', {}).get('frames')}")
    target.write_bytes(data)
    after = await wait_for_key(ed, key)
    check(after["import"]["frames"] == 2, f"the whole file imports: {after['import']['frames']}")


async def test_latency(ed: Editor):
    target = ed.project / FILE
    sources = [FOUR_FRAMES, TWO_FRAMES]
    times = []
    key = (await import_info(ed))["import"]["cache"]["key"]
    for i in range(10):
        start = time.monotonic()
        replace_atomically(sources[i % 2], target)
        info = await wait_for_key(ed, key)
        times.append(time.monotonic() - start)
        key = info["import"]["cache"]["key"]
    times.sort()
    p95 = times[min(len(times) - 1, int(len(times) * 0.95))]
    over = over_budget("aseprite save to new import (p95 of 10)", {"p95": p95, "median": times[len(times) // 2]}, 0.3)
    check(not over, f"over budget: {over}")
