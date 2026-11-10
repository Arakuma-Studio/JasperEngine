"""S25 editor search and organisation (docs/specs/S25-editor-search-organisation.md §7).

The command catalog behind Help, the menus and list_shortcuts (AC1, AC6), the Search palette as
search_editor and run_command (AC11), window layout presets (AC13, the D1 reload spike), the
Project search grammar over asset labels (AC16, AC17), saved searches (AC19) and bookmarks
(AC20). Tests run in order and share the editor, on a new project holding the two textures the
labels, searches and bookmarks use (a CI file: no default fixture, run_tests.py).
"""

from pathlib import Path

import pngdiff
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Search Organisation"}
TIMEOUT = 900

REPO = Path(__file__).resolve().parents[3]
HERO = ["assets/sprites/rock.png", "assets/sprites/background.png"]


async def add_hero_textures(ed: Editor) -> None:
    """Imports the two textures (as a drop on the Project window), then waits until the asset
    database gives them an identity"""
    source = ed.scratch.root / "hero-textures"
    source.mkdir(exist_ok=True)
    for i, path in enumerate(HERO):
        (source / Path(path).name).write_bytes(pngdiff.encode(8, 8, bytes([60 + 120 * i, 90, 140, 255]) * 64))
    await ed.call("import_files", sources=[str(source / Path(path).name) for path in HERO], to_dir=str(Path(HERO[0]).parent))
    got = None
    for _ in range(120):
        got = await ed.call_raw("edit_asset_labels", {"action": "get", "assets": HERO})
        if not got.is_error:
            return
        await ed.call("wait", frames=5)
    ed.fail(f"the asset database never gave {HERO} an identity: {got.text[:300]}")


def by_id(listed: dict) -> dict:
    return {item["id"]: item for item in listed["shortcuts"]}


async def wait_frames(ed: Editor, count: int = 2) -> dict:
    status = {}
    for _ in range(count):
        status = await ed.call("editor_status")
    return status


async def test_help_lists_tools(ed: Editor):
    shortcuts = by_id(await ed.call("list_shortcuts"))
    ed.check_eq(shortcuts["tools.hand"]["chords"], ["Q"], "Q is the Hand Tool")
    ed.check_eq(shortcuts["tools.hand"]["name"], "Hand Tool", "name")
    ed.check_eq(shortcuts["tools.rect"]["chords"], ["T"], "T is the Rect Tool")
    ed.check_eq(shortcuts["tools.rect"]["name"], "Rect Tool", "name")
    ed.check_eq(shortcuts["tools.toggleGlobalLocal"]["chords"], ["X"], "X toggles Global/Local")
    ed.check_eq(shortcuts["tools.toggleGlobalLocal"]["name"], "Toggle Global/Local", "name")
    ed.check(not any(item["chords"] == ["T"] and "Global" in item["name"] for item in shortcuts.values()),
             "no command pairs T with Global/Local")
    ed.check_eq(shortcuts["edit.redo"]["default_chords"], ["Ctrl+Y", "Ctrl+Shift+Z"], "Redo's defaults")
    ed.check_eq(shortcuts["sceneView.orbit"]["read_only"], True, "gestures are reference rows")
    ed.check_eq(shortcuts["file.exit"].get("agent_safe"), False, "Exit is not for agents")
    # Help > Keyboard Shortcuts is the generated reference (CommandCatalog.MarkdownReferenceMatchesDocs)
    reference = (REPO / "docs/editor/keyboard-shortcuts.md").read_text()
    for text in ("| Hand Tool | Q |", "| Rect Tool | T |", "| Toggle Global/Local | X |"):
        ed.check_in(text, reference, "docs/editor/keyboard-shortcuts.md")
    ed.check(not any("| T |" in line and "Global" in line for line in reference.splitlines()), "no T + Global/Local line")
    names = {tool.name for tool in await ed.tools("all")}
    for tool in ("search_editor", "run_command", "list_shortcuts", "set_shortcut", "edit_layout_presets",
                 "edit_asset_labels", "edit_saved_searches", "edit_bookmarks"):
        ed.check_in(tool, names, "the editor_ux group")


async def test_set_shortcut_updates_menu(ed: Editor):
    bound = await ed.call("set_shortcut", id="tools.rect", chords=["Y"])
    ed.check_eq(bound["chords"], ["Y"], "set_shortcut answers with the binding")
    ed.check_eq(by_id(await ed.call("list_shortcuts", group="Tools"))["tools.rect"]["chords"], ["Y"], "list_shortcuts shows Y")

    # a command with a main-menu item: its shortcut text follows
    before = by_id(await ed.call("list_shortcuts", group="Window"))["window.console"]
    ed.check("Console" in (before.get("menu_path") or "") and (before.get("menu_path") or "").startswith("Window > "),
             f"the Console's menu item: {before.get('menu_path')}")
    ed.check_eq(before.get("menu_shortcut"), "Ctrl+Shift+C", "its shortcut text")
    await ed.call("set_shortcut", id="window.console", chords=["Ctrl+Alt+K"])
    await wait_frames(ed)
    after = by_id(await ed.call("list_shortcuts", group="Window"))["window.console"]
    ed.check_eq(after.get("menu_shortcut"), "Ctrl+Alt+K", "the menu model shows the new chord")

    # a conflict is refused without force, and names the holder
    refused = await ed.call("set_shortcut", id="tools.rect", chords=["Ctrl+D"], expect_error=True)
    ed.check_in("edit.duplicate", refused, "the conflict names Duplicate")
    ed.check_eq(by_id(await ed.call("list_shortcuts", group="Tools"))["tools.rect"]["chords"], ["Y"], "unchanged after the refusal")
    bad = await ed.call("set_shortcut", id="tools.rect", chords=["Hyper+Y"], expect_error=True)
    ed.check_in("not a chord", bad, "a bad chord")

    # null resets to the default
    reset = await ed.call("set_shortcut", id="tools.rect", chords=None)
    ed.check_eq(reset["chords"], ["T"], "reset")
    await ed.call("set_shortcut", id="window.console", chords=None)
    ed.check_eq(by_id(await ed.call("list_shortcuts", group="Window"))["window.console"]["chords"], ["Ctrl+Shift+C"], "reset")


async def test_palette_search(ed: Editor):
    inspector = await ed.call("search_editor", query="inspector")
    kinds = {r["kind"] for r in inspector["results"]}
    ed.check("panel" in kinds and "command" in kinds, f"a panel and a command: {inspector['results'][:6]}")

    commands = await ed.call("search_editor", query=">dup")
    ed.check(commands["results"], "commands match")
    ed.check(all(r["kind"] == "command" for r in commands["results"]), "> keeps commands")
    ed.check_eq(commands["results"][0]["label"], "Duplicate", "Duplicate first")
    ed.check_eq(commands["results"][0].get("shortcut"), "Ctrl+D", "with its shortcut")

    # no project-wide gravity setting exists (physics gravity is per scene): the Physics page's
    # collision matrix stands in for it (spec deviation)
    settings = await ed.call("search_editor", query=":collision")
    keys = [r["key"] for r in settings["results"]]
    ed.check("setting:physics.layer_collision_matrix" in keys, f"the physics collision row: {keys}")
    ed.check(all(r["kind"] == "setting" for r in settings["results"]), ": keeps settings")

    rigid = await ed.call("search_editor", query="+rigid")
    labels = [r["label"] for r in rigid["results"]]
    ed.check("Rigidbody" in labels, f"+rigid finds Rigidbody: {labels}")

    ran = await ed.call("run_command", id="window.console")
    ed.check_eq(ran["id"], "window.console", "ran")
    focused = None
    for _ in range(5):
        focused = (await ed.call("editor_status"))["windows"]["focused"]
        if focused == "Console":
            break
    ed.check_eq(focused, "Console", "the Console has the focus")

    refused = await ed.call("run_command", id="file.exit", expect_error=True)
    ed.check_in("not run for agents", refused, "Exit is refused")
    missing = await ed.call("run_command", id="no.such.command", expect_error=True)
    ed.check_in("list_shortcuts", missing, "an unknown id names the tool that lists them")

    # a palette key runs as the palette does
    panel = next(r for r in inspector["results"] if r["kind"] == "panel")
    await ed.call("run_command", key=panel["key"])
    status = await wait_frames(ed, 3)
    ed.check_eq(status["windows"]["open"].get("Inspector"), True, "the Inspector is open")


# window.* catalog commands the palette's window list leaves out, and why (BF23 F21)
PALETTE_WINDOW_EXCLUSIONS = {
    # toggles a preference, opens no window
    "window.detachableWindows",
    # a toggle: a row that "opens and focuses" would close an open Recorder (in the palette's commands)
    "window.recorder",
    # actions on layouts, in the palette's commands
    "window.layouts.apply", "window.layouts.save", "window.layouts.rename", "window.layouts.delete",
    "window.layouts.saveToFile", "window.layouts.loadFromFile", "window.layouts.reset",
    # version control's windows: only with a repository, in the palette's commands
    "window.vcs.commit", "window.vcs.history", "window.vcs.sceneDiff", "window.vcs.conflicts",
}


async def test_palette_lists_every_window(ed: Editor):
    """The palette's list is kept by hand (SearchPalette.cpp): every window.* command has a row"""
    commands = {i: item for i, item in by_id(await ed.call("list_shortcuts")).items() if i.startswith("window.")}
    ed.check("window.animator" in commands and "window.console" in commands, f"the catalog's windows: {sorted(commands)}")
    for excluded in PALETTE_WINDOW_EXCLUSIONS:
        ed.check_in(excluded, commands, "an exclusion names a catalog command")
    listed = await ed.call("search_editor", query="/", scopes=["panel"], limit=100)
    ed.check(listed["total"] <= 100, f"the limit holds them all: {listed['total']}")
    keys = {r["key"] for r in listed["results"]}
    missing = sorted(i for i, item in commands.items() if i not in PALETTE_WINDOW_EXCLUSIONS and "panel:" + item["name"] not in keys)
    ed.check_eq(missing, [], f"window.* commands the palette lacks (it lists {sorted(keys)})")
    ed.check("panel:Recorder" not in keys, "the Recorder's command is a toggle, not a row that opens and focuses")


async def test_layout_presets_record_the_audio_mixer(ed: Editor):
    """BF23 F20: the Audio Mixer's visibility is in a preset like any other panel's"""
    await ed.call("run_command", id="window.audioMixer")
    ed.check_eq((await wait_frames(ed, 3))["windows"]["open"].get("Audio Mixer"), True, "the Audio Mixer is open")
    saved = await ed.call("edit_layout_presets", action="save", name="Mixer")
    ed.check_in("Audio Mixer", Path(saved["files_written"][0]).read_text(), "the preset's panels")
    await ed.call("edit_layout_presets", action="apply", name="Default")
    ed.check_eq((await wait_frames(ed, 3))["windows"]["open"].get("Audio Mixer"), False, "the Default layout closes it")
    await ed.call("edit_layout_presets", action="apply", name="Mixer")
    ed.check_eq((await wait_frames(ed, 3))["windows"]["open"].get("Audio Mixer"), True, "the preset restores it")
    await ed.call("edit_layout_presets", action="apply", name="Default")
    await ed.call("edit_layout_presets", action="delete", name="Mixer")


async def test_layout_presets(ed: Editor):
    saved = await ed.call("edit_layout_presets", action="save", name="A")
    ed.check(saved["files_written"], f"the preset file: {saved}")
    preset_file = Path(saved["files_written"][0])
    ed.check(preset_file.exists() and preset_file.name.endswith(".layout.yaml"), f"{preset_file}")
    names = [p["name"] for p in saved["presets"]]
    ed.check("Default" in names and "A" in names, f"listed: {names}")
    at_save = (await wait_frames(ed))["windows"]["open"]

    # change a panel, go to Default, come back to A
    await ed.call("focus_window", window="terrain")
    changed = (await wait_frames(ed))["windows"]["open"]
    ed.check(changed != at_save or changed.get("Terrain") != at_save.get("Terrain"), "the Terrain panel changed")
    await ed.call("edit_layout_presets", action="apply", name="Default")
    applied = await ed.call("edit_layout_presets", action="apply", name="A")
    ed.check_eq(applied["active"], "A", "A is active")
    restored = (await wait_frames(ed))["windows"]["open"]
    ed.check_eq(restored, at_save, "the panels as saved")
    working = Path(applied["working_layout"])
    ed.check(working.exists(), f"the working layout file exists: {working}")
    ed.check(".jasperengine/user" in working.as_posix(), f"in the project's user folder: {working}")

    # D1 spike: applying again and again leaks no dock nodes
    nodes = applied["dock_nodes"]
    for i in range(50):
        last = await ed.call("edit_layout_presets", action="apply", name="A" if i % 2 else "Default")
    last = await ed.call("edit_layout_presets", action="apply", name="A")
    ed.check(last["dock_nodes"] <= nodes + 2, f"dock nodes after 51 applies: {last['dock_nodes']} (was {nodes})")

    # rename, export, import makes a unique name
    await ed.call("edit_layout_presets", action="rename", name="A", new_name="Wide")
    export = ed.scratch.root / "wide.layout.yaml"
    await ed.call("edit_layout_presets", action="export", name="Wide", path=str(export))
    ed.check(export.exists(), "exported")
    imported = await ed.call("edit_layout_presets", action="import", path=str(export))
    names = [p["name"] for p in imported["presets"]]
    ed.check("Wide (2)" in names, f"importing again: a unique name: {names}")
    await ed.call("edit_layout_presets", action="delete", name="Wide (2)")
    refused = await ed.call("edit_layout_presets", action="delete", name="Default", expect_error=True)
    ed.check(refused, "Default cannot be deleted")

    # a corrupt preset falls back to Default with a warning
    listed = await ed.call("edit_layout_presets", action="list")
    wide = next(p for p in listed["presets"] if p["name"] == "Wide")
    ed.check(wide, "Wide listed")
    preset_dir = preset_file.parent
    wide_file = next(f for f in preset_dir.glob("*.layout.yaml") if "name: Wide" in f.read_text() and "(2)" not in f.read_text())
    await ed.console_since(None, limit=1)
    wide_file.write_text("layoutVersion: 1\nname: Wide\nini: [not, a, string\n")
    fallback = await ed.call("edit_layout_presets", action="apply", name="Wide")
    ed.check_eq(fallback["active"], "Default", "the Default layout is used")
    warnings = [e["message"] for e in await ed.console_since() if "Layout 'Wide'" in e["message"]]
    ed.check(warnings and "Default layout is used" in warnings[0], f"the warning: {warnings}")
    await ed.call("edit_layout_presets", action="reset")


async def label(ed: Editor, action: str, **args) -> dict:
    return await ed.call("edit_asset_labels", action=action, **args)


async def test_project_search(ed: Editor):
    await add_hero_textures(ed)
    await label(ed, "add", assets=HERO, labels=["Hero"])
    found = await ed.call("list_assets", search="t:texture l:Hero")
    ed.check_eq(sorted(e["path"] for e in found["entries"]), sorted(HERO), "exactly the two labelled textures")
    ed.check_eq(found["entries"][0]["labels"], ["Hero"], "with their labels")
    either = await ed.call("list_assets", search="l:Hero OR ext:ogg")
    ed.check(len(either["entries"]) >= 2, "OR groups")
    bad = await ed.call("list_assets", search="re:([", expect_error=True)
    ed.check_in("regular expression", bad, "a bad re: says why")
    palette = await ed.call("search_editor", query="asset: l:hero t:texture")
    ed.check_eq(sorted(r["detail"] for r in palette["results"]), sorted(HERO), "the palette's asset: scope uses the grammar")


async def test_asset_labels(ed: Editor):
    root = ed.project
    listed = {l["name"]: l for l in (await label(ed, "list"))["labels"]}
    ed.check("Hero" in listed and listed["Hero"]["assets"] == 2, f"Hero on two assets: {listed}")
    meta = (root / (HERO[0] + ".meta")).read_text()
    ed.check_in("labels:", meta, "the meta sidecar has the labels")
    ed.check_in("Hero", meta, "the meta sidecar has the labels")

    renamed = await label(ed, "rename_label", label="hero", new_name="Protagonist")
    written = renamed["files_written"]
    for path in HERO:
        ed.check_in(path + ".meta", written, "files_written lists every rewritten meta")
    ed.check_eq((await ed.call("list_assets", search="l:Hero"))["entries"], [], "the old name is gone")
    ed.check_eq(len((await ed.call("list_assets", search="l:protagonist"))["entries"]), 2, "the new name")

    undone = await ed.call("undo", scope="project")
    ed.check(undone, "undo")
    ed.check_eq(len((await ed.call("list_assets", search="l:Hero"))["entries"]), 2, "undo restores the old name")

    colored = await label(ed, "set_color", label="Hero", color="#336699")
    ed.check(colored["files_written"], "project.yaml holds the colour")
    ed.check_in("#336699", (root / "project.yaml").read_text(), "the colour in project.yaml")
    refused = await label(ed, "add", assets=[HERO[0]], labels=["bad/label"], expect_error=True)
    ed.check(refused, "a bad label is refused")
    got = await label(ed, "get", assets=[HERO[0]])
    ed.check_eq(got["assets"][0]["labels"], ["Hero"], "get")


async def test_saved_searches(ed: Editor):
    root = ed.project
    saved = await ed.call("edit_saved_searches", action="save", name="Heroes", query="t:texture l:Hero")
    ed.check(any(s["name"] == "Heroes" for s in saved["searches"]), "saved")
    ran = await ed.call("edit_saved_searches", action="run", name="Heroes")
    direct = await ed.call("list_assets", search="t:texture l:Hero")
    ed.check_eq(sorted(e["path"] for e in ran["entries"]), sorted(e["path"] for e in direct["entries"]), "run == list_assets search")
    user_file = root / ".jasperengine/user/organisation.yaml"
    ed.check(user_file.exists() and "Heroes" in user_file.read_text(), "a user search lands in organisation.yaml")

    await ed.call("edit_saved_searches", action="save", name="Shared Heroes", query="l:Hero", scope="project")
    ed.check_in("Shared Heroes", (root / "project.yaml").read_text(), "a project search lands in project.yaml")
    listed = await ed.call("edit_saved_searches", action="list")
    names = [s["name"] for s in listed["searches"]]
    for name in ("Heroes", "Shared Heroes", "All Materials", "All Prefabs"):
        ed.check_in(name, names, "listed with the built-ins")
    builtin = await ed.call("edit_saved_searches", action="delete", name="All Materials", expect_error=True)
    ed.check(builtin, "a built-in cannot be deleted")
    await ed.call("edit_saved_searches", action="delete", name="Heroes")
    await ed.call("undo", scope="project")
    names = [s["name"] for s in (await ed.call("edit_saved_searches", action="list"))["searches"]]
    ed.check_in("Heroes", names, "undo brings the deleted search back")


async def test_bookmarks(ed: Editor):
    first = await ed.call("edit_bookmarks", action="add", ref=HERO[0])
    second = await ed.call("edit_bookmarks", action="add", ref="assets/sprites")
    ed.check_eq([b["path"] for b in second["bookmarks"]], [HERO[0], "assets/sprites"], "in the order added")
    ed.check_eq(first["files_written"], [".jasperengine/user/organisation.yaml"], "organisation.yaml")
    moved = await ed.call("edit_bookmarks", action="move", index=1, to=0)
    ed.check_eq([b["path"] for b in moved["bookmarks"]], ["assets/sprites", HERO[0]], "moved")

    # an asset moved by S04 keeps its bookmark
    await ed.call("move_asset", path=HERO[0], to="assets/moved_rock.png")
    listed = await ed.call("edit_bookmarks", action="list")
    ed.check_eq(listed["bookmarks"][1]["path"], "assets/moved_rock.png", "the bookmark follows the GUID")
    ed.check_eq(listed["bookmarks"][1]["missing"], False, "not missing")
    await ed.call("undo", scope="project")   # the move

    removed = await ed.call("edit_bookmarks", action="remove", index=0)
    ed.check_eq(len(removed["bookmarks"]), 1, "removed")
    await ed.call("undo", scope="project")
    back = await ed.call("edit_bookmarks", action="list")
    ed.check_eq([b["path"] for b in back["bookmarks"]], ["assets/sprites", HERO[0]], "undo re-adds it in its place")
    went = await ed.call("edit_bookmarks", action="navigate", index=1)
    ed.check(went, "navigate")


async def test_brush_contexts_list_their_keys(ed: Editor):
    """T1 R57, T6 R60: an armed brush's keys are listed, and rebindable, by their own context."""
    terrain = by_id(await ed.call("list_shortcuts", context="terrain_brush"))
    table5 = {"terrain.tool.raise": ["R"], "terrain.tool.setHeight": ["H"], "terrain.tool.smooth": ["S"], "terrain.tool.ramp": ["A"],
              "terrain.tool.noise": ["N"], "terrain.tool.terrace": ["T"], "terrain.tool.erode": ["E"], "terrain.tool.stamp": ["K"],
              "terrain.tool.paintTexture": ["B"], "terrain.tool.paintDetails": ["D"], "terrain.tool.placeObjects": ["P"],
              "terrain.tool.cutHoles": ["X"], "terrain.pick": ["I"], "terrain.invertSlopeFilter": ["Y"], "terrain.alternate": ["C"],
              "terrain.overlay.height": ["1"], "terrain.overlay.slope": ["2"], "terrain.overlay.contours": ["3"],
              "terrain.overlay.layerWeights": ["4"], "terrain.overlay.off": ["0"], "terrain.disarm": ["Escape"]}
    for command, chords in table5.items():
        ed.check(command in terrain, f"{command} is listed under terrain_brush: {sorted(terrain)}")
        ed.check_eq(terrain[command]["chords"], chords, command)
        ed.check_eq(terrain[command]["context"], "terrain_brush", command)
    ed.check(all(item["context"] == "terrain_brush" for item in terrain.values()), "only the brush's own commands")
    voxel = by_id(await ed.call("list_shortcuts", context="voxel_brush"))
    ed.check_eq(voxel["voxel.tool.dig"]["chords"], ["D"], "the voxel brush's keys by their context too")
    await ed.call("set_shortcut", id="terrain.alternate", chords=["G"])
    ed.check_eq(by_id(await ed.call("list_shortcuts", context="terrain_brush"))["terrain.alternate"]["chords"], ["G"], "rebound")
    await ed.call("set_shortcut", id="terrain.alternate", chords=None)
