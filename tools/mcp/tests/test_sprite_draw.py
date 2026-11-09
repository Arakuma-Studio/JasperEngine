"""edit_sprite (docs/specs/S47-agent-sprite-drawing.md AC8, AC9, AC13, AC16): atomic batches, echoes,
no-op batches writing nothing, the undo step, the memory of 1,000 one-pixel edits, lints after a
draw, the tools hidden until the group is enabled, and the CLI's schema check."""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import harness
from harness import Editor
from sprite_draw_util import digest, enable, err, images, ok

CI = True
EDITOR = {"create": True, "name": "SpriteDraw"}
TIMEOUT = 600
HERO = "assets/sprites/hero.aseprite"
TOOLS = {"create_sprite", "get_sprite", "read_sprite_pixels", "edit_sprite", "render_sprite", "diff_sprite"}


async def test_hidden_until_enabled(ed: Editor):
    names = {t.name for t in await ed.tools(groups=None)}
    ed.check(not (TOOLS & names), f"the sprites group is hidden by default: {TOOLS & names}")
    await enable(ed)
    names = {t.name for t in await ed.tools(groups=None)}
    ed.check(TOOLS <= names, f"tool_groups lists them: {TOOLS - names}")
    made = await ok(ed, "create_sprite", path=HERO, size=[32, 32], palette=["#ff0000", "#00ff00", "#0000ff"])
    ed.check(made["mode"] == "indexed" and made["size"] == [32, 32], f"created: {made}")
    ed.check((ed.project / HERO).is_file(), "the file is on disk")


async def test_batch_failing_at_its_third_op_changes_nothing(ed: Editor):
    before = digest(ed.project / HERO)
    revision = (await ok(ed, "get_sprite", sprite=HERO))["revision"]
    e = await err(ed, "edit_sprite", sprite=HERO, ops=[
        {"op": "pixel", "points": [[0, 0]], "color": 1},
        {"op": "line", "points": [[0, 1], [5, 1]], "color": 2},
        {"op": "pixel", "points": [[0, 2]], "color": 99},
    ])
    ed.check(e["op_index"] == 2 and e["kind"] == "bad_color", f"names the failing op: {e}")
    ed.check("0-3" in e["fix"], f"the fix gives the valid range: {e['fix']}")
    ed.check(digest(ed.project / HERO) == before, "the file is unchanged")
    ed.check((await ok(ed, "get_sprite", sprite=HERO))["revision"] == revision, "and the revision")


async def test_field_mistake_fails_before_any_op(ed: Editor):
    before = digest(ed.project / HERO)
    e = await err(ed, "edit_sprite", sprite=HERO, ops=[
        {"op": "pixel", "points": [[0, 0]], "color": 1},
        {"op": "pixel", "points": [[1, 0]], "colour": 1},
    ])
    ed.check(e["kind"] == "bad_op_field" and e["op_index"] == 1, f"{e}")
    ed.check("points, color" in e["fix"], f"the fix lists pixel's fields: {e['fix']}")
    ed.check(digest(ed.project / HERO) == before, "nothing written")


async def test_echo_covers_exactly_the_dirty_rect(ed: Editor):
    r = await ok(ed, "edit_sprite", sprite=HERO, ops=[
        {"op": "pixel", "points": [[3, 4], [5, 6]], "color": 1},
    ])
    rects = r["changed"]["rects"]
    ed.check(r["changed"]["pixels"] == 2 and len(rects) == 1 and rects[0]["rect"] == [3, 4, 3, 3], f"the dirty rect: {r['changed']}")
    echo = r["echo"][0]
    ed.check(echo["rect"] == [3, 4, 3, 3] and echo["encoding"] == "chars", f"echo: {echo}")
    ed.check(echo["rows"] == ["1..", "...", "..1"], f"the rect's grid: {echo['rows']}")
    ed.check(echo["used"]["1"] == "#ff0000", f"used names the colours: {echo['used']}")
    ed.check(r["previous_revision"] != r["revision"], f"the revision moved: {r}")


async def test_echo_max_truncates_with_the_rects_listed(ed: Editor):
    await ok(ed, "create_sprite", path="assets/sprites/big.aseprite", size=[64, 64], palette=["#ff0000", "#00ff00"])
    r = await ok(ed, "edit_sprite", sprite="assets/sprites/big.aseprite", echo_max=16, ops=[
        {"op": "rect", "rect": [0, 0, 32, 32], "color": 1, "fill": True},
    ])
    ed.check(r.get("echo") == [], f"too big to echo: {r.get('echo')}")
    trunc = r["echo_truncated"]
    ed.check(len(trunc) == 1 and trunc[0]["rect"] == [0, 0, 32, 32], f"listed: {trunc}")
    ed.check("read_sprite_pixels" in r["next"], f"the hint: {r['next']}")


async def test_batch_changing_nothing_writes_nothing(ed: Editor):
    before = digest(ed.project / HERO)
    steps = len((await ok(ed, "get_undo_history", scope="project"))["entries"])
    r = await ok(ed, "edit_sprite", sprite=HERO, ops=[{"op": "pixel", "points": [[999, 999]], "color": 1}])
    ed.check(r["changed"]["pixels"] == 0 and r["applied"] == 1, f"applied, nothing changed: {r}")
    ed.check(r["warnings"][0]["kind"] == "nothing_drawn" and r["warnings"][0]["op_index"] == 0, f"{r['warnings']}")
    ed.check(digest(ed.project / HERO) == before, "the file is unchanged")
    after = await ok(ed, "get_undo_history", scope="project")
    ed.check(len(after["entries"]) == steps, f"no undo step: {steps} -> {len(after['entries'])}")


async def test_open_and_no_op_leave_the_bytes_identical(ed: Editor):
    before = digest(ed.project / HERO)
    await ok(ed, "get_sprite", sprite=HERO)
    await ok(ed, "read_sprite_pixels", sprite=HERO, rect=[0, 0, 4, 4])
    await ok(ed, "render_sprite", sprite=HERO)
    ed.check(digest(ed.project / HERO) == before, "reads never write")


async def test_undo(ed: Editor):
    path = "assets/sprites/undoable.aseprite"
    await ok(ed, "create_sprite", path=path, size=[16, 16], palette=["#ff0000", "#00ff00"])
    first = (ed.project / path).read_bytes()
    r = await ok(ed, "edit_sprite", sprite=path, ops=[
        {"op": "line", "points": [[0, 0], [5, 0]], "color": 1},
        {"op": "fill", "at": [10, 10], "color": 2},
    ])
    changed = (ed.project / path).read_bytes()
    ed.check(changed != first, "the edit wrote")
    ed.check(r["undo"]["name"].endswith("Edit Sprite (undoable.aseprite: line, fill)"), f"the step's name: {r['undo']}")
    history = await ok(ed, "get_undo_history", scope="project", limit=5)
    step = next(e for e in history["entries"] if e["id"] == r["undo"]["id"])
    ed.check(step["origin"]["kind"] == "agent" and step["type"] == "SpriteEditCmd", f"attributed to the agent: {step}")
    ed.check(step["name"] == r["undo"]["name"], "named the same in the history")

    undone = await ok(ed, "undo", scope="timeline", expect_top_id=r["undo"]["id"])
    ed.check(undone["steps"], f"one step: {undone}")
    ed.check((ed.project / path).read_bytes() == first, "undo restores the bytes exactly")
    await ok(ed, "redo", scope="timeline")
    ed.check((ed.project / path).read_bytes() == changed, "redo gives the edited bytes back")


async def test_undo_is_refused_in_timeline_order(ed: Editor):
    path = "assets/sprites/order.aseprite"
    await ok(ed, "create_sprite", path=path, size=[8, 8], palette=["#ff0000", "#00ff00"])
    a = await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "points": [[0, 0]], "color": 1}])
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "points": [[1, 0]], "color": 2}])
    raw = await ed.call_raw("undo", {"scope": "timeline", "expect_top_id": a["undo"]["id"]})
    ed.check(raw.is_error, "expect_top_id refuses a step that is not on top")
    grid = (await ok(ed, "read_sprite_pixels", sprite=path, rect=[0, 0, 2, 1]))["frames"][0]["rows"][0]
    ed.check(grid == "12", f"nothing moved: {grid}")


async def test_one_thousand_one_pixel_edits_stay_under_200_kb(ed: Editor):
    path = "assets/sprites/memory.aseprite"
    await ok(ed, "create_sprite", path=path, size=[128, 128], frames=8, palette=["#ff0000", "#00ff00"])
    before = (await ok(ed, "get_undo_history", scope="project", limit=1))["memory"]["used"]
    for i in range(1000):
        await ok(ed, "edit_sprite", sprite=path, echo="none", ops=[{"op": "pixel", "points": [[i % 128, (i // 128) % 128]], "color": 1 + i % 2}])
    used = (await ok(ed, "get_undo_history", scope="project", limit=1))["memory"]["used"] - before
    print(f"     [memory] 1000 edits: {used} bytes")
    ed.check(used <= 200 * 1024, f"1,000 one-pixel edits cost {used} bytes")


async def test_lint_after_draw(ed: Editor):
    path = "assets/sprites/lint.aseprite"
    await ok(ed, "create_sprite", path=path, size=[32, 32], palette=["#ff0000", "#00ff00"])
    # an L-corner at every step of a staircase, each a separate stroke
    corners = [{"op": "line", "points": [[4 + i * 4, 4 + i * 4], [8 + i * 4, 4 + i * 4], [8 + i * 4, 8 + i * 4]], "color": 1, "pixel_perfect": False}
               for i in range(5)]
    r = await ok(ed, "edit_sprite", sprite=path, lint=["doubles"], ops=corners)
    results = r["lints"]["results"]
    ed.check(len(results) == 1 and results[0]["id"] == "doubles" and results[0]["status"] == "WARN", f"WARN with spots: {r['lints']}")
    spots = results[0]["at"]
    ed.check(spots and all(s[0] == 1 for s in spots), f"spots use frames from 1: {spots}")
    fix = await ok(ed, "edit_sprite", sprite=path, lint=["doubles"],
                   ops=[{"op": "pixel", "points": [[s[1], s[2]] for s in spots], "color": "transparent"}])
    ed.check(fix["lints"]["results"] == [], f"fixing the spots leaves no non-PASS lint: {fix['lints']}")
    blocked = await ok(ed, "edit_sprite", sprite=path, lint=True, ops=[{"op": "pixel", "points": [[0, 0]], "color": 2}])
    ed.check("counts" in blocked["lints"], f"lint: true returns counts: {blocked['lints']}")


async def test_call_cli_validates_the_op_names_and_the_handler_the_fields(ed: Editor):
    path = "assets/sprites/cli.aseprite"
    await ok(ed, "create_sprite", path=path, size=[8, 8], palette=["#ff0000"])
    before = digest(ed.project / path)
    # the CLI's schema check knows the op names only (attached to this editor through HTTP is out
    # of the harness' reach here: the handler's check is reached through a normal call)
    e = await err(ed, "edit_sprite", sprite=path, ops=[{"op": "pixle", "points": [[0, 0]], "color": 1}])
    ed.check(e["kind"] == "bad_op_field" and "pixel" in e["fix"], f"an unknown op lists the ops: {e}")
    e = await err(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "pointz": [[0, 0]], "color": 1}])
    ed.check(e["kind"] == "bad_op_field" and "points" in e["fix"], f"a misspelled field names the op's fields: {e}")
    ed.check(digest(ed.project / path) == before, "the file bytes stay")


async def test_cli_refuses_an_unknown_op_before_starting_an_editor(ed: Editor):
    work = ed.project.parent / "cli-work"
    work.mkdir(exist_ok=True)
    (work / "settings.yaml").write_text("")
    proc = subprocess.run(harness.editor_command() + ["call", "--dry-run", "edit_sprite", "--sprite", "x.aseprite", "--ops", json.dumps([{"op": "pixle"}])],
                          cwd=str(work), capture_output=True, text=True, timeout=60)
    ed.check(proc.returncode == 2 and "must be one of" in proc.stderr, f"fails before any editor: {proc.returncode} {proc.stderr}")


async def test_every_structure_op_writes_a_file_aseprite_opens(ed: Editor):
    """A sprite through every structure kind; JASPER_SHOTS_DIR keeps it for the real-Aseprite check"""
    path = "assets/sprites/full.aseprite"
    await ok(ed, "create_sprite", path=path, size=[16, 16], frames=2, layers=["back", "front"], palette=["#ff0000", "#00ff00", "#0000ff"])
    r = await ok(ed, "edit_sprite", sprite=path, ops=[
        {"op": "rect", "rect": [2, 2, 8, 8], "color": 1, "fill": True, "layer": "back"},
        {"op": "ellipse", "rect": [4, 4, 8, 8], "color": 2, "layer": "front", "frame": 2},
        {"op": "layer", "action": "add", "name": "fx"},
        {"op": "pixel", "points": [[1, 1]], "color": 3, "layer": "fx"},
        {"op": "frame", "action": "add", "link": True},
        {"op": "frame", "action": "duplicate", "from": 1},
        {"op": "tag", "name": "walk", "from": 1, "to": 3, "direction": "pingpong", "repeat": 2},
        {"op": "slice", "name": "hit", "rect": [1, 1, 6, 6], "pivot": [3, 3]},
        {"op": "palette", "action": "add", "colors": ["#ffff00"]},
        {"op": "cel", "action": "set", "frame": 2, "layer": "back", "opacity": 128},
        {"op": "canvas", "action": "resize", "size": [20, 20], "anchor": "center"},
    ])
    ed.check(r["changed"]["pixels"] > 0 and r["structure"]["frames"], f"{r['changed']}")
    shots = os.environ.get("JASPER_SHOTS_DIR")
    if shots:
        Path(shots).mkdir(parents=True, exist_ok=True)
        (Path(shots) / "full.aseprite").write_bytes((ed.project / path).read_bytes())
        (Path(shots) / "hero.aseprite").write_bytes((ed.project / HERO).read_bytes())


async def test_schema_declares_the_ops(ed: Editor):
    tools = {t.name: t for t in await ed.tools(groups="all")}
    ops = tools["edit_sprite"].input_schema["properties"]["ops"]
    ed.check(ops["items"]["properties"]["op"]["enum"] and "paste" in ops["items"]["properties"]["op"]["enum"], f"the op enum: {ops}")
    ed.check(ops["items"]["additionalProperties"] is True, "op items are open (closed in the handler)")


async def test_image_flag_returns_a_render(ed: Editor):
    data, imgs = await images(ed, "edit_sprite", sprite=HERO, image=True, ops=[{"op": "pixel", "points": [[7, 7]], "color": 2}])
    ed.check(len(imgs) == 1 and imgs[0].startswith(b"\x89PNG"), f"a PNG: {len(imgs)}")


async def test_missing_sprite_and_not_a_sprite_say_how_to_fix_it(ed: Editor):
    e = await err(ed, "get_sprite", sprite="assets/sprites/nope.aseprite")
    ed.check(e["kind"] == "sprite_missing" and "list_assets" in e["fix"], f"{e}")
    (ed.project / "assets" / "note.txt").write_text("hello")
    e = await err(ed, "get_sprite", sprite="assets/note.txt")
    ed.check(e["kind"] == "not_a_sprite", f"{e}")
