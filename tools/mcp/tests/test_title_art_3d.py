# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

"""S86 AC31 (partly): the 3D surface of the title_art tools. render_title's quality and debug_view
options with stats_3d, try on 3D fields, the 3D ops through edit_title, get_title's three_d and
stats_3d, check_title's 3d group and export_title format glb."""

import json
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


async def test_realtime_3d_quality_setting(ed: Editor):
    """S86 R37: title_art.realtime_3d_quality, a tier per platform, undoable; a wrong tier or platform
    is refused naming the valid ones."""
    await setup(ed)
    got = (await ed.call("get_project_settings", keys=["title_art.realtime_3d_quality"]))
    text = json.dumps(got)
    for platform, tier in (("windows", "high"), ("ios", "medium"), ("android", "low"), ("web", "low")):
        ed.check_in(f'"{platform}": "{tier}"', text, f"the default of {platform}: {got}")
    await ed.call("set_project_settings", values={"title_art.realtime_3d_quality": {"android": "medium"}})
    ed.check_in('"android": "medium"', json.dumps(await ed.call("get_project_settings", keys=["title_art.realtime_3d_quality"])))
    await ed.call("undo", scope="project")
    ed.check_in('"android": "low"', json.dumps(await ed.call("get_project_settings", keys=["title_art.realtime_3d_quality"])), "undo")
    bad = await ed.check_error("set_project_settings", values={"title_art.realtime_3d_quality": {"android": "ultra"}})
    ed.check_in("high, medium or low", bad)
    bad = await ed.check_error("set_project_settings", values={"title_art.realtime_3d_quality": {"psp": "low"}})
    ed.check_in("windows, linux, macos, ios, android or web", bad)


async def test_export_cooks_realtime_3d(ed: Editor):
    """S86 R26, R38 (AC26's export side): a source export of a Realtime 3D title whose geometry is fixed
    ships its meshes at every Realtime tier and its environment cube per texture family
    (assets/je-titleart/index.json); an Exposed text on a second Realtime 3D title ships no mesh for
    that layer. (A source export keeps every module: the mesher switch is the Compile smoke's,
    tools/fixtures/title3d-fixed, and TitleArtModuleUsage.ThreeDSubOptions'.)"""
    await setup(ed)
    fixed, dynamic = "assets/titles/fixed3d.titleart", "assets/titles/dynamic3d.titleart"
    for path in (fixed, dynamic):
        await create3D(ed, path)
        await ed.call("edit_title", path=path, ops=[{"op": "set", "values": {"scene_3d.environment.preset": "softbox"}}])
        await ed.call("export_title", path=path)
    # a new title exposes its text: the fixed one drops that, the dynamic one keeps it
    await ed.call("edit_title", path=fixed, ops=[{"op": "unexpose", "name": "text"}])
    await ed.call("export_title", path=fixed)
    await ed.call("set_project_settings", values={"title_art.realtime_3d_quality": {p: "low" for p in
                                                  ("windows", "linux", "macos", "ios", "android", "web")}})
    made = await ed.call("create_scene", name="Cooked3D", type="UI", path="scenes/Cooked3D")
    scene = made["scene"]["id"]
    await ed.call("create_ui", scene=scene, root={"type": "image", "name": "Fixed", "layout": {"anchor": "center", "size": [300, 150]}})
    await ed.call("add_component", scene=scene, entity="Fixed", component="titleart")
    await ed.call("set_properties", scene=scene, entity="Fixed", values={"titleart": {"title": fixed, "renderMode": "realtime",
                                                                                       "prebakeInitialState": False}})
    await ed.call("save_scene", scene=scene)
    await ed.call("set_project_settings", values={"title_art.stale_bakes_on_export": "warn"})

    async def export(name: str):
        out = ed.scratch.root / name
        r = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "platform": "linux", "backends": ["opengl"],
                                                "allow_missing_assets": True, "overwrite": True, "timeout_seconds": 900})
        ed.check(not r.is_error and (r.structured or {}).get("status") == "succeeded", f"the export: {r.text[:400]}")
        return out

    await ed.call("set_project_settings", values={"title_art.frame_budget_ms": {"desktop": 3.5}, "title_art.reference_height": 1440})
    out = await export("export-cooked3d")
    # S81 R58: the project's settings reach the build
    written = json.loads(next(out.rglob("je-titleart/settings.json")).read_text())
    ed.check_eq(written["frame_budget_ms"]["desktop"], 3.5)
    ed.check_eq(written["reference_height"], 1440)
    await ed.call("set_project_settings", values={"title_art.reference_height": 0})
    indexes = list(out.rglob("je-titleart/index.json"))
    ed.check_eq(len(indexes), 1, f"one index: {indexes}")
    index = json.loads(indexes[0].read_text())
    root = indexes[0].parent
    ed.check_eq(index["version"], 1)
    ed.check_eq(index["realtime_3d_quality"]["linux"], "low")
    entry = index["titles"].get(fixed)
    ed.check(entry is not None, f"the fixed title is listed: {sorted(index['titles'])}")
    ed.check_eq(sorted(entry["environments"]), ["softbox"])
    ed.check_eq(len(entry["meshes"]), 1, f"its 3D layer's meshes: {entry['meshes']}")
    tiers = next(iter(entry["meshes"].values()))
    ed.check_eq(sorted(tiers), ["high", "low", "medium"], "a mesh per Realtime tier")
    for file in tiers.values():
        data = (root / file).read_bytes()
        ed.check(data[:4] == b"JTM1", f"{file} is a .titlemesh")
    families = index["environments"]
    ed.check_eq(sorted(families), ["desktop", "mobile", "web"], "a source export carries every family (android_etc2 only with its fallback setting)")
    for family, presets in families.items():
        ed.check((root / presets["softbox"]).read_bytes()[:12] == b"\xabKTX 20\xbb\r\n\x1a\n", f"{family}'s softbox cube is a KTX2")

    await ed.call("create_ui", scene=scene, root={"type": "image", "name": "Dynamic", "layout": {"anchor": "top_center", "size": [300, 150]}})
    await ed.call("add_component", scene=scene, entity="Dynamic", component="titleart")
    await ed.call("set_properties", scene=scene, entity="Dynamic", values={"titleart": {"title": dynamic, "renderMode": "realtime"}})
    await ed.call("save_scene", scene=scene)
    out = await export("export-dynamic3d")
    index = json.loads(next(out.rglob("je-titleart/index.json")).read_text())
    ed.check_eq(index["titles"][dynamic]["meshes"], {}, "an Exposed text: the layer meshes at runtime")
