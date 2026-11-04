"""The Inspector's checkboxes run script callbacks while playing (script-lifecycle 8.1): a script's
enabled checkbox and the GameObject's Active checkbox, clicked like a human with xdotool, fire
onEnable / onDisable, also when those instantiate many GameObjects (the component arrays move)
or destroy one at once, without crashing the editor (the Inspector runs these commands after its
drawing). Widgets are found through the editor's test-only locator (Editor(ui_probe=True)).
"""

from __future__ import annotations

import asyncio
import os
import subprocess

from harness import Editor

EDITOR = {"xvfb": "harness", "create": True, "name": "Lifecycle Inspector", "ui_probe": True}
CI = True
TIMEOUT = 600

TOGGLE = """local Toggle = {}
function Toggle:onEnable()
    if self.gameObject.name ~= "Host" then return end
    print("toggle enable")
    if self.enabledBefore then
        for i = 1, 64 do GameObject.instantiate(self.gameObject) end
    end
    self.enabledBefore = true
end
function Toggle:onDisable()
    if self.gameObject.name ~= "Host" then return end
    print("toggle disable")
    local victim = GameObject.find("Victim")
    if victim then GameObject.destroyImmediate(victim) end
end
return Toggle
"""


class Ui:
    def __init__(self, ed: Editor):
        self.ed = ed
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    async def focus(self):
        windows = self.xdo("search", "--name", ".").split()
        self.ed.check(windows, "the editor window")
        self.xdo("windowfocus", "--sync", windows[-1])

    async def click_item(self, name: str):
        rect = None
        for _ in range(40):
            await self.ed.call("wait", frames=2)
            rect = self.ed.ui_items().get(name)
            if rect:
                break
        self.ed.check(rect, f"{name} is drawn: {sorted(self.ed.ui_items())}")
        x, y = (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0
        self.xdo("mousemove", str(int(x)), str(int(y)))
        await asyncio.sleep(0.3)
        self.xdo("click", "1")
        await asyncio.sleep(0.6)
        await self.ed.call("wait", frames=3)


def toggles(entries: list[dict]) -> list[str]:
    return [e["message"] for e in entries if e["message"].startswith("toggle ")]


async def test_inspector_toggles_run_callbacks_safely(ed: Editor):
    ui = Ui(ed)
    host = (await ed.call("create_entity", name="Host", type="empty"))["entity_id"]
    await ed.call("create_entity", name="Victim", type="empty")
    await ed.call("create_script", kind="lua", name="Toggle", content=TOGGLE, entity=host)
    await ed.console_since(None)
    await ed.call("play", action="start", fixed_fps=60)
    try:
        await ed.call("wait", frames=3)
        ed.check_eq(toggles(await ed.console_since()), ["toggle enable"])
        before = len((await ed.call("list_runtime_entities"))["entities"])
        await ed.call("select_entities", entities=[host])
        await ui.focus()

        await ui.click_item("inspector.scriptEnabled.0")      # off: onDisable destroys Victim at once
        ed.check_eq(toggles(await ed.console_since()), ["toggle disable"])
        live = await ed.call("get_runtime_entity", entity="Host")
        ed.check_eq(live["scripts"][0]["enabled"], False)
        await ui.click_item("inspector.scriptEnabled.0")      # on: onEnable instantiates 64 copies
        lines = toggles(await ed.console_since())
        ed.check(lines and lines[0] == "toggle enable", f"onEnable from the checkbox: {lines}")
        after = len((await ed.call("list_runtime_entities"))["entities"])
        ed.check(after >= before + 63, f"the copies exist ({before} -> {after})")

        await ui.click_item("inspector.active")               # Active off: onDisable of the host
        lines = toggles(await ed.console_since())
        ed.check("toggle disable" in lines, f"onDisable from the Active checkbox: {lines}")
        state = await ed.call("eval_lua", code="return object('Host').activeSelf")
        ed.check_eq(state["values"], [False], "the Active checkbox deactivated the host")
        await ed.check_alive()
    finally:
        await ed.call("play", action="stop", fixed_fps=0)
    await ed.check_alive()
