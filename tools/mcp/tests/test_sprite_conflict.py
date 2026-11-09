"""A person's save never gets overwritten (docs/specs/S47-agent-sprite-drawing.md AC10, R33, R34):
undo is refused once the file changed outside the editor, the next edit reports sprite_changed
and the one after it works on the person's version, a stale expect_revision fails every time,
and a Console warning names the overwritten agent steps."""

from __future__ import annotations

import time

from harness import Editor
from sprite_draw_util import digest, enable, err, ok

CI = True
EDITOR = {"create": True, "name": "SpriteConflict"}
TIMEOUT = 600
PATH = "assets/sprites/hero.aseprite"


async def external_save(ed: Editor, data: bytes) -> str:
    """Writes the file as another program would and waits until the editor has seen it"""
    target = ed.project / PATH
    time.sleep(0.05)   # distinct mtime on coarse filesystems
    target.write_bytes(data)
    await ed.call("sync_files", paths=[PATH])
    return digest(target)


async def test_setup(ed: Editor):
    await enable(ed)
    await ok(ed, "create_sprite", path=PATH, size=[16, 16], palette=["#ff0000", "#00ff00"])
    S["clean"] = (ed.project / PATH).read_bytes()
    r = await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "pixel", "points": [[0, 0]], "color": 1}])
    S["undo"] = r["undo"]["id"]
    S["agent"] = (ed.project / PATH).read_bytes()
    S["revision"] = r["revision"]


S: dict = {}


async def test_undo_refused_after_an_outside_save(ed: Editor):
    # the person's version: a different pixel on the clean file, written by "another program"
    person = await ok(ed, "create_sprite", path="assets/sprites/person.aseprite", size=[16, 16], palette=["#ff0000", "#00ff00"])
    await ok(ed, "edit_sprite", sprite="assets/sprites/person.aseprite", ops=[{"op": "pixel", "points": [[9, 9]], "color": 2}])
    theirs = (ed.project / "assets/sprites/person.aseprite").read_bytes()
    await external_save(ed, theirs)
    await ed.call("wait", frames=3)
    raw = await ed.call_raw("undo", {"scope": "timeline", "expect_top_id": S["undo"]})
    ed.check(raw.is_error or not (raw.structured or {}).get("steps"), f"the undo is refused: {raw.structured}")
    ed.check("changed outside the editor" in raw.text, f"the refusal reads as R33: {raw.text}")
    ed.check((ed.project / PATH).read_bytes() == theirs, "the person's bytes stay")


async def test_next_edit_reports_sprite_changed_then_works(ed: Editor):
    e = await err(ed, "edit_sprite", sprite=PATH, ops=[{"op": "pixel", "points": [[1, 1]], "color": 1}])
    ed.check(e["kind"] == "sprite_changed", f"{e}")
    ed.check(e["last_seen_revision"] == S["revision"] and e["current_revision"] != S["revision"], f"revisions: {e}")
    ed.check(e["changed_by"] == "external", f"{e['changed_by']}")
    ed.check({"frame": 1, "rect": [0, 0, 10, 10]} in e["changed_rects"] or e["changed_rects"], f"changed_rects: {e['changed_rects']}")
    ed.check("read the region again" in e["fix"] and "expect_revision" in e["fix"], f"{e['fix']}")
    ok_again = await ok(ed, "edit_sprite", sprite=PATH, ops=[{"op": "pixel", "points": [[2, 2]], "color": 2}])
    rows = (await ok(ed, "read_sprite_pixels", sprite=PATH, rect=[0, 0, 10, 10]))["frames"][0]["rows"]
    ed.check(rows[9][9] == "2" and rows[2][2] == "2", f"the edit landed on the person's version: {rows}")
    ed.check(ok_again["revision"] != S["revision"], "a new revision")


async def test_stale_expect_revision_fails_every_time(ed: Editor):
    for _ in range(2):
        e = await err(ed, "edit_sprite", sprite=PATH, expect_revision=S["revision"], ops=[{"op": "pixel", "points": [[3, 3]], "color": 1}])
        ed.check(e["kind"] == "sprite_changed", f"{e}")
    current = (await ok(ed, "get_sprite", sprite=PATH))["revision"]
    fine = await ok(ed, "edit_sprite", sprite=PATH, expect_revision=current, ops=[{"op": "pixel", "points": [[3, 3]], "color": 1}])
    ed.check(fine["changed"]["pixels"] == 1, f"the current one works: {fine['changed']}")


async def test_console_warns_of_overwritten_agent_edits(ed: Editor):
    path = "assets/sprites/warned.aseprite"
    await ok(ed, "create_sprite", path=path, size=[16, 16], palette=["#ff0000", "#00ff00"])
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "points": [[0, 0]], "color": 1}])
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "fill", "at": [8, 8], "color": 2}])
    cursor = (await ed.call("read_console", limit=1)).get("next_cursor", 0)
    other = await ok(ed, "create_sprite", path="assets/sprites/other.aseprite", size=[16, 16], palette=["#ff0000", "#00ff00"])
    await ok(ed, "edit_sprite", sprite="assets/sprites/other.aseprite", ops=[{"op": "pixel", "points": [[1, 2]], "color": 2}])
    target = ed.project / path
    time.sleep(0.05)
    target.write_bytes((ed.project / "assets/sprites/other.aseprite").read_bytes())
    await ed.call("sync_files", paths=[path])
    deadline = time.monotonic() + 20
    found = None
    while time.monotonic() < deadline and not found:
        await ed.call("wait", frames=2)
        log = await ed.call("read_console", since=cursor, limit=200)
        found = next((e for e in log.get("entries", []) if "saved from outside the editor" in str(e.get("message", ""))), None)
    ed.check(found, f"a Console warning: {log}")
    message = found["message"]
    ed.check("warned.aseprite" in message and "2 agent edits" in message, f"names the steps: {message}")
    ed.check("can no longer be undone" in message, f"{message}")
