"""Play the upgrade fixture in an editor (old or new) and record its behaviour transcript.

Shared by gen_fixture.py (the old editor writes the reference) and check.py (the new editor
must print the same). The game prints `T|<scene>|<frame>|<key>|<values...>` lines (see
game/lua/lib/transcript.lua); a run plays the start scene at a fixed 1/60 s per frame with
auto_pause (the game advances only inside our calls), so every run of one build sees the same
frames whatever the machine's speed.

Two kinds of run:
  input      the editor sends the keys of the game's own schedule (lib/transcript.lua
             T.schedules) through send_input, at the scene frames the schedule names;
  autopilot  the editor was started with UPGRADE_AUTOPILOT=1: the game plays its schedule
             itself, as exported players do.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

T = "require('lib.transcript')"
KEYS = ["right", "left", "up", "down", "space", "enter"]
LINE = re.compile(r"(T\|[^\r\n]*)")
# The scenes in play order and the scene frames to screenshot in each
SHOTS = {"Title": [20], "Level1": [100, 300], "Arena2D": [120]}


def transcript_lines(entries: list[dict]) -> list[str]:
    out = []
    for entry in entries:
        message = entry.get("message", "")
        for m in LINE.finditer(message):
            out.append(m.group(1).rstrip())
    return out


async def console_all(ed, since: Any) -> tuple[list[dict], Any]:
    """Every Console entry after cursor `since` (all pages)."""
    entries: list[dict] = []
    cursor = since
    while True:
        args: dict[str, Any] = {"limit": 1000, "max_message_chars": 100000}
        if cursor is not None:
            args["since"] = cursor
        page = await ed.call("read_console", args)
        entries += page.get("entries", [])
        cursor = page.get("next_cursor", cursor)
        if not page.get("more"):
            return entries, cursor


async def lua(ed, code: str) -> Any:
    r = await ed.call("eval_lua", code=code)
    values = r.get("values", [])
    return values[0] if values else None


async def wait_frame(ed, scene: str, frame: int, timeout: float = 600) -> None:
    cond = f"{T}.scene == '{scene}' and {T}.frame >= {frame}"
    r = await ed.call("wait", {"until": cond, "timeout_seconds": timeout, "console_limit": 0})
    if r.get("reason") != "condition":
        raise RuntimeError(f"waiting for {scene} frame {frame}: {json.dumps(r)[:800]}")


async def play(ed, out_dir: Path, mode: str, start_scene: str) -> dict:
    """One run from the start scene to the end of the last scene. Writes
    <out_dir>/<mode>.txt (the transcript), <mode>-console.json (problems) and screenshots
    <mode>-<scene>-<frame>.png. Returns {lines, problems, shots}."""
    out_dir.mkdir(parents=True, exist_ok=True)
    # the Game view at the project's aspect (the canvas, 16:9): its size follows the editor's
    # layout, which differs between versions; screenshots are compared scaled to one size
    await ed.call("configure_game_view", aspect="project")
    await ed.call("open_scene", scene=start_scene)
    before = await ed.call("read_console", limit=1)
    cursor = before.get("next_cursor")
    await ed.call("play", action="start", scene=start_scene, fixed_fps=60, auto_pause=True, time_scale=1,
                  timeout_seconds=600)
    codes = await lua(ed, "return {" + ", ".join(f"Input.KEY_{k.upper()}" for k in KEYS) + "}")
    key_of = {int(c): name for c, name in zip(codes, KEYS)}
    shots: list[str] = []
    try:
        for scene in SHOTS:
            # the scene starts (its Director's init ran)
            await wait_frame(ed, scene, 0)
            events: list[tuple[int, str, str]] = []
            if mode == "input":
                schedule = await lua(ed, f"return {T}.schedules['{scene}'] or {{}}") or []
                for key, start, end in schedule:
                    events.append((int(start), "down", key_of[int(key)]))
                    events.append((int(end), "up", key_of[int(key)]))
            for frame in SHOTS[scene]:
                events.append((frame, "shot", ""))
            for frame, what, key in sorted(events, key=lambda e: (e[0], e[1] != "up")):
                await wait_frame(ed, scene, frame)
                if what == "shot":
                    name = f"{mode}-{scene}-{frame}.png"
                    await ed.screenshot("game", path=out_dir / name, max_width=0)
                    shots.append(name)
                else:
                    await ed.call("send_input", events=[{"type": "key", "key": key, "action": what}], frames_after=0)
        r = await ed.call("wait", {"until": f"{T}.done", "timeout_seconds": 600, "console_limit": 0})
        if r.get("reason") != "condition":
            raise RuntimeError(f"the game did not reach its end: {json.dumps(r)[:800]}")
    finally:
        await ed.call("play", action="stop", timeout_seconds=600)
    entries, _ = await console_all(ed, cursor)
    lines = transcript_lines(entries)
    problems = [e for e in entries if e.get("type") in ("warning", "error")]
    (out_dir / f"{mode}.txt").write_text("\n".join(lines) + "\n")
    (out_dir / f"{mode}-console.json").write_text(json.dumps(problems, indent=1) + "\n")
    return {"lines": lines, "problems": problems, "shots": shots}
