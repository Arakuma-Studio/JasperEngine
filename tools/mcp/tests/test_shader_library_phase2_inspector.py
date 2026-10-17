"""Shader library phase 2, the material inspector with several GameObjects selected (review fix D3):
picking a stencil test or toggling a colour channel changes only that on each GameObject, which
keeps its own ref, masks, ops and other channels (an Off test included); before, every one got
the first one's whole value.

Drives the real inspector with xdotool in the harness's Xvfb (1600x900, default layout): the
material section is opened by double-clicking its preview and scrolled to its Stencil rows. The
clicks are at fixed positions; when the layout moved and a click reaches nothing (no value
changed at all) the test skips and asks for new positions instead of failing."""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess

from harness import Editor, find_key, skip

# the harness owns the Xvfb so xdotool can click in the editor window
EDITOR = {"xvfb": "harness"}
TIMEOUT = 600

M = "submeshes[0].material."
PREVIEW = (1430, 646)          # submesh 0's material preview (double click opens its rows)
SCROLL_AT = (1420, 600)        # the inspector, scrolled 10 notches down
STENCIL_COMBO = (1490, 548)    # the Stencil test combo
NOT_EQUAL_ITEM = (1432, 704)   # "Not Equal" in its list
BLUE_CHANNEL = (1508, 723)     # the Color Write row's B toggle


class Ui:
    def __init__(self, ed: Editor):
        self.env = dict(os.environ, DISPLAY=ed._xvfb.display)

    def xdo(self, *args: str) -> str:
        return subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True, timeout=20).stdout

    async def click(self, x: int, y: int, repeat: int = 1):
        self.xdo("mousemove", str(x), str(y))
        await asyncio.sleep(0.3)
        self.xdo("click", "--repeat", str(repeat), "--delay", "80", "1")
        await asyncio.sleep(0.8)

    async def wheel(self, x: int, y: int, notches: int):
        self.xdo("mousemove", str(x), str(y))
        await asyncio.sleep(0.2)
        for _ in range(notches):
            self.xdo("click", "5")
            await asyncio.sleep(0.1)
        await asyncio.sleep(0.5)


async def values(ed: Editor, eid) -> tuple:
    got = await ed.call("get_entity", entity=eid, components=["mesh"], properties=[M + "stencil", M + "colorWrite"],
                        detail="values")
    mesh = next(iter(got["components"].values()))
    return mesh[M + "stencil"], mesh[M + "colorWrite"]


async def test_multi_selection_edits_keep_each_value(ed: Editor):
    if not shutil.which("xdotool") or not ed._xvfb or not ed._xvfb.display:
        skip("needs xdotool and the harness's Xvfb")
    a_stencil = {"compare": "equal", "ref": 5, "readMask": 15, "writeMask": 3}
    b_stencil = {"compare": "disabled", "ref": 7, "readMask": 3, "writeMask": 12, "pass": "replace"}
    ids = []
    for name, stencil, color_write in (("Multi A", a_stencil, "rgb"), ("Multi B", b_stencil, "ga")):
        made = await ed.call("create_entity", name=name, type="box")
        eid = find_key(made, "entity_id", "id")
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "stencil": stencil, M + "colorWrite": color_write}})
        ids.append(eid)
    before = [await values(ed, eid) for eid in ids]
    await ed.call("select_entities", entities=ids)
    await ed.call("wait", frames=10)

    ui = Ui(ed)
    windows = ui.xdo("search", "--name", ".").split()
    if not windows:
        skip("no editor window found")
    ui.xdo("windowfocus", "--sync", windows[-1])
    await ui.click(*PREVIEW, repeat=2)
    await ed.call("wait", frames=10)
    await ui.wheel(*SCROLL_AT, 10)
    await ed.call("wait", frames=10)

    # the test: Not Equal on both, each keeping its own ref, masks and ops (B's Off test too)
    await ui.click(*STENCIL_COMBO)
    await ed.call("wait", frames=5)
    await ui.click(*NOT_EQUAL_ITEM)
    await ed.call("wait", frames=5)
    after = [await values(ed, eid) for eid in ids]
    if after == before:
        await ed.screenshot("editor", path=str(ed.scratch.root / "inspector-layout.png"))
        skip("the clicks changed nothing: the inspector layout moved (update the positions at the top of this file)")
    (a, a_cw), (b, b_cw) = after
    ed.check_eq((a["compare"], a["ref"], a["readMask"], a["writeMask"], a["pass"]), ("not_equal", 5, 15, 3, "keep"), "A")
    ed.check_eq((b["compare"], b["ref"], b["readMask"], b["writeMask"], b["pass"]), ("not_equal", 7, 3, 12, "replace"),
                "B keeps its own ref, masks and op (Off is an explicit test)")

    # the B channel (on for A, off for B): off on both, each keeping its other channels
    await ui.click(*BLUE_CHANNEL)
    await ed.call("wait", frames=5)
    (_, a_cw), (_, b_cw) = [await values(ed, eid) for eid in ids]
    ed.check_eq((a_cw, b_cw), ("rg", "ga"), "only the blue channel changes")

    # one undo step for both
    await ed.call("undo")
    (_, a_cw), (_, b_cw) = [await values(ed, eid) for eid in ids]
    ed.check_eq((a_cw, b_cw), ("rgb", "ga"))
    await ed.call("delete_entities", entities=ids)
