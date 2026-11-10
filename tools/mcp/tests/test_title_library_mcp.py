# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT

"""S84 AC8, AC9, AC19 (font import), AC22: the Title Art library through MCP. list_title_library's
tiers (catalogue, rows with a `use` op that edit_title applies, one item's detail, a numbered sheet),
apply_template's keep-and-replace, the library ops (save, copy, delete) and the first-use import of
a pack font with its OFL.txt and provenance."""

import json
import time

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Library"}
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


async def wait_job(ed: Editor, tool: str, args: dict):
    r = await ed.call_raw(tool, args)
    job = (r.structured or {}).get("job_id") if not r.is_error else None
    if not job or (r.structured or {}).get("status") == "succeeded":
        return r
    deadline = time.monotonic() + TIMEOUT
    state = await ed.call("get_job", job_id=job)
    while state.get("status") == "running" and time.monotonic() < deadline:
        state = await ed.call("get_job", job_id=job)
    ed.check_eq(state.get("status"), "succeeded", f"the {tool} job: {state}")
    return state


async def test_group_hidden(ed: Editor):
    """The library is served by list_title_library, in the title_art group: hidden until enabled."""
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("list_title_library" not in default, "title_art hidden by default")
    await setup(ed)
    listed = {t.name for t in await ed.tools(groups=None)}
    ed.check("list_title_library" in listed, "listed once title_art is enabled")


async def test_tiers(ed: Editor):
    """AC22: the catalogue is small, rows are small and carry a use op that applies, a detail fits."""
    await setup(ed)
    cat = await ed.call_raw("list_title_library", {})
    library = cat.structured.get("library") or {}
    ed.check(library.get("looks", {}).get("total", 0) > 0, f"the catalogue counts looks: {library}")
    ed.check(len(json.dumps(library, separators=(",", ":"))) <= 600, f"catalogue <= 600 chars: {len(json.dumps(library))}")
    rows = await ed.call("list_title_library", kind="looks", query="gold")
    ed.check(rows["total"] > 0, "gold finds looks")
    for row in rows["rows"]:
        ed.check(len(json.dumps(row, separators=(",", ":"), ensure_ascii=False)) <= 300, f"a row stays small: {row}")
    first = next(r for r in rows["rows"] if r["available"] and r.get("dim") == "2d")
    path = "assets/titles/tier.titleart"
    await ed.call("edit_title", path=path, create={"text": "HERO"}, bake="never")
    op = dict(first["use"])
    op["layer"] = "Title"
    applied = await ed.call_raw("edit_title", {"path": path, "ops": [op]})
    ed.check(not applied.is_error, f"the row's use op applies: {applied.text[:300]}")
    ed.check_in("Apply Look", applied.text, "named after the look")
    detail = await ed.call_raw("list_title_library", {"kind": "looks", "id": first["id"]})
    ed.check(not detail.is_error, detail.text[:200])
    ed.check(len(json.dumps(detail.structured, ensure_ascii=False)) <= 1600 * 2, "a detail stays small")
    ed.check_in("contract", detail.structured, "the detail says what applying keeps and replaces")
    typo = await ed.call_raw("list_title_library", {"kind": "looks", "id": first["id"] + "x"})
    ed.check(typo.is_error and "list_title_library" in typo.text, f"a typo suggests and points at the listing: {typo.text[:200]}")


async def test_sheet(ed: Editor):
    """AC22: a 9-cell sheet is one image within 1024 x 576, its cells numbered as the rows."""
    await setup(ed)
    r = await wait_job(ed, "list_title_library", {"kind": "looks", "query": "gold", "sheet": {"text": "HERO", "cell": [320, 160], "columns": 3}})
    result = r.structured if hasattr(r, "structured") else r.get("result", {})
    sheet = result.get("sheet") or {}
    ed.check(0 < sheet.get("width", 0) <= 1024 and 0 < sheet.get("height", 0) <= 576, f"the sheet fits: {sheet}")
    numbers = [row["n"] for row in result.get("rows", [])]
    ed.check_eq(numbers, list(range(1, len(numbers) + 1)), "cells numbered 1..n")
    ed.check(len(numbers) <= 9, "at most 9 cells")


async def test_template_and_font_import(ed: Editor):
    """AC8, AC19: apply_template keeps guid, name and texts; apply_font imports a pack font once
    with its OFL.txt and a provenance; one undo step removes the import again."""
    await setup(ed)
    templates = await ed.call("list_title_library", kind="templates", query="clean")
    tmpl = next((r for r in templates["rows"] if r["available"]), None)
    ed.check(tmpl is not None, f"a template to apply: {templates}")
    path = "assets/titles/tmpl.titleart"
    await ed.call("edit_title", path=path, create={"text": "MY TITLE"}, bake="never")
    before = json.loads((ed.project / path).read_text())
    out = await ed.call_raw("edit_title", {"path": path, "ops": [{"op": "apply_template", "template": tmpl["id"]}]})
    ed.check(not out.is_error, out.text[:300])
    after = json.loads((ed.project / path).read_text())
    ed.check_eq(after["guid"], before["guid"], "the guid is kept")
    ed.check_eq(after["name"], before["name"], "the name is kept")
    texts = [l.get("text") for l in after["layers"] if l.get("type") == "text"]
    ed.check_in("MY TITLE", texts, f"keep_text carries the title: {texts}")
    ja = await ed.call_raw("edit_title", {"path": path, "ops": [{"op": "apply_template", "template": tmpl["id"], "keep_text": False, "language": "ja"}]})
    ed.check(not ja.is_error, ja.text[:300])
    # a look with its font pair: the pack font comes in once
    fpath = "assets/titles/font.titleart"
    await ed.call("edit_title", path=fpath, create={"text": "HERO"}, bake="never")
    look = {"id": "casino/vegas-marquee"}    # a look naming a font pair
    first = await ed.call_raw("edit_title", {"path": fpath, "ops": [{"op": "apply_look", "layer": "Title", "look": look["id"], "apply_font": True}]})
    ed.check(not first.is_error, first.text[:300])
    details = (first.structured or {}).get("details", {}).get("0", {})
    imported = details.get("imported") or []
    fonts = [p for p in imported if p.startswith("TitleArt/Fonts/")]
    ed.check(fonts, f"a pack font imported: {details}")
    # the assets directory of a created project is its root or assets/: find the copy
    copied = next(ed.project.rglob(fonts[0].rsplit("/", 1)[1]))
    ed.check((copied.parent / "OFL.txt").exists(), "its OFL.txt beside it")
    meta = copied.with_name(copied.name + ".meta").read_text()
    ed.check("OFL-1.1" in meta and "titleart-pack" in meta, f"the meta's provenance: {meta}")
    again = await ed.call("edit_title", path=fpath, ops=[{"op": "apply_look", "layer": "Title", "look": look["id"], "apply_font": True}])
    ed.check_eq(again.get("details", {}).get("0", {}).get("imported", []), [], "a second apply imports nothing")


async def test_library_ops(ed: Editor):
    """AC9: save_look writes the project folder, undo removes it; a clash needs overwrite; copy to
    user and delete from user (to .trash); a built-in cannot be deleted; save_template without a
    placeholder fails with the fix."""
    await setup(ed)
    path = "assets/titles/save.titleart"
    await ed.call("edit_title", path=path, create={"text": "MINE"}, bake="never")
    saved = await ed.call("edit_title", path=path, ops=[{"op": "save_look", "layer": "Title", "name": "My Look"}])
    file = ed.project / "EditorSettings" / "TitleLibrary" / "Looks" / "my-look.titlelook"
    ed.check(file.exists(), f"the project look: {saved}")
    clash = await ed.check_error("edit_title", path=path, ops=[{"op": "save_look", "layer": "Title", "name": "My Look"}])
    ed.check_in("exists", clash, "an id clash is refused")
    await ed.call("undo", scope="project")
    ed.check(not file.exists(), "undo removes the saved look")
    await ed.call("edit_title", path=path, ops=[{"op": "save_look", "layer": "Title", "name": "My Look"}])
    copied = await ed.call_raw("edit_title", {"ops": [{"op": "copy_library_item", "ref": "project/my-look", "to": "user"}]})
    ed.check(not copied.is_error, copied.text[:300])
    deleted = await ed.call_raw("edit_title", {"ops": [{"op": "delete_library_item", "ref": "user/my-look"}]})
    ed.check(not deleted.is_error, deleted.text[:300])
    builtin = await ed.call("list_title_library", kind="looks", limit=1)
    msg = await ed.check_error("edit_title", ops=[{"op": "delete_library_item", "ref": builtin["rows"][0]["id"]}])
    ed.check_in("built in", msg, "built-ins cannot be deleted")
    tpath = "assets/titles/noexpose.titleart"
    await ed.call("edit_title", path=tpath, create={"text": "X"}, bake="never")
    await ed.call("edit_title", path=tpath, ops=[{"op": "unexpose", "name": "text"}])
    msg = await ed.check_error("edit_title", path=tpath, ops=[{"op": "save_template", "name": "No Placeholder"}])
    ed.check_in("placeholder", msg, "a template needs a placeholder")


async def test_listing_latency(ed: Editor):
    """AC21 (R40): the catalogue and rows in at most 30 ms. The time includes MCP's round trip, so
    it is an upper bound; the shared build box gets three times the budget."""
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("list_title_library")
    # what MCP itself costs on this machine, to tell it from the tool's own time
    baseline = []
    for _ in range(9):
        start = time.monotonic()
        await ed.call("editor_status")
        baseline.append((time.monotonic() - start) * 1000.0)
    round_trip = sorted(baseline)[len(baseline) // 2]
    print(f"[AC21] an editor_status round trip: {round_trip:.1f} ms")
    for label, args in (("catalogue", {}), ("rows", {"kind": "looks", "limit": 40})):
        samples = []
        for _ in range(9):
            start = time.monotonic()
            await ed.call("list_title_library", **args)
            samples.append((time.monotonic() - start) * 1000.0)
        median = sorted(samples)[len(samples) // 2]
        print(f"[AC21] list_title_library {label}: {median:.1f} ms, {median - round_trip:.1f} ms over a round trip (R40 30)")
        ed.check(median <= 90.0, f"{label} within three times the budget: {median:.1f} ms")


async def test_fallback_parity_after_a_render(ed: Editor):
    """A forced-RGBA8 render must not reuse the node cache of the float render that came before it
    (red run: sports/goal-banner measured mean 80.65 after render_title, 0.31 before it)."""
    await ed.call("tool_groups", enable=["title_art"])
    path = "assets/parity-after-render.titleart"
    await ed.call("edit_title", path=path, create={"template": "sports/goal-banner", "language": "en"}, bake="never")
    args = {"path": path, "max_width": 480, "backgrounds": ["transparent"]}
    r = await ed.call_raw("render_title", args)
    job = (r.structured or {}).get("job_id")
    while job and (await ed.call("get_job", job_id=job)).get("status") == "running":
        pass
    report = await ed.call("check_title", path=path, checks=["fallback_parity"], backgrounds=["black"], render_scale=0.25)
    parity = next(c for c in report["checks"] if c["id"] == "fallback_parity")
    ed.check_eq(parity["status"], "PASS", f"parity after a render: {parity.get('measured')}")
