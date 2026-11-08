# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

"""S86 AC31 (partly): the 3D surface of the title_art tools. render_title's quality and debug_view
options with stats_3d, try on 3D fields, the 3D ops through edit_title, get_title's three_d and
stats_3d, check_title's 3d group and export_title format glb."""

import time

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Art 3D"}
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    return S


async def render(ed: Editor, args: dict):
    """render_title, waiting for its job when the render outlasts the call (a cold environment or
    new programs drawn a frame apart)."""
    r = await ed.call_raw("render_title", args)
    job = (r.structured or {}).get("job_id") if not r.is_error else None
    if not job:
        return r
    deadline = time.monotonic() + TIMEOUT
    state = await ed.call("get_job", job_id=job)
    while state.get("status") == "running" and time.monotonic() < deadline:
        state = await ed.call("get_job", job_id=job)
    ed.check_eq(state.get("status"), "succeeded", f"the render job: {state}")
    # the finished render is cached: asked again, it answers with the image at once
    return await ed.call_raw("render_title", args)


async def create3D(ed: Editor, path: str) -> None:
    await ed.call("edit_title", path=path, create={"text": "Gold"}, bake="never")
    await ed.call("edit_title", path=path, ops=[{"op": "set_dimension", "layer": "Title", "dimension": "3d"},
                                                {"op": "apply_material", "layer": "Title", "part": "face", "preset": "gold"}])


async def test_group_hidden(ed: Editor):
    """The 3D surface lives in the title_art group: hidden until tool_groups enables it."""
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("render_title" not in default and "edit_title" not in default, "title_art tools hidden by default")
    await setup(ed)
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("render_title" in listed, "listed once title_art is enabled")


async def test_quality_and_debug_views(ed: Editor):
    """R60: quality renders as that tier and reports it with stats_3d (k = 1 at low); every
    debug_view returns an image."""
    await setup(ed)
    path = "assets/titles/gold3d.titleart"
    await create3D(ed, path)
    low = await render(ed, {"path": path, "options": {"quality": "low"}})
    ed.check(not low.is_error and low.images, f"a low tier image: {low.text[:300]}")
    ed.check_eq(low.structured.get("quality"), "low", "the tier is reported")
    stats = low.structured.get("stats_3d") or []
    ed.check_eq(len(stats), 1, f"one 3D layer in stats_3d: {stats}")
    ed.check_eq(stats[0]["tier"], "low", "stats at the requested tier")
    ed.check_eq(stats[0]["k"], 1, "low renders without supersampling")
    ed.check(stats[0]["triangles"] > 0, "the mesh has triangles")
    ed.check_in("low", stats[0]["tiers"], "the tier fits are listed")
    for view in ["none", "normals", "environment", "ao", "glint", "ink", "wireframe"]:
        r = await render(ed, {"path": path, "options": {"debug_view": view}})
        ed.check(not r.is_error and r.images, f"debug_view {view}: {r.text[:300]}")
    msg = await ed.check_error("render_title", path=path, options={"debug_view": "x"})
    ed.check_in("bad_option", msg, "an unknown view")
    msg = await ed.check_error("render_title", path=path, options={"quality": "ultra"})
    ed.check_in("bad_option", msg, "an unknown tier")


async def test_try_leaves_the_file(ed: Editor):
    """R60: try variants work on 3D fields and never touch the file."""
    await setup(ed)
    path = "assets/titles/try3d.titleart"
    await create3D(ed, path)
    text = (ed.project / path).read_text()
    sheet = await ed.call_raw("render_title", {"path": path, "try": [
        {"label": "coarse", "ops": [{"op": "set", "layer": "Title", "values": {"3d.curve_segments": 3}}]},
        {"label": "chrome", "ops": [{"op": "apply_material", "layer": "Title", "part": "face", "preset": "chrome"}]}]})
    ed.check(not sheet.is_error, sheet.text[:300])
    ed.check_eq([c["label"] for c in sheet.structured["cells"]], ["current", "coarse", "chrome"], "the try cells")
    ed.check_eq((ed.project / path).read_text(), text, "try never touches the file")


async def test_ops_get_and_checks(ed: Editor):
    """R59, R61: band ops undo by name; get_title returns three_d and stats_3d; check_title's 3d
    group runs the tier budgets and the z-fighting check (WARN with a fix that, applied, PASSes)."""
    await setup(ed)
    path = "assets/titles/bands3d.titleart"
    await create3D(ed, path)
    added = await ed.call_raw("edit_title", {"path": path, "ops": [{"op": "add_band", "layer": "Title", "preset": "plate"}]})
    ed.check(not added.is_error, added.text[:300])
    ed.check_in("Add Band", added.text, "the undo step is named after the op")
    got = await ed.call("get_title", path=path)
    layer = next(l for l in got["layers"] if l.get("name") == "Title")
    ed.check_in("three_d", layer, "a 3D layer reports its 3D style")
    ed.check(got.get("stats_3d"), "get_title reports stats_3d")
    await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"band#0.offset": -0.1}}])
    report = await ed.call("check_title", path=path, checks=["3d"])
    ids = {c["id"]: c for c in report["checks"]}
    ed.check_in("3d_tier_budget.low", ids, f"the tier budgets run: {list(ids)}")
    z = ids.get("3d_z_fighting")
    ed.check(z and z["status"] == "WARN" and z.get("fix"), f"z-fighting warns with a fix: {z}")
    await ed.call("edit_title", path=path, ops=z["fix"]["ops"])
    again = await ed.call("check_title", path=path, checks=["3d_z_fighting"])
    ed.check_eq(again["checks"][0]["status"], "PASS", "the fix passes")


async def test_export_glb(ed: Editor):
    """R50: export_title format glb writes a binary glTF beside the document; dry_run writes nothing."""
    await setup(ed)
    path = "assets/titles/glb3d.titleart"
    await create3D(ed, path)
    dry = await ed.call("export_title", path=path, format="glb", dry_run=True)
    ed.check(not (ed.project / dry["path"]).exists(), "dry_run writes nothing")
    out = await ed.call("export_title", path=path, format="glb")
    ed.check_eq(out["layers"], 1, "one 3D layer")
    data = (ed.project / out["path"]).read_bytes()
    ed.check_eq(data[:4], b"glTF", "a binary glTF")
    ed.check(out["triangles"] > 0, "with triangles")
