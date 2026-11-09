"""get_sprite, read_sprite_pixels and diff_sprite (docs/specs/S47-agent-sprite-drawing.md AC11, R29,
R30): region reads with a size cap, what a region contains, the structure, and diff rows that paste
back as a patch byte for byte."""

from __future__ import annotations

import shutil
import time

from harness import REPO, Editor
from sprite_draw_util import enable, err, expand_frames, ok

CI = True
EDITOR = {"create": True, "name": "SpriteRead"}
TIMEOUT = 600
BIG = "assets/sprites/big.aseprite"
GROUPS = "assets/sprites/groups.aseprite"
FIXTURES = REPO / "editor" / "tests" / "fixtures" / "aseprite"


async def test_setup(ed: Editor):
    await enable(ed)
    await ok(ed, "create_sprite", path=BIG, size=[128, 128], palette=["#ff0000", "#00ff00", "#0000ff"])
    await ok(ed, "edit_sprite", sprite=BIG, ops=[{"op": "rect", "rect": [0, 0, 4, 4], "color": 1, "fill": True},
                                                 {"op": "pixel", "points": [[70, 70]], "color": 2}])
    (ed.project / "assets" / "sprites").mkdir(parents=True, exist_ok=True)
    shutil.copy(FIXTURES / "aseprite-tests" / "groups2.aseprite", ed.project / GROUPS)
    await ed.call("sync_files", paths=[GROUPS])


async def test_region_read_and_its_cap(ed: Editor):
    r = await ok(ed, "read_sprite_pixels", sprite=BIG, rect=[0, 0, 64, 64])
    frame = r["frames"][0]
    ed.check(r["encoding"] == "chars" and len(frame["rows"]) == 64 and len(frame["rows"][0]) == 64, f"a 64x64 region: {r['encoding']}")
    ed.check(frame["rows"][0][:5] == "1111." and r["size"] == [128, 128] and r["mode"] == "indexed", f"{frame['rows'][0][:8]}")
    e = await err(ed, "read_sprite_pixels", sprite=BIG)
    ed.check(e["kind"] == "region_too_large", f"the full frame is refused: {e}")
    ed.check("rect [0,0,64,64]" in e["fix"] and "render_sprite" in e["fix"], f"the fix suggests tiles: {e['fix']}")
    small = await ok(ed, "read_sprite_pixels", sprite=BIG, rect=[60, 60, 16, 16], max_pixels=16384)
    ed.check(small["used"] == {"2": "#00ff00"}, f"used lists only the region's colours: {small['used']}")
    # max_pixels counts one frame (R9), so a big region over two frames passes when each fits
    two = await ok(ed, "read_sprite_pixels", sprite=BIG, rect=[0, 0, 32, 32], frame=[1, 1], max_pixels=1024)
    ed.check(two["frames"][0]["rect"] == [0, 0, 32, 32], f"per frame: {two['frames'][0]['rect']}")
    empty = await ok(ed, "read_sprite_pixels", sprite=BIG, rect=[100, 100, 2, 2])
    ed.check(empty["frames"][0]["rows"] == ["..", ".."] and empty["used"] == {}, f"no cel reads as dots: {empty}")


async def test_get_sprite_structure(ed: Editor):
    r = await ok(ed, "get_sprite", sprite=GROUPS)
    frames = expand_frames(r)
    ed.check(frames[0]["frame"] == 1 and frames[-1]["frame"] == len(frames), f"frames from 1: {r['frames']}")
    ed.check(any(l.get("kind") == "group" for l in r["layers"]), f"a group layer: {r['layers']}")
    ed.check(any("group" in l for l in r["layers"]), f"a layer names its group: {r['layers']}")
    path = "assets/sprites/tagged.aseprite"
    await ok(ed, "create_sprite", path=path, size=[16, 16], frames=3, palette=["#ff0000"])
    await ok(ed, "edit_sprite", sprite=path, ops=[
        {"op": "tag", "name": "walk", "from": 1, "to": 2},
        {"op": "slice", "name": "hit", "rect": [1, 1, 4, 4]},
    ])
    s = await ok(ed, "get_sprite", sprite=path)
    ed.check(s["tags"][0] == {"name": "walk", "from": 1, "to": 2}, f"tag frames from 1: {s['tags']}")
    ed.check(s["slices"][0]["name"] == "hit" and s["slices"][0]["rect"] == [1, 1, 4, 4], f"slices: {s['slices']}")


async def test_used_by_names_the_scene_and_object(ed: Editor):
    path = "assets/sprites/usedby.aseprite"
    await ok(ed, "create_sprite", path=path, size=[8, 8], palette=["#ff0000"])
    scenes = await ed.call("list_scenes")
    main = scenes["selected_scene_id"]
    await ed.call("sync_files", paths=[path])
    deadline = time.monotonic() + 60
    created = None
    while time.monotonic() < deadline:
        raw = await ed.call_raw("set_sprite_frames", {"scene": main, "create": {"name": "UsesHero"}, "sheet": path, "frame": 0})
        if not raw.is_error:
            created = raw.structured
            break
        await ed.call("wait", seconds=0.2)
    ed.check(created, "the Sprite was made once the sheet imported")
    used = []
    while time.monotonic() < deadline and not used:
        used = (await ok(ed, "get_sprite", sprite=path)).get("used_by", [])
        if not used:
            await ed.call("wait", seconds=0.2)
    ed.check(used and any(u["object"] == "UsesHero" for u in used), f"names the GameObject: {used}")
    ed.check(all(str(u["file"]).endswith(".scene") for u in used), f"names the scene file: {used}")


async def test_diff_previous_rows_paste_back_byte_for_byte(ed: Editor):
    path = "assets/sprites/diffed.aseprite"
    await ok(ed, "create_sprite", path=path, size=[16, 16], palette=["#ff0000", "#00ff00", "#0000ff"])
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "rect", "rect": [1, 1, 3, 3], "color": 1, "fill": True}])
    before = (ed.project / path).read_bytes()
    edited = await ok(ed, "edit_sprite", sprite=path, ops=[
        {"op": "pixel", "points": [[2, 2]], "color": 2},
        {"op": "pixel", "points": [[6, 3]], "color": 3},
    ])
    after = (ed.project / path).read_bytes()
    d = await ok(ed, "diff_sprite", sprite=path, against="previous")
    ed.check(d["changed"] == 2 and not d["same"], f"two pixels differ: {d}")
    frame = d["frames"][0]
    ed.check(frame["bbox"] == [2, 2, 5, 2], f"bbox: {frame['bbox']}")
    ed.check({(c["x"], c["y"], c["to"]) for c in frame["cells"]} == {(2, 2, 2), (6, 3, 3)}, f"cells: {frame['cells']}")
    ed.check(frame["rows"] == ["2----", "----3"], f"rows only cover the changes: {frame['rows']}")
    # undo the edit, then paste the diff back: the same file, byte for byte
    await ok(ed, "undo", scope="timeline", expect_top_id=edited["undo"]["id"])
    ed.check((ed.project / path).read_bytes() == before, "undone")
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "paste", "rows": frame["rows"], "at": frame["bbox"][:2], "encoding": frame["encoding"]}])
    ed.check((ed.project / path).read_bytes() == after, "pasted back byte for byte")


async def test_diff_against_a_revision_a_file_and_a_frame(ed: Editor):
    path = "assets/sprites/rev.aseprite"
    await ok(ed, "create_sprite", path=path, size=[8, 8], frames=2, palette=["#ff0000", "#00ff00"])
    first = (await ok(ed, "get_sprite", sprite=path))["revision"]
    await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "points": [[1, 1]], "color": 1, "frame": 1}])
    second = await ok(ed, "edit_sprite", sprite=path, ops=[{"op": "pixel", "points": [[2, 2]], "color": 2, "frame": 1}])
    d = await ok(ed, "diff_sprite", sprite=path, against=first)
    ed.check(d["changed"] == 2, f"against an older revision: {d}")
    frames = await ok(ed, "diff_sprite", sprite=path, against={"frame": 2}, frames=1)
    ed.check(frames["changed"] == 2, f"frame 1 against frame 2: {frames}")
    e = await err(ed, "diff_sprite", sprite=path, against="deadbeefcafe")
    ed.check(e["kind"] == "revision_unknown" and second["revision"] in e["fix"], f"lists the known revisions: {e}")
    other = "assets/sprites/rev2.aseprite"
    await ok(ed, "create_sprite", path=other, size=[9, 9], palette=["#ff0000"])
    e = await err(ed, "diff_sprite", sprite=path, against={"sprite": other})
    ed.check(e["kind"] == "size_mismatch", f"{e}")
