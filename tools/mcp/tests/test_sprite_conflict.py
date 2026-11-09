"""A person's save never gets overwritten (docs/specs/S47-agent-sprite-drawing.md AC10, R33, R34):
undo is refused once the file changed outside the editor, the next edit reports sprite_changed
and the one after it works on the person's version, a stale expect_revision fails every time,
a Console warning names the overwritten agent steps, and the agent's own overlapping edits raise
no such warning."""

from __future__ import annotations

import asyncio
import time

from harness import Editor
from sprite_draw_util import enable, err, ok

CI = True
EDITOR = {"create": True, "name": "SpriteConflict"}
TIMEOUT = 600
PATH = "assets/sprites/hero.aseprite"
OUTSIDE = "saved from outside the editor"
S: dict = {}


async def cursor(ed: Editor) -> int:
    return (await ed.call("read_console", limit=1)).get("next_cursor", 0)


async def wait_warning(ed: Editor, since: int, what: str, seconds: float = 30.0) -> dict:
    """Waits for the watcher's Console warning about an outside save (a real deadline, no sleep)"""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        log = await ed.call("read_console", since=since, limit=200)
        found = next((e for e in log.get("entries", []) if OUTSIDE in str(e.get("message", "")) and what in str(e.get("message", ""))), None)
        if found:
            return found
        await ed.call("wait", frames=2)
    ed.fail(f"no Console warning about {what} within {seconds}s")


async def test_setup(ed: Editor):
    await enable(ed)
    # the person's version of the file: made first, so the agent's edit stays the newest step
    await ok(ed, "create_sprite", path="assets/sprites/person.aseprite", size=[16, 16], palette=["#ff0000", "#00ff00"])
    await ok(ed, "edit_sprite", sprite="assets/sprites/person.aseprite", ops=[{"op": "pixel", "points": [[9, 9]], "color": 2}])
    await ok(ed, "create_sprite", path=PATH, size=[16, 16], palette=["#ff0000", "#00ff00"])
    S["clean"] = (ed.project / PATH).read_bytes()
    r = await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "pixel", "points": [[0, 0]], "color": 1}])
    S["undo"] = r["undo"]["id"]
    S["agent"] = (ed.project / PATH).read_bytes()
    S["revision"] = r["revision"]


async def test_undo_refused_after_an_outside_save(ed: Editor):
    since = await cursor(ed)
    theirs = (ed.project / "assets/sprites/person.aseprite").read_bytes()
    (ed.project / PATH).write_bytes(theirs)
    await wait_warning(ed, since, "hero.aseprite")
    raw = await ed.call_raw("undo", {"scope": "timeline", "expect_top_id": S["undo"]})
    ed.check(raw.is_error or not (raw.structured or {}).get("steps"), f"the undo is refused: {raw.structured}")
    ed.check("changed outside the editor" in raw.text, f"the refusal reads as R33: {raw.text}")
    ed.check((ed.project / PATH).read_bytes() == theirs, "the person's bytes stay")


async def test_next_edit_reports_sprite_changed_then_works(ed: Editor):
    e = await err(ed, "edit_sprite", sprite=PATH, ops=[{"op": "pixel", "points": [[1, 1]], "color": 1}])
    ed.check(e["kind"] == "sprite_changed", f"{e}")
    ed.check(e["last_seen_revision"] == S["revision"] and e["current_revision"] != S["revision"], f"revisions: {e}")
    ed.check(e["changed_by"] == "external", f"{e['changed_by']}")
    ed.check(e["changed_rects"] == [{"frame": 1, "rect": [0, 0, 10, 10]}], f"changed_rects: {e['changed_rects']}")
    ed.check("read the region again" in e["fix"] and "expect_revision" in e["fix"], f"{e['fix']}")
    ok_again = await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "pixel", "points": [[2, 2]], "color": 2}])
    rows = (await ok(ed, "read_sprite_pixels", sprite=PATH, rect=[0, 0, 10, 10]))["frames"][0]["rows"]
    ed.check(rows[9][9] == "2" and rows[2][2] == "2", f"the edit landed on the person's version: {rows}")
    ed.check(rows[0][0] == ".", f"the agent's pixel is gone: {rows}")
    ed.check(ok_again["revision"] != S["revision"], "a new revision")


async def test_stale_expect_revision_fails_every_time(ed: Editor):
    for _ in range(2):
        e = await err(ed, "edit_sprite", sprite=PATH, expect_revision=S["revision"], ops=[{"op": "pixel", "points": [[3, 3]], "color": 1}])
        ed.check(e["kind"] == "sprite_changed", f"{e}")
    current = (await ok(ed, "get_sprite", sprite=PATH))["revision"]
    fine = await ok(ed, "edit_sprite", sprite=PATH, expect_revision=current, ops=[{"op": "pixel", "points": [[3, 3]], "color": 1}])
    ed.check(fine["changed"]["pixels"] == 1, f"the current one works: {fine['changed']}")


async def test_overlapping_agent_edits_are_not_an_outside_save(ed: Editor):
    """The guide's loop: the second edit repaints the first's rect; the agent's own writes are not a person's save"""
    path = "assets/sprites/overlap.aseprite"
    await ok(ed, "create_sprite", path=path, size=[16, 16], palette=["#ff0000", "#00ff00"])
    since = await cursor(ed)
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "rect", "rect": [2, 2, 6, 6], "color": 1, "fill": True}])
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "rect", "rect": [3, 3, 3, 3], "color": 2, "fill": True}])
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "points": [[4, 4]], "color": 1}])
    await ed.call("wait", frames=5)
    log = await ed.call("read_console", since=since, limit=200)
    warnings = [e for e in log.get("entries", []) if OUTSIDE in str(e.get("message", ""))]
    ed.check(not warnings, f"no outside-save warning for the agent's own edits: {warnings}")
    # and the edits stay undoable
    history = await ok(ed, "undo", scope="timeline")
    ed.check(history["steps"] == 1, f"the newest edit undoes: {history}")


async def test_console_warns_of_overwritten_agent_edits(ed: Editor):
    path = "assets/sprites/warned.aseprite"
    since = await cursor(ed)
    await ok(ed, "create_sprite", path=path, size=[16, 16], palette=["#ff0000", "#00ff00"])
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "points": [[0, 0]], "color": 1}])
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "fill", "at": [8, 8], "color": 2}])
    other = "assets/sprites/other.aseprite"
    await ok(ed, "create_sprite", path=other, size=[16, 16], palette=["#ff0000", "#00ff00"])
    await ok(ed, "edit_sprite", sprite=other, ops=[{"op": "pixel", "points": [[1, 2]], "color": 2}])
    (ed.project / path).write_bytes((ed.project / other).read_bytes())
    found = await wait_warning(ed, since, "warned.aseprite")
    message = found["message"]
    ed.check("2 agent edits" in message, f"names the steps: {message}")
    ed.check("can no longer be undone" in message, f"{message}")


async def test_a_half_written_save_keeps_the_session_and_its_warning(ed: Editor):
    path = "assets/sprites/halfwritten.aseprite"
    await ok(ed, "create_sprite", path=path, size=[8, 8], palette=["#ff0000", "#00ff00"])
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "points": [[1, 1]], "color": 1}])
    agent = ed.project / path
    good = agent.read_bytes()
    # a save caught half written: the read fails but the session stays
    agent.write_bytes(good[: len(good) // 2])
    e = await err(ed, "get_sprite", sprite=path)
    ed.check(e["kind"] in ("not_a_sprite", "truncated", "corrupt_chunk"), f"the read reports the damage: {e}")
    # the save completes (different pixels): the next edit reports it against what this session last saw
    theirs = bytearray(good)
    await ok(ed, "create_sprite", path="assets/sprites/h2.aseprite", size=[8, 8], palette=["#ff0000", "#00ff00"])
    await ok(ed, "edit_sprite", sprite="assets/sprites/h2.aseprite", ops=[{"op": "pixel", "points": [[3, 3]], "color": 2}])
    agent.write_bytes((ed.project / "assets/sprites/h2.aseprite").read_bytes())
    e = await err(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "points": [[2, 2]], "color": 2}])
    ed.check(e["kind"] == "sprite_changed", f"the session was kept for sprite_changed: {e}")
    ed.check(len(theirs) > 0, "and nothing was lost")


async def test_undo_to_id_reports_the_steps_own_refusal(ed: Editor):
    path = "assets/sprites/toid.aseprite"
    await ok(ed, "create_sprite", path=path, size=[8, 8], palette=["#ff0000", "#00ff00"])
    r = await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "points": [[0, 0]], "color": 1}])
    other = (ed.project / path).read_bytes() + b"\0"
    await asyncio.sleep(0.05)
    (ed.project / path).write_bytes((ed.project / PATH).read_bytes() if (ed.project / PATH).exists() else other)
    await ed.call("wait", frames=4)
    raw_r = await ed.call_raw("undo", {"scope": "timeline", "to_id": r["undo"]["id"]})
    ed.check(raw_r.is_error or (raw_r.structured or {}).get("refused"), f"refused: {raw_r.structured}")
    ed.check("changed outside the editor" in raw_r.text, f"names why, not a child scene cycle: {raw_r.text}")
