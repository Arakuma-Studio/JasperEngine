"""edit_sprite (docs/specs/S47-agent-sprite-drawing.md AC8, AC9, AC13, AC16, AC18, R38): atomic batches, echoes,
no-op batches writing nothing, the undo step, the memory of 1,000 one-pixel edits, lints after a
draw, the tools hidden until the group is enabled, and the CLI's schema check."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import time
from pathlib import Path

import harness
from harness import Editor, over_budget
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
    history = await ok(ed, "get_undo_history", scope="project", limit=5)
    step = next(e for e in history["entries"] if e["id"] == r["undo"]["id"])
    ed.check(step["origin"]["kind"] == "agent" and step["type"] == "SpriteEditCmd", f"attributed to the agent: {step}")
    client = step["origin"]["client"]
    ed.check(client, f"the step names its client: {step['origin']}")
    expected = f"{client}: Edit Sprite (undoable.aseprite: line, fill)"
    ed.check(r["undo"]["name"] == expected, f"the step's name: {r['undo']['name']!r}, expected {expected!r}")
    ed.check(step["name"] == expected, f"named the same in the history: {step['name']!r}")

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


def l_corner(x: int, y: int) -> dict:
    """One separate L stroke of 9 pixels with a single right-angle corner"""
    return {"op": "line", "points": [[x, y], [x + 4, y], [x + 4, y + 4]], "color": 1, "pixel_perfect": False}


async def test_lint_doubles_warns_from_the_third_corner(ed: Editor):
    path = "assets/sprites/lint.aseprite"
    await ok(ed, "create_sprite", path=path, size=[64, 64], palette=["#ff0000", "#00ff00"])
    two = await ok(ed, "edit_sprite", sprite=path, lint=["doubles"], ops=[l_corner(4 + i * 12, 4) for i in range(2)])
    ed.check(two["lints"]["results"] == [], f"two corners: PASS, nothing listed: {two['lints']}")
    three = await ok(ed, "edit_sprite", sprite=path, lint=["doubles"], ops=[l_corner(4 + 2 * 12, 4)])
    results = three["lints"]["results"]
    ed.check(len(results) == 1 and results[0]["id"] == "doubles" and results[0]["status"] == "WARN", f"three corners: WARN: {three['lints']}")
    spots = results[0]["at"]
    ed.check(len(spots) == 3 and results[0]["count"] == 3, f"the three corners, by count and spot: {results[0]}")
    ed.check(all(s[0] == 1 for s in spots), f"spots use frames from 1: {spots}")
    fix = await ok(ed, "edit_sprite", sprite=path, lint=["doubles"],
                   ops=[{"op": "pixel", "points": [[s[1], s[2]] for s in spots], "color": "transparent"}])
    ed.check(fix["lints"]["results"] == [], f"fixing the spots leaves no non-PASS lint: {fix['lints']}")
    blocked = await ok(ed, "edit_sprite", sprite=path, lint=True, ops=[{"op": "pixel", "points": [[0, 0]], "color": 2}])
    ed.check("counts" in blocked["lints"], f"lint: true returns counts: {blocked['lints']}")


async def test_flicker_runs_on_every_touched_tag(ed: Editor):
    path = "assets/sprites/flicker.aseprite"
    await ok(ed, "create_sprite", path=path, size=[16, 16], frames=6, palette=["#ff0000", "#00ff00"])
    await ok(ed, "edit_sprite", sprite=path, ops=[
        {"op": "rect", "rect": [1, 1, 4, 1], "color": 1, "fill": True, "frame": "all"},
        {"op": "tag", "name": "a", "from": 1, "to": 3},
        {"op": "tag", "name": "b", "from": 4, "to": 6},
    ])
    r = await ok(ed, "edit_sprite", sprite=path, lint=["flicker"], ops=[
        {"op": "pixel", "points": [[8, 8], [9, 8]], "color": 2, "frame": 2},
        {"op": "pixel", "points": [[8, 9], [9, 9]], "color": 2, "frame": 5},
    ])
    flicker = [res for res in r["lints"]["results"] if res["id"] == "flicker"]
    ed.check(flicker, f"flicker reported: {r['lints']}")
    frames = {spot[0] for res in flicker for spot in res["at"]}
    ed.check({2, 5} <= frames, f"both touched tags (frames from 1): {frames}, results {flicker}")


async def test_handler_refuses_a_misspelled_field(ed: Editor):
    path = "assets/sprites/cli.aseprite"
    await ok(ed, "create_sprite", path=path, size=[8, 8], palette=["#ff0000"])
    before = digest(ed.project / path)
    e = await err(ed, "edit_sprite", sprite=path, ops=[{"op": "pixle", "points": [[0, 0]], "color": 1}])
    ed.check(e["kind"] == "bad_op_field" and "pixel" in e["fix"], f"an unknown op lists the ops: {e}")
    e = await err(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "pointz": [[0, 0]], "color": 1}])
    ed.check(e["kind"] == "bad_op_field" and "points" in e["fix"], f"a misspelled field names the op's fields: {e}")
    ed.check(digest(ed.project / path) == before, "the file bytes stay")


async def test_cli_one_shot_reports_a_misspelled_field(ed: Editor):
    # `call --project` on a copy (this editor holds the project), a headless one-shot editor
    copy = ed.project.parent / "cli-copy"
    shutil.copytree(ed.project, copy, ignore=shutil.ignore_patterns("*.lock", "lock", "build"))
    path = "assets/sprites/cli.aseprite"
    before = digest(copy / path)
    home = ed.project.parent / "cli-home"
    work = ed.project.parent / "cli-oneshot"
    home.mkdir(exist_ok=True)
    work.mkdir(exist_ok=True)
    (work / "settings.yaml").write_text("")
    env = {k: v for k, v in os.environ.items() if k not in ("JASPER_MCP_URL", "JASPER_MCP_TOKEN", "DISPLAY", "XDG_CONFIG_HOME", "WAYLAND_DISPLAY")}
    env["HOME"] = str(home)
    proc = subprocess.run(harness.editor_command() + ["call", "--project", str(copy), "edit_sprite", "--sprite", path,
                                                      "--ops", json.dumps([{"op": "pixel", "pointz": [[0, 0]], "color": 1}])],
                          cwd=str(work), env=env, capture_output=True, text=True, timeout=300)
    output = proc.stdout + proc.stderr
    ed.check(proc.returncode != 0, f"fails: {proc.returncode} {output[-400:]}")
    ed.check("bad_op_field" in output and "points" in output, f"names the op's fields: {output[-400:]}")
    ed.check(digest(copy / path) == before, "the copy's file bytes stay")


async def test_huge_rect_is_clipped_quickly(ed: Editor):
    path = "assets/sprites/huge.aseprite"
    await ok(ed, "create_sprite", path=path, size=[16, 16], palette=["#ff0000"])
    r = await ed.call_raw("edit_sprite", {"sprite": path, "ops": [{"op": "rect", "rect": [0, 0, 2000000000, 2000000000], "color": 1, "fill": True}]}, timeout=30)
    ed.check(not r.is_error, f"a rect far past the canvas is clipped, not refused: {r.text}")
    ed.check(r.structured["changed"]["pixels"] == 16 * 16, f"the whole canvas: {r.structured['changed']}")
    await ed.call("editor_status")


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


def hundred_ops() -> list[dict]:
    variety = [
        {"op": "pixel", "points": [[1, 1]], "color": 1},
        {"op": "line", "points": [[0, 0], [15, 9]], "color": 2},
        {"op": "rect", "rect": [2, 2, 6, 5], "color": 3, "radius": 1},
        {"op": "ellipse", "rect": [3, 3, 8, 7], "color": 4, "fill": True},
        {"op": "polygon", "points": [[0, 0], [8, 2], [4, 9]], "color": 5, "fill": True},
        {"op": "paste", "rows": ["1.2", ".3."], "at": [12, 12]},
        {"op": "fill", "at": [31, 31], "color": 1},
        {"op": "replace", "from": 1, "to": 2},
        {"op": "dither", "colors": [3, 4], "level": 0.4, "rect": [0, 16, 16, 8]},
        {"op": "clear", "rect": [4, 4, 3, 3]},
        {"op": "outline", "color": 5},
        {"op": "inline", "color": 1},
        {"op": "shade", "mode": "ramp", "ramp": [1, 2, 3]},
        {"op": "shift", "by": [3, 2], "wrap": True},
        {"op": "flip", "axis": "y"},
        {"op": "rotate", "angle": 37},
        {"op": "rotate", "angle": 90},
        {"op": "scale", "factor": 2, "rect": [8, 8, 8, 8]},
        {"op": "mirror", "axis": "x"},
        {"op": "copy", "source": {"rect": [0, 0, 8, 8]}, "at": [16, 16]},
        {"op": "select", "shape": "ellipse", "rect": [4, 4, 20, 20]},
        {"op": "select", "shape": "none"},
        {"op": "layer", "action": "set", "name": "A", "opacity": 200},
        {"op": "frame", "action": "set", "duration_ms": 90},
        {"op": "tag", "action": "set", "name": "t", "from": 1, "to": 1},
        {"op": "slice", "action": "set", "name": "s", "rect": [1, 1, 3, 3]},
        {"op": "cel", "action": "set", "opacity": 180},
        {"op": "palette", "action": "replace", "index": 5, "color": "#ff00aa"},
    ]
    ops = [variety[i % len(variety)] for i in range(98)]
    ops.append({"op": "canvas", "action": "resize", "size": [34, 34], "anchor": "center"})
    ops.append({"op": "canvas", "action": "resize", "size": [32, 32], "anchor": "center"})
    return ops


async def test_same_ops_give_the_same_bytes_and_echo(ed: Editor):
    """AC18 through the real handler: two identical sprites, the same 100 ops"""
    paths = ("assets/sprites/det_a.aseprite", "assets/sprites/det_b.aseprite")
    results = []
    for path in paths:
        await ok(ed, "create_sprite", path=path, size=[32, 32], layers=["A"], palette=["#ff0000", "#00ff00", "#0000ff", "#ffff00", "#ff00ff"])
        r = await ok(ed, "edit_sprite", sprite=path, ops=hundred_ops(), echo_max=4096, image=False)
        results.append(r)
    ed.check((ed.project / paths[0]).read_bytes() == (ed.project / paths[1]).read_bytes(), "the same file bytes")
    a, b = results
    ed.check(a["applied"] == 100, f"all ran: {a['applied']}")
    ed.check(a["echo"] == b["echo"] and a["changed"] == b["changed"] and a["revision"] == b["revision"], f"the same echo: {a['changed']} / {b['changed']}")


async def test_fifty_paint_ops_on_128_frames_stays_in_budget(ed: Editor):
    """R38 through the handler (enforced only in a benchmark run: JASPER_TEST_PERF_SLACK)"""
    path = "assets/sprites/perf.aseprite"
    await ok(ed, "create_sprite", path=path, size=[128, 128], frames=8, palette=["#ff0000", "#00ff00", "#0000ff"])
    times = []
    for run in range(5):
        ops = [{"op": "line", "points": [[(i + run) % 100, 0], [(i + run) % 100 + 20, 40]], "color": 1 + i % 3, "frame": 1 + i % 8} for i in range(50)]
        started = time.perf_counter()
        await ok(ed, "edit_sprite", sprite=path, echo="none", ops=ops)
        times.append((time.perf_counter() - started) * 1000.0)
    times.sort()
    over = over_budget("edit_sprite 50 paint ops, 128x128x8 indexed (round trip)", {"median": times[len(times) // 2]}, 40, "ms")
    ed.check(not over, f"over budget: {over}")


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
