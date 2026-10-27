"""Real input for the MCP UI tests: xdotool in the harness's Xvfb, paced by editor frames.

    from ui_input import Ui
    EDITOR = {"xvfb": "harness", ...}          # the harness owns the display xdotool talks to
    ui = Ui(ed)                                # skips the test without xdotool or that display
    item = await ui.find("Apply", window="Inspector")
    await ui.click(ui.centre(item))
    await ui.drag(start, end)
    await ui.key("ctrl+z")

The editor reads X events at the start of a frame and ImGui acts on them in that frame, so every
step here waits for editor frames (the `wait` tool counts them), never for wall-clock time alone:
on a loaded host one frame can take a second, and a fixed sleep let the next step start before
the editor had seen the last one (a drag that never started, a press read before its release, a
history row read before its click). Two rules on top of that:
 - a press and its release, and each step of a drag, land in different frames: ImGui starts a
   drag only from a move it sees with the button already down, and a widget never sees a press
   that is released in the same frame;
 - a click waits until ImGui's clock has moved past its double-click time since the previous
   release (DOUBLE_CLICK_SECONDS; ImGui's clock is the wall clock between frames). On a busy host
   frames can come in bursts: two clicks a couple of fast frames apart read as a double click.
Positions are window coordinates from the editor (find_ui_item rects, view_rect_in_window, the
rects of editor_status and focus_window), turned into screen points with the window's origin.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
import time

from harness import Editor, skip

# ImGui's io.MouseDoubleClickTime is 0.30 s; the margin covers the frame that reads the release
DOUBLE_CLICK_SECONDS = 0.45
# frames after an input event: the frame that reads it, and one that draws its effect
SETTLE_FRAMES = 2


class Ui:
    """xdotool input in the editor's window, each step followed by editor frames."""

    def __init__(self, ed: Editor, window: str | None = None, window_name: str = "JasperEngine"):
        if not shutil.which("xdotool") or not getattr(ed, "_xvfb", None) or not ed._xvfb.display:
            skip("needs xdotool and the harness's Xvfb (EDITOR = {\"xvfb\": \"harness\", ...})")
        self.ed = ed
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)
        if window is None:
            windows = self.xdo("search", "--name", window_name).split() or self.xdo("search", "--name", ".").split()
            if not windows:
                skip("no editor window found")
            window = windows[-1]
        self.window = window
        self.origin = (0, 0)
        for line in self.xdo("getwindowgeometry", self.window).splitlines():
            if "Position:" in line:
                x, y = (int(v) for v in line.split("Position:")[1].split("(")[0].strip().split(","))
                self.origin = (x, y)
        self.last_release = 0.0     # time.monotonic() once the frame after the last release ran
        self.buttons_down: set[str] = set()

    # -- plumbing ------------------------------------------------------------------------------

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    async def frames(self, count: int = SETTLE_FRAMES) -> None:
        """Waits until the editor has run `count` frames (it has read and drawn what was sent)."""
        await self.ed.call("wait", frames=max(1, count))

    async def settle(self, seconds: float = 0.0, frames: int = SETTLE_FRAMES) -> None:
        """Editor frames, after a minimum of wall-clock time (for what is timed by the clock:
        a tooltip's delay, a key repeat). The frames are what make the input count."""
        if seconds > 0:
            await asyncio.sleep(seconds)
        await self.frames(frames)

    def focus(self) -> None:
        self.xdo("windowactivate", "--sync", self.window)
        self.xdo("windowfocus", "--sync", self.window)

    # -- positions -----------------------------------------------------------------------------

    def at(self, rect, offset: tuple[float, float] = (0.0, 0.0)) -> tuple[int, int]:
        """Screen point of a window-coordinate rect's corner plus offset ({x, y, ...} or [x, y, ...])."""
        x, y = (rect["x"], rect["y"]) if isinstance(rect, dict) else (rect[0], rect[1])
        return (int(self.origin[0] + x + offset[0]), int(self.origin[1] + y + offset[1]))

    def centre(self, rect) -> tuple[int, int]:
        w, h = (rect["width"], rect["height"]) if isinstance(rect, dict) else (rect[2], rect[3])
        return self.at(rect, (w / 2, h / 2))

    async def find(self, label: str, window: str | None = None, *, index: int = 0, frames: int = 60,
                   **args) -> dict | None:
        """The index-th drawn, visible find_ui_item match, once the editor draws it (up to
        `frames` editor frames), or None."""
        query = {"label": label, **args}
        if window:
            query["window"] = window
        for _ in range(max(1, frames // 2)):
            items = [i for i in (await self.ed.call("find_ui_item", **query)).get("items", []) if i.get("visible", True)]
            if len(items) > index:
                return items[index]
            await self.frames(2)
        return None

    async def click_item(self, label: str, window: str | None = None, *, index: int = 0, **args) -> dict:
        item = await self.find(label, window, index=index, **args)
        if item is None:
            self.ed.fail(f"no drawn, visible UI item {label!r}" + (f" in {window}" if window else ""))
        await self.click(self.centre(item.get("rect", item)))
        return item

    # -- pointer -------------------------------------------------------------------------------

    async def _pointer(self, point: tuple[int, int], frames: int = SETTLE_FRAMES) -> None:
        # what every method here moves with: a subclass may give move() other coordinates
        self.xdo("mousemove", str(int(point[0])), str(int(point[1])))
        await self.frames(frames)

    async def move(self, point: tuple[int, int], frames: int = SETTLE_FRAMES) -> None:
        """To a screen point (at(), centre())."""
        await self._pointer(point, frames)

    async def _clear_double_click(self) -> None:
        wait = DOUBLE_CLICK_SECONDS - (time.monotonic() - self.last_release)
        if wait > 0:
            await asyncio.sleep(wait)
            await self.frames(1)

    async def press(self, point: tuple[int, int] | None = None, button: str = "1") -> None:
        if point is not None:
            await self._pointer(point)
        await self._clear_double_click()
        self.xdo("mousedown", button)
        self.buttons_down.add(button)
        await self.frames()

    async def release(self, button: str = "1") -> None:
        self.xdo("mouseup", button)
        self.buttons_down.discard(button)
        await self.frames()
        self.last_release = time.monotonic()

    async def click(self, point: tuple[int, int] | None = None, button: str = "1",
                    modifier: str | None = None) -> None:
        """A single click: press and release frames apart, never read as a double click; with
        modifier ("ctrl", "shift", "alt") held from a frame before the press to the release."""
        await self._modifier_down(modifier)
        await self.press(point, button)
        await self.release(button)
        await self._modifier_up(modifier)

    async def _modifier_down(self, modifier: str | None) -> None:
        if modifier:
            self.xdo("keydown", modifier)
            await self.frames(1)

    async def _modifier_up(self, modifier: str | None) -> None:
        if modifier:
            self.xdo("keyup", modifier)
            await self.frames()

    async def double_click(self, point: tuple[int, int] | None = None, button: str = "1") -> None:
        """Two clicks inside ImGui's double-click time: both pairs land before the next frame
        (ImGui's queue gives each its own frame, and a double click needs them close in time,
        so this one is timed by the editor, not paced)."""
        if point is not None:
            await self._pointer(point)
        await self._clear_double_click()
        self.xdo("click", "--repeat", "2", "--delay", "40", button)
        await self.frames(4)
        self.last_release = time.monotonic()

    async def drag(self, start: tuple[int, int], end: tuple[int, int], steps: int = 12,
                   release: bool = True, button: str = "1", modifier: str | None = None) -> None:
        """Press at start, move to end a frame per step, release (unless release=False; a
        modifier is then left down too)."""
        await self._modifier_down(modifier)
        await self.press(start, button)
        for k in range(1, steps + 1):
            self.xdo("mousemove", str(int(start[0] + (end[0] - start[0]) * k / steps)),
                     str(int(start[1] + (end[1] - start[1]) * k / steps)))
            await self.frames(1)
        await self.frames()
        if release:
            await self.release(button)
            await self._modifier_up(modifier)

    async def drag_to(self, start: tuple[int, int], dx: float, dy: float, steps: int = 12) -> None:
        """Moves a held button from start by (dx, dy), a frame per step (press first)."""
        for k in range(1, steps + 1):
            self.xdo("mousemove", str(int(start[0] + dx * k / steps)), str(int(start[1] + dy * k / steps)))
            await self.frames(1)
        await self.frames()

    async def wheel(self, point: tuple[int, int] | None, notches: int, modifier: str | None = None) -> None:
        """notches > 0 scrolls down (button 5), < 0 up (button 4); one notch a frame."""
        if point is not None:
            await self._pointer(point)
        if modifier:
            self.xdo("keydown", modifier)
            await self.frames(1)
        for _ in range(abs(notches)):
            self.xdo("click", "5" if notches > 0 else "4")
            await self.frames(1)
        if modifier:
            self.xdo("keyup", modifier)
        await self.frames()

    # -- keyboard ------------------------------------------------------------------------------

    async def key(self, *keys: str) -> None:
        """Keys and chords ("ctrl+z", "Escape"): a chord's modifiers go down first and up last,
        frames apart (`xdotool key ctrl+z` can land Z's press and Ctrl's release in one frame,
        where Ctrl reads up)."""
        for key in keys:
            *mods, main = key.split("+")
            for mod in mods:
                self.xdo("keydown", mod)
            if mods:
                await self.frames(1)
            self.xdo("key", main)
            await self.frames(1)
            for mod in reversed(mods):
                self.xdo("keyup", mod)
            await self.frames()

    async def type(self, text: str) -> None:
        self.xdo("type", "--delay", "40", text)
        await self.frames(max(SETTLE_FRAMES, 2 + len(text) // 8))

    async def release_all(self) -> None:
        """Lets go of every button and modifier a failed test may have left down."""
        for button in ("1", "2", "3"):
            self.xdo("mouseup", button)
        for mod in ("ctrl", "shift", "alt", "super"):
            self.xdo("keyup", mod)
        self.buttons_down.clear()
        await self.frames()


def for_editor(ed: Editor) -> Ui:
    """The editor's Ui, made on first use (tests with function helpers share one per editor)."""
    ui = getattr(ed, "_ui_input", None)
    if ui is None:
        ui = Ui(ed)
        ed._ui_input = ui
    return ui
