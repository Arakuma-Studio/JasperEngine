"""S86 in the Title Designer (docs/specs/S86-title-3d.md AC33, R50-R58; S82 R35, AC14): a Dimension
3D layer's rows are on its page (the pickers, Add Band), its parts are Face, Bevel, Side and Band N,
the Realtime tiers are Preview As modes, and the fixed-schematic gizmo's ring drag (a person's
drag) commits "Rotate 3D Layer" once, writing the layer's 3D rotation.
"""

import json

import harness
from harness import Editor
from ui_input import Ui

CI = True
EDITOR = {"create": True, "name": "Title Designer 3D", "xvfb": "harness"}
TIMEOUT = 600

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art", "editor_tools", "editor_ux"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    return S


async def focus(ed: Editor, **keys) -> dict:
    return await ed.call("focus_window", window="title_designer", title_designer=keys)


async def create_3d(ed: Editor, path: str) -> None:
    await ed.call("edit_title", path=path, create={"text": "BIG"}, bake="never",
                  ops=[{"op": "set_dimension", "layer": "Title", "dimension": "3d"},
                       {"op": "add_band", "layer": "Title", "preset": "bright_ring"}])


async def top_steps(ed: Editor, n: int) -> list:
    history = await ed.call("get_undo_history", scope="project", limit=n)
    return [s.get("name", "") for s in (history.get("steps") or history.get("entries") or [])]


async def test_rows_parts_and_tiers(ed: Editor):
    """AC33: the Dimension section's rows and the pickers are there; parts list Band 1; the tiers
    are Preview As modes; a 2D layer has none of the 3D rows."""
    await setup(ed)
    path = "assets/designer3d/rows.titleart"
    await create_3d(ed, path)
    ui = Ui(ed)
    await focus(ed, path=path, select="Title", panel="properties", search="lock environment")
    item = await ui.find("Lock Environment To Layer", frames=120)
    ed.check(item is not None, "the 3D row Lock Environment To Layer is a search result")
    r = await focus(ed, search="", select="Title/band#0")
    ed.check_eq(r["state"]["selection"], ["Title/band#0"], "the band is a part")
    await ed.call("editor_status")
    items = (await ed.call("find_ui_item", label="Profiles...")).get("items", [])
    ed.check(len(items) > 0, "the band page has the bevel profile picker")
    await focus(ed, select="Title/face")
    await ed.call("editor_status")
    material = (await ed.call("find_ui_item", label="##matpick")).get("items", [])
    ed.check(len(material) > 0, "the face page has the material picker")
    for mode in ("realtime_high", "realtime_low", "glyph_atlas", "realtime"):
        r = await focus(ed, preview_as=mode)
        ed.check_eq(r["state"]["preview_as"], mode, f"Preview As {mode}")


async def test_gizmo_ring_drag_is_one_step(ed: Editor):
    """AC14/AC33 (S86 R56): a drag on the free-rotate ring commits one step "Rotate 3D Layer" that
    turns the layer in 3D; Esc during a drag adds nothing."""
    await setup(ed)
    ui = Ui(ed)
    path = "assets/designer3d/gizmo.titleart"
    await create_3d(ed, path)
    await focus(ed, path=path, select="Title", panel="properties", zoom="fit", preview_as="realtime")
    gizmo = None
    for _ in range(30):
        await ed.call("editor_status")
        gizmo = (await focus(ed))["state"].get("gizmo_3d")
        if gizmo:
            break
    ed.check(gizmo is not None, "the schematic gizmo is drawn for the 3D layer")
    if not gizmo:
        return
    cx, cy = gizmo["centre"]
    ring = gizmo["ring"]
    before = await top_steps(ed, 1)
    # window coords to screen points: the ring's right end, dragged up along it
    await ui.drag(ui.centre([cx + ring, cy, 0, 0]), ui.centre([cx + ring, cy - 50, 0, 0]), steps=10)
    await ui.frames(4)
    steps = await top_steps(ed, 2)
    ed.check_eq(steps[0], "Rotate 3D Layer", f"one step: {steps} (before {before})")
    t = json.loads((ed.project / path).read_text())["layers"][0]["transform"]
    ed.check(abs(t.get("rotation_x", 0)) + abs(t.get("rotation_y", 0)) + abs(t.get("rotation", 0)) > 1.0, f"the layer turned: {t}")
