// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DefaultCommands.h"

#include "AppMenuCommand.h"
#include "window/HierarchyWindow.h"

#include <cctype>

using namespace jasperengine::editor;

namespace {

    using C = ShortcutContext;

    Chord chord(const char* text) {
        if (!text || !*text) return Chord();
        // the table is authored here: a typo must fail the tests, not leave a key unbound
        return Chord::parse(text).value_or(Chord());
    }

    struct Builder {
        std::vector<CommandDef>& out;
        std::string group;

        CommandDef& add(const char* id, const char* name, C context, const char* primary = nullptr,
                        const char* secondary = nullptr) {
            CommandDef def;
            def.id = id;
            def.group = group;
            def.name = name;
            def.context = context;
            def.primary = chord(primary);
            def.secondary = chord(secondary);
            out.push_back(std::move(def));
            return out.back();
        }

        CommandDef& menu(const char* id, const char* name, AppMenuCommand command, C context = C::Editor,
                         const char* primary = nullptr, const char* secondary = nullptr) {
            CommandDef& def = add(id, name, context, primary, secondary);
            def.menuCommand = static_cast<uint32_t>(command);
            return def;
        }

        CommandDef& gesture(const char* id, const char* name, C context, const char* gestureText, const char* description) {
            CommandDef& def = add(id, name, context);
            def.readOnly = true;
            def.gesture = gestureText;
            def.description = description;
            return def;
        }
    };

    // "3D Object" -> "3dObject", "Directional Light" -> "directionalLight"
    std::string camel(const char* text) {
        std::string out;
        bool upperNext = false;
        for (const char* c = text; *c; ++c) {
            const unsigned char ch = static_cast<unsigned char>(*c);
            if (std::isalnum(ch) == 0) {
                upperNext = !out.empty();
                continue;
            }
            if (out.empty()) out.push_back(static_cast<char>(std::tolower(ch)));
            else out.push_back(static_cast<char>(upperNext ? std::toupper(ch) : ch));
            upperNext = false;
        }
        return out;
    }

}

std::string jasperengine::editor::createEntryCommandId(size_t entryIndex) {
    const auto& entries = HierarchyWindow::getCreateMenuEntries();
    if (entryIndex >= entries.size()) return {};
    const CreateMenuEntry& entry = entries[entryIndex];
    if (entry.command) return entry.command;
    std::string id = "gameobject.create.";
    if (entry.submenu) id += camel(entry.submenu) + ".";
    return id + camel(entry.label);
}

std::vector<CommandDef> jasperengine::editor::defaultCommands() {
    std::vector<CommandDef> out;
    Builder b{out, ""};

    b.group = "File";
    b.menu("file.newScene3D", "New Scene (3D)", AppMenuCommand::NewScene3D);
    b.menu("file.newScene2D", "New Scene (2D)", AppMenuCommand::NewScene2D);
    b.menu("file.newSceneUI", "New Scene (UI)", AppMenuCommand::NewSceneUI);
    b.menu("file.save", "Save", AppMenuCommand::Save, C::Global, "Ctrl+S").description = "Save the current scene or script";
    b.menu("file.saveAll", "Save All", AppMenuCommand::SaveAll, C::Global, "Ctrl+Shift+S").description =
        "Save every scene, script and the project";
    b.menu("file.removeScene", "Remove Scene from Project", AppMenuCommand::RemoveScene);
    b.menu("file.newProject", "New Project...", AppMenuCommand::NewProject).agentSafe = false;
    b.menu("file.projectHub", "Project Hub...", AppMenuCommand::ProjectHub).agentSafe = false;
    b.menu("file.saveProjectAsTemplate", "Save Project as Template...", AppMenuCommand::SaveProjectAsTemplate).description =
        "Save the project as a starter project of the Project Hub's New Project";
    b.menu("file.openProject", "Open Project...", AppMenuCommand::OpenProject, C::Global, "Ctrl+O").agentSafe = false;
    b.menu("file.clearRecentProjects", "Clear Recent Projects", AppMenuCommand::ClearRecentProjects).agentSafe = false;
    b.menu("file.saveProjectAs", "Save Project As...", AppMenuCommand::SaveProjectAs);
    b.menu("file.buildSettings", "Build Settings...", AppMenuCommand::ExportProject, C::Editor, "Ctrl+Shift+B");
    b.menu("file.exit", "Exit", AppMenuCommand::Exit).agentSafe = false;

    b.group = "Edit";
    CommandDef& undo = b.menu("edit.undo", "Undo", AppMenuCommand::Undo, C::Editor, "Ctrl+Z");
    undo.repeat = true;
    CommandDef& redo = b.menu("edit.redo", "Redo", AppMenuCommand::Redo, C::Editor, "Ctrl+Y", "Ctrl+Shift+Z");
    redo.macPrimary = chord("Ctrl+Shift+Z");
    redo.repeat = true;
    for (auto [id, name, command, key] : {
             std::tuple{"edit.selectAll", "Select All", AppMenuCommand::SelectAll, "Ctrl+A"},
             std::tuple{"edit.cut", "Cut", AppMenuCommand::CutSelection, "Ctrl+X"},
             std::tuple{"edit.copy", "Copy", AppMenuCommand::CopySelection, "Ctrl+C"},
             std::tuple{"edit.paste", "Paste", AppMenuCommand::PasteSelection, "Ctrl+V"},
             std::tuple{"edit.pasteAsChild", "Paste As Child", AppMenuCommand::PasteAsChild, "Ctrl+Shift+V"}}) {
        CommandDef& def = b.menu(id, name, command, C::Hierarchy, key);
        def.alsoIn = {C::SceneView};
    }
    b.menu("edit.duplicate", "Duplicate", AppMenuCommand::DuplicateSelection, C::Editor, "Ctrl+D");
    b.menu("edit.rename", "Rename", AppMenuCommand::RenameSelection, C::Editor, "F2");
    b.menu("edit.delete", "Delete", AppMenuCommand::DeleteSelection, C::Editor, "Delete", "Backspace").repeat = true;
    b.menu("edit.frameSelected", "Frame Selected", AppMenuCommand::FrameSelected, C::Editor, "F").repeat = true;
    b.menu("edit.searchAll", "Search All...", AppMenuCommand::SearchAll, C::Global, "Ctrl+K").description =
        "Find any command, window, setting, asset, GameObject or component";
    b.menu("edit.play", "Play", AppMenuCommand::TogglePlay, C::Global, "Ctrl+P").description = "Enter or leave Play mode";
    b.menu("edit.pause", "Pause", AppMenuCommand::TogglePause, C::Global, "Ctrl+Shift+P").description = "Pause or resume Play mode";
    b.menu("edit.step", "Step", AppMenuCommand::StepFrame, C::Global, "Ctrl+Alt+P").description = "Advance a paused game by one frame";
    b.menu("edit.stop", "Stop", AppMenuCommand::StopScene);
    b.menu("edit.projectSettings", "Project Settings...", AppMenuCommand::ProjectSettings);
    b.menu("edit.clearUserSettings", "Clear User Settings (PlayerPrefs)...", AppMenuCommand::ClearUserSettings).description =
        "Delete what the game saved with UserSettings / PlayerPrefs on this machine";
    b.menu("edit.preferences", "Preferences...", AppMenuCommand::EditorSettings);
    b.menu("edit.shortcuts", "Shortcuts...", AppMenuCommand::Shortcuts).description = "View and rebind the editor's keyboard shortcuts";

    b.group = "Play Mode";
    b.add("playmode.playOrResume", "Play or Resume", C::Global, "F5");
    b.add("playmode.pauseOnly", "Pause", C::Global, "F6");
    b.add("playmode.stop", "Stop", C::Global, "F7");
    // Escape in the Game view does it too (Unity), the key still reaching the game
    b.add("playmode.releaseCursor", "Release Game Cursor", C::Global).description =
        "Give a cursor the running game captured back to the editor; click the Game view to return it";
    CommandDef& maximize = b.menu("playmode.maximizeOnPlay", "Maximize On Play", AppMenuCommand::ToggleMaximizeOnPlay, C::Global, "F11");
    maximize.description = "Fill the editor with the Game view while playing (the Game view toolbar's toggle)";
    // macOS takes F11 for Show Desktop: Control+Cmd+F, the Mac's full-screen key, comes first there
    maximize.macPrimary = chord("Ctrl+Super+F");

    b.group = "Assets";
    b.menu("assets.createFolder", "Create Folder", AppMenuCommand::AssetsCreateFolder);
    b.menu("assets.createLuaScript", "Create Lua Script", AppMenuCommand::AssetsCreateLuaScript);
    b.menu("assets.createCppScript", "Create C++ Script", AppMenuCommand::AssetsCreateCppScript);
    b.menu("assets.createCppSource", "Create C++ Source File", AppMenuCommand::AssetsCreateCppSource);
    b.menu("assets.createCppHeader", "Create C++ Header File", AppMenuCommand::AssetsCreateCppHeader);
    b.menu("assets.createHtmlDocument", "Create HTML Document", AppMenuCommand::AssetsCreateHtmlDocument);
    b.menu("assets.createPoseAnimation", "Create Pose Animation", AppMenuCommand::AssetsCreatePoseAnimation);
    b.menu("assets.createAnimationClip", "Create Animation Clip", AppMenuCommand::AssetsCreateAnimationClip);
    b.menu("assets.createAnimatorController", "Create Animator Controller", AppMenuCommand::AssetsCreateAnimatorController);
    b.menu("assets.createAvatarMask", "Create Avatar Mask", AppMenuCommand::AssetsCreateAvatarMask);
    b.menu("assets.createAnimationMontage", "Create Animation Montage", AppMenuCommand::AssetsCreateAnimationMontage);
    b.menu("assets.createAudioMixer", "Create Audio Mixer", AppMenuCommand::AssetsCreateAudioMixer);
    b.menu("assets.createSoundEvent", "Create Sound Event", AppMenuCommand::AssetsCreateSoundEvent);
    b.menu("assets.createSoundEventFromClips", "Create Sound Event from Clips", AppMenuCommand::AssetsCreateSoundEventFromClips);
    b.menu("assets.createPlaceholderSound", "Create Placeholder Sound", AppMenuCommand::AssetsCreatePlaceholderSound);
    b.menu("assets.reanalyzeAudio", "Reanalyze Audio", AppMenuCommand::AssetsReanalyzeAudio).description =
        "Drop the cached measurements of the selected audio files: they are measured again";
    b.menu("assets.importSoundEssentials", "Import Sound Essentials", AppMenuCommand::AssetsImportSoundEssentials).description =
        "Import the 40 built-in CC0 clips into sounds/essentials (one undo step)";
    b.menu("assets.createShakePreset", "Create Shake Preset", AppMenuCommand::AssetsCreateShakePreset);
    b.menu("assets.createPalette", "Create Palette", AppMenuCommand::AssetsCreatePalette).description =
        "A new palette (black and white): edit it in Window > 2D > Palette";
    // S45
    b.menu("assets.createSpriteAtlas", "Create Sprite Atlas", AppMenuCommand::AssetsCreateSpriteAtlas);
    b.menu("assets.createSpriteAtlasFromSelection", "Create Sprite Atlas from Selection", AppMenuCommand::AssetsCreateSpriteAtlasFromSelection);
    b.menu("assets.convertInlineSpriteFrames", "Convert Inline Sprite Frames...", AppMenuCommand::AssetsConvertInlineSpriteFrames);
    b.menu("assets.createPhysicsMaterial", "Create Physics Material", AppMenuCommand::AssetsCreatePhysicsMaterial);
    b.menu("assets.createPhysicsMaterial2D", "Create Physics Material 2D", AppMenuCommand::AssetsCreatePhysicsMaterial2D);
    b.menu("assets.createNativePlugin", "Create Native Plugin...", AppMenuCommand::AssetsCreateNativePlugin);
    b.menu("assets.showInFileManager", "Show in File Manager", AppMenuCommand::AssetsShowInFileManager);
    b.menu("assets.open", "Open", AppMenuCommand::AssetsOpen);
    b.menu("assets.rename", "Rename Asset", AppMenuCommand::AssetsRename);
    b.menu("assets.duplicate", "Duplicate Asset", AppMenuCommand::AssetsDuplicate).description =
        "Copy the selected assets beside themselves (Unity names: hero 1.png)";
    b.menu("assets.delete", "Delete Asset", AppMenuCommand::AssetsDelete);
    b.menu("assets.import", "Import New Asset...", AppMenuCommand::AssetsImport);
    b.menu("assets.refresh", "Refresh", AppMenuCommand::AssetsRefresh, C::Editor, "Ctrl+R");
    b.menu("assets.reimportTextures", "Reimport All Textures", AppMenuCommand::AssetsReimportTextures).description =
        "Cook every texture of the project again for the editor (the cache answers what did not change)";
    b.menu("assets.migrateReferences", "Migrate References", AppMenuCommand::MigrateAssetReferences);
    b.menu("assets.scenesInBuild", "Scenes in Build...", AppMenuCommand::ProjectScenes);
    b.menu("assets.prefabs", "Prefabs...", AppMenuCommand::ProjectPrefabs);
    b.menu("assets.emptyTrash", "Empty Trash...", AppMenuCommand::ClearTrash).agentSafe = false;
    b.add("assets.clearShaderWarmup", "Clear Shader Warm-up List", C::Editor).description =
        "Empty the project's shader warm-up list (ShaderWarmup.json): Record During Play builds it again";
    // S29: the Project window's selected files; Commit... opens the Commit window either way
    b.menu("assets.vcs.commit", "Commit...", AppMenuCommand::VcsCommit).description = "Version control: commit changed files (the Commit window)";
    b.menu("assets.vcs.revert", "Revert...", AppMenuCommand::VcsRevert).description = "Version control: discard the selected files' changes (undoable)";
    b.menu("assets.vcs.lock", "Lock", AppMenuCommand::VcsLock).description = "Version control: lock the selected files on the server";
    b.menu("assets.vcs.unlock", "Unlock", AppMenuCommand::VcsUnlock).description = "Version control: release the selected files' locks";
    b.menu("assets.vcs.history", "Show History", AppMenuCommand::VcsShowHistory).description = "Version control: the selected file's history";
    b.menu("assets.vcs.diff", "Diff Against Baseline", AppMenuCommand::VcsDiffAgainstBaseline).description =
        "Version control: the selected file against its last commit (scenes and prefabs: the Scene Diff)";
    b.menu("assets.vcs.refresh", "Refresh Version Control", AppMenuCommand::VcsRefresh).description = "Version control: read the status and the locks now";
    b.menu("scripts.reload", "Reload Scripts", AppMenuCommand::ScriptsReload);
    b.menu("scripts.rebuildCpp", "Rebuild C++ Scripts", AppMenuCommand::ScriptsRebuildCpp);
    b.menu("scripts.cancelBuild", "Cancel Script Build", AppMenuCommand::ScriptsCancelBuild);

    b.group = "GameObject";
    const auto& entries = HierarchyWindow::getCreateMenuEntries();
    for (size_t i = 0; i < entries.size(); i++) {
        const CreateMenuEntry& entry = entries[i];
        const std::string id = createEntryCommandId(i);
        const std::string name = entry.submenu ? std::string(entry.submenu) + " / " + entry.label : std::string(entry.label);
        CommandDef& def = b.menu("", "", AppMenuCommand::CreateGameObject, C::Editor,
                                 entry.type == EntityCreationType::GAMEOBJECT ? "Ctrl+Shift+N" : nullptr);
        def.id = id;
        def.name = name;
        def.menuPayload = std::to_string(i);
        if (entry.type == EntityCreationType::GAMEOBJECT) {
            b.menu("gameobject.createEmptyChild", "Create Empty Child", AppMenuCommand::CreateEmptyChild, C::Editor, "Alt+Shift+N");
        }
    }

    b.menu("gameobject.addCameraShakerToMainCamera", "Add Camera Shaker to Main Camera", AppMenuCommand::AddCameraShakerToMainCamera);

    b.group = "Component";
    b.menu("component.add", "Add Component", AppMenuCommand::AddComponent, C::Editor, "Ctrl+Shift+A");

    b.group = "Window";
    b.menu("window.scene", "Scene", AppMenuCommand::FocusScene, C::Editor, "Ctrl+1");
    b.menu("window.game", "Game", AppMenuCommand::FocusGame, C::Editor, "Ctrl+2");
    b.menu("window.inspector", "Inspector", AppMenuCommand::ToggleInspector, C::Editor, "Ctrl+3");
    b.menu("window.hierarchy", "Hierarchy", AppMenuCommand::ToggleHierarchy, C::Editor, "Ctrl+4");
    b.menu("window.project", "Project", AppMenuCommand::ToggleProject, C::Editor, "Ctrl+5");
    b.menu("window.animation", "Animation", AppMenuCommand::ToggleAnimation, C::Editor, "Ctrl+6");
    b.menu("window.poseAnimation", "Pose Animation", AppMenuCommand::TogglePoseAnimation);
    b.menu("window.animator", "Animator", AppMenuCommand::ToggleAnimator);
    b.menu("window.vertexAnimationBaker", "Vertex Animation Baker", AppMenuCommand::ToggleVertexAnimationBaker);
    b.menu("window.profiler", "Profiler", AppMenuCommand::ToggleProfiler, C::Editor, "Ctrl+7");
    b.menu("window.console", "Console", AppMenuCommand::ToggleConsole, C::Editor, "Ctrl+Shift+C");
    b.menu("window.bookmarks", "Bookmarks", AppMenuCommand::ToggleBookmarks);
    b.menu("window.undoHistory", "Undo History", AppMenuCommand::ToggleUndoHistory);
    b.menu("window.playModeChanges", "Play Mode Changes", AppMenuCommand::TogglePlayModeChanges);
    b.menu("window.colorPicker", "Color Picker", AppMenuCommand::ToggleColorPicker).description =
        "The Color window on its own: eyedropper, swatches, hexadecimal";
    b.menu("window.recorder", "Recorder", AppMenuCommand::ToggleRecorder).description =
        "Record the Game view to a PNG sequence or a movie";
    b.menu("recorder.toggle", "Start Recording", AppMenuCommand::ToggleRecording, C::Global, "F10").description =
        "Start or stop the Recorder (enters Play when needed)";
    b.menu("window.terrain", "Terrain", AppMenuCommand::ToggleTerrain);
    b.menu("window.soundBrowser", "Sound Browser", AppMenuCommand::ToggleSoundBrowser);
    b.menu("window.audioCaptures", "Audio Captures", AppMenuCommand::ToggleAudioCaptures);
    b.menu("window.audioMixer", "Audio Mixer", AppMenuCommand::ToggleAudioMixer);
    b.menu("audioMixer.resetLoudness", "Reset Integrated Loudness", AppMenuCommand::ResetLoudness).description =
        "Start the Audio Mixer's integrated loudness and true-peak hold over";
    b.menu("window.palette", "Palette", AppMenuCommand::TogglePalette).description =
        "Pixel-art palettes: swatches, ramps, variants and how a texture uses them";
    b.menu("window.inputActions", "Input Actions", AppMenuCommand::ToggleInputActions);
    b.menu("window.navigation", "Navigation", AppMenuCommand::ToggleNavigation);
    b.menu("window.textureSynthesis", "Texture Synthesis", AppMenuCommand::ToggleTextureSynthesis);
    b.menu("window.spriteNormals", "Sprite Normal Map", AppMenuCommand::ToggleSpriteNormals).description =
        "Generate and edit normal maps for 2D sprites";
    b.menu("window.physicsPlacement", "Physics Placement", AppMenuCommand::TogglePhysicsPlacement);
    b.menu("window.spritePreview", "Sprite Preview", AppMenuCommand::ToggleSpritePreview).description =
        "Play an .aseprite file's tags, with onion skin, slices and its import side by side";
    b.menu("window.missingAssets", "Missing Assets", AppMenuCommand::ToggleMissingAssets);
    b.menu("window.assetReferences", "References", AppMenuCommand::ToggleAssetReferences);
    b.menu("window.frameDebugger", "Frame Debugger", AppMenuCommand::ToggleFrameDebugger);
    b.menu("window.systems", "Systems", AppMenuCommand::ToggleSystems);
    b.menu("window.jobsMemory", "Jobs & Memory", AppMenuCommand::ToggleJobsMemory);
    b.menu("window.meshAudit", "Mesh Optimization Audit", AppMenuCommand::ToggleMeshAudit);
    b.menu("window.buildReport", "Build Report", AppMenuCommand::ToggleBuildReport);
    b.menu("window.animationStatistics", "Animation Statistics", AppMenuCommand::ToggleAnimationStatistics);
    b.menu("window.mcpActivity", "MCP Server", AppMenuCommand::ToggleMcpActivity);
    b.menu("window.vcs.commit", "Commit", AppMenuCommand::ToggleVcsCommit);
    b.menu("window.vcs.history", "History", AppMenuCommand::ToggleVcsHistory);
    b.menu("window.vcs.sceneDiff", "Scene Diff", AppMenuCommand::ToggleVcsSceneDiff);
    b.menu("window.vcs.conflicts", "Conflicts", AppMenuCommand::ToggleVcsConflicts);
    b.menu("window.detachableWindows", "Detachable Windows", AppMenuCommand::ToggleDetachableWindows);

    b.group = "Layouts";
    b.menu("window.layouts.apply", "Apply Layout", AppMenuCommand::LayoutApply).description = "Switch to a saved window layout";
    b.menu("window.layouts.save", "Save Layout...", AppMenuCommand::LayoutSave).description = "Save the window layout as a named preset";
    b.menu("window.layouts.rename", "Rename Layout", AppMenuCommand::LayoutRename);
    b.menu("window.layouts.delete", "Delete Layout", AppMenuCommand::LayoutDelete);
    b.menu("window.layouts.saveToFile", "Save Layout to File...", AppMenuCommand::LayoutSaveToFile);
    b.menu("window.layouts.loadFromFile", "Load Layout from File...", AppMenuCommand::LayoutLoadFromFile);
    b.menu("window.layouts.reset", "Reset Layout", AppMenuCommand::ResetLayout).description = "Apply the Default layout";

    b.group = "Help";
    b.menu("help.about", "About JasperEngine", AppMenuCommand::About);
    b.menu("help.documentation", "Documentation", AppMenuCommand::Documentation, C::Global, "F1");
    b.menu("help.keyboardShortcuts", "Keyboard Shortcuts", AppMenuCommand::KeyboardShortcuts);
    b.menu("help.reportIssue", "Report an Issue...", AppMenuCommand::ReportIssue);

    b.group = "Tools";
    b.add("tools.hand", "Hand Tool", C::Editor, "Q").description = "Pan the Scene view";
    b.add("tools.move", "Move Tool", C::Editor, "W");
    b.add("tools.rotate", "Rotate Tool", C::Editor, "E");
    b.add("tools.scale", "Scale Tool", C::Editor, "R");
    b.add("tools.rect", "Rect Tool", C::Editor, "T").description = "2D and UI scenes";
    b.add("tools.toggleGlobalLocal", "Toggle Global/Local", C::Editor, "X").description =
        "Toggle tool handle rotation between Global and Local";
    b.add("tools.physics", "Physics", C::Editor, "G").description =
        "Move, Rotate and Scale push the selection through a physics simulation (3D scenes, not in Play)";

    b.group = "Scene View";
    // S26 (docs/specs/S26-scene-view.md R14-R24): Alt+F as Unity's, Home kept
    b.add("sceneView.frameAll", "Frame All", C::SceneView, "Alt+F", "Home").description =
        "Frame everything the view shows and can pick";
    b.add("sceneView.viewFront", "Front View", C::SceneView, "Numpad1");
    b.add("sceneView.viewBack", "Back View", C::SceneView, "Ctrl+Numpad1");
    b.add("sceneView.viewRight", "Right View", C::SceneView, "Numpad3");
    b.add("sceneView.viewLeft", "Left View", C::SceneView, "Ctrl+Numpad3");
    b.add("sceneView.viewTop", "Top View", C::SceneView, "Numpad7", "Ctrl+Numpad9");
    b.add("sceneView.viewBottom", "Bottom View", C::SceneView, "Numpad9", "Ctrl+Numpad7");
    b.add("sceneView.hideSelection", "Hide Selection", C::SceneView, "H").description =
        "Hide the selection in the Scene view (again: show it)";
    b.add("sceneView.showAll", "Show All", C::SceneView, "Alt+H").description = "Show every GameObject hidden in the Scene view";
    // S41 R21: a 2D scene drawn through a Pixel Perfect Camera
    b.add("sceneView.pixelZoom1x", "Pixel Zoom 1x", C::SceneView, "Alt+1").description =
        "Pixel Zoom: one art pixel per screen pixel (a 2D scene with a Pixel Perfect Camera)";
    b.add("sceneView.pixelZoomGame", "Pixel Zoom Game", C::SceneView, "Alt+2").description =
        "Pixel Zoom: the Game view's pixel scale (a 2D scene with a Pixel Perfect Camera)";
    b.add("sceneView.togglePickLock", "Toggle Pick Lock", C::SceneView, "L").description =
        "Lock the selection against picking in the Scene view, or unlock it";
    b.add("sceneView.measure", "Measure", C::SceneView, "M").description =
        "Measure distances: drag from A to B (Esc ends it)";
    b.add("sceneView.deleteMeasure", "Delete Measure", C::SceneView, "Delete", "Backspace").description =
        "Delete the selected measure (while one is selected)";
    // S75 R25: rebindable, no default key
    b.add("navigation.pathTester", "Path Tester", C::SceneView).description =
        "Query navigation paths in the Scene view: Shift+click the start, click the end (Esc ends it)";
    for (int slot = 1; slot <= 9; slot++) {
        static const char* recallIds[] = {"sceneView.recallBookmark1", "sceneView.recallBookmark2", "sceneView.recallBookmark3",
                                          "sceneView.recallBookmark4", "sceneView.recallBookmark5", "sceneView.recallBookmark6",
                                          "sceneView.recallBookmark7", "sceneView.recallBookmark8", "sceneView.recallBookmark9"};
        static const char* storeIds[] = {"sceneView.storeBookmark1", "sceneView.storeBookmark2", "sceneView.storeBookmark3",
                                         "sceneView.storeBookmark4", "sceneView.storeBookmark5", "sceneView.storeBookmark6",
                                         "sceneView.storeBookmark7", "sceneView.storeBookmark8", "sceneView.storeBookmark9"};
        static const char* recallNames[] = {"Recall Camera Bookmark 1", "Recall Camera Bookmark 2", "Recall Camera Bookmark 3",
                                            "Recall Camera Bookmark 4", "Recall Camera Bookmark 5", "Recall Camera Bookmark 6",
                                            "Recall Camera Bookmark 7", "Recall Camera Bookmark 8", "Recall Camera Bookmark 9"};
        static const char* storeNames[] = {"Store Camera Bookmark 1", "Store Camera Bookmark 2", "Store Camera Bookmark 3",
                                           "Store Camera Bookmark 4", "Store Camera Bookmark 5", "Store Camera Bookmark 6",
                                           "Store Camera Bookmark 7", "Store Camera Bookmark 8", "Store Camera Bookmark 9"};
        static const char* recallKeys[] = {"Shift+1", "Shift+2", "Shift+3", "Shift+4", "Shift+5", "Shift+6", "Shift+7", "Shift+8", "Shift+9"};
        static const char* storeKeys[] = {"Ctrl+Shift+1", "Ctrl+Shift+2", "Ctrl+Shift+3", "Ctrl+Shift+4", "Ctrl+Shift+5",
                                          "Ctrl+Shift+6", "Ctrl+Shift+7", "Ctrl+Shift+8", "Ctrl+Shift+9"};
        b.add(recallIds[slot - 1], recallNames[slot - 1], C::SceneView, recallKeys[slot - 1]);
        b.add(storeIds[slot - 1], storeNames[slot - 1], C::SceneView, storeKeys[slot - 1]);
    }
    b.gesture("sceneView.cycleCandidates", "Cycle Overlapping Objects", C::SceneView, "Ctrl+Mouse Wheel",
              "Step through what lies under the pointer; the next click confirms");
    b.gesture("sceneView.measureVertexSnap", "Measure: Snap to Vertices", C::SceneView, "Hold V",
              "While measuring: the ends snap to the nearest vertex");
    b.gesture("sceneView.flyThrough", "Fly Through (3D)", C::SceneView, "Right Mouse + W/A/S/D/Q/E",
              "Hold Shift to move faster");
    b.gesture("sceneView.orbit", "Orbit (3D)", C::SceneView, "Alt+Left Drag", "Orbit around the view's pivot");
    b.gesture("sceneView.pan", "Pan", C::SceneView, "Middle Drag (2D: also Space+Left Drag)", "Move the view");
    b.gesture("sceneView.zoom", "Zoom", C::SceneView, "Mouse Wheel", "Zoom the view");
    b.gesture("sceneView.physicsDrop", "Drop (Physics)", C::SceneView, "Hold Space", "Drop the selection under gravity");
    b.gesture("sceneView.physicsSnap", "Snap onto Surfaces (Physics)", C::SceneView, "Shift+Left Drag",
              "Drag a selected object onto what is under the pointer");
    b.gesture("sceneView.cancelGesture", "Cancel Gesture", C::SceneView, "Escape",
              "Stops a physics gesture, the Paint Dye brush or the measure tool, or clears the lattice points (Ctrl+Z also undoes)");

    b.group = "Lattice";
    b.add("lattice.selectNone", "Select No Points", C::SceneView, "Alt+A");
    b.add("lattice.invert", "Invert Point Selection", C::SceneView, "Ctrl+I");
    b.add("lattice.expand", "Expand Point Selection", C::SceneView, "Ctrl+=", "Ctrl+NumpadAdd");
    b.add("lattice.shrink", "Shrink Point Selection", C::SceneView, "Ctrl+-", "Ctrl+NumpadSubtract");
    b.add("lattice.toggleFalloff", "Toggle Falloff", C::SceneView, "O").description = "Proportional editing of the lattice points";

    b.group = "Terrain";
    b.add("terrain.brushSmaller", "Brush Smaller", C::SceneView, "[").repeat = true;   // also Paint Dye's radius
    b.add("terrain.brushLarger", "Brush Larger", C::SceneView, "]").repeat = true;
    b.add("terrain.brushWeaker", "Brush Weaker", C::SceneView, "Shift+[").repeat = true;
    b.add("terrain.brushStronger", "Brush Stronger", C::SceneView, "Shift+]").repeat = true;

    // The Animation window editing a clip (docs/animation-clips.md): while it has focus
    b.group = "Animation";
    b.add("animation.playPause", "Play / Pause Preview", C::Animation, "Space");
    b.add("animation.addKey", "Add Key", C::Animation, "K").description = "Key every property at the playhead";
    b.add("animation.addKeyModified", "Key Modified Properties", C::Animation, "Shift+K").description =
        "Key only the properties changed since the last key";
    b.add("animation.previousFrame", "Previous Frame", C::Animation, ",").repeat = true;
    b.add("animation.nextFrame", "Next Frame", C::Animation, ".").repeat = true;
    b.add("animation.previousKey", "Previous Key", C::Animation, "Alt+,").repeat = true;
    b.add("animation.nextKey", "Next Key", C::Animation, "Alt+.").repeat = true;
    b.add("animation.delete", "Delete Keys", C::Animation, "Delete", "Backspace").description =
        "The selected keys, else the selected event";
    b.add("animation.copy", "Copy Keys", C::Animation, "Ctrl+C");
    b.add("animation.paste", "Paste Keys", C::Animation, "Ctrl+V").description = "At the playhead";
    b.add("animation.duplicate", "Duplicate Keys", C::Animation, "Ctrl+D");
    b.add("animation.frameSelected", "Frame Selected Keys", C::Animation, "F");
    b.add("animation.frameAll", "Frame All Keys", C::Animation, "A");
    b.add("animation.record", "Record", C::Animation, "R").description = "Record mode on or off";
    // timeline audio (S40 R10, R22): the selected cue or audio action, in either mode
    b.add("animation.alignCueStart", "Align Start to Playhead", C::Animation).description = "The selected audio cue starts at the playhead";
    b.add("animation.alignCuePeak", "Align Peak to Playhead", C::Animation, "Shift+A").description =
        "The selected audio cue's loudest moment lands on the playhead";
    b.add("animation.alignCueEnd", "Align End to Playhead", C::Animation).description = "The selected audio cue ends at the playhead";
    b.add("animation.addAudioCue", "Add Audio Cue", C::Animation).description = "A clip or Sound Event at the playhead (clip mode)";
    b.add("animation.addSourceCue", "Add Source Cue", C::Animation).description = "Starts an Audio Source of the hierarchy (clip mode)";
    b.add("animation.addSnapshotCue", "Add Snapshot Cue", C::Animation).description = "A mixer snapshot transition (clip mode)";
    b.add("animation.addAudioAction", "Add Audio Clip Action", C::Animation).description = "An audio action at the playhead (Animation sequence)";
    b.add("animation.addSoundEventAction", "Add Sound Event Action", C::Animation).description =
        "A Sound Event action at the playhead (Animation sequence)";
    b.add("animation.addSnapshotAction", "Add Mixer Snapshot Action", C::Animation).description =
        "A Mixer Snapshot action at the playhead (Animation sequence)";

    // The Pose Animation window (docs/pose-animation.md): while it has focus, or in a Scene view
    // posing its character
    b.group = "Pose Animation";
    b.add("pose.playPause", "Play / Pause Preview", C::PoseAnimation, "Space");
    b.add("pose.selectPrevious", "Select Previous Pose", C::PoseAnimation, "Left").repeat = true;
    b.add("pose.selectNext", "Select Next Pose", C::PoseAnimation, "Right").repeat = true;
    b.add("pose.moveLeft", "Move Poses Left", C::PoseAnimation, "Shift+Left").repeat = true;
    b.add("pose.moveRight", "Move Poses Right", C::PoseAnimation, "Shift+Right").repeat = true;
    b.add("pose.selectFirst", "Select First Pose", C::PoseAnimation, "Home");
    b.add("pose.selectLast", "Select Last Pose", C::PoseAnimation, "End");
    b.add("pose.previousFrame", "Previous Frame", C::PoseAnimation, ",").repeat = true;
    b.add("pose.nextFrame", "Next Frame", C::PoseAnimation, ".").repeat = true;
    b.add("pose.rename", "Rename Pose", C::PoseAnimation, "F2");
    b.add("pose.duplicate", "Duplicate Poses", C::PoseAnimation, "Ctrl+D");
    b.add("pose.duplicateMirrored", "Duplicate Poses Mirrored", C::PoseAnimation, "Ctrl+Shift+D");
    b.add("pose.mirror", "Mirror Poses", C::PoseAnimation, "M").description = "Swap the left and right sides";
    b.add("pose.delete", "Delete Poses", C::PoseAnimation, "Delete", "Backspace");
    b.add("pose.ground", "Ground Pose", C::PoseAnimation, "G").description = "Put the feet on the ground";
    b.add("pose.copy", "Copy Pose", C::PoseAnimation, "Ctrl+C");
    b.add("pose.paste", "Paste Pose", C::PoseAnimation, "Ctrl+V");
    b.add("pose.add", "Add Pose", C::PoseAnimation, "N");
    b.add("pose.globalEdit", "Global Edit", C::PoseAnimation, "E").description = "Edit every pose at once";

    // The Sprite Preview window (docs/specs/S44-aseprite-import.md R31): while it has focus; , and .
    // step as in Aseprite
    b.group = "Sprite Preview";
    b.add("spritePreview.playPause", "Play / Pause", C::SpritePreview, "Space");
    b.add("spritePreview.previousFrame", "Previous Frame", C::SpritePreview, ",").repeat = true;
    b.add("spritePreview.nextFrame", "Next Frame", C::SpritePreview, ".").repeat = true;
    b.add("spritePreview.firstFrame", "First Frame", C::SpritePreview, "Home").description = "The first frame of the tag (or of the file)";
    b.add("spritePreview.lastFrame", "Last Frame", C::SpritePreview, "End").description = "The last frame of the tag (or of the file)";
    b.add("spritePreview.zoomIn", "Zoom In", C::SpritePreview, "=", "NumpadAdd").repeat = true;
    b.add("spritePreview.zoomOut", "Zoom Out", C::SpritePreview, "-", "NumpadSubtract").repeat = true;

    // The Palette window (S43): while it, or its Apply Palette dialog, has focus
    b.group = "Palette";
    b.add("palette.delete", "Delete Colours", C::Palette, "Delete", "Backspace");
    b.add("palette.duplicate", "Duplicate Colours", C::Palette, "Ctrl+D");
    b.add("palette.moveLeft", "Move Colours Left", C::Palette, "Alt+Left").repeat = true;
    b.add("palette.moveRight", "Move Colours Right", C::Palette, "Alt+Right").repeat = true;
    b.add("palette.flickerCompare", "Flicker Compare", C::Palette, "Space").description =
        "Apply Palette: the After half shows Before, and back";

    b.group = "Project";
    b.add("project.copy", "Copy Assets", C::Project, "Ctrl+C");
    b.add("project.cut", "Cut Assets", C::Project, "Ctrl+X");
    b.add("project.paste", "Paste Assets", C::Project, "Ctrl+V");
    b.add("project.duplicate", "Duplicate Assets", C::Project, "Ctrl+D");
    b.add("project.selectAll", "Select All Assets", C::Project, "Ctrl+A");
    b.add("project.delete", "Delete Assets", C::Project, "Delete", "Backspace");
    b.add("project.rename", "Rename Asset", C::Project, "F2");
    b.gesture("project.resizeItems", "Resize Items", C::Project, "Ctrl+Mouse Wheel", "Change the icon size in 16 px steps");

    b.group = "Sound Browser";
    b.add("audioCaptures.playMoment", "Play Moment", C::AudioCaptures, "Enter");
    b.add("audioCaptures.stop", "Stop", C::AudioCaptures, "Escape");
    b.add("audioCaptures.nextMoment", "Next Moment", C::AudioCaptures, "Down");
    b.add("audioCaptures.previousMoment", "Previous Moment", C::AudioCaptures, "Up");
    b.add("soundBrowser.focusSearch", "Search Sounds", C::SoundBrowser, "Ctrl+F");
    b.add("soundBrowser.next", "Next Sound", C::SoundBrowser, "Down", "Right");
    b.add("soundBrowser.previous", "Previous Sound", C::SoundBrowser, "Up", "Left");
    b.add("soundBrowser.play", "Play or Stop Sound", C::SoundBrowser, "Space");
    b.add("soundBrowser.playMatched", "Play Sound Loudness-Matched", C::SoundBrowser, "Shift+Space");
    b.add("soundBrowser.use", "Use Sound", C::SoundBrowser, "Enter").description = "Selects it; a built-in clip is imported first";
    b.add("soundBrowser.selectAll", "Select All Sounds", C::SoundBrowser, "Ctrl+A");
    b.add("soundBrowser.stop", "Stop Sound or Clear Search", C::SoundBrowser, "Escape");

    b.group = "Sound Event";
    b.add("soundEvent.audition", "Audition x8", C::Editor).description = "The selected Sound Event's next 8 plays, rendered offline";
    b.add("soundEvent.matchLevels", "Match Clip Levels", C::Editor).description = "The selected Sound Event's clips at their average loudness";
    b.add("soundEvent.playAsMusic", "Play as Music", C::Editor).description = "The selected Sound Event as a music playlist, through the music system";

    b.group = "Audio Clip";
    b.add("audioClip.detectTempo", "Detect Tempo", C::Editor).description =
        "The selected audio file's tempo, beats per bar and first beat, drawn in its Inspector; Use writes them";

    b.group = "Console";
    b.add("console.copy", "Copy Message", C::Console, "Ctrl+C");

    // The Curve window (docs/editor/curves.md): while it has focus
    b.group = "Curve";
    b.add("curve.frame", "Frame Curve", C::Curve, "F").description = "The selected keys, else the whole curve";
    b.add("curve.delete", "Delete Curve Keys", C::Curve, "Delete", "Backspace");
    b.add("curve.copy", "Copy Curve Keys", C::Curve, "Ctrl+C");
    b.add("curve.paste", "Paste Curve Keys", C::Curve, "Ctrl+V").description = "At the pointer's time, else at the last click's";

    // The Color window (docs/editor/color-picker.md): while it has focus
    b.group = "Color Picker";
    b.add("colorPicker.eyedropper", "Eyedropper", C::ColorPicker, "I").description = "Take a colour from anywhere on the screen";

    return out;
}
