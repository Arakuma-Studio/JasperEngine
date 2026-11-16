"""S95 String Tables (docs/specs/S95-lua-runtime-gaps.md REQ-25 to REQ-30, AC-22): edit_string_table's
create / get (the keys each language misses) / set (one undo step) / remove, a language outside the
project's refused, a key two tables define reported, the CSV round trip, and Play: a Text's
Localization Key and Localization.get in the Game view's simulated language.
"""

import json

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "String Tables"}
TIMEOUT = 600

S: dict = {}

UI = "assets/Localization/UI.stringtable"


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    await ed.call("tool_groups", enable=["localization"])
    await ed.call("set_project_settings", values={"localization.languages": ["en", "es"]})
    # only once both took: a test that failed here sets them up again
    S["ed"] = ed
    return S


async def test_group_hidden_until_enabled(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    if "edit_string_table" in default:
        # an earlier test of this file enabled it in this session
        return
    await ed.call("tool_groups", enable=["localization"])
    ed.check("edit_string_table" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")


async def test_create_set_missing_and_undo(ed: Editor):
    await setup(ed)
    r = await ed.call("edit_string_table", action="create", path=UI, entries={
        "menu.play": {"en": "Play", "comment": "Main menu button"},
        "hud.coins": {"en": {"one": "{count} coin", "other": "{count} coins"}},
    })
    ed.check_eq(r["keys"], 2)
    ed.check_eq(r["languages"], ["en", "es"], "the project's languages")
    ed.check_eq(sorted(r["missing"]["es"]), ["hud.coins", "menu.play"], "Spanish misses both")
    ed.check_eq(r["missing"]["en"], [], "English has both")
    ed.check_in("es misses 2 key(s)", r.get("next", ""))
    ed.check((ed.project / UI).exists(), "the file is written")
    ed.check(len(r.get("guid", "")) > 0, "it gets a GUID (.meta)")
    ed.check((ed.project / (UI + ".meta")).exists(), "its .meta is written")

    r = await ed.call("edit_string_table", action="set", path=UI, entries={
        "menu.play": {"es": "Jugar"},
        "hud.coins": {"es": {"one": "{count} moneda"}},
    })
    ed.check_eq(r["missing"]["es"], ["hud.coins"], "a plural value without `other` still misses a form")
    await ed.call("edit_string_table", action="set", path=UI, entries={"hud.coins": {"es": {"one": "{count} moneda", "other": "{count} monedas"}}})
    g = await ed.call("edit_string_table", action="get", path=UI, keys=["menu.play"])
    ed.check_eq(len(g["entries"]), 1, "keys filters")
    ed.check_eq(g["entries"][0]["values"], {"en": "Play", "es": "Jugar"})
    ed.check_eq(g["entries"][0]["comment"], "Main menu button")

    # one undo step per set: the plural's second form goes, then Jugar
    await ed.call("undo", scope="project")
    g = await ed.call("edit_string_table", action="get", path=UI)
    ed.check_eq(g["missing"]["es"], ["hud.coins"], "undo restores the previous file")
    await ed.call("redo", scope="project")
    g = await ed.call("edit_string_table", action="get", path=UI)
    ed.check_eq(g["missing"]["es"], [], "redo")

    # a language the project does not ship: refused, the fix named, nothing written
    before = (ed.project / UI).read_text()
    err = await ed.call("edit_string_table", action="set", path=UI, entries={"menu.play": {"fr": "Jouer"}}, expect_error=True)
    ed.check_in("localization.languages", err)
    ed.check_eq((ed.project / UI).read_text(), before, "nothing written")
    err = await ed.call("edit_string_table", action="set", path=UI, entries={"bad:key": {"en": "x"}}, expect_error=True)
    ed.check_in("':'", err)

    # remove a language's value, then a key
    r = await ed.call("edit_string_table", action="remove", path=UI, keys=["menu.play"], language="es")
    ed.check_eq(r["missing"]["es"], ["menu.play"])
    await ed.call("edit_string_table", action="set", path=UI, entries={"menu.play": {"es": "Jugar"}, "tmp": {"en": "x"}})
    r = await ed.call("edit_string_table", action="remove", path=UI, keys=["tmp"])
    ed.check_eq(r["keys"], 2)


async def test_duplicate_keys_reported(ed: Editor):
    await setup(ed)
    other = "assets/Localization/Other.stringtable"
    r = await ed.call("edit_string_table", action="create", path=other, entries={"dup.key": {"en": "B"}})
    r = await ed.call("edit_string_table", action="create", path="assets/Localization/Another.stringtable", entries={"dup.key": {"en": "A"}})
    dups = r.get("duplicates", [])
    ed.check_eq([d["key"] for d in dups], ["dup.key"], "the key two tables define")
    ed.check_in("Another:key", r.get("next", "").replace("\"", ""))
    await ed.call("edit_string_table", action="remove", path=other, keys=["dup.key"])


async def test_csv_round_trip(ed: Editor):
    await setup(ed)
    if not (ed.project / UI).exists():
        await test_create_set_missing_and_undo(ed)
    r = await ed.call("edit_string_table", action="export", path=UI, format="csv")
    text = r["files"][0]["text"]
    ed.check_in("key,context,English (en),Spanish (es)", text)
    ed.check_in("{count, plural, one {{count} moneda} other {{count} monedas}}", text, "a plural as an ICU plural")
    r = await ed.call("edit_string_table", action="export", path=UI, format="csv", file="assets/Localization/ui.csv")
    ed.check_eq(r["written"], ["assets/Localization/ui.csv"])
    copy = "assets/Localization/Copy.stringtable"
    await ed.call("edit_string_table", action="create", path=copy)
    r = await ed.call("edit_string_table", action="import", path=copy, file="assets/Localization/ui.csv")
    ed.check_eq(r["added"], 2, "both keys")
    a = await ed.call("edit_string_table", action="get", path=UI)
    b = await ed.call("edit_string_table", action="get", path=copy)
    ed.check_eq(b["entries"], a["entries"], "the CSV round trip keeps every value, plural and comment")
    # importing the same file again changes nothing
    r = await ed.call("edit_string_table", action="import", path=copy, file="assets/Localization/ui.csv")
    ed.check_eq((r["added"], r["changed"]), (0, 0))
    r = await ed.call("edit_string_table", action="export", path=UI, format="xliff")
    ed.check_eq([f["language"] for f in r["files"]], ["es"], "XLIFF: one file per target language")
    ed.check_in('target-language="es"', r["files"][0]["text"])
    # the copy's keys would be duplicates of UI's
    await ed.call("delete_asset", paths=[copy])


async def test_play_in_the_simulated_language(ed: Editor):
    await setup(ed)
    if not (ed.project / UI).exists():
        await test_create_set_missing_and_undo(ed)
    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/LocHud")
    sid = made["scene"]["id"]
    await ed.call("create_ui", scene=sid, root={"type": "text", "name": "Label", "text": "placeholder",
                                                "layout": {"anchor": "center", "size": [300, 60]}})
    await ed.call("set_properties", scene=sid, entity="Label", values={"text": {"localizationKey": "menu.play"}})
    await ed.call("save_scene", scene=sid)
    await ed.call("configure_game_view", language="es-ES")
    await ed.call("play", action="start", scene=sid)
    try:
        await ed.call("wait", frames=5)
        r = await ed.call("eval_lua", code=(
            "return Localization.language, Localization.get('menu.play'), Localization.get('hud.coins', {count = 1}), "
            "Localization.get('hud.coins', {count = 4}), object('Label', 'Text').text, Localization.has('UI:menu.play')"))
        v = r["values"]
        ed.check_eq(v[0], "es", "the simulated device language, matched against the project's")
        ed.check_eq(v[1], "Jugar")
        ed.check_eq(v[2], "1 moneda")
        ed.check_eq(v[3], "4 monedas")
        ed.check_eq(v[4], "Jugar", "the Text follows its Localization Key")
        ed.check_eq(v[5], True)
        # one language for titles and strings: TitleArt.setLanguage moves the Text too
        await ed.call("eval_lua", code="TitleArt.setLanguage('en')")
        await ed.call("wait", frames=3)
        r = await ed.call("eval_lua", code="return object('Label', 'Text').text, Localization.language")
        ed.check_eq(r["values"], ["Play", "en"])
    finally:
        await ed.call("play", action="stop")
        await ed.call("configure_game_view", language="")
    # the edit-time Text keeps its key (Play changes stay in Play)
    g = await ed.call("get_entity", scene=sid, entity="Label", components="text")
    ed.check_in("menu.play", json.dumps(g))
