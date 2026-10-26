"""S25 by real input: xdotool keys in the harness's Xvfb, checked through MCP (no pixel offsets).

- The Search palette: Ctrl+K opens it, typed text filters it (prefixes included), Enter runs
  the selected entry (the Console gets the focus), Esc closes it.
- The one dispatcher: Ctrl+Shift+C focuses the Console from the Hierarchy; after set_shortcut
  rebinds it, the old chord does nothing and the new one works; keys typed into the palette's
  field never run commands.
- Edit > Shortcuts... (AC8): capturing Ctrl+S on Duplicate does not save the scene, the conflict
  modal appears, and Cancel leaves both bindings as they were.

Widgets are found through editor_status (windows.focused, ui.search_palette) and find_ui_item
(where a control is drawn now), never by fixed pixel offsets.
"""

from __future__ import annotations

import asyncio
import os
import subprocess

from harness import Editor, skip

EDITOR = {"xvfb": "harness", "create": True, "name": "Search UI"}
TIMEOUT = 600


class Keys:
    def __init__(self, ed: Editor):
        if not getattr(ed, "_xvfb", None):
            skip("needs the harness's Xvfb")
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)
        windows = self.xdo("search", "--name", "JasperEngine").split() or self.xdo("search", "--name", ".").split()
        if not windows:
            skip("no editor window found")
        self.window = windows[-1]
        self.ed = ed
        x, y = 0, 0
        for line in self.xdo("getwindowgeometry", self.window).splitlines():
            if "Position:" in line:
                x, y = (int(v) for v in line.split("Position:")[1].split("(")[0].strip().split(","))
        self.origin = (x, y)

    async def click_item(self, label: str, window: str | None = None):
        """a real click at the centre of where find_ui_item says the control is drawn"""
        for _ in range(30):
            args = {"label": label} | ({"window": window} if window else {})
            visible = [i for i in (await self.ed.call("find_ui_item", **args))["items"] if i["visible"]]
            if visible:
                rect = visible[0]["rect"]
                at = (int(self.origin[0] + rect["x"] + rect["width"] / 2), int(self.origin[1] + rect["y"] + rect["height"] / 2))
                self.xdo("mousemove", str(at[0]), str(at[1]))
                await asyncio.sleep(0.3)
                # press and release frames apart: in one frame the widget never sees the press
                self.xdo("mousedown", "1")
                await asyncio.sleep(0.15)
                self.xdo("mouseup", "1")
                await asyncio.sleep(0.5)
                return
            await self.ed.call("wait", frames=5)
        self.ed.fail(f"find_ui_item: no visible {label!r}" + (f" in {window!r}" if window else ""))

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    def focus(self):
        self.xdo("windowactivate", "--sync", self.window)
        self.xdo("windowfocus", "--sync", self.window)

    async def key(self, *keys: str):
        for key in keys:
            # modifiers down first and up last, frames apart (one frame could see Ctrl up)
            *mods, main = key.split("+")
            for mod in mods:
                self.xdo("keydown", mod)
            if mods:
                await asyncio.sleep(0.1)
            self.xdo("key", main)
            if mods:
                await asyncio.sleep(0.1)
            for mod in reversed(mods):
                self.xdo("keyup", mod)
            await asyncio.sleep(0.3)

    async def type(self, text: str):
        self.xdo("type", "--delay", "60", text)
        await asyncio.sleep(0.4)


async def status(ed: Editor) -> dict:
    return await ed.call("editor_status")


async def wait_for(ed: Editor, predicate, what: str, tries: int = 25) -> dict:
    last = {}
    for _ in range(tries):
        last = await status(ed)
        if predicate(last):
            return last
        await asyncio.sleep(0.2)
    ed.fail(f"timed out waiting for {what}: windows {last.get('windows')}, palette {last.get('ui', {}).get('search_palette')}")
    return last


def palette(s: dict) -> dict:
    return s["ui"]["search_palette"]


async def test_palette_by_keyboard(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    await ed.call("focus_window", window="hierarchy")
    await wait_for(ed, lambda s: s["windows"]["focused"] == "Hierarchy", "the Hierarchy focused")

    await keys.key("ctrl+k")
    await wait_for(ed, lambda s: palette(s)["open"], "the palette to open")
    await keys.type("console")
    s = await wait_for(ed, lambda s: palette(s)["query"] == "console" and palette(s)["results"], "results for 'console'")
    ed.check(any("Console" in r for r in palette(s)["results"][:3]), f"the Console near the top: {palette(s)['results'][:5]}")
    # the typed letters ran nothing (c, o, n... are no commands while the field has the keys)
    ed.check_eq(s["windows"]["open"].get("Console"), True, "the Console is open (default layout)")

    # Enter runs the selected entry; select the Console's first
    results = palette(s)["results"]
    target = next(i for i, r in enumerate(results) if "Console" in r)
    for _ in range(target - palette(s)["selected"]):
        await keys.key("Down")
    await keys.key("Return")
    s = await wait_for(ed, lambda s: not palette(s)["open"] and s["windows"]["focused"] == "Console", "Enter to run it")

    # a prefix keeps one kind; Esc closes
    await keys.key("ctrl+k")
    await wait_for(ed, lambda s: palette(s)["open"], "the palette to open again")
    await keys.type(">dup")
    s = await wait_for(ed, lambda s: palette(s)["query"] == ">dup" and palette(s)["results"], "results for '>dup'")
    ed.check(all(r.startswith("command: ") for r in palette(s)["results"]), f"commands only: {palette(s)['results']}")
    ed.check(palette(s)["results"][0] == "command: Duplicate", f"Duplicate first: {palette(s)['results'][:3]}")
    await keys.key("Escape")
    await wait_for(ed, lambda s: not palette(s)["open"], "Esc to close it")


async def test_dispatcher_follows_rebinding(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    await ed.call("focus_window", window="hierarchy")
    await wait_for(ed, lambda s: s["windows"]["focused"] == "Hierarchy", "the Hierarchy focused")
    await keys.key("ctrl+shift+c")
    await wait_for(ed, lambda s: s["windows"]["focused"] == "Console", "Ctrl+Shift+C to focus the Console")

    await ed.call("set_shortcut", id="window.console", chords=["Ctrl+Alt+K"])
    try:
        await ed.call("focus_window", window="hierarchy")
        await wait_for(ed, lambda s: s["windows"]["focused"] == "Hierarchy", "the Hierarchy focused")
        await keys.key("ctrl+shift+c")
        for _ in range(4):
            s = await status(ed)
        ed.check_eq(s["windows"]["focused"], "Hierarchy", "the old chord does nothing")
        await keys.key("ctrl+alt+k")
        await wait_for(ed, lambda s: s["windows"]["focused"] == "Console", "the new chord")
    finally:
        await ed.call("set_shortcut", id="window.console", chords=None)


async def test_shortcut_capture_conflict_cancel(ed: Editor):
    keys = Keys(ed)
    keys.focus()
    made = await ed.call("create_scene", name="Capture", type="3D", path="scenes/Capture")
    scene = made["scene"]["id"]
    await ed.call("select_scene", scene=scene)
    await ed.call("save_scene", scene=scene, path="scenes/Capture.scene")
    await ed.call("create_entity", scene=scene, type="box", name="Unsaved")
    ed.check(scene in (await status(ed))["unsaved"]["scene_ids"], "the scene has unsaved changes")

    await ed.call("run_command", id="edit.shortcuts")
    await keys.click_item("##ShortcutSearch", "Shortcuts")
    await keys.type("duplicate")
    await keys.click_item("Ctrl+D", "Shortcuts")          # Duplicate's binding: capture
    await keys.key("ctrl+s")                              # captured, not run
    s = await status(ed)
    ed.check(scene in s["unsaved"]["scene_ids"], "Ctrl+S during the capture did not save")
    await keys.key("Return")                              # keep it: Save holds it
    await keys.click_item("Cancel", "Shortcut in Use")
    for _ in range(3):
        s = await status(ed)
    ed.check(scene in s["unsaved"]["scene_ids"], "still not saved")
    listed = {i["id"]: i for i in (await ed.call("list_shortcuts"))["shortcuts"]}
    ed.check_eq(listed["edit.duplicate"]["chords"], ["Ctrl+D"], "Cancel keeps Duplicate's binding")
    ed.check_eq(listed["file.save"]["chords"], ["Ctrl+S"], "and Save's")
    await keys.key("Escape")
