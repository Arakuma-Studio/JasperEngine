"""S87 language checks (docs/specs/S87-title-localisation-interchange.md AC5, R18; jasper://docs/title-art
"Languages"): each of the seven checks and the per-language glyphs_present fire on a seeded defect,
every fix op offered makes the check PASS when applied through edit_title (needs_input fixes are
checked for shape), and a clean three-language document passes them all.
"""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Language Checks"}
TIMEOUT = 900

COUNTER = [0]
PASSING = ("PASS", "SKIP")


async def fresh(ed: Editor, text: str, ops: list) -> str:
    """A new document with `ops` applied: every fix is tried on its own copy of the defect."""
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    COUNTER[0] += 1
    path = f"assets/titles/check{COUNTER[0]}.titleart"
    await ed.call("edit_title", path=path, create={"text": text, "canvas": [1024, 512]}, bake="never", ops=ops)
    return path


async def run(ed: Editor, path: str, check: str, languages="all") -> list:
    r = await ed.call("check_title", path=path, checks=[check], languages=languages)
    return [c for c in r["checks"] if c["id"] == check]


async def one(ed: Editor, path: str, check: str, language: str) -> dict:
    results = await run(ed, path, check, [language])
    found = [c for c in results if c.get("language") == language]
    ed.check_eq(len(found), 1, f"{check} reports {language} once: {results}")
    return found[0]


async def test_languages_complete(ed: Editor):
    ops = [{"op": "add_language", "code": "de", "source": "en"}]
    path = await fresh(ed, "BIG WIN", ops)
    result = await one(ed, path, "languages_complete", "de")
    ed.check_eq(result["status"], "FAIL", "no German text")
    fix = result["fix"]
    ed.check(fix.get("needs_input"), "the text is the agent's to write")
    ed.check_eq(fix["ops"][0], {"op": "set_translation", "layer": "Title", "language": "de", "text": "BIG WIN"}, "the placeholder op")
    await ed.call("edit_title", path=path, bake="never", ops=fix["alternative"]["ops"])
    ed.check_eq((await one(ed, path, "languages_complete", "de"))["status"], "PASS", "translate false takes the layer out")
    other = await fresh(ed, "BIG WIN", ops)
    await ed.call("edit_title", path=other, bake="never", ops=[dict(fix["ops"][0], text="GROSSER GEWINN")])
    ed.check_eq((await one(ed, other, "languages_complete", "de"))["status"], "PASS", "the op with real text")


async def test_translation_stale(ed: Editor):
    path = await fresh(ed, "BIG WIN", [
        {"op": "add_language", "code": "de", "source": "en"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GROSSER GEWINN"},
        {"op": "set_text", "layer": "Title", "text": "HUGE WIN"},
    ])
    result = await one(ed, path, "translation_stale", "de")
    ed.check_eq(result["status"], "WARN", "the source changed")
    ed.check_eq(result["measured"]["stale"], [{"layer": "Title", "reason": "source_changed"}], "and why")
    await ed.call("edit_title", path=path, bake="never", ops=result["fix"]["ops"])
    ed.check_eq((await one(ed, path, "translation_stale", "de"))["status"], "PASS", "mark_reviewed")


OVERFLOW = [
    {"op": "add_language", "code": "de", "source": "en"},
    {"op": "set_translation", "layer": "Title", "language": "de", "text": "UNGLAUBLICHER GEWINN",
     "values": {"fit.mode": "shrink", "fit.min_scale": 0.95}},
]


async def test_text_overflow_every_fix(ed: Editor):
    path = await fresh(ed, "WIN", OVERFLOW)
    result = await one(ed, path, "text_overflow", "de")
    ed.check_eq(result["status"], "FAIL", "over the box at min_scale 0.95")
    ed.check_eq(result["measured"]["layers"][0]["layer"], "Title", "names the layer")
    ed.check_eq(result["fix"].get("choose"), "one", "candidates to choose from")
    offered = result["fix"]["ops"]
    ed.check(len(offered) >= 2, f"several candidates: {offered}")
    for op in offered:
        trial = await fresh(ed, "WIN", OVERFLOW)
        await ed.call("edit_title", path=trial, bake="never", ops=[op])
        ed.check_eq((await one(ed, trial, "text_overflow", "de"))["status"], "PASS", f"fits once applied: {op}")


LOW = [
    {"op": "add_language", "code": "de", "source": "en"},
    {"op": "set_translation", "layer": "Title", "language": "de", "text": "GEWINN", "values": {"fit.mode": "shrink", "fit.min_scale": 0.3}},
]


async def test_fit_scale_low_every_fix(ed: Editor):
    path = await fresh(ed, "WIN", LOW)
    result = await one(ed, path, "fit_scale_low", "de")
    ed.check_eq(result["status"], "WARN", f"shrunk below 0.75: {result}")
    ed.check(result["measured"]["layers"][0]["scale"] < 0.75, "the measured scale")
    offered = result["fix"]["ops"]
    ed.check(offered, "candidates are offered")
    for op in offered:
        trial = await fresh(ed, "WIN", LOW)
        await ed.call("edit_title", path=trial, bake="never", ops=[op])
        ed.check_eq((await one(ed, trial, "fit_scale_low", "de"))["status"], "PASS", f"no longer low: {op}")


async def test_fit_without_box(ed: Editor):
    path = await fresh(ed, "WIN", [
        {"op": "add_language", "code": "de", "source": "en"},
        {"op": "set", "layer": "Title", "values": {"fit.mode": "shrink", "fit.box": [0, 0]}},
    ])
    for language in ("en", "de"):
        result = await one(ed, path, "fit_without_box", language)
        ed.check_eq(result["status"], "WARN", f"{language}: a fit with no box")
        await ed.call("edit_title", path=path, bake="never", ops=result["fix"]["ops"])
        ed.check_eq((await one(ed, path, "fit_without_box", language))["status"], "PASS", f"{language}: the box is set")


async def test_project_language_missing(ed: Editor):
    path = await fresh(ed, "WIN", [{"op": "add_language", "code": "de", "source": "en"}])
    await ed.call("set_project_settings", values={"localization.languages": ["en", "de", "fr"]})
    try:
        results = await run(ed, path, "project_language_missing")
        ed.check_eq(len(results), 1, "one result for the document")
        ed.check_eq(results[0]["status"], "WARN", "French is missing")
        ed.check_eq(results[0]["measured"]["missing"], ["fr"], "and only French")
        await ed.call("edit_title", path=path, bake="never", ops=results[0]["fix"]["ops"])
        ed.check_eq((await run(ed, path, "project_language_missing"))[0]["status"], "PASS", "add_language")
    finally:
        await ed.call("set_project_settings", values={"localization.languages": []})
    ed.check_eq((await run(ed, path, "project_language_missing"))[0]["status"], "SKIP", "no list in the project")


async def test_translation_unused(ed: Editor):
    path = await fresh(ed, "WIN", [
        {"op": "add_language", "code": "de", "source": "en"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GEWINN"},
        {"op": "set", "layer": "Title", "values": {"translate": False}},
    ])
    results = await run(ed, path, "translation_unused")
    ed.check_eq(results[0]["status"], "WARN", "a brand word keeps a translation nothing uses")
    ed.check_eq(results[0]["measured"]["unused"], [{"layer": "Title", "language": "de", "reason": "translate_false"}], "and why")
    await ed.call("edit_title", path=path, bake="never", ops=results[0]["fix"]["ops"])
    ed.check_eq((await run(ed, path, "translation_unused"))[0]["status"], "PASS", "revert_translation")


async def test_glyphs_per_language(ed: Editor):
    path = await fresh(ed, "WIN", [
        {"op": "add_language", "code": "ja", "source": "en"},
        {"op": "set_translation", "layer": "Title", "language": "ja", "text": "大当たり"},
    ])
    results = await run(ed, path, "glyphs_present")
    by = {c.get("language", ""): c for c in results}
    ed.check_eq(by["ja"]["status"], "FAIL", f"the Latin face has no kanji: {results}")
    ed.check(by["ja"]["measured"]["missing"] >= 3, "the characters are counted")
    ed.check(by[""]["status"] in PASSING or by.get("en", {}).get("status") in PASSING, f"the source text is covered: {results}")


async def test_clean_document_passes(ed: Editor):
    path = await fresh(ed, "BIG WIN", [
        {"op": "add_language", "code": "de", "source": "en"},
        {"op": "add_language", "code": "fr"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "BIG WIN!"},
        {"op": "set_translation", "layer": "Title", "language": "fr", "text": "GROS GAIN"},
    ])
    r = await ed.call("check_title", path=path, checks=["languages"], languages="all")
    ed.check(r["checks"], "the group runs")
    for c in r["checks"]:
        ed.check(c["status"] in PASSING, f"{c['id']} {c.get('language')}: {c['status']} {c['message']}")
