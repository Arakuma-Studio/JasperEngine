"""Every modal dialog can be seen and answered by an agent (S69): editor_status.dialog reports the
top-most modal's title, message, choices and fields; respond_dialog fills its fields (values) and
presses its buttons; edits are refused meanwhile; file pickers wait for values.path in a headless
editor. Ported from the sound layer's test of the same name and extended.

Before S69 MCP calls, which run before the frame draws its windows, never saw a modal: dialog is
null, respond_dialog says none is open, and edits went through under it. The red run is in docs/specs/S69-editor-modal.md.

SITES below is the table of every EditorModal::begin in the editor: how the test drives it, or why it
does not. test_sites_table_is_complete reads the sources, so a new modal must be added here.
"""

from __future__ import annotations

import asyncio
import functools
import re
import shutil
from pathlib import Path

import ui_input
from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Modals", "xvfb": "harness"}
TIMEOUT = 1500

REPO = Path(__file__).resolve().parents[3]

# (file relative to editor/, title): "test name that drives it", or "not driven: why"
SITES = {
    ("App.cpp", "alert: popupName"): "test_alert_keeps_its_kind",
    ("App.cpp", "alert: draw"): "test_alert_keeps_its_kind",
    ("App.cpp", "Creating Project##hub"): "not driven: the Project Hub's progress (Role::Progress), never reported; the hub needs a project-less editor",
    ("App.cpp", "Save Project as Template##saveTemplate"): "test_save_template_fields",
    ("shortcuts/ShortcutsWindow.cpp", "CONFLICT_POPUP"): "not driven: opens on a key captured in the Shortcuts window's row; set_shortcut reports the same conflict to agents",
    ("widget/ColorPickerWindow.cpp", "popup"): "not driven: New/Rename Library live in the color picker's popup, reached only by mouse from a color field",
    ("widget/ColorPickerWindow.cpp", "Rename Swatch"): "not driven: as the library prompts, from the picker's swatch context menu",
    ("layout/LayoutManager.cpp", "popup"): "test_layout_name_prompt",
    ("layout/LayoutManager.cpp", "exportPopup"): "test_layout_save_to_file_opens_a_picker",
    ("window/GameWindow.cpp", "Add Game View Resolution"): "test_add_game_resolution",
    ("window/HumanoidConvertDialog.cpp", "Convert to Humanoid Clip###HumanoidConvert"): "not driven: needs a clip of a skinned model; convert_animation_clip does the same for agents",
    ("window/ReduceKeysDialog.cpp", "Reduce Keys###ReduceKeys"): "not driven: test_animation_quality_ui.py drives it (a clip with a rig); optimize_animation does the same for agents",
    ("window/dialog/ShaderForkDialog.cpp", "popupName"): "test_shader_fork",
    ("window/dialog/ExportWindowShipping.cpp", "Keystore Passwords##ExportKeystore"): "not driven: needs a signing keystore and the Android SDK",
    ("window/dialog/ExportWindowShipping.cpp", "Upload to Steam##PublishUpload"): "not driven: needs a release folder and SteamCMD",
    ("window/dialog/ExportWindow.cpp", "Build Settings##ExportModal"): "test_em1_build_settings, test_export_completes",
    ("window/dialog/ExportWindow.cpp", "Directory Not Empty##ExportOverwrite"): "test_export_asks_before_overwriting",
    ("window/dialog/ExportWindow.cpp", "Add Shader##AddShaderModal"): "test_add_shader_nested",
    ("window/AudioMixerWindow.cpp", "Expose Parameter##AudioMixer"): "not driven: opens from a parameter's context menu in the Mixer window; edit_audio_mixer exposes for agents",
    ("window/AudioMixerWindow.cpp", "Delete Group##AudioMixer"): "not driven: from a group's context menu in the Mixer window; edit_audio_mixer removes for agents",
    ("window/ProjectWindowSound.cpp", "Placeholder Sound"): "not driven: Assets > Create > Placeholder Sound... (S36) draws its own widgets; generate_sound makes the same clips for agents",
    ("window/ImporterFields.cpp", "Unapplied Import Settings##importer_prompt"): "not driven: the Model Import inspector asks it on leaving unapplied settings; set_model_import_settings applies them for agents",
    ("window/ProjectWindow.cpp", "Delete Confirmation"): "test_em2_delete_confirmation_no, test_em3_delete_confirmation_yes",
    ("window/ProjectWindow.cpp", "New Folder / Lua Script / C++ ... / HTML Document###ProjectNewItem"): "test_create_folder_prompt",
    ("window/ProjectWindow.cpp", "Rename File"): "test_rename_prompt",
    ("window/ProjectWindow.cpp", "File Already Exists"): "test_rename_prompt",
    ("window/ProjectWindow.cpp", "Rename Error"): "not driven: only a .vat pair's rename refusal or a failing file system opens it (the .vat rename is in test_vat_inspector)",
    ("window/ProjectWindow.cpp", "Invalid Name (file)"): "test_rename_prompt",
    ("window/ProjectWindow.cpp", "POPUP"): "not driven: Create Prefab or Variant? opens on dragging an instance into the Project window; create_prefab decides for agents",
    ("window/dialog/ProjectSettingsWindow.cpp", "Change Asset Directories"): "not driven: opens from Project Settings' OK with another Assets or Lua Directory; set_project_settings (dry_run) gives agents the same plan (test_asset_roots)",
    ("window/dialog/RagdollWizardDialog.cpp", "popupName"): "not driven: needs a skeleton; create_ragdoll does the same for agents",
    ("window/HierarchyPrefabRows.cpp", "POPUP (replace)"): "not driven: from a prefab instance's context menu; replace_prefab does the same for agents",
    ("window/HierarchyPrefabRows.cpp", "POPUP (restructure)"): "not driven: opens on dragging a child of an instance in the Hierarchy; move_entity refuses with the same message",
    ("window/InspectorWindowVat.cpp", "popup"): "not driven: Create Crowd needs a baked .vat (test_vat_inspector clicks it)",
    ("window/dialog/NativePluginDialog.cpp", "title"): "test_native_plugin_completes",
    ("window/AssetPanels.cpp", "Choose Asset##missing_assets_picker"): "not driven: opens from a row of the Missing Assets panel; fix_missing_asset does the same for agents",
    ("window/UndoHistoryWindow.cpp", "Clear Undo History"): "test_clear_undo_history",
    ("window/dialog/PrefabsWindow.cpp", "Prefabs##PrefabsModal"): "test_about_scenes_prefabs_preferences",
    ("window/PoseAnimationWindow.cpp", "Map Bones###PoseMapBones"): "not driven: needs a skinned model (test_pose_animation_window opens it)",
    ("window/CodeEditor.cpp", "Files Changed###FilesChanged"): "not driven: needs an unsaved buffer and an external change at once; code_editor resolves it for agents",
    ("htmlui/HtmlUiImportPanel.cpp", "Remove unused imports##htmlui"): "not driven: its button shows on a document with imports (network fixture); the HTML UI tools remove them for agents",
    ("window/LoadingWindow.cpp", "Loading"): "not driven: Role::Progress, never reported (EditorModalTest); that it no longer closes a dialog: test_save_template_fields, EditorModalLoadingTest",
    ("window/AvatarWindow.cpp", "Unsaved Avatar###AvatarClose"): "not driven: needs a humanoid model with edits in the Avatar window",
    ("window/PoseAnimationWindowTemplates.cpp", "New Pose Animation from Template###PoseTemplateCreate"): "not driven: from a card of the Pose window's template panel, which needs a bound rig",
    ("window/PoseAnimationWindowTemplates.cpp", "Delete Template###PoseTemplateDelete"): "not driven: as above",
    ("window/PoseAnimationWindowTemplates.cpp", "Save as Template###PoseTemplateSave"): "not driven: as above",
    ("window/dialog/ScriptCreateDialog.cpp", "popupName"): "test_script_create",
    ("window/dialog/ScriptCreateDialog.cpp", "Script Files Already Exist"): "test_script_create",
    ("window/ProjectWindowSearch.cpp", "savePopup"): "not driven: the search box's Save Search button; organise_saved_searches does the same for agents",
    ("window/ProjectWindowSearch.cpp", "renamePopup"): "not driven: as above",
    ("window/ProjectWindowSearch.cpp", "editPopup"): "not driven: as above",
    ("window/ProjectWindowSearch.cpp", "labelPopup"): "not driven: a selection's context menu; edit_asset_labels does the same for agents",
    ("window/dialog/ScenesWindow.cpp", "Scenes in Build##ScenesModal"): "test_about_scenes_prefabs_preferences",
    ("window/dialog/SceneSaveDialog.cpp", "Save Scene##SaveSceneModal"): "test_save_scene_dialog_completes",
    ("window/dialog/ProjectSaveDialog.cpp", "Save Project##SaveProjectModal"): "test_save_scene_dialog_completes",
    ("window/dialog/EditorSettingsWindow.cpp", "Preferences##EditorSettingsModal"): "test_about_scenes_prefabs_preferences",
    ("window/dialog/AboutWindow.cpp", "About JasperEngine##AboutModal"): "test_about_scenes_prefabs_preferences",
    ("window/dialog/DataSafetyDialogs.cpp", "title"): "not driven: Recover, Saving Would Lose Data and Project Already Open need a crash or a second editor; open_project's allow_data_loss / recovery answer them",
    ("window/vcs/VcsUi.cpp", "popupId"): "not driven: needs a repository with a conflict; the vcs tools answer for agents",
    ("window/dialog/TextureSlicerToolDialog.cpp", "popupTitle"): "test_texture_slicer",
    ("window/dialog/ProjectSettingsWindow.cpp", "Project Settings##ProjectSettingsModal"): "test_project_settings_merges_edits",
    ("window/dialog/ProjectSettingsWindow.cpp", "Privacy Manifest Preview"): "not driven: a read-only preview from Player > iOS/macOS > Privacy Manifest > Preview; native_plugins privacy_manifest gives agents the same manifest",
    ("window/TitleDesignerProperties.cpp", "Expose as Parameter###exposedialog"): "not driven: a Title Designer row's context menu; edit_title's expose op does the same for agents (TitleDesignerWindowDeathTest.ExposeDialogIsAnEditorModal answers it)",
    ("window/TitleDesignerLibrary.cpp", "Replace the whole title?###replacetitle"): "not driven: a Library template applied to a document with content needs S84's provider; edit_title replaces a title for agents",
    ("window/AsepriteImporterView.cpp", "Extract Clips##aseprite_extract"): "test_aseprite_extract_clips",
    ("pixelart/aseprite/AsepriteEditorHooks.cpp", "Aseprite Not Found##aseprite_open"): "test_aseprite_not_found",
    ("window/dialog/SpriteConversionDialog.cpp", "title"): "test_sprite_conversion_dialog",
    ("window/PixelArtCheckWindow.cpp", "kPreviewName"): "not driven here: it needs findings; test_pixel_art_check_window.py's test_fix_all_preview_counts_the_plan opens and answers it",
    ("window/dialog/PixelArtPresetDialog.cpp", "POPUP_NAME"): "not driven here: a button of Project Settings > Pixel Art opens it; test_pixel_art_check_window.py's test_preset_dialog answers it",
    ("window/dialog/CreateTerrainDialog.cpp", "Create Terrain##CreateTerrainModal"): "not driven: from the Hierarchy's create menu and the Terrain window's empty state; create_entity type terrain, then edit_terrain create, does the same for agents (TerrainGenerateSeamTest drives its presets)",
    ("terrainworld/TerrainWorldPanel.cpp", "Create Terrain World##CreateTerrainWorldModal"): "not driven: from the Hierarchy's create menu and the Terrain World panel; edit_terrain_world create does the same for agents (test_terrain_world)",
    ("terrainworld/TerrainWorldPanel.cpp", "Convert to Terrain World##ConvertTerrainWorldModal"): "not driven: from a Terrain's Inspector; edit_terrain_world create.from_terrain does the same for agents (test_terrain_world)",
    ("terrainworld/TerrainWorldPanel.cpp", "Import Heightmap##TerrainWorldImportModal"): "not driven: from a Terrain World's Inspector; edit_terrain_world op import_heightmap does the same for agents",
    ("window/TerrainSettingsPanel.cpp", "Resize Terrain Map"): "not driven: from a map row's Resize... in the Terrain window's Terrain Settings tool, which needs a selected Terrain with maps",
    ("window/TerrainSettingsPanel.cpp", "Import Raw Heightmap"): "not driven: as above, Import Raw...; edit_terrain op import_heightmap does the same for agents",
    ("window/TerrainSettingsPanel.cpp", "Export Raw Heightmap"): "not driven: as above, Export Raw...; export_terrain_heightmap does the same for agents",
    ("window/TerrainFoliagePanel.cpp", "Remove Tree Prototype"): "not driven: from a tree prototype's remove button in the Terrain window's Paint Trees tool, when trees use it; set_properties treePrototypes and edit_terrain clear_foliage do the same for agents",
    ("window/TerrainFoliagePanel.cpp", "Mass Place Trees"): "not driven: from the Paint Trees tool, which needs tree prototypes; edit_terrain scatter_by_rules does the same for agents (test_foliage)",
    ("window/VoxelCreateDialog.cpp", "Create Voxel Volume"): "not driven: from the Hierarchy's create menu; edit_voxels create does the same for agents",
}
# InspectorWindow's error boxes share one shape: they open when a file picker's answer is refused
INSPECTOR_BOXES = 11


async def dialog(ed: Editor):
    return (await ed.call("editor_status")).get("dialog")


async def wait_dialog(ed: Editor, title: str | None = None, seconds: float = 20.0):
    for _ in range(int(seconds / 0.25)):
        d = await dialog(ed)
        if d and (title is None or d.get("title") == title):
            return d
        await asyncio.sleep(0.25)
    return None


async def frame_dialog(ed: Editor, title: str | None = None):
    await ui_input.frames(ed)
    d = await dialog(ed)
    return d if d and (title is None or d.get("title") == title) else None


async def open_modal(ed: Editor, command: str, title: str):
    await ed.call("run_command", id=command)
    d = await frame_dialog(ed, title)
    ed.check(d, f"{command} opens {title}: {await dialog(ed)}")
    return d


async def close_all(ed: Editor):
    """leaves no dialog behind, whatever a failed check left open"""
    for _ in range(6):
        d = await dialog(ed)
        if not d:
            return
        for choice in ("cancel", "no", "ok"):
            if choice in d.get("choices", []):
                await ed.call_raw("respond_dialog", {"choice": choice})
                break
        await ed.call_raw("find_ui_item", {"label": "##frame pacer"})


def clean(test):
    """the tests share an editor: each starts with no dialog open, whatever the last one left"""
    @functools.wraps(test)
    async def run(ed: Editor):
        await close_all(ed)
        await test(ed)
    return run


def fields(d: dict) -> dict:
    return {f["name"]: f for f in d.get("fields", [])}


def ui(ed: Editor) -> ui_input.Ui:
    return ui_input.Ui(ed)


def root_entries(ed: Editor) -> list[str]:
    return sorted(p.name for p in ed.project.iterdir() if not p.name.startswith("."))


async def test_sites_table_is_complete(ed: Editor):
    """AC1: every EditorModal::begin in the editor is in SITES; AC3: and no raw modal exists"""
    sources = list((REPO / "editor").rglob("*.cpp"))
    found: dict[str, int] = {}
    for path in sources:
        rel = path.relative_to(REPO / "editor").as_posix()
        if rel in ("window/EditorModal.cpp",) or rel.startswith("tests/"):
            continue
        count = len(re.findall(r"EditorModal::begin\(", path.read_text(encoding="utf-8", errors="replace")))
        if count:
            found[rel] = count
    listed: dict[str, int] = {}
    for (rel, _title) in SITES:
        listed[rel] = listed.get(rel, 0) + 1
    listed["window/InspectorWindow.cpp"] = INSPECTOR_BOXES
    ed.check_eq(dict(sorted(found.items())), dict(sorted(listed.items())), "every modal site is in the table, with its count")
    # the spec's 71 sites, S36's Placeholder Sound and the Model Import prompt (73); the project
    # window's folder prompt and its three nested boxes became one name dialog (errors inline), and
    # Change Asset Directories came: 71; mobile-shipping's Privacy Manifest Preview: 72; S44's Extract
    # Clips and Aseprite Not Found: 74; S45's Convert Inline Sprite Frames: 75; S77's Reduce Keys: 76;
    # S42's Fix All and Apply Pixel Art Preset: 78; T1's Create Terrain and its three Terrain Settings
    # dialogs, T4's three Terrain World dialogs, T5's Remove Tree Prototype and Mass Place Trees, T6's
    # Create Voxel Volume: 88; S82's Expose as Parameter and Replace the whole title: 90
    ed.check_eq(sum(found.values()), 90, "the 90 sites")
    for site, how in SITES.items():
        if not how.startswith("not driven"):
            for name in how.split(", "):
                ed.check(name in globals(), f"{site}: {name} exists")
    import subprocess
    lint = subprocess.run(["python3", "-I", str(REPO / "tools/check-editor-modals.py"), "--root", str(REPO)], capture_output=True, text=True)
    ed.check_eq(lint.returncode, 0, f"the lint: {lint.stdout}{lint.stderr}")


@clean
async def test_em1_build_settings(ed: Editor):
    """EM-1: Build Settings is reported (kind modal, cancel closes it), refuses edits while open, and
    respond_dialog cancel closes it"""
    ed.require_tools("editor_status", "respond_dialog", "create_entity")
    d = await open_modal(ed, "file.buildSettings", "Build Settings")
    ed.check_eq(d.get("kind"), "modal", "an ImGui modal")
    ed.check_in("cancel", d.get("choices"), "a modal window is closed with cancel")
    r = await ed.call_raw("create_entity", {"name": "Refused", "type": "cube"})
    ed.check(r.is_error and "Build Settings" in r.text, f"an edit is refused while it is open: {r.text[:300]}")
    r = await ed.call_raw("respond_dialog", {"choice": "yes"})
    ed.check(r.is_error and "cancel" in r.text, f"a choice it does not have is refused, naming the valid one: {r.text[:300]}")
    r = await ed.call_raw("respond_dialog", {"choice": "cancel"})
    ed.check(not r.is_error, f"respond_dialog cancel: {r.text[:300]}")
    ed.check_eq(await dialog(ed), None, "Build Settings closed")
    made = await ed.call("create_entity", name="Allowed", type="cube")
    ed.check(made, "edits work again")


@clean
async def test_about_scenes_prefabs_preferences(ed: Editor):
    """AC1: the plain windows: each reported with its message and answered through its own button"""
    for command, title, choice in (("help.about", "About JasperEngine", "ok"),
                                   ("assets.scenesInBuild", "Scenes in Build", "ok"),
                                   ("assets.prefabs", "Prefabs", "cancel"),
                                   ("edit.preferences", "Preferences", "cancel")):
        d = await open_modal(ed, command, title)
        ed.check(d.get("message"), f"{title} says what it is: {d}")
        ed.check_in(choice, d.get("choices"), title)
        r = await ed.call("respond_dialog", choice=choice)
        ed.check_eq(r["answered"], {"title": title, "choice": choice}, title)
        ed.check("dialog" not in r, f"{title} closed: {r}")
        ed.check(await dialog(ed) is None, f"{title} closed")


@clean
async def test_focus_window_drives_preferences_under_itself(ed: Editor):
    """focus_window's page and close act on Preferences, so its refusal of edits does not stop them; any
    other panel is refused meanwhile"""
    await ed.call("focus_window", window="preferences", page="Logging")
    d = await frame_dialog(ed, "Preferences")
    ed.check(d, "open")
    r = await ed.call_raw("focus_window", {"window": "preferences", "page": "General"})
    ed.check(not r.is_error, f"another page: {r.text[:300]}")
    r = await ed.call_raw("focus_window", {"window": "console"})
    ed.check(r.is_error and "Preferences" in r.text, f"another panel is refused: {r.text[:300]}")
    r = await ed.call_raw("focus_window", {"window": "preferences", "close": True})
    ed.check(not r.is_error, f"close: {r.text[:300]}")
    await ed.call("find_ui_item", label="##frame pacer")
    ed.check_eq(await dialog(ed), None, "closed")


@clean
async def test_project_settings_merges_edits(ed: Editor):
    """R4: Project Settings keeps concurrent edits: reported, answerable, and it refuses nothing"""
    d = await open_modal(ed, "edit.projectSettings", "Project Settings")
    ed.check_eq(d.get("blocks_edits"), False, "says it does not block")
    made = await ed.call("create_entity", name="UnderSettings", type="cube")
    ed.check(made, "an edit runs under it")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await dialog(ed), None, "closed")


@clean
async def test_error_box_is_dismissed_by_the_next_edit(ed: Editor):
    """R4: a modal whose only choice is ok is dismissed by the next edit, like an info alert"""
    helper = ui(ed)
    await open_rename(ed, helper)
    # no name: Invalid Name on top
    await ed.call_raw("respond_dialog", {"choice": "ok", "values": {"name": ""}})
    box = await frame_dialog(ed, "Invalid Name")
    ed.check_eq(box.get("choices"), ["ok"], "an error box")
    made = await ed.call("create_entity", name="AfterError", type="cube")
    got = await ed.call("get_entity", entity="AfterError", detail="summary")
    ed.check_eq(got["id"], made["entity"]["id"], "the next edit ran")
    await ui_input.frames(ed)
    ed.check_eq((await dialog(ed))["title"], "Rename File", "the edit dismissed only the error box")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await dialog(ed), None, "closed")
    await ed.call("delete_entities", entity=made["entity"]["id"])
    (ed.project / "RenameMe.lua").unlink()
    (ed.project / "Taken.lua").unlink()


@clean
async def test_create_folder_prompt(ed: Editor):
    """R3: a name prompt whose refusal stays in it (EditorModal::error): the reply says why, the prompt
    stays open, and the name then completes it"""
    await ed.call("write_file", path="scripts/Keep.lua", content="return {}\n")
    await ed.call("focus_window", window="project")
    d = await open_modal(ed, "assets.createFolder", "New Folder")
    ed.check_eq(fields(d)["name"]["type"], "text", f"its name field: {d}")
    ed.check_in("yes", d["choices"], "Create")

    # a folder that exists: the refusal comes back, the prompt stays
    existing = next(p.name for p in ed.project.iterdir() if p.is_dir() and not p.name.startswith("."))
    r = await ed.call_raw("respond_dialog", {"choice": "yes", "values": {"name": existing}})
    ed.check(r.is_error and "already exists" in r.text, f"the refusal comes back: {r.text[:300]}")
    ed.check_eq((await dialog(ed))["title"], "New Folder", "the prompt stays open")

    # the empty name
    r = await ed.call_raw("respond_dialog", {"choice": "yes", "values": {"name": ""}})
    ed.check(r.is_error and "Enter a name" in r.text, f"{r.text[:300]}")

    # unknown field, wrong kind of value: refused before anything applies
    r = await ed.call_raw("respond_dialog", {"values": {"nope": "x"}})
    ed.check(r.is_error and "name" in r.text, f"names the fields: {r.text[:300]}")
    r = await ed.call_raw("respond_dialog", {"values": {"name": 3}})
    ed.check(r.is_error and "string" in r.text, f"{r.text[:300]}")

    # values alone fill the field and leave it open
    r = await ed.call("respond_dialog", values={"name": "made_by_agent"})
    ed.check_eq(fields(r["dialog"])["name"]["value"], "made_by_agent", f"{r}")
    r = await ed.call("respond_dialog", choice="yes")
    ed.check("dialog" not in r, f"completed: {r}")
    found = list(ed.project.rglob("made_by_agent"))
    ed.check(found and found[0].is_dir(), "the folder exists")


async def select_entry(ed: Editor, helper: ui_input.Ui, name: str):
    for _ in range(40):
        status = await ed.call("editor_status")
        entries = status["ui"]["project_window"]["entries"]
        rect = next((e["rect"] for e in entries if e["name"] == name), None)
        if rect:
            await helper.click(helper.centre(rect))
            return
        await helper.frames(4)
    ed.fail(f"{name} is not in the Project window: {entries}")


async def open_rename(ed: Editor, helper: ui_input.Ui) -> dict:
    """the Project window shows the project's root: two files there, the first selected, F2"""
    (ed.project / "RenameMe.lua").write_text("return {}\n")
    (ed.project / "Taken.lua").write_text("return {}\n")
    await ed.call("focus_window", window="project")
    await select_entry(ed, helper, "RenameMe.lua")
    return await open_modal(ed, "project.rename", "Rename File")


@clean
async def test_rename_prompt(ed: Editor):
    """R3: the rename prompt through to the renamed file; a taken name comes back in the reply"""
    helper = ui(ed)
    d = await open_rename(ed, helper)
    ed.check_eq(fields(d)["name"]["value"], "RenameMe.lua", f"its current value: {d}")
    ed.check_in("cancel", d["choices"], "Cancel")

    r = await ed.call_raw("respond_dialog", {"choice": "ok", "values": {"name": "Taken.lua"}})
    ed.check(r.is_error and "already exists" in r.text, f"the refusal: {r.text[:300]}")
    await ed.call("respond_dialog", choice="ok")
    ed.check_eq((await dialog(ed))["title"], "Rename File", "back in the prompt")

    r = await ed.call_raw("respond_dialog", {"choice": "ok", "values": {"name": ""}})
    ed.check(r.is_error and "valid file name" in r.text, f"{r.text[:300]}")
    await ed.call("respond_dialog", choice="ok")

    r = await ed.call("respond_dialog", choice="ok", values={"name": "Renamed.lua"})
    ed.check("dialog" not in r, f"completed: {r}")
    ed.check((ed.project / "Renamed.lua").exists() and not (ed.project / "RenameMe.lua").exists(), "the file was renamed")
    (ed.project / "Renamed.lua").unlink()
    (ed.project / "Taken.lua").unlink()


async def open_delete(ed: Editor, helper: ui_input.Ui):
    """the Project window (at the project's root): Delete asks first"""
    (ed.project / "modal_files").mkdir(exist_ok=True)
    (ed.project / "modal_files" / "One.lua").write_text("return {}\n")
    await ed.call("focus_window", window="project")
    await select_entry(ed, helper, "modal_files")
    return await open_modal(ed, "project.delete", "Delete Confirmation")


@clean
async def test_em2_delete_confirmation_no(ed: Editor):
    """EM-2: the Project window's Delete Confirmation: choices yes/no, its message lists the selection,
    edits refused; no closes it and deletes nothing"""
    helper = ui(ed)
    d = await open_delete(ed, helper)
    before = root_entries(ed)
    ed.check_eq(d.get("choices"), ["yes", "no"], "Yes and No")
    ed.check_in("modal_files", d.get("message", ""), "the message names the selection")
    r = await ed.call_raw("write_file", {"path": "scripts/Refused.lua", "content": "return {}\n"})
    ed.check(r.is_error and "Delete Confirmation" in r.text, f"write_file refused while it is open: {r.text[:300]}")
    await ed.call("respond_dialog", choice="no")
    ed.check_eq(await dialog(ed), None, "the confirmation closed")
    ed.check_eq(root_entries(ed), before, "nothing deleted")
    ed.check(not (ed.project / "scripts/Refused.lua").exists(), "the refused write did not happen")


@clean
async def test_em3_delete_confirmation_yes(ed: Editor):
    """EM-3: yes presses the Yes button's own code: the selection is deleted (one project undo step,
    which brings it back)"""
    helper = ui(ed)
    d = await open_delete(ed, helper)
    before = root_entries(ed)
    ed.check_in("modal_files", d.get("message", ""), "the selection")
    r = await ed.call("respond_dialog", choice="yes")
    ed.check(r, "answered")
    after = root_entries(ed)
    ed.check("modal_files" not in after, f"the Yes button deleted the selection: {before} -> {after}")
    await ed.call("undo", scope="project")
    await ed.call("wait", frames=5)
    ed.check((ed.project / "modal_files/One.lua").exists(), "one project undo brings it back, with its files")
    shutil.rmtree(ed.project / "modal_files", ignore_errors=True)


@clean
async def test_export_completes(ed: Editor):
    """R3: the export dialog completed by an agent: its platform, build type and folder by values,
    Build by choice, then its progress and Close. A build it cannot do comes back as the refusal."""
    await ed.call("create_entity", name="Exported", type="cube")
    await ed.call("save_project", untitled_scene_dir="scenes")
    target = ed.scratch.root / "exported"
    target.mkdir(exist_ok=True)
    d = await open_modal(ed, "file.buildSettings", "Build Settings")
    ed.check_in("platform", fields(d), f"the platform field: {d}")
    r = await ed.call("respond_dialog", values={"platform": "Linux", "build_type": "C++ Source Project", "output_directory": str(target)})
    d = r["dialog"]
    ed.check_eq(fields(d)["platform"]["value"], "Linux", "platform set")
    ed.check_eq(fields(d)["build_type"]["value"], "C++ Source Project", "build type set")
    ed.check_eq(fields(d)["output_directory"]["value"], str(target), "folder set")
    r = await ed.call("respond_dialog", choice="ok")
    # the same window shows the export's progress, and its Close when done
    message = ""
    for _ in range(600):
        d = await dialog(ed)
        message = (d or {}).get("message", "")
        if d and ("done" in message or "failed" in message):
            break
        await asyncio.sleep(0.5)
    ed.check("done" in message, f"the export finished: {d}")
    await ed.call("respond_dialog", choice="ok")
    ed.check_eq(await dialog(ed), None, "Close closed it")
    ed.check(any(target.iterdir()), "the project was exported")


@clean
async def test_export_asks_before_overwriting(ed: Editor):
    """Nested: Build on a full folder asks Directory Not Empty; cancel goes back to Build Settings"""
    target = ed.scratch.root / "occupied"
    target.mkdir(exist_ok=True)
    (target / "file.txt").write_text("x")
    await ed.call("save_project", untitled_scene_dir="scenes")
    d = await open_modal(ed, "file.buildSettings", "Build Settings")
    await ed.call("respond_dialog", values={"platform": "Linux", "build_type": "C++ Source Project", "output_directory": str(target)})
    r = await ed.call("respond_dialog", choice="ok")
    ed.check_eq(r["dialog"]["title"], "Directory Not Empty", f"it asks: {r}")
    ed.check_in("cancel", r["dialog"]["choices"], "Cancel")
    ed.check_in("occupied", r["dialog"]["message"], "names the folder")
    r = await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(r["dialog"]["title"], "Build Settings", "back under it")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await dialog(ed), None, "closed")


@clean
async def test_add_shader_nested(ed: Editor):
    """Add Shader opens inside Build Settings: the top-most is reported, its type a field"""
    helper = ui(ed)
    await open_modal(ed, "file.buildSettings", "Build Settings")
    await ed.call("respond_dialog", values={"platform": "Custom Graphics APIs"})
    # under the Shaders foldout, closed or open as the last visit left it
    if await helper.find("+ Add", "Build Settings", frames=4) is None:
        await helper.click_revealed("Shaders", "Build Settings")
    add = await helper.reveal("+ Add", "Build Settings")
    await helper.click(helper.centre(add["rect"]))
    d = await frame_dialog(ed, "Add Shader")
    ed.check(d, "Add Shader is reported")
    ed.check_in("type", fields(d), f"its type: {d}")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq((await dialog(ed))["title"], "Build Settings", "back under it")
    await ed.call("respond_dialog", values={"platform": "Linux"})
    await ed.call("respond_dialog", choice="cancel")


@clean
async def test_save_scene_dialog_completes(ed: Editor):
    """R3, R5: Save Scene keeps its kind and now completes: name and folder by values, ok saves"""
    made = await ed.call("create_scene", name="ToSave", type="3D")
    await ed.call("select_scene", scene=made["scene"]["id"])
    await ed.call("create_entity", name="Dirty", type="empty")
    (ed.project / "scenes").mkdir(exist_ok=True)
    await ed.call("run_command", id="file.save")
    d = await wait_dialog(ed, "Save Scene")
    ed.check(d, "Ctrl+S on an untitled scene asks")
    ed.check_eq(d["kind"], "save_dialog", "its kind")
    ed.check_in("name", fields(d), f"{d}")
    ed.check_in("folder", fields(d), f"{d}")
    r = await ed.call("respond_dialog", choice="ok", values={"name": "agent_saved", "folder": "scenes"})
    ed.check("dialog" not in r, f"completed: {r}")
    # the save is asynchronous, then a project in the temporary folder asks where it goes: reported the same way
    d = await wait_dialog(ed, "Save Project")
    ed.check(d, "Save All goes on to Save Project")
    ed.check_eq(d["kind"], "save_dialog", "its kind")
    ed.check_in("location", fields(d), f"{d}")
    ed.check_in("ok", d.get("disabled_choices", []), f"no location yet: {d}")
    ed.check_in("location", d.get("error", ""), f"says why: {d}")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await dialog(ed), None, "cancelled")
    ed.check((ed.project / "scenes/agent_saved.scene").exists(), "the scene was saved there")


async def test_save_template_fields(ed: Editor):
    """A dialog that says why its button is greyed out: dialog.error, and the refusal in the reply. A
    template is made from a saved project, and one under the system's temporary folder is not: this editor's
    temporary folder is elsewhere."""
    other_temp = ed.scratch.root / "other-temp"
    other_temp.mkdir(exist_ok=True)
    async with Editor(project=REPO / "tools/fixtures/stencil", xvfb="editor", env={"TMPDIR": str(other_temp)}) as saved:
        # right after the project opens, while its shaders build: asked once, as a person does
        await saved.call("run_command", id="file.saveProjectAsTemplate")
        d = await frame_dialog(saved, "Save Project as Template")
        ed.check(d, f"it opens: {await dialog(saved)}")
        ed.check_in("name", fields(d), f"{d}")
        ed.check_in("description", fields(d), f"{d}")
        r = await saved.call_raw("respond_dialog", {"choice": "ok", "values": {"name": "!!!"}})
        ed.check(r.is_error and "letters or digits" in r.text, f"the refusal: {r.text[:300]}")
        d = await dialog(saved)
        ed.check_in("ok", d.get("disabled_choices", []), f"greyed out: {d}")
        ed.check_in("letters or digits", d.get("error", ""), f"why: {d}")
        await saved.call("respond_dialog", choice="cancel")
        ed.check_eq(await dialog(saved), None, "closed")


@clean
async def test_layout_name_prompt(ed: Editor):
    """Save Layout: a prompt whose validity greys its button: an invalid name is refused in the reply"""
    d = await open_modal(ed, "window.layouts.save", "Save Layout")
    ed.check_in("name", fields(d), f"{d}")
    r = await ed.call_raw("respond_dialog", {"choice": "ok", "values": {"name": ""}})
    ed.check(r.is_error, f"the empty name: {r.text[:300]}")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await dialog(ed), None, "closed")


@clean
async def test_layout_save_to_file_opens_a_picker(ed: Editor):
    """R5: a modal's button that opens a file picker: the choice's reply comes at once (the picker holds
    the main thread), reports the picker, and values.path answers it"""
    d = await open_modal(ed, "window.layouts.saveToFile", "Save Layout to File")
    ed.check_in("layout", fields(d), f"{d}")
    r = await ed.call("respond_dialog", choice="ok")
    ed.check_eq(r["dialog"]["kind"], "file_picker", f"the picker: {r}")
    target = ed.scratch.root / "my.layout.yaml"
    r = await ed.call("respond_dialog", values={"path": str(target)})
    ed.check("dialog" not in r, f"answered: {r}")
    ed.check(target.exists(), "the layout was saved where the agent said")


@clean
async def test_file_picker_from_a_command(ed: Editor):
    """R5, R7: assets.import opens a picker: the tool replies right away with what to answer; meanwhile
    every call is told, a path that is not there comes back as the refusal, and values.path completes it"""
    source = ed.scratch.root / "imported_by_agent.lua"
    source.write_text("return {}\n")
    r = await ed.call("run_command", id="assets.import")
    ed.check_eq(r["dialog"]["kind"], "file_picker", f"the tool says so: {r}")
    d = await dialog(ed)
    ed.check_eq(d["kind"], "file_picker", f"editor_status reports it: {d}")
    ed.check_in("paths", d["message"], "this one takes several")
    ed.check_eq(d["choices"], ["ok", "cancel"], "ok, cancel")
    ed.check_in("path", fields(d), "the path field")

    r = await ed.call_raw("create_entity", {"name": "Blocked", "type": "cube"})
    ed.check(r.is_error and "respond_dialog" in r.text, f"an edit is told: {r.text[:300]}")
    r = await ed.call_raw("get_scene_tree", {})
    ed.check(r.is_error and "respond_dialog" in r.text, f"so is a read: nothing runs while it waits: {r.text[:300]}")

    r = await ed.call_raw("respond_dialog", {"values": {"path": str(ed.scratch.root / "missing.lua")}})
    ed.check(r.is_error and "does not exist" in r.text, f"the refusal: {r.text[:300]}")
    r = await ed.call_raw("respond_dialog", {"choice": "yes"})
    ed.check(r.is_error and "ok cancel" in r.text, f"{r.text[:300]}")
    r = await ed.call_raw("respond_dialog", {"values": {"nope": "x"}})
    ed.check(r.is_error and "values.path" in r.text, f"{r.text[:300]}")

    r = await ed.call("respond_dialog", values={"path": str(source)})
    ed.check_eq(r["answered"]["title"], "Open Files", f"{r}")
    await ed.call("find_ui_item", label="##frame pacer")
    ed.check(list(ed.project.rglob("imported_by_agent.lua")), "the Project window imported it")

    # cancel: nothing imported, nothing left open
    r = await ed.call("run_command", id="assets.import")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await dialog(ed), None, "cancelled")


@clean
async def test_file_picker_from_a_click(ed: Editor):
    """R5: a real click on a Browse button: pickers opened by the window's own code report the same"""
    helper = ui(ed)
    await open_modal(ed, "file.buildSettings", "Build Settings")
    await ed.call("respond_dialog", values={"platform": "Linux"})
    browse = await helper.find("Browse##target", "Build Settings")
    ed.check(browse, "its Browse button")
    x, y = helper.centre(browse["rect"])
    helper.xdo("mousemove", str(x), str(y))
    await helper.frames(4)
    # not helper.click: its frame pacer is refused once the picker holds the editor
    helper.xdo("mousedown", "1")
    await helper.frames(2)
    helper.xdo("mouseup", "1")
    d = None
    for _ in range(80):
        r = await ed.call_raw("editor_status", {})
        d = (r.structured or {}).get("dialog")
        if d and d["kind"] == "file_picker":
            break
        await asyncio.sleep(0.25)
    ed.check(d and d["kind"] == "file_picker", f"the picker: {d}")
    ed.check_eq(d["title"], "Choose Folder", "a folder")
    folder = ed.scratch.root / "picked"
    folder.mkdir(exist_ok=True)
    await ed.call("respond_dialog", values={"path": str(folder)})
    d = await frame_dialog(ed, "Build Settings")
    ed.check_eq(fields(d)["output_directory"]["value"], str(folder), "the Browse button took the answer")
    await ed.call("respond_dialog", choice="cancel")


@clean
async def test_native_plugin_completes(ed: Editor):
    """R3: checkboxes, enums and a list field through to the files it writes"""
    d = await open_modal(ed, "assets.createNativePlugin", "Create Native Plugin")
    for name in ("name", "android", "android_language", "methods", "replace_existing"):
        ed.check_in(name, fields(d), f"{d}")
    ed.check_eq(fields(d)["android"]["type"], "bool", "android")
    ed.check_eq(fields(d)["android_language"]["options"], ["Java", "Kotlin"], "its languages")
    r = await ed.call_raw("respond_dialog", {"choice": "ok", "values": {"name": ""}})
    ed.check(r.is_error, f"no name: refused: {r.text[:300]}")
    r = await ed.call("respond_dialog", choice="ok", values={"name": "AgentPlugin", "android": False, "ios": False, "macos": False, "web": True, "cpp": False, "methods": "ping,work:async"})
    ed.check("dialog" not in r, f"completed: {r}")
    ed.check(list(ed.project.rglob("*AgentPlugin*")), "the plugin's files exist")


@clean
async def test_add_game_resolution(ed: Editor):
    """Fields of three kinds: Add Game View Resolution, from the Game view's toolbar"""
    helper = ui(ed)
    await ed.call("focus_window", window="game")
    await helper.frames(4)
    combo = await helper.find("##GameAspect", "Game")
    ed.check(combo, "the Game view's resolution menu")
    await helper.click(helper.centre(combo["rect"]))
    await helper.click_item("+ Add custom...")
    d = await frame_dialog(ed, "Add Game View Resolution")
    ed.check(d, "the dialog")
    ed.check_eq(fields(d)["width"]["type"], "int", f"{d}")
    ed.check_eq(fields(d)["dpi"]["type"], "float", f"{d}")
    r = await ed.call_raw("respond_dialog", {"choice": "ok", "values": {"name": "Agent Res", "width": 640, "height": 360}})
    ed.check(not r.is_error, f"{r.text[:300]}")
    ed.check_eq(await dialog(ed), None, "added")


@clean
async def test_clear_undo_history(ed: Editor):
    """A question from a window's own button: Undo History's Clear"""
    helper = ui(ed)
    await ed.call("create_entity", name="ToClear", type="cube")
    await ed.call("focus_window", window="undo_history")
    clear = await helper.find("Clear History...", "Undo History")
    ed.check(clear, "the Clear button")
    await helper.click(helper.centre(clear["rect"]))
    d = await frame_dialog(ed, "Clear Undo History")
    ed.check(d, "the question")
    ed.check_eq(d["choices"], ["yes", "cancel"], f"{d}")
    ed.check_in("[yes = Clear, cancel = Cancel]", d["message"], "says what yes presses")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await dialog(ed), None, "closed")


async def open_new_script(helper: ui_input.Ui):
    """Add Component's list ends with New Script"""
    add = await helper.reveal("Add Component", "Inspector")
    await helper.click(helper.centre(add["rect"]))
    # its rows draw their label themselves, so they are found by id: the top level's last row, scrolled to
    for _ in range(30):
        rows = (await helper.ed.call("find_ui_item", label="##row", window="##Popup")).get("items", [])
        if rows:
            row = await helper.reveal("##row", "##Popup", index=len(rows) - 1)
            await helper.click(helper.centre(row["rect"]))
            return
        await helper.frames(2)
    helper.ed.fail("Add Component's list did not open")


@clean
async def test_script_create(ed: Editor):
    """AC1: Add Script (the New Script dialog): the name field, then the nested question when its files exist"""
    helper = ui(ed)
    (ed.project / "scripts").mkdir(exist_ok=True)
    made = await ed.call("create_entity", name="Scripted", type="cube")
    await ed.call("select_entities", entities=[made["entity"]["id"]])
    await ed.call("focus_window", window="inspector")
    await helper.frames(4)
    await open_new_script(helper)
    d = await frame_dialog(ed, "New Script")
    ed.check(d, "the dialog")
    ed.check_eq(fields(d)["source"]["options"], ["New Script", "Existing Script"], f"{d}")
    ed.check_in("Lua Script", fields(d)["template"]["options"], f"{d}")
    r = await ed.call("respond_dialog", values={"template": "Lua Script", "name": "AgentScript"})
    ed.check_eq(fields(r["dialog"])["name"]["value"], "AgentScript", f"{r}")
    r = await ed.call("respond_dialog", choice="yes")
    ed.check("dialog" not in r, f"completed: {r}")
    ed.check(list(ed.project.rglob("AgentScript.lua")), "the script exists")

    # again: its files exist, which is asked
    await helper.frames(4)
    await open_new_script(helper)
    await frame_dialog(ed, "New Script")
    await ed.call("respond_dialog", values={"template": "Lua Script", "name": "AgentScript"})
    r = await ed.call("respond_dialog", choice="yes")
    ed.check_eq(r["dialog"]["title"], "Script Files Already Exist", f"{r}")
    await ed.call("respond_dialog", choice="cancel")
    await close_all(ed)


@clean
async def test_texture_slicer(ed: Editor):
    """AC1: the Slicer Tool from a Sprite's Inspector: grid fields by values, then cancel"""
    helper = ui(ed)
    (ed.project / "assets").mkdir(exist_ok=True)
    import struct
    import zlib

    def png(width: int, height: int) -> bytes:
        raw = b"".join(b"\x00" + b"\xff\x00\x00\xff" * width for _ in range(height))
        def chunk(tag: bytes, data: bytes) -> bytes:
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
        return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")

    (ed.project / "sheet.png").write_bytes(png(64, 64))
    made = await ed.call("create_entity", name="Slicing", type="sprite")
    entity = made["entity"]["id"]
    await ed.call("select_entities", entities=[entity])
    await ed.call("focus_window", window="inspector")
    await helper.frames(4)
    button = await helper.reveal("\uf58d Slicer Tool", "Inspector")
    await helper.click(helper.centre(button["rect"]))
    d = await frame_dialog(ed)
    ed.check(d and "Slice" in d["title"], f"the dialog: {d}")
    ed.check_in("columns", fields(d), f"{d}")
    r = await ed.call("respond_dialog", values={"columns": 4, "rows": 2})
    ed.check_eq(fields(r["dialog"])["columns"]["value"], 4, f"{r}")
    ed.check_eq(fields(r["dialog"])["rows"]["value"], 2, f"{r}")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await dialog(ed), None, "closed")

    # a texture's stored Sprite Mode: the Slicer opens on it and Slice keeps its pivot (S45 R12)
    (ed.project / "grid.png").write_bytes(png(64, 32))
    (ed.project / "grid.png.meta").write_text("guid: 5a1ce5a1ce5a1ce5a1ce5a1ce5a1ce5a\nimport:\n  spriteSheet: {version: 1, mode: grid, grid: {cell: [32, 32], offset: [0, 0], "
                                             "spacing: [0, 0], count: 0, skipEmpty: false, prefix: g_}, pivot: bottom_center}\n")
    await ed.call("sync_files", paths=["grid.png", "grid.png.meta"])
    made = await ed.call("set_sprite_frames", create={"name": "GridSprite"}, sheet="grid.png", frame=0)
    await ed.call("select_entities", entities=[made["sprite"]["id"]])
    await ed.call("focus_window", window="inspector")
    await helper.frames(4)
    button = await helper.reveal("\uf58d Slicer Tool", "Inspector")
    await helper.click(helper.centre(button["rect"]))
    d = await frame_dialog(ed)
    ed.check(d and "pivot" in fields(d), f"the Sprite Mode fields: {d}")
    ed.check_eq(fields(d)["pivot"]["value"], "Bottom Center", f"the stored pivot is shown: {fields(d)['pivot']}")
    await ed.call("respond_dialog", choice="yes")
    await helper.frames(4)
    meta = (ed.project / "grid.png.meta").read_text()
    ed.check("bottom_center" in meta, f"Slice kept the pivot: {meta}")


@clean
async def test_shader_fork(ed: Editor):
    """AC1: Fork a built-in shader from the Inspector: its name field, a refusal, a cancel"""
    helper = ui(ed)
    made = await ed.call("create_entity", name="Forking", type="cube")
    await ed.call("select_entities", entities=[made["entity"]["id"]])
    await ed.call("focus_window", window="inspector")
    await helper.frames(4)
    fork = await helper.reveal("##fork_custom_shader", "Inspector")
    await helper.click(helper.centre(fork["rect"]))
    d = await frame_dialog(ed)
    ed.check(d and "Fork" in d["title"], f"the dialog: {d}")
    ed.check_in("name", fields(d), f"{d}")
    ed.check_in("fork_includes", fields(d), f"{d}")
    r = await ed.call_raw("respond_dialog", {"choice": "yes", "values": {"name": ""}})
    ed.check(r.is_error, f"no name: refused: {r.text[:300]}")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await dialog(ed), None, "closed")


@clean
async def test_alert_keeps_its_kind(ed: Editor):
    """The alert still reports first, as itself, and takes no values"""
    await ed.call("run_command", id="assets.prefabs")
    await frame_dialog(ed, "Prefabs")
    await ed.call("respond_dialog", choice="cancel")
    # Remove Scene asks before discarding this case's own unsaved scene.
    made = await ed.call("create_scene", name="ModalAlert", type="3D")
    await ed.call("create_entity", scene=made["scene"]["id"], name="AlertDirty", type="empty")
    r = await ed.call_raw("run_command", {"id": "file.removeScene"})
    d = await dialog(ed)
    ed.check(d and d.get("kind") == "alert", f"Remove Scene asks about the unsaved scene: {r.text[:200]} {d}")
    ed.check_in(d["choices"][0], ("ok", "yes"), f"{d}")
    bad = await ed.call_raw("respond_dialog", {"choice": d["choices"][0], "values": {"name": "x"}})
    ed.check(bad.is_error and "no fields" in bad.text, f"{bad.text[:300]}")
    await ed.call("respond_dialog", choice="no" if "no" in d["choices"] else d["choices"][0])
    await close_all(ed)


@clean
async def test_headless_modal_is_answerable_after_idling(ed: Editor):
    """R7: an editor with its own Xvfb, idle for a while: a modal is drawn, reported and answered without
    any input to wake it"""
    async with Editor(create=True, name="ModalsHeadless") as other:
        await asyncio.sleep(3.0)
        await other.call("focus_window", window="preferences")
        status = await other.call("editor_status")
        ed.check(status.get("dialog"), f"reported right after it opened: {status.get('dialog')}")
        ed.check_eq(status["dialog"]["title"], "Preferences", "the modal")
        await asyncio.sleep(3.0)
        r = await other.call("respond_dialog", choice="cancel")
        ed.check("dialog" not in r, f"answered after idling: {r}")
        ed.check((await other.call("editor_status")).get("dialog") is None, "closed")


async def imported_aseprite(ed: Editor, name: str) -> None:
    """S44's character (two tag clips) at the project's root, its import done"""
    shutil.copy(REPO / "editor" / "tests" / "fixtures" / "aseprite" / "s44-mcp" / "hero_idle.aseprite", ed.project / name)
    await ed.call("sync_files", paths=[name])
    state = None
    for _ in range(200):
        info = await ed.call("get_asset_info", path=name)
        state = info.get("aseprite", info).get("import_state")
        if state in ("imported", "failed"):
            break
        await ed.call("wait", seconds=0.1)
    ed.check_eq(state, "imported", f"{name} imports")


async def aseprite_inspector_button(ed: Editor, helper: ui_input.Ui, name: str, label: str) -> None:
    """the Project window's `name` selected, so the Inspector (in front) shows the Aseprite Importer:
    `label` clicked"""
    # (the tests before may have left GameObjects selected, which the Inspector would show instead)
    await ed.call("select_entities", entities=[])
    # its sections' headers: a click the Project window takes before it has laid out its entries
    # selects nothing, so again
    anchor = None
    for _ in range(4):
        await ed.call("focus_window", window="project")
        await select_entry(ed, helper, name)
        await ed.call("focus_window", window="inspector")
        for header in ("Texture", "Layers", "General"):
            anchor = await helper.find(header, "Inspector", frames=20)
            if anchor:
                break
        if anchor:
            break
    ed.check(anchor, f"the Inspector shows the Aseprite Importer for {name}")
    # the buttons end the importer, below the fold of a small Inspector: ImGui draws (and
    # find_ui_item finds) only what is in view, so it scrolls down until they are
    button = None
    for _ in range(40):
        button = await helper.find(label, "Inspector", frames=2)
        if button:
            break
        await helper.wheel(helper.centre(anchor["window_rect"]), 3)
    ed.check(button, f"{label} comes into view in the Inspector")
    await helper.click(helper.centre(button["rect"]))


@clean
async def test_aseprite_extract_clips(ed: Editor):
    """S44: the Aseprite Importer's Extract Clips... lists the .anim files it writes; cancel writes
    nothing, ok writes one per tag clip"""
    helper = ui(ed)
    await imported_aseprite(ed, "HeroClips.aseprite")
    clips = [ed.project / "HeroClips walk.anim", ed.project / "HeroClips Idle.anim"]
    await aseprite_inspector_button(ed, helper, "HeroClips.aseprite", "Extract Clips...")
    d = await wait_dialog(ed, "Extract Clips")
    ed.check(d, f"Extract Clips is reported: {await dialog(ed)}")
    ed.check(set(d.get("choices", [])) >= {"ok", "cancel"}, f"Extract (ok) and Cancel: {d}")
    ed.check("HeroClips walk.anim" in d.get("message", "") and "HeroClips Idle.anim" in d["message"], f"says what it writes: {d}")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await frame_dialog(ed), None, "cancel closes it")
    ed.check(not any(c.exists() for c in clips), "and writes nothing")

    await aseprite_inspector_button(ed, helper, "HeroClips.aseprite", "Extract Clips...")
    ed.check(await wait_dialog(ed, "Extract Clips"), "reopened")
    r = await ed.call("respond_dialog", choice="ok")
    ed.check("dialog" not in r, f"completed: {r}")
    for _ in range(40):
        if all(c.exists() for c in clips):
            break
        await ed.call("wait", seconds=0.1)
    ed.check(all(c.exists() for c in clips), f"ok writes the tag clips: {root_entries(ed)}")


@clean
async def test_aseprite_not_found(ed: Editor):
    """S44: Open in Aseprite with no Aseprite installed asks to locate it; the reply says where to get
    it, cancel closes it"""
    helper = ui(ed)
    await imported_aseprite(ed, "HeroOpen.aseprite")
    await aseprite_inspector_button(ed, helper, "HeroOpen.aseprite", "Open in Aseprite")
    d = await wait_dialog(ed, "Aseprite Not Found")
    ed.check(d, f"Aseprite Not Found is reported: {await dialog(ed)}")
    ed.check(set(d.get("choices", [])) >= {"locate", "cancel"}, f"Locate... and Close (cancel): {d}")
    ed.check("aseprite.org" in d.get("message", ""), f"says where to get it: {d}")
    r = await ed.call_raw("create_entity", {"name": "Refused", "type": "empty"})
    ed.check(r.is_error and "Aseprite Not Found" in r.text, f"an edit is refused while it is open: {r.text[:300]}")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await frame_dialog(ed), None, "cancel closes it")


@clean
async def test_sprite_conversion_dialog(ed: Editor):
    """S45: Assets > Convert Inline Sprite Frames... lists the legacy sprites (a dry run) and is
    answered by agents: Convert All and Close (cancel), cancel closes it, nothing converted"""
    d = await open_modal(ed, "assets.convertInlineSpriteFrames", None)
    ed.check(d and "Convert Inline Sprite Frames" in d.get("title", ""), f"the dialog: {d}")
    ed.check({"convert_all", "cancel"} <= set(d.get("choices", [])), f"Convert All and Close: {d}")
    ed.check("legacy sprite" in d.get("message", ""), f"says what it lists: {d}")
    await ed.call("respond_dialog", choice="cancel")
    ed.check_eq(await frame_dialog(ed), None, "cancel closes it")
