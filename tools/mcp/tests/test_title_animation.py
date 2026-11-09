"""S85 Title animation through the title_art tools (docs/specs/S85-title-animation.md AC13, AC18,
AC19, AC23; jasper://docs/title-art): every animation op with undo and dry_run, get_title's
animations, render_title's frames and filmstrips, the anim_* checks with seeded failures beside
their fixes (the red controls), export_title's flipbooks, list_title_library's presets.
"""

from pathlib import Path

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Animation"}
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> None:
    if S.get("ed") is ed:
        return
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art", "editor_tools"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})


async def title(ed: Editor, path: str, text: str = "WIN", ops: list | None = None) -> dict:
    await setup(ed)
    r = await ed.call("edit_title", path=path, create={"text": text, "overwrite": True}, bake="never")
    if ops:
        r = await ed.call("edit_title", path=path, ops=ops, bake="never")
    return r


def check_of(report: dict, check: str) -> dict:
    for c in report["checks"]:
        if c["id"] == check:
            return c
    raise AssertionError(f"no {check} in {[c['id'] for c in report['checks']]}")


async def test_ops_undo_dry_run_and_all_or_nothing(ed: Editor):
    path = "assets/anim_ops.titleart"
    await title(ed, path)
    # dry run applies nothing
    r = await ed.call("edit_title", path=path, dry_run=True, ops=[{"op": "add_animation", "name": "Dry"}], bake="never")
    ed.check("animation_changes" in r or r.get("applied") == 1, str(r)[:300])
    got = await ed.call("get_title", path=path)
    ed.check("animations" not in got, "a dry run writes nothing")

    r = await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "add_animation", "name": "Intro", "role": "intro", "duration": 0.6},
        {"op": "set_track", "animation": "Intro", "field": "transform.scale", "relative": True,
         "keys": [[0, 2, "quad_in"], [0.6, 1]]},
        {"op": "set_marker", "animation": "Intro", "name": "onImpact", "time": 0.3},
    ])
    changes = r["animation_changes"]
    ed.check(changes[0]["animation"] == "Intro" and changes[0]["class"] == "title_live", str(changes))
    ed.check("Intro" in r["animation_cost"], str(r)[:400])

    # the outline, and the animation in full with keys in the compact grammar
    got = await ed.call("get_title", path=path, animation="Intro")
    row = got["animations"][0]
    ed.check(row["name"] == "Intro" and row["tracks"] == 1 and row["markers"] == 1, str(row))
    ed.check(row["classes"] == {"title_live": 1}, str(row["classes"]))
    ed.check(got["animation"]["tracks"][0]["keys"][0] == [0, 2, "quad_in"], str(got["animation"]))

    # all or nothing: the bad op's index, did-you-mean on the effect parameter
    await ed.call("edit_title", path=path, bake="never", ops=[{"op": "add_effect", "layer": "Title", "type": "outer_glow"}])
    r = await ed.call_raw("edit_title", {"path": path, "bake": "never", "ops": [
        {"op": "set_marker", "animation": "Intro", "name": "second", "time": 0.1},
        {"op": "set_key", "animation": "Intro", "layer": "Title", "field": "fx:outer_glow.intensty", "time": 0, "value": 1},
    ]})
    ed.check(r.is_error, "the batch fails")
    ed.check("Nothing was applied" in r.text, r.text[:400])
    ed.check("intensity" in r.text, r.text[:600])
    got = await ed.call("get_title", path=path)
    ed.check(got["animations"][0]["markers"] == 1, "nothing of the batch was applied")

    # not_animatable lists the layer's animatable fields
    r = await ed.call_raw("edit_title", {"path": path, "bake": "never", "ops": [
        {"op": "set_key", "animation": "Intro", "layer": "Title", "field": "name", "time": 0, "value": "x"}]})
    ed.check(r.is_error and "is not animatable" in r.text and "opacity" in r.text, r.text[:400])

    # one undo step per batch
    await ed.call("undo", scope="project")
    got = await ed.call("get_title", path=path)
    ed.check(got["animations"][0]["tracks"] == 1, "undo took the effect's step")
    await ed.call("undo", scope="project")
    got = await ed.call("get_title", path=path)
    ed.check("animations" not in got, str(got.get("animations")))


async def test_filmstrip_times_rise_and_motion_is_read_as_numbers(ed: Editor):
    path = "assets/anim_strip.titleart"
    await title(ed, path, ops=[
        {"op": "add_animation", "name": "Intro", "preset": "stamp_in"},
    ])
    r = await ed.call("render_title", path=path, image="none", options={"animation": {"name": "Intro", "frames": 6}})
    cells = r["cells"]
    ed.check(len(cells) == 6, str(cells)[:300])
    times = [c["time"] for c in cells]
    ed.check(times == sorted(times) and times[0] == 0 and abs(times[-1] - 0.5) < 1e-6, str(times))
    ed.check(any(c["changed_pct"] > 0 for c in cells[1:]), str(cells))
    # opacity 0 at the start: no ink; ink at the end
    ed.check(cells[0]["coverage_pct"] == 0, str(cells[0]))
    ed.check(cells[-1]["coverage_pct"] > 0, str(cells[-1]))
    # `as`, times and an unknown name
    r = await ed.call("render_title", path=path, image="none", options={"animation": {"name": "Intro", "times": [0.1], "as": "baked"}})
    ed.check(r["as"] == "baked" and len(r["cells"]) == 1, str(r)[:300])
    r = await ed.call_raw("render_title", {"path": path, "image": "none", "options": {"animation": {"name": "Nope"}}})
    ed.check(r.is_error and "Intro" in r.text, r.text[:300])
    r = await ed.call_raw("render_title", {"path": path, "image": "none", "options": {"animation": {"name": "Intro", "frames": 99}}})
    ed.check(r.is_error, r.text[:300])


async def test_glyph_cascade_renders_by_cells(ed: Editor):
    path = "assets/anim_glyphs.titleart"
    await title(ed, path, text="ABC", ops=[{"op": "add_animation", "name": "Cascade", "preset": "letter_cascade"}])
    r = await ed.call("render_title", path=path, image="none", options={"animation": {"name": "Cascade", "times": [0, 0.05, 0.4]}})
    cells = r["cells"]
    ed.check(cells[0]["coverage_pct"] == 0, f"every letter starts invisible: {cells[0]}")
    ed.check(cells[1]["glyph_cells"], str(cells[1]))
    ed.check(0 < cells[1]["coverage_pct"] < cells[2]["coverage_pct"], str(cells))


async def test_checks_seeded_failures_beside_their_fixes(ed: Editor):
    path = "assets/anim_checks.titleart"
    await title(ed, path, ops=[
        {"op": "add_animation", "name": "Seam", "role": "idle", "duration": 1, "wrap": "loop"},
        {"op": "set_track", "animation": "Seam", "field": "opacity", "drive": {"from": 0.2, "to": 1, "rate": 0.5}},
        {"op": "add_animation", "name": "Still", "role": "emphasis", "duration": 0.5},
        {"op": "set_track", "animation": "Still", "field": "opacity", "keys": [[0, 1], [0.5, 1]]},
        {"op": "add_animation", "name": "OffIntro", "role": "intro", "duration": 0.5},
        {"op": "set_track", "animation": "OffIntro", "field": "transform.position", "relative": True, "keys": [[0, [0, 0]], [0.5, [200, 0]]]},
        {"op": "add_animation", "name": "Visible", "role": "outro", "duration": 0.5},
        {"op": "set_track", "animation": "Visible", "field": "opacity", "keys": [[0, 1], [0.5, 0.8]]},
        {"op": "add_animation", "name": "Strobe", "role": "emphasis", "duration": 1},
        {"op": "set_track", "animation": "Strobe", "field": "flash.amount",
         "keys": [[k / 20, 3 if k % 2 else 0, "step"] for k in range(21)]},
    ])

    async def run(animation: str, check: str) -> dict:
        return check_of(await ed.call("check_title", path=path, checks=[check], animation=animation), check)

    # a loop with `rate` FAILs; the fix (cycles) PASSes
    seam = await run("Seam", "anim_loop_seamless")
    ed.check(seam["status"] == "FAIL", str(seam))
    await ed.call("edit_title", path=path, bake="never", ops=seam["fix"]["ops"])
    fixed = await run("Seam", "anim_loop_seamless")
    ed.check(fixed["status"] == "PASS", str(fixed))

    # a static animation FAILs and one moving key PASSes
    still = await run("Still", "anim_static")
    ed.check(still["status"] == "FAIL", str(still))
    await ed.call("edit_title", path=path, bake="never", ops=[{"op": "set_key", "animation": "Still", "field": "opacity", "time": 0.25, "value": 0.2}])
    ed.check((await run("Still", "anim_static"))["status"] == "PASS", "a moving key")

    # an intro ending off the still WARNs and its closing key PASSes
    off = await run("OffIntro", "anim_end_state")
    ed.check(off["status"] == "WARN", str(off))
    await ed.call("edit_title", path=path, bake="never", ops=off["fix"]["ops"])
    ed.check((await run("OffIntro", "anim_end_state"))["status"] == "PASS", "the closing key")

    # a visible outro end WARNs, with opacity 0 at D as the fix
    vis = await run("Visible", "anim_end_state")
    ed.check(vis["status"] == "WARN", str(vis))
    await ed.call("edit_title", path=path, bake="never", ops=vis["fix"]["ops"])
    ed.check((await run("Visible", "anim_end_state"))["status"] == "PASS", "ends hidden")

    # a 10 Hz strobe FAILs; retimed to about 2 Hz it PASSes (flash adds light, so the ink is dark:
    # on white ink it could not change the luminance at all)
    await ed.call("edit_title", path=path, bake="never", ops=[{"op": "set", "layer": "Title", "values": {"face.fill.color": "#303030"}}])
    strobe = await run("Strobe", "anim_flash")
    ed.check(strobe["status"] == "FAIL", str(strobe))
    ed.check("not a certification" in strobe["message"], strobe["message"])
    await ed.call("edit_title", path=path, bake="never", ops=[{"op": "retime", "animation": "Strobe", "scale": 5}])
    ed.check((await run("Strobe", "anim_flash"))["status"] == "PASS", "2 Hz")


async def test_mode_support_and_orphans(ed: Editor):
    path = "assets/anim_modes.titleart"
    await title(ed, path, ops=[
        {"op": "add_rim", "layer": "Title"},
        {"op": "add_animation", "name": "Win", "duration": 1},
        {"op": "set_track", "animation": "Win", "layer": "Title", "field": "rim#0.width", "keys": [[0, 4], [0.5, 8], [1, 4]]},
    ])
    await ed.call("edit_title", path=path, bake="never", ops=[{"op": "set", "values": {"runtime.glyph_layer": await layer_id(ed, path, "Title")}}])
    report = await ed.call("check_title", path=path, checks=["anim_mode_support"], animation="Win")
    rows = [c for c in report["checks"] if c["measured"].get("mode") == "glyph_atlas"]
    ed.check(rows and rows[0]["status"] == "FAIL", str(report["checks"])[:500])
    await ed.call("edit_title", path=path, bake="never", ops=rows[0]["fix"]["ops"])
    report = await ed.call("check_title", path=path, checks=["anim_mode_support"], animation="Win")
    ed.check(all(c["status"] != "FAIL" for c in report["checks"]), str(report["checks"])[:500])

    # an orphan WARNs and its fix applies
    await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "add_layer", "type": "shape", "name": "Gone"},
        {"op": "set_key", "animation": "Win", "layer": "Gone", "field": "opacity", "time": 0, "value": 1},
        {"op": "remove_layer", "layer": "Gone"},
    ])
    orphan = check_of(await ed.call("check_title", path=path, checks=["anim_orphans"]), "anim_orphans")
    ed.check(orphan["status"] == "WARN", str(orphan))
    await ed.call("edit_title", path=path, bake="never", ops=orphan["fix"]["ops"])
    orphan = check_of(await ed.call("check_title", path=path, checks=["anim_orphans"]), "anim_orphans")
    ed.check(orphan["status"] == "PASS", str(orphan))


async def layer_id(ed: Editor, path: str, name: str) -> str:
    got = await ed.call("get_title", path=path)
    return next(l["id"] for l in got["layers"] if l["name"] == name)


async def test_export_dry_run_and_files(ed: Editor):
    path = "assets/anim_export.titleart"
    await title(ed, path, ops=[
        {"op": "add_rim", "layer": "Title"},
        {"op": "add_animation", "name": "Pulse", "duration": 0.2},
        {"op": "set_animation", "animation": "Pulse", "values": {"flipbook.fps": 10, "flipbook.scale": 0.5}},
        {"op": "set_track", "animation": "Pulse", "layer": "Title", "field": "rim#0.width", "keys": [[0, 2], [0.1, 6], [0.2, 2]]},
        {"op": "add_animation", "name": "Fade", "preset": "fade_in"},
    ])
    root = Path(ed.project)
    r = await ed.call("export_title", path=path, dry_run=True)
    book = r["flipbooks"]
    pulse = next(a for a in book["animations"] if a["animation"] == "Pulse")
    ed.check(pulse["needed"] and pulse["frames"] == 3 and pulse["unique"] >= 2, str(pulse))
    ed.check(pulse["bytes"]["mobile"] > 0, str(pulse))
    fade = next(a for a in book["animations"] if a["animation"] == "Fade")
    ed.check(not fade["needed"], "a title-live animation needs no frames")
    ed.check(book["budget"]["mobile"]["budget_mib"] == 8, str(book["budget"]))
    ed.check(not (root / "assets/anim_export.titleart.anim.json").exists(), "a dry run writes nothing")

    r = await ed.call("export_title", path=path)
    data = root / "assets/anim_export.titleart.anim.json"
    ed.check(data.exists(), str(r)[:400])
    pulse = next(a for a in r["flipbooks"]["animations"] if a["animation"] == "Pulse")
    for f in pulse["files"]:
        ed.check((root / f).exists() and (root / (f + ".meta")).exists(), f)
    meta = (root / (pulse["files"][0] + ".meta")).read_text()
    ed.check("mipmaps: false" in meta or "mipmaps: 0" in meta, meta[:300])
    # a bake is not an undo step
    got = await ed.call("get_title", path=path)
    row = next(a for a in got["animations"] if a["name"] == "Pulse")
    ed.check(row["cost"]["baked"]["flipbook"]["baked"], str(row["cost"]["baked"]))


async def test_presets_listed(ed: Editor):
    await setup(ed)
    r = await ed.call("list_title_library", kind="animation_presets", limit=200)
    ed.check(r["total"] >= 28, str(r)[:300])
    row = next(x for x in r["rows"] if x["id"] == "stamp_in")
    ed.check(row["name"] == "Stamp In" and row["group"] == "in" and row["title_live"], str(row))
    detail = await ed.call("list_title_library", kind="animation_presets", id="stamp_in")
    ed.check(any(p["name"] == "from_scale" for p in detail["params"]), str(detail)[:300])
    r = await ed.call_raw("list_title_library", {"kind": "animation_presets", "id": "stamp_inn"})
    ed.check(r.is_error and "stamp_in" in r.text, r.text[:300])
    filtered = await ed.call("list_title_library", kind="animation_presets", category="out")
    ed.check(filtered["total"] == 3, str(filtered)[:300])
