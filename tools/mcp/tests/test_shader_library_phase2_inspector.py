"""Shader library phase 2, the material inspector with several GameObjects selected (review fix D3):
picking a stencil test or toggling a colour channel changes only that on each GameObject, which
keeps its own ref, masks, ops and other channels (an Off test included); before, every one got
the first one's whole value.

Drives the real inspector with xdotool in the harness's Xvfb: the material section is opened with
its arrow button and the Stencil rows are clicked where the editor draws them now (find_ui_item,
scrolled into view with the wheel), so a layout change does not move the clicks off them."""

from __future__ import annotations

from harness import Editor, find_key
import ui_input

# the harness owns the Xvfb so xdotool can click in the editor window
EDITOR = {"xvfb": "harness", "create": True, "name": "Multi Selection Inspector"}
CI = True
TIMEOUT = 600

M = "submeshes[0].material."
INSPECTOR = "Inspector"
MATERIAL_TOGGLE = "##toggle_mesh"         # submesh 0's material row: opens its rows (as a double click on the preview)
STENCIL_COMBO = "##stencil_compare"       # the Stencil test combo
BLUE_CHANNEL = "B##color_write_B"         # the Color Write row's B toggle


async def values(ed: Editor, eid) -> tuple:
    got = await ed.call("get_entity", entity=eid, components=["mesh"], properties=[M + "stencil", M + "colorWrite"],
                        detail="values")
    mesh = next(iter(got["components"].values()))
    return mesh[M + "stencil"], mesh[M + "colorWrite"]


async def test_multi_selection_edits_keep_each_value(ed: Editor):
    a_stencil = {"compare": "equal", "ref": 5, "readMask": 15, "writeMask": 3}
    b_stencil = {"compare": "disabled", "ref": 7, "readMask": 3, "writeMask": 12, "pass": "replace"}
    ids = []
    for name, stencil, color_write in (("Multi A", a_stencil, "rgb"), ("Multi B", b_stencil, "ga")):
        made = await ed.call("create_entity", name=name, type="box")
        eid = find_key(made, "entity_id", "id")
        await ed.call("set_properties", entity=eid, values={"mesh": {M + "stencil": stencil, M + "colorWrite": color_write}})
        ids.append(eid)
    await ed.call("select_entities", entities=ids)
    await ed.call("wait", frames=10)

    ui = ui_input.Ui(ed)               # skips without xdotool or the harness's display
    await ed.call("focus_window", window="inspector")
    ui.focus()
    await ui.frames()
    if not (await ed.call("find_ui_item", label=STENCIL_COMBO, window=INSPECTOR))["items"]:
        await ui.click_revealed(MATERIAL_TOGGLE, INSPECTOR)

    # the test: Not Equal on both, each keeping its own ref, masks and ops (B's Off test too)
    await ui.click_revealed(STENCIL_COMBO, INSPECTOR)
    await ui.click_revealed("Not Equal", "##Combo")     # in the open combo's popup
    after = [await values(ed, eid) for eid in ids]
    (a, a_cw), (b, b_cw) = after
    ed.check_eq((a["compare"], a["ref"], a["readMask"], a["writeMask"], a["pass"]), ("not_equal", 5, 15, 3, "keep"), "A")
    ed.check_eq((b["compare"], b["ref"], b["readMask"], b["writeMask"], b["pass"]), ("not_equal", 7, 3, 12, "replace"),
                "B keeps its own ref, masks and op (Off is an explicit test)")

    # the B channel (on for A, off for B): off on both, each keeping its other channels
    await ui.click_revealed(BLUE_CHANNEL, INSPECTOR)
    (_, a_cw), (_, b_cw) = [await values(ed, eid) for eid in ids]
    ed.check_eq((a_cw, b_cw), ("rg", "ga"), "only the blue channel changes")

    # one undo step for both
    await ed.call("undo")
    (_, a_cw), (_, b_cw) = [await values(ed, eid) for eid in ids]
    ed.check_eq((a_cw, b_cw), ("rgb", "ga"))
    await ed.call("delete_entities", entities=ids)
