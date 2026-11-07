"""S80 Title Art core: the `title_art` group (docs/specs/S80-title-art-core.md AC5-AC10, AC26, AC27,
AC34-AC37, AC48; jasper://docs/title-art): list_title_library, get_title, edit_title,
render_title, check_title, export_title, the .titleart parts of get_asset_info and view_asset,
Missing Assets, rename and move of a document with its derived PNGs, hot reload, Project Settings >
Title Art, the export's handling of documents and stale bakes, and `check --titles`.
"""

import asyncio
import json
import struct
import subprocess
import zlib
from pathlib import Path

import harness
import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Art Tools"}
TIMEOUT = 900

S: dict = {}

GROUP_TOOLS = ["list_title_library", "get_title", "edit_title", "render_title", "check_title", "export_title"]


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art", "editor_tools"])
    # bakes only when a test asks (Auto Bake is tested on its own)
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    status = await ed.call("editor_status")
    S["client"] = status.get("mcp", {}).get("client", {}).get("name", "") if isinstance(status, dict) else ""
    return S


async def create(ed: Editor, path: str, text: str = "Title", **create_args) -> dict:
    args = {"text": text}
    args.update(create_args)
    return await ed.call("edit_title", path=path, create=args, bake="never")


async def need_renderer(ed: Editor, path: str) -> None:
    """The document renders (every editor has the Title Art renderer): a failure here is the test's."""
    r = await ed.call_raw("render_title", {"path": path, "image": "none"})
    ed.check(not r.is_error, f"render_title: {r.text[:400]}")


def png_with_recipe(width: int, height: int, recipe: dict) -> bytes:
    """A transparent RGBA PNG carrying a jasper.titleart iTXt recipe (what a bake writes)."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + b"\x00" * (width * 4) for _ in range(height))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    itxt = b"jasper.titleart\x00\x00\x00\x00\x00" + json.dumps(recipe).encode()
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"iTXt", itxt) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


# ---------------------------------------------------------------------------------------- group


async def test_group_hidden(ed: Editor):
    """AC34: the six tools are hidden until tool_groups enables title_art; diff_images is in editor_tools."""
    default = {t.name for t in await ed.tools(groups=None)}
    for name in GROUP_TOOLS:
        ed.check(name not in default, f"{name} hidden until title_art is enabled")
    ed.check("diff_images" not in default, "diff_images hidden until editor_tools is enabled")
    await ed.call("tool_groups", enable=["title_art"])
    listed = {t.name: t for t in await ed.tools(groups=None)}
    for name in GROUP_TOOLS:
        ed.check(name in listed, f"{name} listed once title_art is enabled")
    await setup(ed)


# ---------------------------------------------------------------------------------------- library


async def test_list_title_library(ed: Editor):
    await setup(ed)
    counts = await ed.call("list_title_library")
    ed.check_in("blend_modes", counts["counts"], "counts per kind")
    ed.check_eq(counts["counts"]["blend_modes"], 18, "18 blend modes")
    modes = await ed.call("list_title_library", kind="blend_modes", limit=50)
    ed.check_eq(modes["rows"][0]["id"], "normal", "Normal is mode 0")
    ed.check_eq(modes["rows"][17]["id"], "luminosity", "Luminosity is mode 17")
    fonts = await ed.call("list_title_library", kind="fonts")
    ed.check(any(r["path"] == "(built-in)" for r in fonts["rows"]), f"the built-in face is a font row: {fonts}")
    builtin = next(r for r in fonts["rows"] if r["path"] == "(built-in)")
    ed.check_in("latin", builtin["scripts"], "the built-in face covers Latin")
    later = await ed.call("list_title_library", kind="looks")
    ed.check_eq(later["rows"], [], "looks arrive later: an empty list")
    ed.check_in("S84", later["note"], "names the spec that brings them")
    msg = await ed.check_error("list_title_library", kind="effects", id="outer_glwo")
    ed.check_in("outer_glwo", msg, "the unknown id is named")
    # rows and details are the registry's own (titleEffectRow / titleEffectDetail, AC25)
    effects = await ed.call("list_title_library", kind="effects", limit=50)
    glow = next(r for r in effects["rows"] if r["id"] == "outer_glow")
    for key in ("name", "category", "cost", "adds_light", "on_parts", "outline", "fallback", "glyph_safety", "live", "aliases"):
        ed.check_in(key, glow, f"an effect row carries {key}: {glow}")
    detail = await ed.call("list_title_library", kind="effects", id="outer_glow")
    ed.check_eq(detail.get("add"), {"op": "add_effect", "layer": "<layer>", "type": "outer_glow"}, f"the op that adds it: {detail}")
    ed.check_in("size", detail.get("schema", {}).get("properties", {}), "the params' schema")
    ed.check(any(line.startswith("size ") for line in detail.get("params", [])), f"one line per param: {detail.get('params')}")
    generators = await ed.call("list_title_library", kind="generators")
    fill = next(r for r in generators["rows"] if r["id"] == "fill")
    ed.check_eq(fill.get("fills_canvas"), True, f"a generator row: {fill}")


# ---------------------------------------------------------------------------------------- edit


async def test_create_and_get(ed: Editor):
    """R7, AC8 (document part): edit_title create makes the document; get_title reads it."""
    await setup(ed)
    made = await create(ed, "assets/titles/hello.titleart", text="Hello")
    ed.check(made.get("created"), f"created: {made}")
    ed.check((ed.project / "assets/titles/hello.titleart").exists(), "the file exists")
    got = await ed.call("get_title", path="assets/titles/hello.titleart")
    ed.check_eq(got["canvas"]["width"], 1920, "canvas 1920 x 1080")
    ed.check_eq(len(got["layers"]), 1, "one layer")
    ed.check_eq(got["layers"][0]["text"], "Hello", "its text")
    ed.check_eq(got["layers"][0]["path"], "Title", "the layer path")
    ed.check_eq(got["exposed"][0]["name"], "text", "the exposed parameter `text`")
    ed.check_eq(got["outputs"][0]["id"], "main", "output main")
    ed.check_eq(got["outputs"][0]["status"], "missing", "never baked")
    again = await ed.check_error("edit_title", path="assets/titles/hello.titleart", create={})
    ed.check_in("exists already", again, "create refuses an existing file")
    ed.check_in("Nothing was applied", again, "and says nothing changed")
    over = await ed.call("get_title", path="assets/titles/hello.titleart", detail="overrides")
    ed.check_eq(over["layers"][0]["overrides"].get("text"), "Hello", "overrides list the non-default text")
    fields = await ed.call("get_title", path="assets/titles/hello.titleart", fields=["size", "style.rims[*].width"])
    ed.check_eq(fields["layers"][0]["fields"].get("size"), 200, "fields read exact values")


async def test_edit_atomic_undo(ed: Editor):
    """AC10: all or nothing, did-you-mean on a bad path, % echoes px, one undo step, expect_rev."""
    await setup(ed)
    path = "assets/titles/atomic.titleart"
    await create(ed, path)
    before = (ed.project / path).read_text()
    rev = (await ed.call("get_title", path=path))["rev"]
    r = await ed.call_raw("edit_title", {"path": path, "ops": [
        {"op": "set", "layer": "Title", "values": {"text": "Changed"}},
        {"op": "set", "layer": "Title", "values": {"style.rims[0].widht": 3}},
    ]})
    ed.check(r.is_error, "a bad op fails the batch")
    ed.check_in("Nothing was applied", r.text, "the error says nothing was applied")
    ed.check_eq((ed.project / path).read_text(), before, "the file is unchanged (all or nothing)")
    bad = await ed.call_raw("edit_title", {"path": path, "ops": [{"op": "set", "layer": "Title", "values": {"sise": 10}}]})
    ed.check(bad.is_error, "an unknown field fails")
    problems = (bad.structured or {}).get("problems", [])
    ed.check(any(p.get("nearest") == "size" for p in problems), f"did you mean size: {problems}")
    ok = await ed.call("edit_title", path=path, ops=[{"op": "add_rim", "layer": "Title", "values": {"width": 5}},
                                                     {"op": "set", "layer": "Title", "values": {"text": "Atomic"}}])
    ed.check_eq(ok["applied"], 2, "two ops applied")
    ed.check(ok["rev"] > rev, "the rev rose")
    undo = ok.get("undo") or {}
    ed.check(undo.get("id"), f"one undo step: {ok}")
    width = await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"rim#0.width": 10}}])
    change = width["changes"][0]
    ed.check_eq(change["path"], "style.rims[0].width", "the shorthand is echoed in its long form")
    ed.check(abs(change.get("px", 0) - 20.0) < 1e-6, f"10 % of size 200 is 20 px: {change}")
    stale = await ed.check_error("edit_title", path=path, expect_rev=rev, ops=[{"op": "set", "layer": "Title", "values": {"text": "x"}}])
    ed.check_in("rev", stale, "expect_rev refuses a document that moved on")
    await ed.call("undo", scope="project")
    got = await ed.call("get_title", path=path)
    ed.check_eq(got["layers"][0]["text"], "Atomic", "undo removes the last step only (the rim width)")
    dry = await ed.call("edit_title", path=path, dry_run=True, ops=[{"op": "set", "layer": "Title", "values": {"text": "Dry"}}])
    ed.check_eq(dry["applied"], 1, "a dry run validates")
    ed.check_eq((await ed.call("get_title", path=path))["layers"][0]["text"], "Atomic", "and writes nothing")


async def test_reserved_and_options(ed: Editor):
    """AC48: reserved ops answer not_available naming their owner; unknown render options and later
    export formats fail before anything renders."""
    await setup(ed)
    path = "assets/titles/reserved.titleart"
    await create(ed, path)
    msg = await ed.check_error("edit_title", path=path, ops=[{"op": "apply_look", "look": "gold"}])
    ed.check_in("S84", msg, "apply_look names S84")
    msg = await ed.check_error("edit_title", path=path, ops=[{"op": "add_band", "layer": "Title"}])
    ed.check_in("S86", msg, "add_band names S86")
    msg = await ed.check_error("render_title", path=path, options={"x": 1})
    ed.check_in("bad_option", msg, "an unknown option key")
    ed.check_in("languages", msg, "lists the keys later features add")
    msg = await ed.check_error("render_title", path=path, options={"animation": {}})
    ed.check_in("S85", msg, "a later spec's key names its owner")
    msg = await ed.check_error("export_title", path=path, format="psd")
    ed.check_in("not_available", msg, "psd is a later format")
    ed.check_in("S87", msg, "naming S87")
    msg = await ed.check_error("export_title", path=path, options={"nope": 1})
    ed.check_in("bleed_px", msg, "bad_option lists the valid keys")
    msg = await ed.check_error("edit_title", path=path, create={"template": "casino"})
    ed.check_in("S84", msg, "templates arrive with S84")


# ---------------------------------------------------------------------------------------- checks


async def test_check_document(ed: Editor):
    """AC36 (document checks): a dead exposed target FAILs with an unexpose fix; applying it passes;
    the guidance is always there."""
    await setup(ed)
    path = "assets/titles/checks.titleart"
    await create(ed, path)
    # a hand edit leaves the exposed parameter `text` pointing at a layer that is gone
    file = ed.project / path
    doc = json.loads(file.read_text())
    doc["exposed"][0]["targets"][0]["layer"] = "deadbeef"
    file.write_text(json.dumps(doc, indent=2))
    report = await ed.call("check_title", path=path, checks=["exposed_targets", "document_valid", "glyphs_present", "assets_present"])
    ed.check_in("passing them does not mean the title looks good", report["guidance"], "the guidance")
    by_id = {c["id"]: c for c in report["checks"]}
    exposed = by_id["exposed_targets"]
    ed.check_eq(exposed["status"], "FAIL", f"the dead target fails: {exposed}")
    fix = exposed.get("fix", {}).get("ops")
    ed.check(fix and fix[0]["op"] == "unexpose", f"a ready fix: {exposed}")
    await ed.call("edit_title", path=path, ops=fix)
    again = await ed.call("check_title", path=path, checks=["exposed_targets"])
    ed.check_eq(again["checks"][0]["status"], "PASS", "the fix makes it pass")
    ed.check_eq(by_id["glyphs_present"]["status"], "PASS", "Latin text has its glyphs")
    unknown = await ed.check_error("check_title", path=path, checks=["no_such_check"])
    ed.check_in("not_clipped", unknown, "an unknown check lists the valid ids")


async def test_missing_font(ed: Editor):
    """AC5: a missing font is a dependency edge Missing Assets lists with the document and field;
    get_title and check_title report it; glyphs it would have drawn are checked against the chain."""
    await setup(ed)
    path = "assets/titles/missing.titleart"
    await create(ed, path)
    await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title",
                                                 "values": {"font": {"path": "fonts/nowhere.ttf", "guid": "0123456789abcdef0123456789abcdef"}}}])
    found = False
    for _ in range(40):
        missing = await ed.call("list_missing_assets")
        for group in missing.get("missing", missing.get("groups", [])):
            for ref in group.get("referrers", []):
                if ref.get("owner", "").endswith("missing.titleart") and "font" in ref.get("field", ""):
                    found = True
        if found:
            break
        await asyncio.sleep(0.25)
    ed.check(found, f"Missing Assets names the document and its font field: {missing}")
    got = await ed.call("get_title", path=path)
    ed.check(any(p.get("code") == "asset_missing" for p in got["problems"]), f"get_title reports it: {got['problems']}")
    report = await ed.call("check_title", path=path, checks=["assets_present"])
    ed.check_eq(report["checks"][0]["status"], "FAIL", "assets_present fails")


async def test_check_titles_cli(ed: Editor):
    """AC37: check --titles without a GPU: codes and exit status on the scratch project."""
    await setup(ed)
    path = "assets/titles/cli.titleart"
    await create(ed, path)
    await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"fallback_fonts": [{"path": "fonts/gone.otf"}]}}])
    proc = await asyncio.to_thread(subprocess.run, harness.editor_command() + ["check", "--titles", "--project", str(ed.project), "--format", "json"],
                                   capture_output=True, text=True, timeout=300)
    out = proc.stdout
    ed.check_in("title-asset-missing", out, f"the missing fallback font is an error: {out[:800]} {proc.stderr[-400:]}")
    ed.check_in("title-bake-missing", out, "an unbaked output is a warning")
    ed.check_in("export_title", out, "with the command that bakes it")
    ed.check_eq(proc.returncode, 1, "errors exit 1")
    files = await asyncio.to_thread(subprocess.run, harness.editor_command() + ["check", "--titles", "--project", str(ed.project),
                                                                                   "--files", str(ed.project / "assets/titles/hello.titleart")],
                                    capture_output=True, text=True, timeout=300)
    ed.check("cli.titleart" not in files.stdout, f"--files checks the named document only: {files.stdout[:400]}")


# ---------------------------------------------------------------------------------------- assets


async def test_asset_info_and_view(ed: Editor):
    """AC37: get_asset_info on a .titleart adds canvas, layers, fonts, outputs and problems."""
    await setup(ed)
    path = "assets/titles/info.titleart"
    await create(ed, path)
    info = await ed.call("get_asset_info", path=path)
    title = info.get("title_art", {})
    ed.check_eq(title.get("canvas"), [1920, 1080], f"canvas: {info}")
    ed.check_eq(title.get("layers", {}).get("text"), 1, "layer counts")
    ed.check(any(f["font"] == "(built-in)" for f in title.get("fonts", [])), "the built-in font")
    ed.check_eq(title["outputs"][0]["status"], "missing", "the output is not baked")
    view = await ed.call_raw("view_asset", {"path": path})
    ed.check(not view.is_error, f"view_asset: {view.text[:300]}")
    ed.check(view.images, "view_asset returns an image")


async def test_move_document(ed: Editor):
    """AC6: moving a .titleart moves its derived PNG and meta, keeps the PNG's GUID and rewrites its
    recipe's source path."""
    await setup(ed)
    path = "assets/titles/mover.titleart"
    await create(ed, path)
    guid = (await ed.call("get_title", path=path))["guid"]
    png = ed.project / "assets/titles/mover.titleart.png"
    png.write_bytes(png_with_recipe(8, 4, {"format": "jasperengine.titleart.bake", "version": 1,
                                           "source": {"guid": guid, "path": "titles/mover.titleart"}, "source_hash": "x",
                                           "assets": [], "bake_version": 1, "output": "main", "frame": [0, 0, 8, 4]}))
    await ed.call("sync_files")
    before = await ed.call("get_asset_info", path="assets/titles/mover.titleart.png")
    png_guid = before.get("guid")
    ed.check_eq(before.get("title_art_bake", {}).get("output"), "main", f"the PNG's recipe names its output: {before}")
    (ed.project / "assets/moved").mkdir(parents=True, exist_ok=True)
    await ed.call("sync_files")
    await ed.call("move_asset", path=path, to="assets/moved/renamed.titleart")
    moved_png = ed.project / "assets/moved/renamed.titleart.png"
    ed.check(moved_png.exists(), "the PNG moved and was renamed with the document")
    ed.check(not png.exists(), "nothing left behind")
    after = await ed.call("get_asset_info", path="assets/moved/renamed.titleart.png")
    ed.check_eq(after.get("guid"), png_guid, "the PNG keeps its GUID (scene references follow)")
    ed.check_eq(after.get("title_art_bake", {}).get("source"), "assets/moved/renamed.titleart", f"the recipe names the new path: {after}")


async def test_hot_reload(ed: Editor):
    """AC7 (document part): another program's edit reloads the open document."""
    await setup(ed)
    path = "assets/titles/reload.titleart"
    await create(ed, path)
    rev = (await ed.call("get_title", path=path))["rev"]
    file = ed.project / path
    file.write_text(file.read_text().replace('"text": "Title"', '"text": "Outside"', 1))
    got = await ed.call("get_title", path=path)
    ed.check_eq(got["layers"][0]["text"], "Outside", "the outside edit is read")
    ed.check(got["rev"] > rev, "a new rev")
    file.write_text("{ not json")
    r = await ed.call_raw("get_title", {"path": path})
    changes = (r.structured or {}).get("external_changes", {})
    ed.check(any("reload.titleart" in p.get("path", "") for p in changes.get("problems", [])) or r.is_error,
             f"a broken file is a problem with its path: {r.text[:300]}")


async def test_project_settings(ed: Editor):
    """R52: Project Settings > Title Art through set_project_settings, undoable."""
    await setup(ed)
    await ed.call("set_project_settings", values={"title_art.stale_bakes_on_export": "warn"})
    got = await ed.call("get_project_settings", keys=["title_art.stale_bakes_on_export", "title_art.auto_bake"])
    text = json.dumps(got)
    ed.check_in("warn", text, f"the policy is stored: {got}")
    await ed.call("undo", scope="project")
    got = await ed.call("get_project_settings", keys=["title_art.stale_bakes_on_export"])
    ed.check_in("rebake", json.dumps(got), f"undo restores the default: {got}")
    bad = await ed.check_error("set_project_settings", values={"title_art.stale_bakes_on_export": "sometimes"})
    ed.check_in("rebake", bad, "the valid values are listed")


async def export(ed: Editor, name: str):
    """A source export for Linux (no compiler needed): the raw result, which may be an error."""
    out = ed.scratch.root / name
    r = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "platform": "linux", "backends": ["opengl"],
                                            "allow_missing_assets": True, "overwrite": True, "timeout_seconds": 900})
    return out, r


async def export_scene(ed: Editor) -> None:
    """An export needs a saved scene in the build (once per editor)."""
    if not S.get("export_scene"):
        await ed.call("create_scene", name="Menu", type="2D", path="scenes/Menu")
        await ed.call("save_project")
        S["export_scene"] = True


async def test_export_excludes_document(ed: Editor):
    """AC9: an export ships the baked PNG and no .titleart."""
    await setup(ed)
    path = "assets/titles/shipped.titleart"
    await create(ed, path)
    await need_renderer(ed, path)
    await ed.call("export_title", path=path)
    await export_scene(ed)
    await ed.call("set_project_settings", values={"title_art.stale_bakes_on_export": "warn"})
    out, r = await export(ed, "export-excludes")
    ed.check(not r.is_error and (r.structured or {}).get("status") == "succeeded", f"the export: {r.text[:300]}")
    ed.check_eq([p.name for p in out.rglob("*.titleart")], [], "no .titleart in the export")
    shipped = [p.name for p in out.rglob("*") if p.is_file() and "shipped" in p.name]
    ed.check(shipped, "the baked PNG ships (as itself or cooked under its name)")


async def test_export_stale_policy(ed: Editor):
    """AC27: title_art.stale_bakes_on_export `rebake` bakes a stale output before the textures cook
    (the document is fresh afterwards, the export succeeds); `fail` fails naming the document and
    writes nothing to its PNG."""
    await setup(ed)
    path = "assets/titles/policy.titleart"
    png = ed.project / "assets/titles/policy.titleart.png"
    await create(ed, path)
    await need_renderer(ed, path)
    await ed.call("export_title", path=path)
    await export_scene(ed)
    try:
        await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"text": "Rebaked"}}])
        ed.check_eq((await ed.call("get_title", path=path))["outputs"][0]["status"], "stale", "the edit made it stale")
        before = png.read_bytes()
        await ed.call("set_project_settings", values={"title_art.stale_bakes_on_export": "rebake"})
        _, r = await export(ed, "export-rebake")
        ed.check(not r.is_error and (r.structured or {}).get("status") == "succeeded", f"rebake exports: {r.text[:300]}")
        ed.check_eq((await ed.call("get_title", path=path))["outputs"][0]["status"], "fresh", "rebake baked the stale output")
        ed.check(png.read_bytes() != before, "the PNG holds the new text")

        await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"text": "Stale again"}}])
        baked = png.read_bytes()
        await ed.call("set_project_settings", values={"title_art.stale_bakes_on_export": "fail"})
        _, r = await export(ed, "export-fail")
        ed.check(r.is_error or (r.structured or {}).get("status") == "failed", f"fail fails the export: {r.text[:300]}")
        ed.check_in("policy.titleart", r.text + json.dumps(r.structured or {}), "naming the document")
        ed.check_eq(png.read_bytes(), baked, "fail bakes nothing")
        ed.check_eq((await ed.call("get_title", path=path))["outputs"][0]["status"], "stale", "the output stays stale")
    finally:
        await ed.call("set_project_settings", values={"title_art.stale_bakes_on_export": "rebake"})


# ---------------------------------------------------------------------------------------- pixels


async def test_render_title(ed: Editor):
    """AC35: try sheets leave the file untouched; compare rev:n is 0 for a no-op and > 0 for a colour
    change; crop and backgrounds change the image only; save_path keeps real alpha."""
    await setup(ed)
    path = "assets/titles/render.titleart"
    await create(ed, path)
    await need_renderer(ed, path)
    text = (ed.project / path).read_text()
    first = await ed.call_raw("render_title", {"path": path, "backgrounds": ["checker", "black"]})
    ed.check(not first.is_error and first.images, f"an image: {first.text[:300]}")
    rev = first.structured["rev"]
    ed.check_eq(len(first.structured["cells"]), 2, "one cell per background")
    sheet = await ed.call_raw("render_title", {"path": path, "try": [
        {"label": "red", "ops": [{"op": "set", "layer": "Title", "values": {"face.fill.color": "#ff0000"}}]},
        {"label": "big", "ops": [{"op": "set", "layer": "Title", "values": {"size": 320}}]},
        {"label": "rim", "ops": [{"op": "add_rim", "layer": "Title", "values": {"width": 6}}]}]})
    ed.check(not sheet.is_error, sheet.text[:300])
    labels = [c["label"] for c in sheet.structured["cells"]]
    ed.check_eq(labels, ["current", "red", "big", "rim"], "the numbered try cells")
    ed.check_eq((ed.project / path).read_text(), text, "try sheets never touch the file")
    same = await ed.call("render_title", path=path, compare=f"rev:{rev}", image="none")
    ed.check_eq(same["diff"]["changed_pct"], 0, "a no-op changes nothing")
    await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"face.fill.color": "#2050ff"}}])
    changed = await ed.call("render_title", path=path, compare=f"rev:{rev}", image="none")
    ed.check(changed["diff"]["changed_pct"] > 0, f"a colour change is seen: {changed['diff']}")
    cropped = await ed.call("render_title", path=path, crop=[760, 440, 400, 200], image="none")
    ed.check(cropped["size"][0] < changed["size"][0], "crop renders a part")
    saved = await ed.call("render_title", path=path, save_path="renders/real_alpha.png", backgrounds=["white"], image="none")
    img = pngdiff.load(str(ed.project / saved["saved_to"]))
    ed.check_eq(img[2][3], 0, "the saved render has real alpha (a transparent corner)")
    metrics = await ed.call("render_title", path=path, image="none", fallback="compare")
    ed.check_in("pixel_paths_diff", metrics, "fallback compare measures both paths")


async def test_bake_stale(ed: Editor):
    """AC26, AC37: export_title writes the shared-frame PNG with round_to sizes and the sidecar; an
    edit makes it stale; dry_run writes nothing."""
    await setup(ed)
    path = "assets/titles/bake.titleart"
    await create(ed, path)
    await need_renderer(ed, path)
    dry = await ed.call("export_title", path=path, dry_run=True)
    ed.check_eq(dry["files"][0]["status"], "would_write", "a dry run says what it would write")
    ed.check(not (ed.project / "assets/titles/bake.titleart.png").exists(), "and writes nothing")
    baked = await ed.call("export_title", path=path)
    f = baked["files"][0]
    ed.check_eq(f["status"], "written", f"written: {baked}")
    ed.check(f["size"][0] % 4 == 0 and f["size"][1] % 4 == 0, f"round_to 4: {f['size']}")
    ed.check(f["size"][0] < 1920, "trimmed to the content")
    ed.check((ed.project / "assets/titles/bake.titleart.bake.json").exists(), "the sidecar")
    sidecar = json.loads((ed.project / "assets/titles/bake.titleart.bake.json").read_text())
    ed.check_eq(sidecar["format"], "jasperengine.titleart.bakeinfo", "the sidecar format")
    ed.check_eq(sidecar["frame"]["w"], f["frame"][2], "the shared frame")
    ed.check_eq((await ed.call("get_title", path=path))["outputs"][0]["status"], "fresh", "fresh after the bake")
    again = await ed.call("export_title", path=path)
    ed.check_eq(again["files"][0]["status"], "fresh", "a fresh output is skipped")
    await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"text": "Stale"}}])
    out = (await ed.call("get_title", path=path))["outputs"][0]
    ed.check_eq(out["status"], "stale", "an edit makes it stale")
    ed.check_in("document", out.get("changed", []), "saying the document changed")


async def test_auto_bake(ed: Editor):
    """AC27 (Auto Bake): with title_art.auto_bake an edit rebakes existing outputs, not as an undo step."""
    await setup(ed)
    path = "assets/titles/auto.titleart"
    await create(ed, path)
    await need_renderer(ed, path)
    await ed.call("export_title", path=path)
    await ed.call("set_project_settings", values={"title_art.auto_bake": True})
    try:
        edited = await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"text": "Auto"}}])
        status = None
        for _ in range(80):
            await asyncio.sleep(0.1)
            status = (await ed.call("get_title", path=path))["outputs"][0]["status"]
            if status == "fresh":
                break
        ed.check_eq(status, "fresh", "Auto Bake rebaked the output")
        history = await ed.call("get_undo_history", limit=3)
        ed.check_eq(history["entries"][0]["id"], edited["undo"]["id"], "the bake is no undo step")
    finally:
        await ed.call("set_project_settings", values={"title_art.auto_bake": False})


async def test_checks_red_green(ed: Editor):
    """AC36 (pixel checks): a glyph clipped at the canvas edge FAILs not_clipped with a fix that
    passes once applied; a no-op effect WARNs with remove_effect."""
    await setup(ed)
    path = "assets/titles/clipped.titleart"
    await create(ed, path)
    await need_renderer(ed, path)
    await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"transform.position": [930, 0]}}])
    report = await ed.call("check_title", path=path, checks=["not_clipped"])
    check = report["checks"][0]
    ed.check_eq(check["status"], "FAIL", f"clipped: {check}")
    ed.check(check.get("at"), "with crops of where to look")
    await ed.call("edit_title", path=path, ops=[{"op": "set", "layer": "Title", "values": {"transform.position": [0, 0]}}])
    report = await ed.call("check_title", path=path, checks=["not_clipped", "not_flat", "deterministic"])
    ed.check(all(c["status"] in ("PASS", "WARN") for c in report["checks"]), f"centred again: {report['checks']}")
    ed.check_in("guidance", report, "the guidance is always present")


async def test_rim_spikes(ed: Editor):
    """rim_spikes (Deviation 53): a wide mitred rim on NMVW in the built-in font pokes its notch
    mitres through the thin strokes: WARN with a fix that rounds the rim's joins; applied, PASS."""
    await setup(ed)
    path = "assets/titles/spikes.titleart"
    await create(ed, path, text="NMVW")
    await ed.call("edit_title", path=path, ops=[{"op": "add_rim", "layer": "Title", "values": {"width": 12}}])
    report = await ed.call("check_title", path=path, checks=["rim_spikes"])
    check = report["checks"][0]
    ed.check_eq(check["status"], "WARN", f"mitre spikes: {check}")
    ed.check(check["measured"]["spike_to_band"] > check["threshold"]["spike_to_band"], f"measured over the threshold: {check}")
    fix = check.get("fix") or {}
    ed.check(fix.get("ops"), f"a fix: {check}")
    await ed.call("edit_title", path=path, ops=fix["ops"])
    again = (await ed.call("check_title", path=path, checks=["rim_spikes"]))["checks"][0]
    ed.check_eq(again["status"], "PASS", f"round joins: {again}")
