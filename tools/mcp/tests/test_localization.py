"""Game UI localization through MCP (docs/specs/game-ui-localization.md R33-R36, AC13), on a new project:

- edit_string_table is in the `localization` group: hidden by default, listed once tool_groups enables it;
- create, ops as one undo step, a failing op refusing the whole call (naming the op and the fix), CSV and XLIFF
  export and import;
- inspect_ui {l10n} on a keyed document: keyed count, literal text, misses;
- assert_ui {languages}: a German text overflowing a fixed button fails naming `de`, English passes;
- configure_game_view {language: "en-XA"}: the pseudo-locale reaches the keyed text in Play;
- in Play, eval_lua Localization.setLanguage('ja') re-translates the same element.
"""

from __future__ import annotations

import harness
from harness import Editor

CI = True
TIMEOUT = 600
EDITOR = {"create": True, "name": "Localization"}

TABLE = "assets/l10n/ui.stringtable"
DOC = "assets/ui/menu.html"
HTML = """<!DOCTYPE html>
<html>
<head><style>
body { margin: 0; font-family: sans-serif; font-size: 16px; }
#play { display: block; width: 80px; height: 24px; white-space: nowrap; overflow: hidden; }
</style></head>
<body>
  <button id="play" data-l10n-id="ui.menu.play">Play</button>
  <p id="options">Options</p>
  <p id="later" data-l10n-id="ui.menu.later">Later</p>
</body>
</html>
"""
S: dict = {}


async def test_group_hidden_until_enabled(ed: Editor):
    await ed.call("get_project_summary")
    (ed.project / "assets").mkdir(exist_ok=True)
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_string_table" not in default, "localization is hidden by default")
    await ed.call("tool_groups", enable=["localization"])
    ed.check("edit_string_table" in {t.name for t in await ed.tools(groups=None)}, "listed once tool_groups enables localization")
    listed = await ed.call("edit_string_table")
    ed.check_eq(listed["tables"], [], "no tables yet")
    ed.check("create" in listed.get("next", ""), f"the next hint says how to create one: {listed}")


async def test_edit_string_table_ops_undo_and_refusal(ed: Editor):
    made = await ed.call("edit_string_table", path=TABLE, create={}, ops=[
        {"op": "set", "key": "menu.play", "text": "Play", "max_chars": 12},
        {"op": "set", "key": "menu.later", "text": "Later"},
        {"op": "add_language", "language": "de"},
        {"op": "add_language", "language": "ja"},
        {"op": "set", "key": "menu.play", "language": "de", "text": "Jetzt sofort spielen"},
        {"op": "set", "key": "menu.play", "language": "ja", "text": "プレイ"}])
    ed.check_eq(made["namespace"], "ui", "the namespace comes from the file stem")
    ed.check((ed.project / "assets/l10n/ui.de.translation").exists(), "the de translation beside the table")
    got = await ed.call("edit_string_table", path=TABLE, get={"keys": ["menu.play"]})
    ed.check_eq(got["entries"][0]["translations"]["de"]["status"], "translated", "translated")
    ed.check(got["entries"][0]["translations"]["de"].get("over_length"), f"over max_chars is flagged: {got['entries'][0]}")

    # one call, one undo step
    await ed.call("edit_string_table", path=TABLE, ops=[{"op": "set", "key": "menu.play", "text": "Start"}])
    got = await ed.call("edit_string_table", path=TABLE, get={"keys": ["menu.play"]})
    ed.check_eq(got["entries"][0]["translations"]["de"]["status"], "needs_review", "a source edit marks translations for review")
    await ed.call("undo", scope="timeline")
    got = await ed.call("edit_string_table", path=TABLE, get={"keys": ["menu.play"]})
    ed.check_eq((got["entries"][0]["text"], got["entries"][0]["translations"]["de"]["status"]), ("Play", "translated"), "undone in one step")

    # a failing op refuses the whole call and names it
    r = await ed.call_raw("edit_string_table", {"path": TABLE, "ops": [
        {"op": "set", "key": "menu.quit", "text": "Quit"},
        {"op": "set", "key": "menu.bad", "text": "{n, plural, one {x}}"}]})
    ed.check(r.is_error and "op 1 (set)" in r.text and "other" in r.text, f"refused naming the op: {r.text}")
    got = await ed.call("edit_string_table", path=TABLE, get={})
    ed.check("menu.quit" not in [e["key"] for e in got["entries"]], "nothing of the refused call was written")


async def test_import_export_csv_xliff(ed: Editor):
    out = await ed.call("edit_string_table", path=TABLE, export={"format": "csv", "out": "translations/ui.csv"})
    csv = (ed.project / "translations/ui.csv").read_text(encoding="utf-8-sig")
    ed.check("ui.menu.play" in csv and "Jetzt sofort spielen" in csv, f"the CSV has keys and translations: {csv[:300]} {out}")
    await ed.call("edit_string_table", path=TABLE, export={"format": "xliff", "languages": ["ja"], "out": "translations/ui.xlf"})
    ed.check((ed.project / "translations/ui.ja.xlf").exists(), "one XLIFF per language")
    (ed.project / "translations/fr.csv").write_text("key,en,fr\r\nui.menu.later,Later,Plus tard\r\nlogo.title,Big,Grand\r\n", encoding="utf-8")
    r = await ed.call("edit_string_table", path=TABLE, **{"import": {"file": "translations/fr.csv", "add_languages": True}})
    ed.check_eq(r["imported"], 1, f"one translation imported: {r}")
    ed.check(any(n["code"] == "unknown_key" and n["key"] == "logo.title" for n in r.get("notes", [])), f"a title's row: {r}")
    got = await ed.call("edit_string_table", path=TABLE, get={"keys": ["menu.later"], "language": "fr"})
    ed.check_eq(got["entries"][0]["translations"]["fr"]["text"], "Plus tard", "imported")


async def test_inspect_ui_l10n_reports_literals_and_misses(ed: Editor):
    made = await ed.call("create_scene", name="Menu", type="UI")
    S["scene"] = made["scene"]["id"]
    await ed.call("write_file", path=DOC, content=HTML)
    ent = await ed.call("create_entity", scene=S["scene"], type="ui_document", name="Menu")
    S["entity"] = ent["entity_id"]
    await ed.call("set_properties", scene=S["scene"], entity=S["entity"], values={"uidocument": {"filename": DOC}})
    await ed.call("save_scene", scene=S["scene"], path="scenes/Menu")
    await ed.call("wait", frames=5)
    l10n = (await ed.call("inspect_ui", entity=S["entity"], l10n=True))["l10n"]
    ed.check(l10n["keyed"] >= 2, f"keyed elements: {l10n}")
    ed.check(any(item["text"].strip() == "Options" for item in l10n["literal"]), f"the hard-coded Options is literal: {l10n}")
    ed.check(not any(item["text"].strip() in ("Play", "Later") for item in l10n["literal"]), f"keyed text is not literal: {l10n}")


async def test_assert_ui_languages_text_fits(ed: Editor):
    r = await ed.call_raw("assert_ui", {"entity": S["entity"], "checks": [{"type": "text_fits", "selector": "#play"}],
                                        "languages": ["en", "de"], "screenshot": "never"})
    ed.check(not r.is_error, f"assert_ui ran: {r.text[:300]}")
    runs = {c.get("language"): c["passed"] for c in r.structured["checks"]}
    ed.check_eq(runs, {"en": True, "de": False}, f"the German text overflows: {r.text[:500]}")
    ed.check("@" in r.text and " de" in r.text, f"the failure names de: {r.text[:500]}")


async def test_pseudo_language_in_game_view(ed: Editor):
    await ed.call("configure_game_view", language="en-XA")
    ed.check(await ed.call("play", action="start", scene=S["scene"], fixed_fps=60), "play started")
    await ed.call("wait", frames=5)
    tree = await ed.call("inspect_ui", entity=S["entity"], selector="#play", detail="full")
    text = str(tree["elements"][0])
    ed.check("⟦" in text, f"the pseudo-locale's brackets reach the button: {text[:400]}")
    await ed.call("play", action="stop")
    await ed.call("configure_game_view", language="system")


async def test_play_switch_language_live(ed: Editor):
    ed.check(await ed.call("play", action="start", scene=S["scene"], fixed_fps=60), "play started")
    await ed.call("wait", frames=5)
    before = await ed.call("inspect_ui", entity=S["entity"], selector="#play", detail="full")
    await ed.call("eval_lua", code="return Localization.setLanguage('ja')")
    await ed.call("wait", frames=2)
    after = await ed.call("inspect_ui", entity=S["entity"], selector="#play", detail="full")
    ed.check_eq(after["elements"][0]["ref"], before["elements"][0]["ref"], "the same element")
    ed.check("プレイ" in str(after["elements"][0]), f"Japanese text after the switch: {str(after['elements'][0])[:400]}")
    lang = (await ed.call("eval_lua", code="return Localization.language"))["values"][0]
    ed.check_eq(lang, "ja", "Localization.language")
    await ed.call("play", action="stop")


if __name__ == "__main__":
    harness.main(globals())
