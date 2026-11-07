"""A hidden exported Linux game updates without drawing (docs/superpowers/specs/2026-11-04-platform-lifecycle-design.md
D1-D4, AC1-AC2): a real prebuilt player, run as a person runs it (not --headless) on an X server of its own,
logs "Window hidden: updating at 60 Hz without drawing" when its window is unmapped, keeps calling onUpdate and
stops drawing; mapped again it logs "Window visible: drawing again" and draws. The same for
_NET_WM_STATE_HIDDEN, the state mutter sets when it minimizes without unmapping. The probe script quits the
game after the second show.

Set JASPER_SLOW_BUILD=1 to run it: it uses the run's prebuilt glcore runtime (build_cache).
"""

import asyncio
import os
import re
import shutil
import subprocess
import threading
import time

import build_cache
from harness import Editor, Xvfb, skip

CI = True
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Hidden Player", "env": build_cache.player_env("glcore", "release")}

PROBE = """local HiddenProbe = {}
function HiddenProbe:init()
    self.updates, self.draws, self.shown, self.drawsAtShow, self.clock = 0, 0, 0, 0, 0
    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onDraw")
    RegisterEngineEvent(self, "onVisibilityChanged")
end
function HiddenProbe:onUpdate(dt)
    self.updates = self.updates + 1
    self.clock = self.clock + Engine.unscaledDeltatime
    if self.clock >= 0.25 then
        self.clock = 0
        print(string.format("hiddenprobe updates=%d draws=%d visible=%s", self.updates, self.draws, tostring(Engine.isVisible)))
    end
    -- the second show, and drawn frames after it: done
    if self.shown >= 2 and self.draws - self.drawsAtShow >= 30 then Engine.quit(0) end
end
function HiddenProbe:onDraw() self.draws = self.draws + 1 end
function HiddenProbe:onVisibilityChanged(visible)
    print("hiddenprobe visibility " .. tostring(visible))
    if visible then self.shown = self.shown + 1; self.drawsAtShow = self.draws end
end
return HiddenProbe
"""

LINE = re.compile(r"hiddenprobe updates=(\d+) draws=(\d+) visible=(\w+)")


class Player:
    def __init__(self, executable: str, cwd, display: str):
        self.env = dict(os.environ, DISPLAY=display)
        self.proc = subprocess.Popen([executable], cwd=cwd, env=self.env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
        self.lines: list[str] = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for line in self.proc.stdout:
            self.lines.append(line.rstrip("\n"))

    def last(self):
        for line in reversed(self.lines):
            found = LINE.search(line)
            if found:
                return int(found.group(1)), int(found.group(2)), found.group(3)
        return None

    async def wait_line(self, text: str, timeout: float = 20.0) -> bool:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if any(text in line for line in self.lines):
                return True
            await asyncio.sleep(0.1)
        return False

    async def counts_over(self, seconds: float) -> tuple[int, int]:
        before = self.last()
        await asyncio.sleep(seconds)
        after = self.last()
        return after[0] - before[0], after[1] - before[1]

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    def xprop(self, *args: str) -> None:
        subprocess.run(["xprop", *args], env=self.env, capture_output=True, text=True, timeout=20)


async def test_a_hidden_player_updates_without_drawing(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real player (the run builds its prebuilt runtime once)")
    if not shutil.which("xdotool") or not shutil.which("xprop"):
        skip("needs xdotool and xprop")
    await ed.call("create_entity", name="Probe", type="empty")
    await ed.call("create_script", kind="lua", name="HiddenProbe", content=PROBE, entity="Probe")
    await ed.call("create_entity", name="Sprite", type="sprite")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scenes_in_build"][0]["id"])

    await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-hidden-player"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    while job["status"] == "running":
        await asyncio.sleep(2)
        job = await ed.call("get_job", job_id=job["job_id"])
    ed.check_eq(job["status"], "succeeded", f"{job}")

    xvfb = Xvfb()
    display = await asyncio.to_thread(xvfb.start)
    player = Player(job["result"]["executable"], out, display)
    try:
        ed.check(await player.wait_line("hiddenprobe updates="), f"the player runs: {player.lines[-30:]}")
        await asyncio.sleep(1.0)
        updates, draws = await player.counts_over(1.0)
        ed.check(draws > 10, f"visible: the player draws ({updates}, {draws})")
        windows = player.xdo("search", "--name", "Hidden Player").split()
        ed.check(windows, "the player's window")
        window = windows[-1]

        for hide, show, what in (
                (lambda: player.xdo("windowunmap", "--sync", window), lambda: player.xdo("windowmap", "--sync", window),
                 "unmapped"),
                (lambda: player.xprop("-id", window, "-f", "_NET_WM_STATE", "32a", "-set", "_NET_WM_STATE",
                                      "_NET_WM_STATE_HIDDEN"),
                 lambda: player.xprop("-id", window, "-remove", "_NET_WM_STATE"), "_NET_WM_STATE_HIDDEN")):
            count = sum("Window hidden: updating at 60 Hz without drawing" in line for line in player.lines)
            hide()
            end = time.monotonic() + 5
            while time.monotonic() < end and sum("Window hidden: updating at 60 Hz without drawing" in line
                                                   for line in player.lines) == count:
                await asyncio.sleep(0.1)
            ed.check(sum("Window hidden: updating at 60 Hz without drawing" in line for line in player.lines) > count,
                     f"{what}: the hidden line: {player.lines[-20:]}")
            await asyncio.sleep(0.5)
            updates, draws = await player.counts_over(1.0)
            ed.check(updates >= 40, f"{what}: the game keeps updating ({updates} in a second)")
            ed.check_eq(draws, 0, f"{what}: nothing is drawn")
            shown = sum(line == "hiddenprobe visibility true" for line in player.lines)
            show()
            end = time.monotonic() + 5
            while time.monotonic() < end and sum(line == "hiddenprobe visibility true" for line in player.lines) == shown:
                await asyncio.sleep(0.1)
            ed.check(sum(line == "hiddenprobe visibility true" for line in player.lines) > shown,
                     f"{what}: shown again: {player.lines[-20:]}")
        code = await asyncio.to_thread(player.proc.wait, 60)
        ed.check_eq(code, 0, f"the probe quits the game after the second show: {player.lines[-30:]}")
        ed.check(any("Window visible: drawing again" in line for line in player.lines), "the visible line")
    finally:
        if player.proc.poll() is None:
            player.proc.kill()
        xvfb.stop()
