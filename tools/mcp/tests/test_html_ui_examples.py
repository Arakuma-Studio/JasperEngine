"""Interactive HTML/CSS UI examples at their public MCP boundary.

Each nonblank example keeps its authored interactions, visual reference and layout checks.
Examples that leave mutated data replay to verify a fresh state; shared font rendering
determinism is owned by RmlRendererP1GoldenTest.OpacityText. Core compatibility
and editor-library tests own exhaustive parsing, template renaming and file-copy behavior.
Project-open refresh/error reporting and LuaLS compatibility remain separate boundary checks.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import harness
from harness import Editor, TestFailure, skip, REPO

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import pngdiff  # noqa: E402  (tools/mcp/pngdiff.py)

EDITOR = {"project": REPO / "tools" / "fixtures" / "html-ui"}
CI_EXCLUDED = ("long-running (about an hour: each example's play golden drawn by the software renderer); "
               "in the full suite (MCP_SUITE=all) on the build servers")
# each test sets up what it needs in the editor it gets (ensure_scene: its own scene, made in that
# editor; the LuaLS test overwrites the scripts it copies; the copy test opens projects of its own):
# the runner may give the tests editors of their own (run_tests.py --split-seconds, tools/mcp/shard.py)
TESTS_INDEPENDENT = "each example's play golden owns its scene; the library tests own their projects"

TIMEOUT = 900

LIBRARY = harness.REPO / "docs" / "mcp" / "guide" / "html-ui-examples"
MANIFEST = json.loads((LIBRARY / "examples.json").read_text(encoding="utf-8"))
ENTRIES = {entry["name"]: entry for entry in MANIFEST["examples"]}
NAMES = [entry["name"] for entry in MANIFEST["examples"]]
GOLDEN_DIR = harness.REPO / "engine" / "tests" / "htmlui" / "golden" / "examples"
UPDATE_GOLDEN = os.environ.get("JASPER_HTMLUI_UPDATE_GOLDEN") == "1"
PROJECT_COPY = Path(".jasperengine") / "engine-api" / "html-ui-examples"
LUA_DIR = "scripts"                      # the fixture's luaDir
BACKGROUND = [0.09, 0.1, 0.13, 1.0]      # the test scenes' clear colour: the goldens' backdrop
GOLDEN_SIZE = (1280, 720)
COMPARE_SIZE = (640, 360)
CHANNEL_TOLERANCE = 24
MAX_FRACTION = 0.01
# Fresh-start captures must agree exactly (historical box-shadow cache/readback regressions).
RUNS_MAX_FRACTION = 0.0
# Reload (rebuild: a patch reload keeps running animations, S17) wall-clock CSS/video animations after
# the fixed UI clock is held.
RESTART_CSS = {"main_menu", "video_menu"}

S: dict = {}   # per example: {"scene", "entity"}, and "editor" (the Editor the scenes were made in)


# ---------------------------------------------------------------------------------------- helpers

def fail(message: str) -> None:
    raise TestFailure(message)


def check(cond, message: str) -> None:
    if not cond:
        fail(message)


def example_files(name: str) -> list[str]:
    """Every file of an example, relative to the library root."""
    root = LIBRARY / name
    return sorted(p.relative_to(LIBRARY).as_posix() for p in root.rglob("*") if p.is_file())


def html_rel(name: str) -> str:
    return f"assets/ui/{name}/{ENTRIES[name]['html']}"


def doc_lua(name: str) -> str:
    """Lua for the example's UIDocument in the played (or selected) scene."""
    return f"UIDocument(scene, entity('{name}'))"


async def lua(ed: Editor, code: str, **args) -> list:
    return (await ed.call("eval_lua", code=code, **args))["values"]


async def click(ed: Editor, name: str, selector: str, frames_after: int = 10) -> None:
    """A click at the centre of the first visible element the selector matches in the example's document."""
    await ed.call("send_input", events=[{"type": "mouse", "action": "click", "target": selector, "document": name}],
                  frames_after=frames_after)


async def hover(ed: Editor, name: str, selector: str, frames_after: int = 10) -> None:
    await ed.call("send_input", events=[{"type": "mouse", "action": "move", "target": selector, "document": name}],
                  frames_after=frames_after)


async def console_messages(ed: Editor, **args) -> list[str]:
    """The Console lines since the previous console_since / console_messages call."""
    return [e.get("message", "") for e in await ed.console_since(limit=1000, **args)]


async def pad(ed: Editor, *buttons: str, frames_after: int = 1) -> None:
    """Gamepad keys, then one update for model bindings; animated panels request their full duration."""
    await ed.call("send_input", events=[{"type": "gamepad", "button": b, "action": "press"} for b in buttons],
                  frames_after=frames_after)


async def focused(ed: Editor, name: str) -> str:
    """The id of the example's focused element ('' for none)."""
    return (await lua(ed, f"local f = {doc_lua(name)}:querySelector(':focus'); return f and f.id or ''"))[0]


async def walk(ed: Editor, name: str, button: str, limit: int = 12) -> list[str]:
    """Presses `button` until the focus stops moving; the ids it went through."""
    seen = []
    for _ in range(limit):
        await pad(ed, button)
        current = await focused(ed, name)
        if seen and current == seen[-1]:
            break
        seen.append(current)
    return seen


async def console_cursor(ed: Editor) -> int:
    """A read_console cursor at the newest line (the steps move console_since's own cursor)."""
    return (await ed.call("read_console", limit=1))["next_cursor"]


async def errors_since(ed: Editor, cursor: int) -> list[str]:
    """Errors, and the warnings of the HTML UI and of scripts, since the cursor."""
    result = await ed.call("read_console", since=cursor, types=["error", "warning"], limit=1000)
    return [e.get("message", "") for e in result["entries"]
            if e.get("type") == "error" or "[HTML UI]" in e.get("message", "") or "Lua" in e.get("message", "")]


async def ensure_scene(ed: Editor, name: str) -> dict:
    """The 2D scene UITest_<name> with a GameObject <name> showing the example (built once per editor):
    create_ui_document {html: assets/ui/<name>/<name>.html, template: name} in one call."""
    state = S.get(name)
    if state and state.get("editor") is ed:
        return state
    entry = ENTRIES[name]
    made = await ed.call("create_scene", name=f"UITest_{name}", type="2D")
    scene = made["scene"]["id"]
    await ed.call("set_scene_settings", scene=scene, settings={"background_color": BACKGROUND})
    # script: blank's Lua too (the examples' default), as every example has it attached; overwrite: the
    # LuaLS test copies the library's scripts (not renamed for <name>_assets/) into the Lua dir first
    created = await ed.call("create_ui_document", scene=scene, name=name, html=html_rel(name), template=name, script=True,
                            overwrite=True)
    entity = created["entity"]["id"]
    check(created["script"] == {"path": f"{LUA_DIR}/{entry['lua']}", "attached": True}, f"{name}: the template's script: {created['script']}")
    counts = created["document"]["diagnostics_count"]
    check((counts["error"], counts["warning"]) == (0, 0), f"{name}: the first load is clean: {created['diagnostics']}")
    await ed.call("save_scene", scene=scene, path=f"scenes/UITest_{name}")
    S[name] = {"editor": ed, "scene": scene, "entity": entity}
    return S[name]


async def capture(ed: Editor, label: str):
    """The Game view's canvas rectangle as a pngdiff image (full size)."""
    shot = await ed.call_raw("screenshot", {"view": "game", "max_width": 0})
    check(not shot.is_error and shot.images, f"{label}: no screenshot: {shot.text[:300]}")
    png = shot.images[0][0]
    (ed.scratch.root / f"{label}.png").write_bytes(png)
    rect = shot.value["canvas_rect_in_image"]
    return pngdiff.crop(pngdiff.decode(png), round(rect["x"]), round(rect["y"]), round(rect["width"]), round(rect["height"]))


async def wait_project_copy(ed: Editor, project: Path, timeout: float = 240) -> Path:
    """The editor writes the copy with the other tool files when it opens a project."""
    copy = project / PROJECT_COPY
    deadline = time.monotonic() + timeout
    await ed.call("get_project_summary")
    while time.monotonic() < deadline:
        if (copy / "examples.json").exists() and all((copy / f).exists() for n in NAMES for f in example_files(n)):
            return copy
        await asyncio.sleep(0.5)
    fail(f"the example library was not written into {copy} in {timeout:.0f}s")
    return copy


# ---------------------------------------------------------------------------------------- pngdiff


# ---------------------------------------------------------------------------------------- library rules


# ---------------------------------------------------------------------------------------- the project copy


async def test_project_copy_follows_the_editor(ed: Editor):
    """Opening a project brings its copy to the editor's examples (an edited file restored, a file and
    a folder the editor no longer has removed); a project at a unicode path gets one; a copy that
    cannot be written is reported in the Console and the project still opens."""
    root = Path(tempfile.mkdtemp(prefix="jasper-examples-copy-"))
    try:
        proj = root / "Projekt ☃ ünïcode"
        # (an atomic save's temporary sibling may come and go while the live project is copied)
        shutil.copytree(ed.project, proj, ignore=shutil.ignore_patterns(".jasperengine", "*.je-tmp"))
        await ed.call("open_project", path=str(proj), unsaved="discard")
        copy = await wait_project_copy(ed, proj)
        check((copy / "hud" / "hud.css").read_bytes() == (LIBRARY / "hud" / "hud.css").read_bytes(), "unicode path: hud.css")

        (copy / "hud" / "hud.css").write_text("/* edited */")
        (copy / "retired_example").mkdir()
        (copy / "retired_example" / "retired.html").write_text("<p>retired</p>")
        (copy / "shop" / "assets" / "items" / "retired.svg").write_text("<svg/>")
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        await ed.call("open_project", path=str(proj), unsaved="discard")
        await wait_project_copy(ed, proj)
        check((copy / "hud" / "hud.css").read_bytes() == (LIBRARY / "hud" / "hud.css").read_bytes(), "the edited file is restored")
        check(not (copy / "retired_example").exists(), "a folder the editor no longer has is removed")
        check(not (copy / "shop" / "assets" / "items" / "retired.svg").exists(), "a file the editor no longer has is removed")

        # a file where the copy's folder has to be: nothing can be written there
        shutil.rmtree(copy)
        copy.write_text("in the way")
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        await ed.console_since(None, limit=1)
        await ed.call("open_project", path=str(proj), unsaved="discard")
        await ed.call("get_project_summary")
        errors = [m for m in await console_messages(ed, types=["error"]) if "HTML UI examples" in m]
        check(errors and "html-ui-examples" in errors[0], f"the failed copy is reported: {errors}")
        check(copy.is_file() and copy.read_text() == "in the way", "the file in the way is left alone")
    finally:
        await ed.call("open_project", path=str(ed.project), unsaved="discard")
        shutil.rmtree(root, ignore_errors=True)


def _find_tool(name: str) -> str | None:
    found = shutil.which(name)
    if found:
        return found
    local = Path("/var/lib/orquester/.local/bin") / name
    return str(local) if local.exists() else None


async def test_lua_scripts_have_no_luals_diagnostics(ed: Editor):
    """The examples' scripts in a project's Lua directory, checked by lua-language-server with the
    editor's stubs and .luarc.json: no diagnostic of Warning or above."""
    exe = _find_tool("lua-language-server")
    if not exe:
        skip("lua-language-server is not installed (https://github.com/LuaLS/lua-language-server/releases)")
    await wait_project_copy(ed, ed.project)
    (ed.project / LUA_DIR).mkdir(parents=True, exist_ok=True)
    for name in NAMES:
        shutil.copy(LIBRARY / name / ENTRIES[name]["lua"], ed.project / LUA_DIR / ENTRIES[name]["lua"])
    await ed.call("sync_files")
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "check.json"
        proc = subprocess.run([exe, f"--check={ed.project}", "--checklevel=Warning", "--check_format=json",
                               f"--check_out_path={out}"], capture_output=True, text=True, timeout=600)
        check(out.exists(), f"lua-language-server wrote no report: {proc.stdout[-2000:]} {proc.stderr[-2000:]}")
        data = json.loads(out.read_text() or "{}") or {}
    scripts = {ENTRIES[n]["lua"] for n in NAMES}
    problems = [f"{uri.rsplit('/', 1)[-1]}:{d['range']['start']['line'] + 1}: {d.get('code')}: {d['message'][:160]}"
                for uri, items in data.items() for d in items if uri.rsplit("/", 1)[-1] in scripts]
    check(not problems, "lua-language-server diagnostics in the examples:\n" + "\n".join(problems))


# ---------------------------------------------------------------------------------------- interaction steps
# Each example: `before` runs from play start to the golden capture (every run), `after` once after
# it (the rest of the behaviour). Both check the spec's §5.4 table through eval_lua and the Console.

async def main_menu_before(ed: Editor, n: str, first: bool) -> None:
    await hover(ed, n, "#settings")
    hovered = (await ed.call("inspect_ui", entity=n, selector="#settings", depth=0))["elements"][0]
    check("hover" in hovered.get("pseudo", []), f"inspect_ui shows #settings hovered: {hovered}")
    await click(ed, n, "#settings", frames_after=30)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('settings-panel').classList:contains('open')") == [True],
          "#settings opens the panel")
    await click(ed, n, "#settings-back", frames_after=30)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('settings-panel').classList:contains('open')") == [False],
          "#settings-back closes it")
    await ed.console_since(None, limit=1)
    await click(ed, n, "#play", frames_after=30)
    check("main_menu: play" in await console_messages(ed), "#play logs main_menu: play")


async def main_menu_after(ed: Editor, n: str) -> None:
    await ed.call("send_input", events=[{"type": "key", "key": "down"}, {"type": "key", "key": "enter"}], frames_after=10)
    check("main_menu: settings" in await console_messages(ed), "ArrowDown then Enter presses #settings")

    # the gamepad (p4-input): B closes the panel, the d-pad reaches every button, A presses
    panel = f"return {doc_lua(n)}:getElementById('settings-panel').classList:contains('open')"
    check(await lua(ed, panel) == [True], "the panel is open")
    await pad(ed, "dpad_down")
    check(await focused(ed, n) == "settings-back", "the d-pad stays on the open panel")
    await pad(ed, "b", frames_after=30)
    check(await lua(ed, panel) == [False] and await focused(ed, n) == "settings", "B closes the panel")
    check(await walk(ed, n, "dpad_down", 3) == ["credits", "quit", "play"], "the d-pad goes through the buttons")
    await pad(ed, "dpad_down", "dpad_down")
    await ed.console_since(None, limit=1)
    await pad(ed, "a", frames_after=10)
    check("main_menu: credits" in await console_messages(ed), "A presses the focused #credits")
    await pad(ed, "dpad_up", "a", frames_after=30)
    check(await lua(ed, panel) == [True] and await focused(ed, n) == "settings-back", "A on #settings opens the panel")
    await pad(ed, "a", frames_after=30)
    check(await lua(ed, panel) == [False], "A on Back closes it")


async def pause_menu_before(ed: Editor, n: str, first: bool) -> None:
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('pause').classList:contains('open')") == [False],
          "the game starts unpaused")
    await ed.call("send_input", events=[{"type": "key", "key": "escape"}], frames_after=20)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('pause').classList:contains('open'), Engine.timeScale") == [True, 0],
          "Escape opens the menu and freezes the game")


async def pause_menu_after(ed: Editor, n: str) -> None:
    await ed.console_since(None, limit=1)
    await click(ed, n, "#options")
    check("pause_menu: options" in await console_messages(ed), "#options logs")
    await ed.call("send_input", events=[{"type": "key", "key": "escape"}], frames_after=20)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('pause').classList:contains('open'), Engine.timeScale") == [False, 1],
          "Escape again closes it and the game runs")
    await ed.call("send_input", events=[{"type": "key", "key": "escape"}], frames_after=5)
    await click(ed, n, "#resume")
    check(await lua(ed, "return Engine.timeScale") == [1], "#resume closes the menu")

    # the gamepad (p4-input): Start opens, the d-pad reaches every button, A presses, B closes
    is_open = f"return {doc_lua(n)}:getElementById('pause').classList:contains('open'), Engine.timeScale"
    await pad(ed, "start", frames_after=20)
    check(await lua(ed, is_open) == [True, 0] and await focused(ed, n) == "resume", "Start opens the menu on #resume")
    check(await walk(ed, n, "dpad_down") == ["options", "restart", "to-menu"], "the d-pad goes through the buttons")
    await pad(ed, "dpad_up")
    await ed.console_since(None, limit=1)
    await pad(ed, "a", frames_after=10)
    check("pause_menu: restart" in await console_messages(ed), "A presses #restart")
    await pad(ed, "b", frames_after=20)
    check(await lua(ed, is_open) == [False, 1], "B closes the menu and the game runs")
    await pad(ed, "start", frames_after=20)
    await pad(ed, "a", frames_after=20)
    check(await lua(ed, is_open) == [False, 1], "A on #resume closes it")


async def hud_before(ed: Editor, n: str, first: bool) -> None:
    # the shield takes damage first: 80 empties the 50 of shield and leaves 70 health
    # (the spec's damage(30) assumed no shield)
    check(await lua(ed, f"local hud = script('{n}'); hud:damage(80); return hud.model.health, hud.model.shield") == [70, 0], "damage(80)")
    await ed.call("wait", frames=30)
    fill, track = await lua(ed, f"""
        local d = {doc_lua(n)}
        local fill = d:getElementById('health-fill')
        return fill:getBoundingClientRect().width, fill.parentElement:getBoundingClientRect().width""")
    check(abs(fill - 0.7 * track) <= 1.0, f"#health-fill is 70% of its track: {fill} of {track}")
    before = int((await lua(ed, f"return {doc_lua(n)}:getElementById('ammo').textContent"))[0])
    await lua(ed, f"local hud = script('{n}'); hud:fire(); hud:fire(); hud:fire()")
    await ed.call("wait", frames=2)
    after = int((await lua(ed, f"return {doc_lua(n)}:getElementById('ammo').textContent"))[0])
    check(after == before - 3, f"three shots: #ammo {before} -> {after}")


async def hud_after(ed: Editor, n: str) -> None:
    await lua(ed, f"script('{n}'):reload()")
    await ed.call("wait", frames=2)
    check(await lua(ed, f"local d = {doc_lua(n)}; return d:getElementById('ammo').textContent, d:getElementById('reserve').textContent")
          == ["30", "/ 87"], "reload refills the magazine from the reserve")
    timer = (await lua(ed, f"return {doc_lua(n)}:getElementById('timer').textContent"))[0]
    check(re.fullmatch(r"\d\d:\d\d", timer.strip()) is not None, f"#timer shows mm:ss: {timer!r}")


async def inventory_before(ed: Editor, n: str, first: bool) -> None:
    check(await lua(ed, f"return #{doc_lua(n)}:querySelectorAll('.grid > .slot')") == [24], "24 slots (the data-for template is no element)")
    await ed.call("send_input", events=[{"type": "mouse", "action": "drag", "target": ".slot:nth-child(1)",
                                         "to_target": ".slot:nth-child(6)", "document": n, "steps": 8}], frames_after=5)
    check(await lua(ed, f"local m = {doc_lua(n)}:getDataModel('inventory'); return m.items[1].id, m.items[6].id") == ["", "sword"],
          "dragging slot 1 onto slot 6 swaps the entries")
    # the drag carried its slot in event.dataTransfer: no drag state is left in the script or the document
    check(await lua(ed, f"local d = {doc_lua(n)}; return #d:querySelectorAll('.htmlui-drag-ghost'), UIDrag.isActive()") == [0, False],
          "the ghost went with the drag")
    await hover(ed, n, ".slot:nth-child(3)", frames_after=30)
    check(await lua(ed, f"local t = {doc_lua(n)}.tooltip; return t.visible, t.text:find('Health Potion') ~= nil") == [True, True],
          "hovering a slot shows its tooltip (el:setTooltip with a function)")


async def inventory_after(ed: Editor, n: str) -> None:
    await hover(ed, n, ".slot:nth-child(7)", frames_after=30)
    check(await lua(ed, f"return {doc_lua(n)}.tooltip.visible") == [False], "an empty slot has no tooltip")
    # the gamepad (p4-input): the first key focuses the first slot, the d-pad moves through the grid
    # (the tooltip follows), A picks an item up and puts it down, B cancels, the close button is reachable
    slot = (f"local d = {doc_lua(n)}; local s = d:querySelectorAll('.grid > .slot'); "
            f"for i, e in ipairs(s) do if e:matches(':focus') then return i end end; return 0")
    await lua(ed, f"local f = {doc_lua(n)}:querySelector(':focus'); if f then f:blur() end")
    await pad(ed, "dpad_right")
    check(await lua(ed, slot) == [1], "the first gamepad key focuses the first slot")
    await pad(ed, "dpad_right", "dpad_down")
    check(await lua(ed, slot) == [8], "right then down: slot 8 (six columns)")
    await ed.call("wait", frames=30)
    check(await lua(ed, f"local t = {doc_lua(n)}.tooltip; return t.visible, t.text:find('Vault Key') ~= nil") == [True, True],
          "the tooltip follows the focus (after its delay)")
    model = f"{doc_lua(n)}:getDataModel('inventory')"
    await pad(ed, "a")
    check(await lua(ed, f"return {model}.picked") == [8], "A picks the key up")
    await pad(ed, "dpad_left", "a")
    check(await lua(ed, f"local m = {model}; return m.picked, m.items[7].id, m.items[8].id") == [0, "key", ""],
          "A on slot 7 puts it down there")
    await pad(ed, "a", "b")
    check(await lua(ed, f"return {model}.picked, {doc_lua(n)}.visible") == [0, True], "B cancels a pick")
    rows = [await lua(ed, slot)]
    for button in ("dpad_up", "dpad_right", "dpad_right", "dpad_right", "dpad_right", "dpad_right", "dpad_up"):
        await pad(ed, button)
        rows.append((await lua(ed, slot))[0])
    check(await focused(ed, n) == "close", f"the close button above the grid is reachable: {rows}")

    await click(ed, n, "#close", frames_after=1)
    check(await lua(ed, f"return {doc_lua(n)}.visible") == [False], "#close hides the document")
    await pad(ed, "back", frames_after=10)
    check(await lua(ed, f"return {doc_lua(n)}.visible") == [True], "View shows it again")
    await pad(ed, "b", frames_after=10)
    check(await lua(ed, f"return {doc_lua(n)}.visible") == [False], "B closes it")


async def settings_before(ed: Editor, n: str, first: bool) -> None:
    model = f"{doc_lua(n)}:getDataModel('settings')"
    check(await lua(ed, f"local m = {model}; return m.music, m.vsync, m.quality, {doc_lua(n)}:getElementById('quality').value")
          == [60, True, "high", "high"], "the controls show the model's values")
    await click(ed, n, "#vsync")
    check(await lua(ed, f"return {model}.vsync") == [False], "the V-Sync switch toggles the model")
    await click(ed, n, "#tab-audio")
    check(await lua(ed, f"local d = {doc_lua(n)}; return d:getElementById('tab-audio').classList:contains('active'), "
                        f"d:getElementById('tab-graphics').classList:contains('active'), "
                        f"d:getElementById('panel-audio').classList:contains('active')") == [True, False, True], "the Audio tab")
    x, y, w, h = await lua(ed, f"local r = {doc_lua(n)}:getElementById('music'):getBoundingClientRect(); return r.x, r.y, r.width, r.height")
    thumb = 18.0
    start = x + thumb / 2 + (w - thumb) * 0.60
    end = x + thumb / 2 + (w - thumb) * 0.25
    await ed.call("send_input", events=[{"type": "mouse", "action": "drag", "x": start, "y": y + h / 2, "to": [end, y + h / 2],
                                         "steps": 8}], frames_after=20)
    music = (await lua(ed, f"return {model}.music"))[0]
    check(abs(music - 25) <= 3, f"the music slider dragged to its 25% point: {music}")


async def settings_after(ed: Editor, n: str) -> None:
    await click(ed, n, "#tab-controls", frames_after=1)
    await click(ed, n, "#bind-jump", frames_after=1)
    check("Press a key" in (await lua(ed, f"return {doc_lua(n)}:getElementById('bind-jump').textContent"))[0], "the binding waits for a key")
    await ed.call("send_input", events=[{"type": "key", "key": "j"}], frames_after=1)
    check(await lua(ed, f"return {doc_lua(n)}:getDataModel('settings').keys.jump") == ["J"], "the next key is the binding")
    await ed.console_since(None, limit=1)
    await click(ed, n, "#apply", frames_after=1)
    applied = [m for m in await console_messages(ed) if m.startswith("settings: apply")]
    check(applied and "music=" in applied[0] and "jump=J" in applied[0] and "vsync=false" in applied[0], f"#apply logs the values: {applied}")
    await click(ed, n, "#reset", frames_after=1)
    check(await lua(ed, f"local m = {doc_lua(n)}:getDataModel('settings'); return m.music, m.vsync, m.keys.jump") == [60, True, "Space"],
          "#reset restores the defaults")

    # the gamepad (p4-input): every control of a tab is reachable, ranges adjust, checkboxes toggle,
    # selects open and choose, B is Back
    model = f"{doc_lua(n)}:getDataModel('settings')"
    await lua(ed, f"{doc_lua(n)}:getElementById('tab-graphics'):focus()")
    await pad(ed, "a", frames_after=1)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('panel-graphics').classList:contains('active')") == [True],
          "A selects the Graphics tab")
    check(await walk(ed, n, "dpad_right") == ["tab-audio", "tab-controls", "tab-gameplay"], "the d-pad goes along the tabs")
    down = await walk(ed, n, "dpad_down")
    for want in ("resolution", "quality", "fov", "vsync", "bloom"):
        check(want in down, f"the d-pad reaches #{want}: {down}")
    check(any(i in down for i in ("back", "reset", "apply")), f"and the footer: {down}")
    footer = await walk(ed, n, "dpad_left") + await walk(ed, n, "dpad_right")
    for want in ("back", "reset", "apply"):
        check(want in footer, f"the footer's #{want} is reachable: {footer}")
    up = await walk(ed, n, "dpad_up")
    check(up and up[-1].startswith("tab-"), f"up reaches the tabs again: {up}")

    await lua(ed, f"{doc_lua(n)}:getElementById('fov'):focus()")
    fov = (await lua(ed, f"return {model}.fov"))[0]
    await pad(ed, "dpad_right", "dpad_right", frames_after=1)
    check(await lua(ed, f"return {model}.fov") == [fov + 2], "the d-pad adjusts the focused range")
    await lua(ed, f"{doc_lua(n)}:getElementById('bloom'):focus()")
    await pad(ed, "a", frames_after=1)
    check(await lua(ed, f"return {model}.bloom") == [False], "A toggles the focused switch")
    await lua(ed, f"{doc_lua(n)}:getElementById('quality'):focus()")
    await pad(ed, "a", "dpad_down", "a", frames_after=1)
    check(await lua(ed, f"return {model}.quality") == ["ultra"], "A opens the select, the d-pad moves, A chooses")
    await ed.console_since(None, limit=1)
    await pad(ed, "b", frames_after=1)
    check("settings: back" in await console_messages(ed), "B is Back")


async def dialogue_before(ed: Editor, n: str, first: bool) -> None:
    await ed.call("wait", frames=240)
    check(await lua(ed, f"return #{doc_lua(n)}:querySelectorAll('.choice')") == [3], "the line is typed and its choices shown")


async def dialogue_after(ed: Editor, n: str) -> None:
    before = (await lua(ed, f"return {doc_lua(n)}:getElementById('line').textContent"))[0]
    await ed.console_since(None, limit=1)
    await click(ed, n, ".choice:nth-child(2)")
    check("dialogue: choice 2" in await console_messages(ed), "the second choice logs")
    after = (await lua(ed, f"return {doc_lua(n)}:getElementById('line').textContent"))[0]
    check(after != before, f"#line changed: {after!r}")
    # Space completes the new line, a click goes on to the next one
    await ed.call("send_input", events=[{"type": "key", "key": "space"}], frames_after=2)
    full = (await lua(ed, f"return {doc_lua(n)}:getElementById('line').textContent"))[0]
    check(full.startswith("Good eye.") and full.endswith("light low."), f"Space completes the line: {full!r}")


async def shop_before(ed: Editor, n: str, first: bool) -> None:
    # the descriptions are clamped to two lines (-webkit-line-clamp, p6-line-clamp): the Ember Blade's runs on past them,
    # the Iron Helm's is one line
    long, scroll, short = await lua(ed, f"local d = {doc_lua(n)}; local a = d:querySelector('.card:nth-child(1) .stats'); "
                                        f"return a.offsetHeight, a.scrollHeight, d:querySelector('.card:nth-child(5) .stats').offsetHeight")
    check(abs(long - 2 * short) <= 1 and scroll > long + short / 2,
          f"a long description shows two lines of more ({long}px of {scroll}px), a short one one ({short}px)")
    check(await lua(ed, f"local d = {doc_lua(n)}; return d:querySelector('.card:nth-child(3) .buy'):hasAttribute('disabled'), "
                        f"d:querySelector('.card:nth-child(2) .buy'):hasAttribute('disabled')") == [True, False],
          "a card the purse cannot pay for has its button disabled")
    await click(ed, n, ".card:nth-child(2) .buy", frames_after=20)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('confirm').classList:contains('open')") == [True], "Buy opens #confirm")


async def shop_after(ed: Editor, n: str) -> None:
    await click(ed, n, "#qty-plus")
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('qty').textContent") == ["2"], "#qty-plus")
    await click(ed, n, "#qty-minus")
    await ed.console_since(None, limit=1)
    await click(ed, n, "#confirm-buy", frames_after=60)
    check("shop: bought axe x1" in await console_messages(ed), "the purchase logs")
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('gold').textContent") == ["520"], "#gold counted down by the price")
    # (read through the computed display: a card data-if hides keeps its last offsetHeight in the engine)
    await click(ed, n, "#chip-potions", frames_after=20)
    shown = await lua(ed, f"local names = {{}}; for _, c in ipairs({doc_lua(n)}:querySelectorAll('.card')) do "
                          f"if c.style.display ~= 'none' then names[#names + 1] = c:querySelector('.name').textContent end end; "
                          f"return table.concat(names, ', ')")
    check(shown == ["Health Potion, Golden Apple"], f"the Potions chip shows the potions: {shown}")

    # the gamepad (p4-input): the dialog keeps the d-pad on its buttons, A presses, B cancels
    await click(ed, n, ".card:nth-child(7) .buy", frames_after=20)
    is_open = f"return {doc_lua(n)}:getElementById('confirm').classList:contains('open')"
    check(await lua(ed, is_open) == [True] and await focused(ed, n) == "confirm-buy", "Buy opens the dialog on its Buy")
    await pad(ed, "dpad_right")
    check(await focused(ed, n) == "qty-plus", "right wraps to the stepper (the disabled minus is skipped)")
    await pad(ed, "a", frames_after=10)
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('qty').textContent") == ["2"], "A on + adds one")
    await pad(ed, "dpad_down", "dpad_down", "dpad_down")
    check(await focused(ed, n) in ("qty-minus", "qty-plus", "confirm-cancel", "confirm-buy"), "the d-pad stays in the dialog")
    await pad(ed, "b", frames_after=20)
    check(await lua(ed, is_open) == [False], "B cancels the dialog")
    check(await lua(ed, f"return {doc_lua(n)}:querySelectorAll('.card')[7]:querySelector('.buy'):matches(':focus')") == [True],
          "the focus goes back to the card's Buy")


async def toasts_before(ed: Editor, n: str, first: bool) -> None:
    await ed.call("wait", frames=30)
    check(await lua(ed, f"return #{doc_lua(n)}:querySelectorAll('.toast')") == [3], "three toasts (the samples removed)")


async def toasts_after(ed: Editor, n: str) -> None:
    await ed.call("wait", game_seconds=4)
    check(await lua(ed, f"return #{doc_lua(n)}:querySelectorAll('.toast')") == [0], "they go after 3 seconds")
    await lua(ed, f"local t = script('{n}'); for i = 1, 7 do t:show('Toast ' .. i, 'error', 3) end")
    await ed.call("wait", frames=30)
    check(await lua(ed, f"return #{doc_lua(n)}:querySelectorAll('.toast')") == [5], "at most five")


async def loading_before(ed: Editor, n: str, first: bool) -> None:
    percent = int((await lua(ed, f"return {doc_lua(n)}:getElementById('percent').textContent"))[0].rstrip("%"))
    check(45 <= percent <= 55, f"#percent after 1.5 s of 3: {percent}")


async def loading_after(ed: Editor, n: str) -> None:
    await ed.console_since(None, limit=1)
    await ed.call("wait", game_seconds=2)
    check("loading: done" in await console_messages(ed), "loading: done")
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('press-key').offsetHeight > 0") == [True], "#press-key shows")
    await ed.call("send_input", events=[{"type": "key", "key": "space"}], frames_after=2)
    check("loading: continue" in await console_messages(ed), "a key continues")


async def video_state(ed: Editor, n: str) -> dict:
    tree = await ed.call("inspect_ui", entity=n, selector="#backdrop", depth=0)
    return tree["elements"][0]["media"]


async def video_menu_before(ed: Editor, n: str, first: bool) -> None:
    m = await video_state(ed, n)
    check(not m["paused"] and m["loop"] and m["muted"] and m["readyState"] == 4, f"the background video plays: {m}")
    check((m["videoWidth"], m["videoHeight"]) == (640, 360) and abs(m["duration"] - 4.0) < 1e-3, f"menu-loop.webm: {m}")
    # reloaded when play is held, then 90 frames of 1/60 s: 1.5 s, frame 45 (frame-exact)
    check(abs(m["currentTime"] - 1.5) < 1e-9 and m["frame"] == 45, f"frame-exact on the UI clock: {m}")
    await hover(ed, n, "#play", frames_after=20)


async def video_menu_after(ed: Editor, n: str) -> None:
    await console_messages(ed)
    await click(ed, n, "#motion", frames_after=5)
    m = await video_state(ed, n)
    check(m["paused"], f"the motion toggle pauses the video: {m}")
    label, pressed = await lua(ed, f"local d = {doc_lua(n)}; return d:getElementById('motion-label').textContent, "
                                   f"d:getElementById('motion'):getAttribute('aria-pressed')")
    check((label, pressed) == ("Play background", "true"), f"the toggle follows the pause event: {label!r} {pressed!r}")
    check("video_menu: background paused" in await console_messages(ed), "logged")
    await ed.call("wait", frames=30)
    check((await video_state(ed, n))["currentTime"] == m["currentTime"], "paused: the time stays")
    await click(ed, n, "#motion", frames_after=5)
    t0 = (await video_state(ed, n))["currentTime"]
    label = (await lua(ed, f"return {doc_lua(n)}:getElementById('motion-label').textContent"))[0]
    check(label == "Pause background", f"playing again: {label!r}")
    # 3 s on: past the 4 s end, the loop wraps (no ended, no pause)
    await ed.call("wait", frames=180)
    m = await video_state(ed, n)
    check(not m["paused"] and not m["ended"] and abs(m["currentTime"] - (t0 + 3.0) % m["duration"]) < 1e-6,
          f"looped: {t0} + 3 s -> {m['currentTime']}")
    await ed.call("send_input", events=[{"type": "key", "key": "down"}, {"type": "key", "key": "enter"}], frames_after=10)
    check(await focused(ed, n) == "play" and "video_menu: play" in await console_messages(ed), "Down focuses Play, Enter presses it")
    await pad(ed, "dpad_down", "a", frames_after=10)
    check(await focused(ed, n) == "continue" and "video_menu: continue" in await console_messages(ed), "the d-pad and A do the same")


def view_lua(n: str, element_id: str) -> str:
    return f"{doc_lua(n)}:getElementById('{element_id}').itemsView"


async def server_browser_before(ed: Editor, n: str, first: bool) -> None:
    view = view_lua(n, "servers")
    check(await lua(ed, f"local v = {view}; return v.count, v:getState().pool <= v:getState().poolBound") == [10000, True],
          "ten thousand servers, a pool of rows")
    await click(ed, n, ".htmlui-header-cell[data-column=name]", frames_after=10)
    check(await lua(ed, f"local v = {view}; local s = v:getState(); return s.sortKey, s.sortDirection") == ["name", "asc"],
          "a header click sorts by its column")
    await click(ed, n, '.htmlui-row[aria-rowindex="2"]', frames_after=10)
    name = await lua(ed, f"return {doc_lua(n)}:getDataModel('browser').selected.name")
    check(name[0] != "" and await lua(ed, f"return {doc_lua(n)}:getElementById('detail-name').textContent") == name,
          f"the selected server fills the details: {name}")


async def server_browser_after(ed: Editor, n: str) -> None:
    view = view_lua(n, "servers")
    await click(ed, n, "#region-eu", frames_after=10)
    regions = await lua(ed, f"local m = {doc_lua(n)}:getDataModel('browser'); return m.servers[1].region, m.servers[#m.servers].region, m.shown")
    check(regions[:2] == ["EU", "EU"] and 0 < regions[2] < 10000, f"the Europe chip filters: {regions}")
    await ed.console_since(None, limit=1)
    # (the chip has the focus: a row click gives it back to the table)
    await click(ed, n, '.htmlui-row[aria-rowindex="2"]', frames_after=10)
    await ed.call("send_input", events=[{"type": "key", "key": "down"}, {"type": "key", "key": "enter"}], frames_after=5)
    check(await lua(ed, f"return {view}.activeIndex") == [3], "Down moves the selection")
    check(any(m.startswith("servers: joining ") for m in await console_messages(ed)), "Enter joins the selected server")
    await hover(ed, n, ".ping-head", frames_after=40)
    check(await lua(ed, f"local t = {doc_lua(n)}.tooltip; return t.visible, t.text:find('round trip') ~= nil") == [True, True],
          "the Ping header's rich tooltip (tooltip=\"#ping-help\")")
    await hover(ed, n, '.htmlui-row[aria-rowindex="4"] .name', frames_after=40)
    check(await lua(ed, f"local t = {doc_lua(n)}.tooltip; return t.visible, t.text:find('players') ~= nil") == [True, True],
          "a row's title tooltip")


async def chat_log_before(ed: Editor, n: str, first: bool) -> None:
    view = view_lua(n, "messages")
    check(await lua(ed, f"local v = {view}; local first, last = v:getVisibleRange(); return v.count, last") == [120, 120],
          "120 messages, the view at the end (anchor=\"end\")")
    await lua(ed, f"{doc_lua(n)}:getElementById('draft').value = 'gg **all**'")
    await ed.console_since(None, limit=1)
    await click(ed, n, "#send", frames_after=10)
    check("chat: sent gg **all**" in await console_messages(ed), "Send posts the draft")
    check(await lua(ed, f"local v = {view}; local first, last = v:getVisibleRange(); return v.count, last") == [121, 121],
          "the new message shows at the end")
    check(await lua(ed, f"return {view}:getRow(121):querySelector('strong').textContent") == ["all"], "its Markdown is rendered")


async def chat_log_after(ed: Editor, n: str) -> None:
    view = view_lua(n, "messages")
    await lua(ed, f"{doc_lua(n)}:getElementById('messages').scrollTop = 0")
    await ed.call("wait", game_seconds=4.5)
    values = await lua(ed, f"local v = {view}; local first, last = v:getVisibleRange(); return v.count, first, last")
    check(values[0] == 122 and values[1] == 1 and values[2] < 122, f"scrolled up, a new message leaves the view where it is: {values}")


async def quest_log_before(ed: Editor, n: str, first: bool) -> None:
    view = view_lua(n, "tree")
    check(await lua(ed, f"return {view}.count") == [8], "the two first chapters open: 3 chapters and 5 quests")
    await click(ed, n, '.htmlui-row[aria-rowindex="3"]', frames_after=10)
    check((await lua(ed, f"return {doc_lua(n)}:getElementById('journal').textContent:find('vault key') ~= nil"))[0],
          "the selected quest's notes in the journal")


async def quest_log_after(ed: Editor, n: str) -> None:
    view = view_lua(n, "tree")
    await click(ed, n, '.htmlui-row[aria-rowindex="1"]', frames_after=10)
    check((await lua(ed, f"return {doc_lua(n)}:getElementById('journal').textContent:find('Journal') ~= nil"))[0],
          "a chapter shows the journal's first page (<markdown src>)")
    await ed.call("send_input", events=[{"type": "key", "key": "left"}], frames_after=5)
    check(await lua(ed, f"return {view}.count, {view}:isExpanded('main')") == [5, False], "Left closes the chapter")
    await ed.call("send_input", events=[{"type": "key", "key": "right"}, {"type": "key", "key": "right"}], frames_after=5)
    check(await lua(ed, f"return {view}.activeIndex, {view}:getNode({view}.activeIndex).key") == [2, "map"],
          "Right opens it, then goes to its first quest")
    await ed.console_since(None, limit=1)
    await pad(ed, "a", frames_after=10)
    check("quests: finished The old map" in await console_messages(ed), "A finishes the quest")
    check(await lua(ed, f"return {doc_lua(n)}:getElementById('progress').textContent") == ["2 of 6 done"], "the progress counts it")


# The portrait composition has frame-by-frame recording/seek/layout coverage in
# test_html_ui_motion_graphics.py; this harness captures landscape interactive menus.
INTERACTIVE_NAMES = [name for name in NAMES if name not in {"blank", "motion_graphics"}]
STEPS = {name: (globals()[f"{name}_before"], globals()[f"{name}_after"]) for name in INTERACTIVE_NAMES}


# ---------------------------------------------------------------------------------------- per example

def _make_tests(name: str) -> None:
    entry = ENTRIES[name]

    async def play_golden(ed: Editor):
        state = await ensure_scene(ed, name)
        before, after = STEPS[name]
        await ed.call("configure_game_view", aspect="project", scale=1, maximize_on_play=True)
        captures = []
        # These have no distinct reset assertion on replay; their second capture duplicates
        # the core font/readback oracles. Other examples leave distinct data to reset.
        runs = 1 if name in {"main_menu", "pause_menu"} else 2
        try:
            for run in range(runs):
                cursor = await console_cursor(ed)
                await ed.call("play", action="start", scene=state["scene"], fixed_fps=60, auto_pause=True)
                if name in RESTART_CSS:
                    await lua(ed, f"{doc_lua(name)}:reload{{mode = \"rebuild\"}}")
                await ed.call("wait", frames=90)
                await before(ed, name, run == 0)
                captures.append(await capture(ed, f"{name}.play{run}"))
                if run == runs - 1:
                    # Probe the captured initial state after the final capture, so alternate
                    # viewport layouts do not change the history of a remaining replay.
                    result = await ed.call("assert_ui", entity=name, checks=entry["checks"], viewports=entry["viewports"])
                    check(result["passed"], f"{name}: assert_ui failed: " +
                          json.dumps([c for c in result["checks"] if not c["passed"]])[:3000])
                if run == 0:
                    await after(ed, name)
                errors = await errors_since(ed, cursor)
                check(not errors, f"{name}: Console problems while playing: {errors[:5]}")
                await ed.call("play", action="stop")
        finally:
            failed = sys.exc_info()[0] is not None
            try:
                await ed.call("play", action="stop", fixed_fps=0, auto_pause=False)
                # Each generated case owns its scene. Release its stopped context before the
                # next example; the initial scene and screenshots remain on disk.
                await ed.call("close_scene", scene=state["scene"], unsaved="discard")
            except Exception as cleanup_error:
                if not failed:
                    raise
                print(f"     [cleanup] {name}: {cleanup_error}", flush=True)
            finally:
                S.pop(name, None)

        first = captures[0]
        if len(captures) == 2:
            same, fraction, diff = pngdiff.compare(first, captures[1], CHANNEL_TOLERANCE, RUNS_MAX_FRACTION)
            if not same:
                (ed.scratch.root / f"{name}.runs.diff.png").write_bytes(diff or b"")
                ed.keep = True
            check(same, f"{name}: two runs differ in {fraction * 100:.2f}% of the pixels ({ed.scratch.root / (name + '.runs.diff.png')})")

        width, height = first[0], first[1]
        check(width >= GOLDEN_SIZE[0] and height >= GOLDEN_SIZE[1],
              f"the Game view shows the canvas at {width}x{height}, smaller than {GOLDEN_SIZE[0]}x{GOLDEN_SIZE[1]}")
        golden_path = GOLDEN_DIR / f"{name}.png"
        if UPDATE_GOLDEN:
            GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
            golden_path.write_bytes(pngdiff.encode(pngdiff.downscale(first, *GOLDEN_SIZE)))
            print(f"     [golden] wrote {golden_path.relative_to(harness.REPO)}: look at it")
            return
        check(golden_path.is_file(), f"no golden {golden_path} (JASPER_HTMLUI_UPDATE_GOLDEN=1 writes it)")
        golden = pngdiff.load(golden_path)
        ok, fraction, diff = pngdiff.compare(pngdiff.downscale(first, *COMPARE_SIZE), pngdiff.downscale(golden, *COMPARE_SIZE),
                                             CHANNEL_TOLERANCE, MAX_FRACTION)
        if not ok:
            actual = ed.scratch.root / f"{name}.actual.png"
            actual.write_bytes(pngdiff.encode(pngdiff.downscale(first, *GOLDEN_SIZE)))
            (ed.scratch.root / f"{name}.diff.png").write_bytes(diff or b"")
            ed.keep = True
            fail(f"{name}: {fraction * 100:.2f}% of the pixels differ from the golden (at most {MAX_FRACTION * 100:.0f}%): "
                 f"{actual}, {ed.scratch.root / (name + '.diff.png')}")

    play_golden.__name__ = play_golden.__qualname__ = f"test_{name}_play_golden"
    play_golden.__doc__ = f"{name}: interactive example, visual reference and layout checks"
    globals()[play_golden.__name__] = play_golden


for _name in INTERACTIVE_NAMES:
    _make_tests(_name)
