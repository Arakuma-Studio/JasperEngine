"""A file saved outside the editor imports again on its own (docs/specs/S44-aseprite-import.md
R22, R23, R28; AC14), on a new project:

- an atomic replace and an in-place overwrite with another real Aseprite file give a new import
  (the cache key and the frame count change), and the Console has one "[Aseprite] Imported" line
  for it;
- a half-written file keeps the previous import (its outputs stay) and imports once it is whole;
- the latency from the write to the new import is reported against R28's 300 ms (p95 of 20 runs);
- a Sprite showing a file in the Scene view: its centre pixel turns from hero_idle's blue to
  hero_idle_recolor's red and back on each save (the latency from the write to the new pixel
  reported against 300 ms, p95 of 20 saves), keeps its colour while the file is half written, and
  changes when the editor itself writes the file (import_files overwriting it, R23).
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
BLUE = FIXTURES / "s44-mcp" / "hero_idle.aseprite"            # Body of frame 0: (40, 120, 220)
RED = FIXTURES / "s44-mcp" / "hero_idle_recolor.aseprite"     # the same file, Body (230, 40, 40)
TINT = "assets/sprites/tint.aseprite"
S: dict = {}


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


async def import_info(ed: Editor, path: str = FILE) -> dict:
    return await ed.call("get_asset_info", path=path)


async def wait_for_key(ed: Editor, old_key: str | None, timeout: float = 10.0, path: str = FILE) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        info = await import_info(ed, path)
        key = (info.get("import") or {}).get("cache", {}).get("key")
        if info.get("import_state") == "imported" and key and key != old_key:
            return info
        await ed.call("wait", seconds=0.02)
    raise TestFailure(f"no new import of {path} within {timeout} s: {info.get('import_state')}")


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
    for i in range(20):
        start = time.monotonic()
        replace_atomically(sources[i % 2], target)
        info = await wait_for_key(ed, key)
        times.append(time.monotonic() - start)
        key = info["import"]["cache"]["key"]
    times.sort()
    p95 = times[min(len(times) - 1, int(len(times) * 0.95))]
    over = over_budget("aseprite save to new import (p95 of 20)", {"p95": p95, "median": times[len(times) // 2]}, 0.3)
    check(not over, f"over budget: {over}")


def tint(rgb) -> str:
    """Which of the two Body colours a pixel shows (the Scene view draws a 2D sprite unlit)"""
    r, g, b = rgb[:3]
    return "blue" if b > r + 60 else "red" if r > b + 60 else f"neither {tuple(rgb[:3])}"


async def centre(ed: Editor) -> str:
    return tint((await ed.call("sample_view_color", view="scene", x=S["x"], y=S["y"]))["bytes"])


async def wait_for_tint(ed: Editor, want: str, timeout: float = 10.0) -> str:
    """The tint once it is `want`, else what shows and the file's import (to tell a missed reload
    from a reload not drawn)"""
    deadline = time.monotonic() + timeout
    got = await centre(ed)
    while got != want and time.monotonic() < deadline:
        await ed.call("wait", seconds=0.02)
        got = await centre(ed)
    if got != want:
        info = await import_info(ed, TINT)
        got += f" (import {info.get('import_state')}, key {(info.get('import') or {}).get('cache', {}).get('key', '')[:12]})"
    return got


async def tint_sprite(ed: Editor) -> None:
    """A 2D scene whose Scene view is framed on a Sprite showing TINT's frame 0, nothing selected"""
    if S.get("ed") is ed:
        return
    target = ed.project / TINT
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(BLUE, target)
    await ed.call("sync_files", paths=[TINT])
    await wait_for_key(ed, None, path=TINT)
    sid = (await ed.call("create_scene", name="Tint", type="2d"))["scene"]["id"]
    await ed.call("select_scene", scene=sid)
    await ed.call("set_sprite_frames", scene=sid, create={"name": "Tint"}, sheet=TINT, frame=0, clips=False)
    await ed.call("select_entities", scene=sid, entities=[])
    await ed.call("frame_entity", scene=sid, entity="Tint")
    probe = await ed.call("sample_view_color", view="scene", x=0, y=0)
    S.update(ed=ed, scene=sid, x=probe["size"]["width"] // 2, y=probe["size"]["height"] // 2)


async def test_scene_view_pixel_follows_saves(ed: Editor):
    await tint_sprite(ed)
    target = ed.project / TINT
    check(await wait_for_tint(ed, "blue") == "blue", f"the Scene view shows frame 0's blue Body: {await centre(ed)}")
    times = []
    for i in range(20):
        want, source = ("red", RED) if i % 2 == 0 else ("blue", BLUE)
        time.sleep(0.05)
        start = time.monotonic()
        replace_atomically(source, target)
        got = await wait_for_tint(ed, want)
        check(got == want, f"save {i}: the Scene view pixel is {got}, not {want}")
        times.append(time.monotonic() - start)
    times.sort()
    p95 = times[min(len(times) - 1, int(len(times) * 0.95))]
    over = over_budget("aseprite save to new Scene view pixel (p95 of 20)", {"p95": p95, "median": times[len(times) // 2]}, 0.3)
    check(not over, f"over budget: {over}")


async def test_half_written_keeps_the_pixel(ed: Editor):
    await tint_sprite(ed)
    target = ed.project / TINT
    shutil.copy(BLUE, target)
    check(await wait_for_tint(ed, "blue") == "blue", "blue before the half write")
    data = RED.read_bytes()
    time.sleep(0.05)
    target.write_bytes(data[: len(data) // 2])
    await ed.call("wait", seconds=0.8)
    check(await centre(ed) == "blue", "a half-written file keeps the previous pixels")
    target.write_bytes(data)
    check(await wait_for_tint(ed, "red") == "red", "the whole file shows")


async def test_editor_write_imports_again(ed: Editor):
    """R23: a write by the editor itself, which the watcher's self-write suppression hides: the
    file imported over itself with import_files {overwrite} (write_file takes text only)"""
    await tint_sprite(ed)
    target = ed.project / TINT
    shutil.copy(RED, target)
    check(await wait_for_tint(ed, "red") == "red", "red before the editor's write")
    key = (await import_info(ed, TINT))["import"]["cache"]["key"]
    source = ed.scratch.root / "editor-write" / "tint.aseprite"
    source.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(BLUE, source)
    r = await ed.call("import_files", sources=[str(source)], to_dir="assets/sprites", overwrite=True)
    check(target.read_bytes() == BLUE.read_bytes(), f"import_files wrote the file: {r}")
    info = await wait_for_key(ed, key, path=TINT)
    check(info["import"]["frames"] == 6, f"the editor's write imports again: {info['import'].get('frames')}")
    check(await wait_for_tint(ed, "blue") == "blue", "and the Scene view shows it")
