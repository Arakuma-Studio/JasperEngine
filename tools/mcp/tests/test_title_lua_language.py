"""S87 languages at runtime (docs/specs/S87-title-localisation-interchange.md AC9, R12-R15, R27, R61; and
the Baked switch of AC7 as far as a frame loop shows it): TitleArt.setLanguage / getLanguage /
onLanguageChanged (once per change), the renderer's `language`, getResolvedLanguage, getLanguages,
isLanguageReady and onLanguageReady, an invalid tag refused with the §6.4 sentence, the simulated device
language of configure_game_view selecting German from `de-DE`, and a Baked title that keeps drawing the
previous language until the new set's texture is loaded.
"""

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Lua Language"}
TIMEOUT = 900

S: dict = {}

DOC = "assets/titles/lang.titleart"


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("edit_title", path=DOC, create={"text": "BIG WIN", "canvas": [800, 400]}, bake="never", ops=[
        {"op": "add_language", "code": "de", "source": "en"},
        {"op": "add_language", "code": "ja"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GROSSER GEWINN"},
        {"op": "set_translation", "layer": "Title", "language": "ja", "text": "大当たり"},
    ])
    await ed.call("export_title", path=DOC)
    made = await ed.call("create_scene", name="Hud", type="UI", path="scenes/LangHud")
    S["scene"] = made["scene"]["id"]
    await ed.call("create_ui", scene=S["scene"], root={"type": "image", "name": "Logo", "layout": {"anchor": "center", "size": [400, 200]}})
    await ed.call("add_component", scene=S["scene"], entity="Logo", component="titleart")
    await ed.call("set_properties", scene=S["scene"], entity="Logo", values={"titleart": {"title": DOC}})
    await ed.call("save_scene", scene=S["scene"])
    return S


async def test_lua_language_api(ed: Editor):
    s = await setup(ed)
    await ed.call("configure_game_view", language="en-US")
    await ed.call("play", action="start", scene=s["scene"])
    try:
        await ed.call("wait", frames=3)
        r = await ed.call("eval_lua", code=(
            "_G.changes = {}\n"
            "TitleArt.onLanguageChanged:add('t', function(code) table.insert(_G.changes, code) end)\n"
            "local t = object('Logo'):getTitleArt()\n"
            "_G.ready = {}\n"
            "t.onLanguageReady:add('t', function(code) table.insert(_G.ready, code) end)\n"
            "local langs = t:getLanguages()\n"
            "local ok, err = pcall(function() TitleArt.setLanguage('xx_!') end)\n"
            "TitleArt.setLanguage('de')\n"
            "TitleArt.setLanguage('de')\n"
            "return #langs, langs[1], langs[2], ok, err, TitleArt.getLanguage(), t:isLanguageReady()"))
        v = r["values"]
        ed.check_eq(v[0], 3, "the source and two languages")
        ed.check_eq(v[1], "en", "the source first")
        ed.check_eq(v[3], False, "an invalid tag raises")
        ed.check_in("TitleArt: 'xx_!' is not a language tag (BCP 47, for example \"de\", \"pt-BR\", \"zh-Hant\").", v[4])
        ed.check_eq(v[5], "de")
        ed.check_eq(v[6], False, "German is on its way: the English image still draws")
        await ed.call("wait", frames=30)
        r = await ed.call("eval_lua", code=(
            "local t = object('Logo'):getTitleArt()\n"
            "return #_G.changes, _G.changes[1], t:getResolvedLanguage(), t:isLanguageReady(), #_G.ready, _G.ready[1]"))
        v = r["values"]
        ed.check_eq(v[0], 1, "onLanguageChanged once per change")
        ed.check_eq(v[1], "de")
        ed.check_eq(v[2], "de")
        ed.check_eq(v[3], True, "ready once the German set is drawn")
        ed.check_eq(v[4], 1, "onLanguageReady once")
        ed.check_eq(v[5], "de")
        # the component's language wins over the global one
        r = await ed.call("eval_lua", code="local t = object('Logo'):getTitleArt()\nt.language = 'ja'\nreturn t.language")
        await ed.call("wait", frames=30)
        r = await ed.call("eval_lua", code="return object('Logo'):getTitleArt():getResolvedLanguage()")
        ed.check_eq(r["values"][0], "ja", "the renderer's language first")
        r = await ed.call("eval_lua", code="local t = object('Logo'):getTitleArt()\nt.language = 'fr'\nreturn 1")
        await ed.call("wait", frames=5)
        r = await ed.call("eval_lua", code="return object('Logo'):getTitleArt():getResolvedLanguage()")
        ed.check_eq(r["values"][0], "de", "a language the title lacks: the next candidate (the global German)")
        await ed.call("eval_lua", code="TitleArt.setLanguage('')")
    finally:
        await ed.call("play", action="stop")


async def test_device_language(ed: Editor):
    """configure_game_view {language: "de-DE"} selects German with no script code (R12 step 3)."""
    s = await setup(ed)
    await ed.call("configure_game_view", language="de-DE")
    await ed.call("play", action="start", scene=s["scene"])
    try:
        await ed.call("wait", frames=30)
        r = await ed.call("eval_lua", code="return object('Logo'):getTitleArt():getResolvedLanguage()")
        ed.check_eq(r["values"][0], "de")
    finally:
        await ed.call("play", action="stop")
        await ed.call("configure_game_view", language="")
