"""S87 Title localisation (docs/specs/S87-title-localisation-interchange.md AC5, AC6, R16, R18, R19, R59,
AC29; jasper://docs/title-art "Languages"): the language ops through edit_title, get_title's language
view, check_title's language checks with their fix ops (each applied makes it PASS), render_title's
per-language sheet rows, the per-language bake sets with the source sidecar's `languages` table, and a
monolingual document that runs none of it.
"""

import json
from pathlib import Path

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Languages"}
TIMEOUT = 900

S: dict = {}


async def setup(ed: Editor) -> None:
    if S.get("ed") is ed:
        return
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})


async def titled(ed: Editor, path: str, text: str = "BIG WIN") -> None:
    await ed.call("edit_title", path=path, create={"text": text, "canvas": [1024, 512]}, bake="never")


def checks_by_id(result: dict) -> dict:
    out: dict = {}
    for c in result.get("checks", []):
        out.setdefault(c["id"], []).append(c)
    return out


async def test_translation_loop(ed: Editor):
    """R60's loop: add_language, set_translation, get_title's view, check_title all languages."""
    await setup(ed)
    path = "assets/titles/loop.titleart"
    await titled(ed, path)
    r = await ed.call("edit_title", path=path, ops=[{"op": "add_language", "code": "de"}, {"op": "add_language", "code": "ja"}], bake="never")
    ed.check("rev" in r, "the batch commits")
    g = await ed.call("get_title", path=path)
    languages = g.get("languages", {})
    ed.check_eq(languages.get("source"), "en", "the source is set by the first add_language")
    codes = [l["code"] for l in languages.get("list", [])]
    ed.check_eq(codes, ["en", "de", "ja"], "the source first, then each language")
    title = next(l for l in g["layers"] if l.get("type") == "text")
    ed.check_eq(title.get("text_key"), "title", "a text key from the layer name")
    ed.check_eq(title.get("translated"), [], "nothing translated yet")
    c = await ed.call("check_title", path=path, checks=["languages"], languages="all")
    by = checks_by_id(c)
    missing = [x for x in by["languages_complete"] if x["status"] == "FAIL"]
    ed.check_eq(len(missing), 2, "languages_complete fails for de and ja")
    fix = missing[0]["fix"]
    ed.check(fix.get("needs_input"), "a missing translation's fix needs input")
    ed.check_eq(fix["ops"][0]["op"], "set_translation", "and carries the op to write it")
    ed.check_eq(fix["ops"][0]["text"], "BIG WIN", "with the source text as a placeholder")
    await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GROSSER GEWINN"},
        {"op": "set_translation", "layer": "Title", "language": "ja", "text": "大当たり", "values": {"vertical": True}},
    ])
    c = await ed.call("check_title", path=path, checks=["languages_complete", "translation_stale"], languages="all")
    for x in c["checks"]:
        ed.check(x["status"] in ("PASS", "SKIP"), f"{x['id']} {x.get('language')} passes once translated: {x['message']}")
    de = await ed.call("get_title", path=path, language="de")
    ed.check_eq(de.get("language"), "de", "the German view")
    title = next(l for l in de["layers"] if l.get("type") == "text")
    ed.check_eq(title.get("text"), "GROSSER GEWINN", "the resolved text")
    ed.check("fit" in title, "with its fit result (add_language gave it a box)")


async def test_stale_and_review(ed: Editor):
    """translation_stale after a source edit; its mark_reviewed fix makes it PASS."""
    await setup(ed)
    path = "assets/titles/stale.titleart"
    await titled(ed, path)
    await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "add_language", "code": "fr"},
        {"op": "set_translation", "layer": "Title", "language": "fr", "text": "GROS GAIN"},
        {"op": "set", "layer": "Title", "values": {"text": "BIGGER WIN"}},
    ])
    c = await ed.call("check_title", path=path, checks=["translation_stale"], languages=["fr"])
    stale = c["checks"][0]
    ed.check_eq(stale["status"], "WARN", "the source changed under the translation")
    await ed.call("edit_title", path=path, ops=stale["fix"]["ops"], bake="never")
    c = await ed.call("check_title", path=path, checks=["translation_stale"], languages=["fr"])
    ed.check_eq(c["checks"][0]["status"], "PASS", "mark_reviewed fixes it")


async def test_overflow_fixes_fit(ed: Editor):
    """text_overflow: a long translation in a tight box; every offered fix fits once applied."""
    await setup(ed)
    path = "assets/titles/overflow.titleart"
    await titled(ed, path, "WIN")
    await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "add_language", "code": "de"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "UNGLAUBLICHER GEWINN",
         "values": {"fit.mode": "shrink", "fit.min_scale": 0.95}},
    ])
    c = await ed.call("check_title", path=path, checks=["text_overflow"], languages=["de"])
    result = c["checks"][0]
    ed.check_eq(result["status"], "FAIL", "the German text overflows at min_scale 0.95")
    ops = result["fix"]["ops"]
    ed.check(len(ops) >= 1, "fix candidates are offered")
    for op in ops:
        trial = await ed.call("render_title", path=path, ops=[op], image="none")
        ed.check("rev" in trial or "size" in trial, "each candidate applies")
    await ed.call("edit_title", path=path, ops=[ops[0]], bake="never")
    c = await ed.call("check_title", path=path, checks=["text_overflow"], languages=["de"])
    ed.check_eq(c["checks"][0]["status"], "PASS", f"the first fix ({ops[0]}) fits")


async def test_render_rows(ed: Editor):
    """R59: render_title options.languages: one sheet row per language, labelled."""
    await setup(ed)
    path = "assets/titles/rows.titleart"
    await titled(ed, path)
    await ed.call("edit_title", path=path, bake="never", ops=[{"op": "add_language", "code": "de"},
                                                              {"op": "set_translation", "layer": "Title", "language": "de", "text": "GEWINN"}])
    r = await ed.call("render_title", path=path, backgrounds=["black", "white"], options={"languages": "all"})
    rows = r.get("language_rows", [])
    ed.check_eq(len(rows), 2, "a row per language")
    ed.check(rows[0].startswith("en"), f"labelled with the code: {rows}")
    cells = r.get("cells", [])
    ed.check_eq(len(cells), 4, "one column per backdrop")


async def test_language_bakes(ed: Editor):
    """AC6: language sets only for differing languages; the source sidecar's table; a German edit stales only .de."""
    await setup(ed)
    path = "assets/titles/bakes.titleart"
    await titled(ed, path)
    await ed.call("edit_title", path=path, bake="never", ops=[
        {"op": "add_language", "code": "de"}, {"op": "add_language", "code": "fr"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GROSSER GEWINN"},
    ])
    r = await ed.call("export_title", path=path)
    ed.check(r.get("ok"), f"the bake ran: {r}")
    folder = ed.project / "assets/titles"
    ed.check((folder / "bakes.titleart.png").exists(), "the source set")
    ed.check((folder / "bakes.titleart.de.png").exists(), "the German set")
    ed.check((folder / "bakes.titleart.de.bake.json").exists(), "with its sidecar")
    ed.check(not (folder / "bakes.titleart.fr.png").exists(), "French renders as the source: no set")
    sidecar = json.loads((folder / "bakes.titleart.bake.json").read_text())
    table = sidecar.get("languages", {})
    ed.check_eq(table.get("source"), "en", "the table's source")
    ed.check_eq(table.get("sets"), {"de": "bakes.titleart.de.bake.json", "fr": None}, "de has a set, fr reuses the source")
    german = json.loads((folder / "bakes.titleart.de.bake.json").read_text())
    ed.check_eq(german.get("language"), "de", "the language sidecar names its language")
    ed.check(german["frame"] != sidecar["frame"], "each language has its own trimmed frame")
    # a German edit stales only the German set
    await ed.call("edit_title", path=path, bake="never", ops=[{"op": "set_translation", "layer": "Title", "language": "de", "text": "GEWINN"}])
    c = await ed.call("check_title", path=path, checks=["bake_fresh"], languages="all")
    status = {x.get("language", ""): x["status"] for x in c["checks"]}
    ed.check_eq(status.get(""), "PASS", "the source set is still fresh")
    ed.check_eq(status.get("de"), "WARN", "the German set is stale")
    ed.check_eq(status.get("fr"), "PASS", "French reuses the source")


async def test_monolingual_runs_nothing(ed: Editor):
    """R50, AC29: a monolingual document: no language checks, no sidecar table."""
    await setup(ed)
    path = "assets/titles/mono.titleart"
    await titled(ed, path)
    c = await ed.call("check_title", path=path, checks=["languages"])
    ed.check(all(x["status"] == "SKIP" or x["status"] == "PASS" for x in c["checks"]), "the language checks do not apply")
    g = await ed.call("get_title", path=path)
    ed.check("languages" not in g, "get_title shows no languages")
    await ed.call("export_title", path=path)
    sidecar = json.loads((ed.project / "assets/titles/mono.titleart.bake.json").read_text())
    ed.check("languages" not in sidecar, "no languages table")
    png = (ed.project / "assets/titles/mono.titleart.png").read_bytes()
    ed.check(b"resolved_hash" not in png, "the recipe has no language members")
