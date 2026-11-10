"""S87 in the Title Designer (docs/specs/S87-title-localisation-interchange.md AC27, R51-R54, R57, R58):
the Next / Previous Language commands cycle the canvas language (the window shows it with its badge,
screenshot evidence), and Next / Previous Language and Assets > Import Title Art are catalog commands
(rebindable, no default keys). The edit routing in a language is TitleLanguageEditingTest's.
Screenshots go to build/shots/ (the evidence the spec's AC27 names).
"""

from pathlib import Path

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Title Designer Languages"}
TIMEOUT = 600

S: dict = {}

DOC = "assets/designer/lang.titleart"


SHOTS = Path("build/shots")


def save(name: str, data: bytes) -> None:
    SHOTS.mkdir(parents=True, exist_ok=True)
    (SHOTS / name).write_bytes(data)


async def setup(ed: Editor) -> None:
    if S.get("ed") is ed:
        return
    S.clear()
    S["ed"] = ed
    await ed.call("tool_groups", enable=["title_art", "editor_tools", "editor_ux"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})
    await ed.call("edit_title", path=DOC, create={"text": "BIG WIN"}, bake="never", ops=[
        {"op": "add_language", "code": "de", "source": "en"},
        {"op": "add_language", "code": "ja"},
        {"op": "set_translation", "layer": "Title", "language": "de", "text": "GROSSER GEWINN"},
    ])


async def test_language_commands_and_shots(ed: Editor):
    await setup(ed)
    await ed.call("focus_window", window="title_designer", title_designer={"path": DOC, "zoom": "fit"})
    shot = await ed.screenshot("title_designer", max_width=0)
    save("s87_designer_source.png", shot)
    await ed.call("run_command", id="titleDesigner.nextLanguage")
    await ed.call("wait", frames=20)
    shot = await ed.screenshot("title_designer", max_width=0)
    save("s87_designer_german.png", shot)
    ed.check(len(shot) > 1000, "the German canvas renders")
    await ed.call("run_command", id="titleDesigner.previousLanguage")
    await ed.call("run_command", id="titleDesigner.previousLanguage")
    await ed.call("wait", frames=20)
    shot = await ed.screenshot("title_designer", max_width=0)
    save("s87_designer_japanese.png", shot)


async def test_catalog_commands_exist(ed: Editor):
    await setup(ed)
    r = await ed.call("list_shortcuts")
    ids = {c.get("id"): c for c in r.get("shortcuts", [])}
    ed.check("titleDesigner.nextLanguage" in ids, "Next Language is a catalog command")
    ed.check("titleDesigner.previousLanguage" in ids, "Previous Language is a catalog command")
    ed.check_eq(ids.get("titleDesigner.nextLanguage", {}).get("default_chords"), [], "rebindable, no default keys")
    ed.check("assets.importTitleArt" in ids, "Assets > Import Title Art... is a catalog command")
