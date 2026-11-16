"""The Hierarchy's timed refresh (S98 E11, REQ-31): with the structure unchanged it fills the rows of
the last full rebuild again in place instead of building the tree anew. A name written without a
command (Lua in Edit mode, not undoable, so no structural change) still shows in its row."""

from __future__ import annotations

from harness import Editor

CI = True
EDITOR = {"xvfb": "harness", "create": True, "name": "Hierarchy Refresh", "ui_probe": True}


async def wait_item(ed: Editor, name: str, frames: int = 300) -> None:
    for _ in range(frames // 5):
        if ed.ui_items().get(name):
            return
        await ed.call("wait", frames=5)
    ed.fail(f"{name} is not drawn: {sorted(k for k in ed.ui_items() if k.startswith('hierarchy.node.'))}")


async def test_name_written_without_a_command_shows(ed: Editor):
    scene = (await ed.call("create_scene", name="Refresh", type="3D"))["scene"]["id"]
    for name in ("Before", "RefreshMe", "After"):
        await ed.call("create_entity", scene=scene, type="cube", name=name, select=False)
    await ed.call("focus_window", window="hierarchy")
    await wait_item(ed, "hierarchy.node.RefreshMe")
    renamed = await ed.call("eval_lua", allow_edit_mode=True,
                            code='local e = scene:findEntity("RefreshMe"); GameObject(scene, e).name = "Refreshed"; return e')
    ed.check(renamed["values"][0], f"the GameObject was renamed: {renamed}")
    await wait_item(ed, "hierarchy.node.Refreshed")
    ed.check(not ed.ui_items().get("hierarchy.node.RefreshMe"), "the old name's row is gone")
    for name in ("Before", "After"):
        ed.check(ed.ui_items().get(f"hierarchy.node.{name}"), f"the row {name} stays")
