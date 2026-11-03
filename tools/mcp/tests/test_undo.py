"""S24 undo (docs/specs/S24-undo.md): named steps, the one timeline, agent attribution and folding,
get_undo_history, undo/redo to_id and expect_top_id, linked batch groups, selection steps and
no-op edits.

Runs in one new empty project; tests run in order and share the editor.
"""

import uuid

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Undo CI"}

TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> dict:
    """A 3D scene of its own, selected; the client's name as the editor shows it."""
    if S.get("ed") is ed:
        await ed.call("select_scene", scene=S["scene"])
        return S
    S.clear()
    S["ed"] = ed
    made = await ed.call("create_scene", name="UndoLab", type="3D", path="scenes/UndoLab")
    S["scene"] = made["scene"]["id"]
    await ed.call("select_scene", scene=S["scene"])
    status = await ed.call("editor_status")
    sessions = status["mcp"]["sessions"]
    ed.check(sessions, f"no MCP session listed: {status['mcp']}")
    client = (sessions[0].get("client") or "").strip()
    S["client"] = client[:32] if client else "Agent"
    return S


async def history(ed: Editor, **args) -> dict:
    return await ed.call("get_undo_history", scene=S["scene"], **args)


async def box(ed: Editor, name: str) -> int:
    made = await ed.call("create_entity", scene=S["scene"], type="box", name=name)
    return made["entity_id"]


async def test_names_and_menu_label(ed: Editor):
    """AC1: set_transform is "Move <name>", the agent's name in front; the Edit menu label is
    "Undo " + that name (editor_status.undo, read from the same calls as the menu)."""
    s = await setup(ed)
    name = "Mover_" + uuid.uuid4().hex[:6]
    eid = await box(ed, name)
    moved = await ed.call("set_transform", scene=s["scene"], entity=eid, position=[3, 0, 0])
    undo = moved.get("undo")
    ed.check(undo and undo.get("id"), f"set_transform result has no undo {{id, name}}: {moved}")
    ed.check_eq(undo["name"], f"{s['client']}: Move {name}", "step name")

    got = await history(ed, limit=5)
    top = got["entries"][0]
    ed.check_eq(top["id"], undo["id"], "the newest step is the call's")
    ed.check_eq(top["name"], f"{s['client']}: Move {name}", "history name")
    ed.check_eq(top["origin"]["kind"], "agent", "origin")
    ed.check_eq(top["origin"].get("tool"), "set_transform", "origin tool")

    status = await ed.call("editor_status")
    ed.check_eq(status["undo"]["undo_name"], top["name"], "editor_status.undo.undo_name")
    ed.check_eq(status["undo"]["undo_label"], "Undo " + top["name"], "the Edit menu label")


async def test_noop_edit_makes_no_step(ed: Editor):
    """AC3: set_properties to the value it already has leaves the history as it was."""
    s = await setup(ed)
    eid = await box(ed, "Same_" + uuid.uuid4().hex[:6])
    await ed.call("set_properties", scene=s["scene"], entity=eid, values={"transform": {"position": [1, 2, 3]}})
    before = await history(ed, limit=50, include_selection=True)
    again = await ed.call("set_properties", scene=s["scene"], entity=eid, values={"transform": {"position": [1, 2, 3]}})
    after = await history(ed, limit=50, include_selection=True)
    ed.check_eq([e["id"] for e in after["entries"]], [e["id"] for e in before["entries"]], "no new step")
    ed.check("undo" not in again, f"a call that made no step has no undo field: {again}")


async def test_history_tool(ed: Editor):
    """AC11: ids newest first, applied flags, agent origin with the client prefix; undo to_id
    stops at the step; a stale expect_top_id changes nothing; edit results carry undo.id."""
    s = await setup(ed)
    ids = []
    for i in range(3):
        made = await ed.call("create_entity", scene=s["scene"], type="empty", name=f"Hist{i}_" + uuid.uuid4().hex[:4])
        ed.check(made.get("undo", {}).get("id"), f"create_entity result lacks undo.id: {made}")
        ids.append(made["undo"]["id"])
    got = await history(ed, limit=10)
    listed = [e["id"] for e in got["entries"]]
    ed.check(listed[:3] == list(reversed(ids)), f"newest first: {listed[:3]} vs {list(reversed(ids))}")
    ed.check(all(e["applied"] for e in got["entries"][:3]), "applied")
    ed.check(all(e["name"].startswith(s["client"] + ": ") for e in got["entries"][:3]), "client prefix")
    ed.check(got["memory"]["used"] > 0 and got["memory"]["limit"] > 0, f"memory: {got['memory']}")

    # a stale expect_top_id: refused, nothing changed
    r = await ed.call_raw("undo", {"scope": "timeline", "scene": s["scene"], "expect_top_id": ids[0]})
    ed.check(r.is_error, f"stale expect_top_id should fail: {r.text}")
    same = await history(ed, limit=10)
    ed.check_eq([e["applied"] for e in same["entries"][:3]], [True, True, True], "nothing undone")

    # to_id: undo until ids[0] is the newest applied step
    done = await ed.call("undo", scope="timeline", scene=s["scene"], to_id=ids[0])
    ed.check_eq([u["id"] for u in done["undone"]], [ids[2], ids[1]], "the two newer steps")
    now = await history(ed, limit=10)
    flags = {e["id"]: e["applied"] for e in now["entries"]}
    ed.check(flags[ids[0]] and not flags[ids[1]] and not flags[ids[2]], f"applied flags: {flags}")
    # expect_top_id right: one undo of exactly that step
    one = await ed.call("undo", scope="timeline", scene=s["scene"], expect_top_id=ids[0])
    ed.check_eq([u["id"] for u in one["undone"]], [ids[0]], "expect_top_id undo")
    # redo to_id
    back = await ed.call("redo", scope="timeline", scene=s["scene"], to_id=ids[2])
    ed.check_eq([u["id"] for u in back["redone"]], ids, "redo to_id")


async def test_attribution_folding(ed: Editor):
    """AC12: one set_properties on three GameObjects across two components is one step named
    "<client>: Set Properties (3 Objects)"."""
    s = await setup(ed)
    eids = [await box(ed, f"Fold{i}_" + uuid.uuid4().hex[:4]) for i in range(3)]
    before = await history(ed, limit=100)
    r = await ed.call("set_properties", scene=s["scene"], entities=eids,
                      values={"transform": {"position": [5, 5, 5]}, "mesh": {"castShadows": False}})
    after = await history(ed, limit=100)
    new = [e for e in after["entries"] if e["id"] not in {b["id"] for b in before["entries"]}]
    ed.check_eq(len(new), 1, f"one step: {new}")
    ed.check_eq(new[0]["name"], f"{s['client']}: Set Properties (3 Objects)", "folded name")
    ed.check_eq(r.get("undo", {}).get("id"), new[0]["id"], "the result names it")


async def test_focus_independent(ed: Editor):
    """AC8: a scene edit, then an asset rename, then the Scene view focused: the next undo names
    the rename; undo scope=timeline reverts it and leaves the scene edit applied."""
    s = await setup(ed)
    eid = await box(ed, "Focus_" + uuid.uuid4().hex[:6])
    await ed.call("set_transform", scene=s["scene"], entity=eid, position=[7, 0, 0])
    stem = "focus_" + uuid.uuid4().hex[:6]
    await ed.call("write_file", path=f"assets/{stem}.txt", content="hello")
    moved = await ed.call("move_asset", path=f"assets/{stem}.txt", to=f"assets/{stem}_renamed.txt")
    await ed.call("focus_window", window="scene", scene=s["scene"])
    status = await ed.call("editor_status")
    ed.check("Rename File" in (status["undo"]["undo_name"] or ""), f"undo names the rename: {status['undo']}")
    ed.check_eq(status["undo"]["undo_label"], "Undo " + status["undo"]["undo_name"], "menu label")
    ed.check_eq((moved.get("undo") or {}).get("name"), status["undo"]["undo_name"], "move_asset result names it")

    done = await ed.call("undo", scope="timeline", scene=s["scene"])
    ed.check_eq(len(done["undone"]), 1, f"one step: {done}")
    ed.check_eq(done["undone"][0]["history"], "project", "an asset step")
    back = await ed.call_raw("read_file", {"path": f"assets/{stem}.txt"})
    ed.check(not back.is_error, f"the file has its old name back: {back.text[:200]}")
    gone = await ed.call_raw("read_file", {"path": f"assets/{stem}_renamed.txt"})
    ed.check(gone.is_error, "the renamed file is gone")
    pos = (await ed.call("get_entity", scene=s["scene"], entity=eid, components=["transform"]))["components"]["transform"]["position"]
    ed.check(abs(pos[0] - 7) < 1e-3, f"the scene edit stays applied: {pos}")


async def test_batch_undo_group_linked(ed: Editor):
    """AC7: batch undo_group with a create_entity and a create_material: one linked step per
    history; one undo scope=timeline reverts both; complete is true."""
    s = await setup(ed)
    name = "Batched_" + uuid.uuid4().hex[:6]
    material = f"materials/{name}.material"
    out = await ed.call("batch", calls=[
        {"tool": "create_entity", "arguments": {"scene": s["scene"], "type": "box", "name": name}},
        {"tool": "create_material", "arguments": {"path": material}},
    ], undo_group=True, undo_name="Make Batched")
    group = out["undo_group"]
    ed.check_eq(group["complete"], True, f"complete: {group}")
    ed.check(group.get("link_id"), f"the groups are linked: {group}")
    got = await history(ed, limit=5)
    ed.check_eq(got["entries"][0]["name"], "Make Batched", "undo_name")

    done = await ed.call("undo", scope="timeline", scene=s["scene"])
    ed.check_eq(len(done["undone"]), 2, f"one undo, both parts: {done}")
    found = await ed.call("find_entities", scene=s["scene"], name=name)
    ed.check(not found["entities"], f"the GameObject is gone: {found}")
    exists = await ed.call_raw("read_file", {"path": material})
    ed.check(exists.is_error, "the material file is gone")
    redone = await ed.call("redo", scope="timeline", scene=s["scene"])
    ed.check_eq(len(redone["redone"]), 2, f"one redo, both parts: {redone}")


async def test_select_entities_undo(ed: Editor):
    """AC9: select_entities is a selection step of the agent; undo scope=scene restores the
    previous selection."""
    s = await setup(ed)
    a = await box(ed, "SelA_" + uuid.uuid4().hex[:4])
    b = await box(ed, "SelB_" + uuid.uuid4().hex[:4])
    await ed.call("select_entities", scene=s["scene"], entities=[a])
    await ed.call("select_entities", scene=s["scene"], entities=[b])
    got = await history(ed, limit=3, include_selection=True)
    top = got["entries"][0]
    ed.check_eq(top["type"], "SelectionCmd", f"a selection step: {top}")
    ed.check(top["name"].startswith(s["client"] + ": Select "), f"name: {top}")
    ed.check_eq(top["origin"]["kind"], "agent", "the agent's")
    hidden = await history(ed, limit=3)
    ed.check(all(e["type"] != "SelectionCmd" for e in hidden["entries"]), "hidden without include_selection")
    await ed.call("undo", scope="scene", scene=s["scene"])
    selection = await ed.call("get_selection", scene=s["scene"])
    ed.check_eq([e["id"] for e in selection["entities"]], [a], "the previous selection is back")
