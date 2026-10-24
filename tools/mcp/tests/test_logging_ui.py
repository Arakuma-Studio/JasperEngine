"""S09 AC18, the human side of logging, driven like a user: Preferences > Logging (the page, a
category level, persistence across a restart) and the Console (Debug rows grey, the Category
filter, the selected row's file:line, Clear on Play, Error Pause). Screenshots of the editor go
to build/test-results/logging-ui/ as evidence.
"""

import contextlib
import re

import yaml

import harness
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Logging UI CI"}

TIMEOUT = 600


async def console(ed: Editor, **options) -> dict:
    r = await ed.call("focus_window", window="console", **({"console": options} if options else {}))
    ed.check(r["shown"], f"the Console is shown: {r}")
    return r["console"]


async def shot(ed: Editor, name: str) -> None:
    out = harness.REPO / "build" / "test-results" / "logging-ui"
    out.mkdir(parents=True, exist_ok=True)
    r = await ed.call_raw("screenshot", {"view": "editor", "max_width": 1280, "save_path": str(out / f"{name}.png")})
    ed.check(not r.is_error and r.images, f"screenshot {name}: {r.text[:300]}")
    ed.check(not (r.structured or {}).get("flat_colour"), f"screenshot {name} is a flat image")


async def test_preferences_logging_page(ed: Editor):
    r = await ed.call("focus_window", window="preferences", page="Logging")
    ed.check(r["shown"] and r["page"] == "Logging", f"Preferences > Logging: {r}")
    await shot(ed, "preferences-logging")
    bad = await ed.call_raw("focus_window", {"window": "preferences", "page": "Nope"})
    ed.check(bad.is_error, "an unknown page is refused")
    await ed.call("focus_window", window="preferences", close=True)

    r = await ed.call("set_project_settings", values={"preferences.logging.levels": {"render": "debug"}})
    ed.check_eq(r["settings"]["preferences.logging.levels"], {"render": "debug"}, "set")
    cfg = await ed.call("get_log_config")
    ed.check_eq([c["level"] for c in cfg["categories"] if c["name"] == "render"], ["debug"], "applied to the logger")
    bad = await ed.call_raw("set_project_settings", {"values": {"preferences.logging.levels": {"render": "loud"}}})
    ed.check(bad.is_error, "an unknown level is refused")
    saved = yaml.safe_load((ed.project.parent / "settings.yaml").read_text())
    ed.check_eq(saved.get("logging", {}).get("levels"), {"render": "debug"}, f"saved to settings.yaml: {saved.get('logging')}")


async def test_console_debug_rows_category_filter_and_source(ed: Editor):
    await ed.call("set_project_settings", values={"preferences.logging.levels": {"render": "debug"}})
    await ed.call("clear_console")
    await ed.call("eval_lua", allow_edit_mode=True, code=(
        "local render = Log.category('render')\n"
        "render:debug('render debug row')\n"
        "render:warn('render warning row')\n"
        "Log.info('lua info row')"))
    state = await console(ed, hidden_categories=[])
    rows = {r["text"]: r for r in state["rows"]}
    ed.check("render debug row" in rows and "render warning row" in rows and "lua info row" in rows, f"rows: {state['rows']}")
    ed.check(rows["render debug row"].get("grey"), f"a Debug row is grey: {rows['render debug row']}")
    ed.check(not rows["render warning row"].get("grey") and rows["render warning row"]["type"] == "warning",
             f"a warning is not: {rows['render warning row']}")
    ed.check("render" in state["categories"] and "lua" in state["categories"], f"categories: {state['categories']}")

    filtered = await console(ed, hidden_categories=["render"])
    texts = [r["text"] for r in filtered["rows"]]
    ed.check("lua info row" in texts and "render debug row" not in texts and "render warning row" not in texts,
             f"the Category filter hides render: {texts}")
    await shot(ed, "console-category-filter")

    shown = await console(ed, hidden_categories=[], select=rows["render warning row"]["id"])
    origin = shown.get("selected", {}).get("origin", "")
    ed.check(origin.startswith("render") and re.search(r"\S+:3\b", origin), f"the details show category and file:line: {origin!r}")
    await shot(ed, "console-selected-source")

    # Show Source Location off: the line goes away
    await ed.call("set_project_settings", values={"preferences.logging.show_source": False})
    hidden = await console(ed, select=rows["render warning row"]["id"])
    ed.check(not re.search(r":3\b", hidden["selected"]["origin"]), f"no file:line: {hidden['selected']['origin']!r}")
    await ed.call("set_project_settings", values={"preferences.logging.show_source": True,
                                                  "preferences.logging.levels": {}})


async def test_clear_on_play_and_error_pause(ed: Editor):
    made = await ed.call("create_scene", name="LogUi", type="3D", path="scenes/LogUi")
    scene = made["scene"]["id"]
    await ed.call("save_scene", scene=scene)
    await ed.call("set_project_settings", values={"preferences.clear_on_play": True, "preferences.error_pause": False})
    await ed.call("eval_lua", allow_edit_mode=True, code="Log.info('before play marker')")
    before = await console(ed)
    ed.check(any(r["text"] == "before play marker" for r in before["rows"]), "logged in edit mode")
    await ed.call("play", action="start", scene=scene)
    await ed.call("wait", frames=3)
    during = await console(ed)
    ed.check(not any(r["text"] == "before play marker" for r in during["rows"]), f"Clear on Play: {during['rows'][:5]}")
    await ed.call("play", action="stop")

    await ed.call("set_project_settings", values={"preferences.clear_on_play": False, "preferences.error_pause": True})
    await ed.call("play", action="start", scene=scene)
    await ed.call("wait", frames=3)
    await ed.call("eval_lua", code="Log.error('error pause marker')")
    await ed.call("wait", frames=3)
    st = await ed.call("get_play_state")
    ed.check_eq(st["state"], "paused", "Error Pause pauses Play on an error")
    await ed.call("play", action="stop")
    await ed.call("set_project_settings", values={"preferences.error_pause": False})


async def test_levels_persist_after_restart():
    """Preferences > Logging survive a restart: a second editor on the same settings.yaml."""
    first = Editor(create=True, name="Logging Restart", keep=True)
    async with first:
        await first.call("set_project_settings", values={"preferences.logging.levels": {"render": "debug", "audio": "error"},
                                                        "preferences.logging.file_level": "warn"})
        await first.call("save_project")
    kept = first.scratch
    original = harness.Scratch.make
    try:
        harness.Scratch.make = classmethod(lambda cls, *args, **kwargs: kept)
        second = Editor(project=kept.project, keep=False)
        async with second:
            s = (await second.call("get_project_settings", keys=["preferences.logging.levels",
                                                                 "preferences.logging.file_level"]))["settings"]
            second.check_eq(s["preferences.logging.levels"], {"audio": "error", "render": "debug"}, "levels after the restart")
            second.check_eq(s["preferences.logging.file_level"], "warn", "file level after the restart")
            cfg = await second.call("get_log_config")
            levels = {c["name"]: c["level"] for c in cfg["categories"]}
            second.check_eq((levels.get("render"), levels.get("audio")), ("debug", "error"), "applied at startup")
            file_sink = [x for x in cfg["sinks"] if x["name"] == "file"]
            second.check(file_sink and file_sink[0]["level"] == "warn", f"the file sink's level: {file_sink}")
    finally:
        harness.Scratch.make = original
        with contextlib.suppress(Exception):
            kept.remove()
